#pragma once

#include <Geode/Geode.hpp>

#include <atomic>
#include <mutex>
#include <vector>

namespace rgd {

// A pass-through FMOD DSP on the master channel group: it hears exactly what the player hears
// (music, SFX, trigger audio) and copies it out as interleaved stereo float while the gate is open.
class AudioCapture {
public:
    static AudioCapture& get();

    // Attaches the DSP once. Returns false if FMOD isn't available.
    bool install();
    int sampleRate() const { return m_sampleRate; }

    void setGate(bool open) { m_gate.store(open, std::memory_order_relaxed); }
    void clear();
    std::vector<float> take();

private:
    AudioCapture() = default;

    static FMOD_RESULT F_CALLBACK readCallback(
        FMOD_DSP_STATE* state, float* inbuffer, float* outbuffer,
        unsigned int length, int inchannels, int* outchannels
    );

    FMOD::DSP* m_dsp = nullptr;
    bool m_installFailed = false;
    int m_sampleRate = 44100;

    std::atomic<bool> m_gate{false};
    std::mutex m_mutex;
    std::vector<float> m_pending;
};

} // namespace rgd
