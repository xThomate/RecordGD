#pragma once

#include <Geode/Geode.hpp>

#include <string>
#include <vector>

namespace rgd {

// Records a microphone through GD's own FMOD system (FMOD record drivers), polled once per frame on
// the main thread. Output is resampled to the game's mixer rate and turned into interleaved stereo,
// so the encoder can add it straight onto the game audio.
class MicCapture {
public:
    static MicCapture& get();

    // Starts (or keeps) recording from `device` (0 = system default, N = Nth driver, 1-based).
    // Returns false if no microphone could be opened.
    bool start(int device, int targetRate);
    void stop();
    bool running() const { return m_sound != nullptr; }
    std::string const& deviceName() const { return m_deviceName; }

    // Reads everything recorded since the last call. With keep = false the audio is thrown away
    // (used while paused or between attempts, so the ring buffer never overflows).
    std::vector<float> poll(bool keep, float gain);

private:
    MicCapture() = default;
    int pickDriver(FMOD::System* system, int device);

    FMOD::Sound* m_sound = nullptr;
    int m_driver = -1;
    int m_requestedDevice = -1;
    int m_nativeRate = 0;
    int m_channels = 1;
    int m_targetRate = 0;
    unsigned int m_lengthFrames = 0;
    unsigned int m_lastPos = 0;
    std::string m_deviceName;

    // Linear resampler state, carried across polls.
    float m_lastSample = 0.f;
    double m_resamplePos = 0.0;
    std::vector<float> m_mono;
};

} // namespace rgd
