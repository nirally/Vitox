// Vitox M9A.4R (UWP platform port) - XAudio2 AudioAdapter, see vitox_xaudio2_audio.h.
#include "vitox_xaudio2_audio.h"

#include <util/log.h>

#include <xaudio2.h>

#include <atomic>
#include <vector>

#pragma comment(lib, "xaudio2.lib")

namespace {
std::atomic<unsigned> g_ports{ 0 }, g_buffers{ 0 }, g_errors{ 0 }, g_underruns{ 0 };

// at most this many guest buffers are queued on a voice; audio_output blocks while the queue is full (upstream's SDL
// adapter keeps ~4 device buffers queued for the same reason)
constexpr UINT32 MAX_QUEUED = 4;
// ring of slots: a slot is rewritten only MAX_QUEUED + 1 submissions after it was queued, when it has been consumed
constexpr UINT32 SLOTS = MAX_QUEUED * 2;

bool check(HRESULT hr, const char *what) {
    if (SUCCEEDED(hr))
        return true;
    ++g_errors;
    LOG_ERROR("Vitox XAudio2: {} failed (hr=0x{:08X})", what, static_cast<uint32_t>(hr));
    return false;
}

struct XAudio2OutPort;

struct VoiceCallback : public IXAudio2VoiceCallback {
    XAudio2OutPort *port = nullptr;
    void STDMETHODCALLTYPE OnBufferEnd(void *) override;
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT hr) override { check(hr, "voice"); }
};

struct XAudio2OutPort : public AudioOutPort {
    IXAudio2SourceVoice *voice = nullptr;
    VoiceCallback callback;
    std::vector<uint8_t> slots;
    UINT32 next_slot = 0;
    int samples_per_buffer = 0;
    bool started = false; // M9A.5: underruns are counted only after the first submission
    std::mutex mutex;
    std::condition_variable cond_var;

    ~XAudio2OutPort() override {
        // DestroyVoice waits for in-flight callbacks, so the callback never sees a destroyed port
        if (voice)
            voice->DestroyVoice();
    }

    UINT32 queued() {
        XAUDIO2_VOICE_STATE st{};
        voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        return st.BuffersQueued;
    }
};

void STDMETHODCALLTYPE VoiceCallback::OnBufferEnd(void *) {
    {
        std::lock_guard<std::mutex> lock(port->mutex);
    }
    port->cond_var.notify_one();
}
} // namespace

VitoxXAudio2Stats vitox_xaudio2_stats() { return { g_ports.load(), g_buffers.load(), g_errors.load(), g_underruns.load() }; }

VitoxXAudio2Adapter::VitoxXAudio2Adapter(AudioState &audio_state)
    : AudioAdapter(audio_state) {}

VitoxXAudio2Adapter::~VitoxXAudio2Adapter() {
    // AudioState destroys out_ports (their source voices) before the adapter
    if (master)
        master->DestroyVoice();
    if (engine)
        engine->Release();
}

bool VitoxXAudio2Adapter::init() {
    if (!check(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR), "XAudio2Create"))
        return false;
    if (!check(engine->CreateMasteringVoice(&master), "CreateMasteringVoice")) {
        engine->Release();
        engine = nullptr;
        return false;
    }
    XAUDIO2_VOICE_DETAILS d{};
    master->GetVoiceDetails(&d);
    LOG_INFO("Vitox XAudio2: engine up, mastering voice {} ch {} Hz", d.InputChannels, d.InputSampleRate);
    return true;
}

void VitoxXAudio2Adapter::switch_state(const bool pause) {
    if (!engine)
        return;
    if (pause)
        engine->StopEngine();
    else
        check(engine->StartEngine(), "StartEngine");
}

AudioOutPortPtr VitoxXAudio2Adapter::open_port(int nb_channels, int freq, int nb_sample) {
    auto port = std::make_shared<XAudio2OutPort>();
    port->callback.port = port.get();
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = static_cast<WORD>(nb_channels);
    fmt.nSamplesPerSec = static_cast<DWORD>(freq);
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = static_cast<WORD>(nb_channels * sizeof(int16_t));
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    if (!check(engine->CreateSourceVoice(&port->voice, &fmt, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &port->callback), "CreateSourceVoice"))
        return nullptr;
    port->samples_per_buffer = nb_sample;
    port->len_microseconds = (nb_sample * 1'000'000ULL) / freq;
    port->len_bytes = nb_sample * nb_channels * sizeof(int16_t);
    port->slots.resize(static_cast<size_t>(port->len_bytes) * SLOTS);
    check(port->voice->Start(0), "Start");
    ++g_ports;
    LOG_INFO("Vitox XAudio2: port {} ch {} Hz {} samples/buffer", nb_channels, freq, nb_sample);
    return port;
}

void VitoxXAudio2Adapter::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;
    auto &port = static_cast<XAudio2OutPort &>(out_port);
    {
        std::unique_lock<std::mutex> lock(port.mutex);
        while (port.queued() >= MAX_QUEUED) {
            port.cond_var.wait_for(lock, std::chrono::microseconds(port.len_microseconds * 2));
            if (out_port.stopping)
                return;
        }
    }
    if (port.started && port.queued() == 0)
        ++g_underruns;
    port.started = true;
    uint8_t *slot = port.slots.data() + static_cast<size_t>(port.next_slot) * port.len_bytes;
    port.next_slot = (port.next_slot + 1) % SLOTS;
    memcpy(slot, buffer, port.len_bytes);
    XAUDIO2_BUFFER b{};
    b.AudioBytes = static_cast<UINT32>(port.len_bytes);
    b.pAudioData = slot;
    if (check(port.voice->SubmitSourceBuffer(&b), "SubmitSourceBuffer"))
        ++g_buffers;
}

void VitoxXAudio2Adapter::set_volume(AudioOutPort &out_port, float volume) {
    auto &port = static_cast<XAudio2OutPort &>(out_port);
    if (port.voice)
        check(port.voice->SetVolume(volume), "SetVolume");
}

int VitoxXAudio2Adapter::get_rest_sample(AudioOutPort &out_port) {
    auto &port = static_cast<XAudio2OutPort &>(out_port);
    return port.voice ? static_cast<int>(port.queued()) * port.samples_per_buffer : 0;
}

void VitoxXAudio2Adapter::wake_all_ports() {
    for (auto &[_, port_ptr] : state.out_ports) {
        auto &port = static_cast<XAudio2OutPort &>(*port_ptr);
        {
            std::lock_guard<std::mutex> lock(port.mutex);
        }
        port.cond_var.notify_all();
    }
}
