#include "AudioCapture.hpp"

#include <cstdio>
#include <cstring>

using namespace geode::prelude;

namespace rgd {

AudioCapture& AudioCapture::get() {
    static AudioCapture* instance = new AudioCapture(); // leaked on purpose: FMOD's mixer thread may call us at exit
    return *instance;
}

bool AudioCapture::install() {
    if (m_dsp) return true;
    if (m_installFailed) return false;

    auto engine = FMODAudioEngine::sharedEngine();
    FMOD::System* system = engine ? engine->m_system : nullptr;
    if (!system) {
        log::warn("FMOD system not available; recording without audio");
        m_installFailed = true;
        return false;
    }

    int rate = 0;
    if (system->getSoftwareFormat(&rate, nullptr, nullptr) == FMOD_OK && rate > 0) m_sampleRate = rate;

    FMOD_DSP_DESCRIPTION desc{};
    desc.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
    std::snprintf(desc.name, sizeof(desc.name), "RecordGD Tap");
    desc.version = 0x00010000;
    desc.numinputbuffers = 1;
    desc.numoutputbuffers = 1;
    desc.read = &AudioCapture::readCallback;

    if (auto res = system->createDSP(&desc, &m_dsp); res != FMOD_OK) {
        log::warn("createDSP failed ({}); recording without audio", static_cast<int>(res));
        m_dsp = nullptr;
        m_installFailed = true;
        return false;
    }

    FMOD::ChannelGroup* master = nullptr;
    if (system->getMasterChannelGroup(&master) != FMOD_OK || !master ||
        master->addDSP(FMOD_CHANNELCONTROL_DSP_HEAD, m_dsp) != FMOD_OK) {
        log::warn("Couldn't attach DSP to the master channel group; recording without audio");
        m_dsp->release();
        m_dsp = nullptr;
        m_installFailed = true;
        return false;
    }

    log::info("Audio capture installed ({} Hz)", m_sampleRate);
    return true;
}

void AudioCapture::clear() {
    std::lock_guard lock(m_mutex);
    m_pending.clear();
}

std::vector<float> AudioCapture::take() {
    std::vector<float> out;
    std::lock_guard lock(m_mutex);
    out.swap(m_pending);
    return out;
}

// Runs on FMOD's mixer thread. Must be cheap and must always pass the audio through untouched.
FMOD_RESULT F_CALLBACK AudioCapture::readCallback(
    FMOD_DSP_STATE*, float* inbuffer, float* outbuffer,
    unsigned int length, int inchannels, int* outchannels
) {
    if (!inbuffer || !outbuffer || inchannels <= 0) return FMOD_OK;

    int const outCh = (outchannels && *outchannels > 0) ? *outchannels : inchannels;
    if (outCh == inchannels) {
        std::memcpy(outbuffer, inbuffer, sizeof(float) * length * static_cast<size_t>(inchannels));
    } else {
        for (unsigned int s = 0; s < length; ++s) {
            for (int c = 0; c < outCh; ++c) {
                outbuffer[s * outCh + c] = c < inchannels ? inbuffer[s * inchannels + c] : 0.f;
            }
        }
    }

    auto& self = get();
    if (!self.m_gate.load(std::memory_order_relaxed)) return FMOD_OK;

    std::lock_guard lock(self.m_mutex);
    // Safety cap (~30 s) in case nobody drains the buffer, e.g. the game window is minimized.
    size_t const cap = static_cast<size_t>(self.m_sampleRate) * 2 * 30;
    if (self.m_pending.size() + length * 2 > cap) return FMOD_OK;

    size_t const base = self.m_pending.size();
    self.m_pending.resize(base + length * 2);
    float* dst = self.m_pending.data() + base;
    for (unsigned int s = 0; s < length; ++s) {
        float const l = inbuffer[s * inchannels];
        float const r = inchannels > 1 ? inbuffer[s * inchannels + 1] : l;
        dst[s * 2] = l;
        dst[s * 2 + 1] = r;
    }
    return FMOD_OK;
}

} // namespace rgd
