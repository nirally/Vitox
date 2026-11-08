// Vitox M9A.4R (UWP platform port) - XAudio2 AudioAdapter, see vitox_xaudio2_audio.h.
#include "vitox_xaudio2_audio.h"

#include <util/log.h>

#include <xaudio2.h>

#include <atomic>
#include <cstdio>
#include <string>
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

// 0.1.1.3 DIAGNOSTIC: first 4 s of each port as WAV
std::mutex g_dump_mutex;
std::wstring g_dump_dir;
unsigned g_dump_next = 0;
struct PortDump {
    FILE *f = nullptr;
    uint32_t bytes = 0, limit = 0;
    int ch = 0, freq = 0;
};
void wav_header(FILE *f, int ch, int freq, uint32_t data) {
    const uint32_t riff = 36 + data, fmt_len = 16, rate = static_cast<uint32_t>(freq), avg = rate * ch * 2;
    const uint16_t pcm = 1, chans = static_cast<uint16_t>(ch), align = static_cast<uint16_t>(ch * 2), bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt_len, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&chans, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&avg, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

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
    PortDump dump; // 0.1.1.3 DIAGNOSTIC
    std::mutex mutex;
    std::condition_variable cond_var;

    ~XAudio2OutPort() override {
        // DestroyVoice waits for in-flight callbacks, so the callback never sees a destroyed port
        if (voice)
            voice->DestroyVoice();
        if (dump.f)
            fclose(dump.f);
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

void vitox_xaudio2_dump_begin(const std::wstring &dir) {
    std::lock_guard<std::mutex> lock(g_dump_mutex);
    g_dump_dir = dir;
    g_dump_next = 0;
    LOG_INFO("Vitox XAudio2: DIAGNOSTIC audio dump dir '{}' ({} chars)", std::string(dir.begin(), dir.end()), dir.size());
}

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
    {
        std::lock_guard<std::mutex> lock(g_dump_mutex);
        if (g_dump_dir.empty())
            LOG_WARN("Vitox XAudio2: DIAGNOSTIC dump skipped (no dump dir)");
        if (!g_dump_dir.empty() && g_dump_next < 4) {
            const std::wstring path = g_dump_dir + L"\\audio_dump_" + std::to_wstring(g_dump_next++) + L".wav";
            port->dump.f = _wfopen(path.c_str(), L"wb");
            if (!port->dump.f)
                LOG_ERROR("Vitox XAudio2: DIAGNOSTIC dump open failed errno {} for '{}'", errno, std::string(path.begin(), path.end()));
            if (port->dump.f) {
                port->dump.ch = nb_channels;
                port->dump.freq = freq;
                port->dump.limit = static_cast<uint32_t>(freq) * nb_channels * 2 * 4;
                wav_header(port->dump.f, nb_channels, freq, 0);
            }
        }
    }
    LOG_INFO("Vitox XAudio2: port {} ch {} Hz {} samples/buffer{}", nb_channels, freq, nb_sample, port->dump.f ? " (first 4 s dumped, DIAGNOSTIC)" : "");
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
    if (port.dump.f) {
        fwrite(slot, 1, port.len_bytes, port.dump.f);
        port.dump.bytes += static_cast<uint32_t>(port.len_bytes);
        if (port.dump.bytes >= port.dump.limit) {
            wav_header(port.dump.f, port.dump.ch, port.dump.freq, port.dump.bytes);
            fclose(port.dump.f);
            port.dump.f = nullptr;
        }
    }
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
