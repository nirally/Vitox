// Vitox M9A.4R (UWP platform port) - XAudio2 AudioAdapter for vita3k/audio (upstream backends are SDL3 and cubeb, neither
// part of this build). Same contract as upstream's SDLAudioAdapter: one host stream (here an IXAudio2SourceVoice, S16LE at
// the guest's channels/frequency) per guest output port, audio_output blocks while enough audio is already queued and is
// woken when the host consumes a buffer, get_rest_sample reports the queued samples, set_volume is the voice gain,
// switch_state(pause) stops/starts the engine. No resampling or mixing here: XAudio2 converts to the mastering voice.
#pragma once

#include <audio/state.h>

#include <condition_variable>
#include <mutex>

struct IXAudio2;
struct IXAudio2MasteringVoice;

class VitoxXAudio2Adapter : public AudioAdapter {
public:
    explicit VitoxXAudio2Adapter(AudioState &audio_state);
    ~VitoxXAudio2Adapter() override;

    bool init() override;
    void switch_state(const bool pause) override;
    AudioOutPortPtr open_port(int nb_channels, int freq, int nb_sample) override;
    void audio_output(AudioOutPort &out_port, const void *buffer) override;
    void set_volume(AudioOutPort &out_port, float volume) override;
    int get_rest_sample(AudioOutPort &out_port) override;
    void wake_all_ports() override;

    IXAudio2 *engine = nullptr;

private:
    IXAudio2MasteringVoice *master = nullptr;
};

// Vitox: counters for the launcher log (ports opened, buffers submitted, host errors)
struct VitoxXAudio2Stats {
    unsigned ports_opened, buffers_submitted, errors;
    unsigned underruns; // M9A.5: a buffer submitted to a voice that had already drained (audible gap)
};
VitoxXAudio2Stats vitox_xaudio2_stats();
