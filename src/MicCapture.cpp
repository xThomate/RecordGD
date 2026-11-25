#include "MicCapture.hpp"

#include <algorithm>
#include <cstring>

using namespace geode::prelude;

namespace rgd {

namespace {

constexpr int kRingSeconds = 4; // FMOD writes into a looping buffer this long

FMOD::System* fmodSystem() {
    auto engine = FMODAudioEngine::sharedEngine();
    return engine ? engine->m_system : nullptr;
}

} // namespace

MicCapture& MicCapture::get() {
    static MicCapture instance;
    return instance;
}

int MicCapture::pickDriver(FMOD::System* system, int device) {
    int numDrivers = 0;
    int numConnected = 0;
    if (system->getRecordNumDrivers(&numDrivers, &numConnected) != FMOD_OK || numDrivers <= 0) return -1;

    int defaultDriver = -1;
    std::string list;
    for (int i = 0; i < numDrivers; ++i) {
        char name[256]{};
        FMOD_DRIVER_STATE state = 0;
        if (system->getRecordDriverInfo(i, name, sizeof(name), nullptr, nullptr, nullptr, nullptr, &state) != FMOD_OK) {
            continue;
        }
        bool const connected = (state & FMOD_DRIVER_STATE_CONNECTED) != 0;
        bool const isDefault = (state & FMOD_DRIVER_STATE_DEFAULT) != 0;
        if (connected && isDefault && defaultDriver < 0) defaultDriver = i;
        list += fmt::format("\n  {}: {}{}{}", i + 1, name, isDefault ? " (default)" : "", connected ? "" : " (disconnected)");
    }
    log::info("Microphone devices (use the number in the 'Microphone Device' setting):{}", list);

    if (device > 0) return device <= numDrivers ? device - 1 : -1;
    return defaultDriver >= 0 ? defaultDriver : 0;
}

bool MicCapture::start(int device, int targetRate) {
    if (targetRate <= 0) return false;
    if (m_sound && device == m_requestedDevice && targetRate == m_targetRate) return true;
    stop();

    auto system = fmodSystem();
    if (!system) return false;

    int const driver = pickDriver(system, device);
    if (driver < 0) {
        log::warn("No microphone found for device setting {}", device);
        return false;
    }

    char name[256]{};
    int rate = 0;
    int channels = 0;
    if (system->getRecordDriverInfo(driver, name, sizeof(name), nullptr, &rate, nullptr, &channels, nullptr) != FMOD_OK ||
        rate <= 0) {
        log::warn("Couldn't query microphone {}", driver + 1);
        return false;
    }
    channels = std::clamp(channels, 1, 8);

    FMOD_CREATESOUNDEXINFO exinfo{};
    exinfo.cbsize = sizeof(exinfo);
    exinfo.numchannels = channels;
    exinfo.format = FMOD_SOUND_FORMAT_PCMFLOAT;
    exinfo.defaultfrequency = rate;
    exinfo.length = static_cast<unsigned int>(rate) * sizeof(float) * channels * kRingSeconds;

    FMOD::Sound* sound = nullptr;
    if (system->createSound(nullptr, FMOD_LOOP_NORMAL | FMOD_OPENUSER, &exinfo, &sound) != FMOD_OK || !sound) {
        log::warn("Couldn't create the microphone buffer");
        return false;
    }
    if (system->recordStart(driver, sound, true) != FMOD_OK) {
        log::warn("Couldn't start recording from microphone '{}'", name);
        sound->release();
        return false;
    }

    m_sound = sound;
    m_driver = driver;
    m_requestedDevice = device;
    m_nativeRate = rate;
    m_channels = channels;
    m_targetRate = targetRate;
    m_lengthFrames = static_cast<unsigned int>(rate) * kRingSeconds;
    m_lastPos = 0;
    m_lastSample = 0.f;
    m_resamplePos = 0.0;
    m_deviceName = name;
    system->getRecordPosition(driver, &m_lastPos);

    log::info("Microphone recording from '{}' ({} Hz, {} ch)", m_deviceName, rate, channels);
    return true;
}

void MicCapture::stop() {
    if (!m_sound) return;
    if (auto system = fmodSystem()) system->recordStop(m_driver);
    m_sound->release();
    m_sound = nullptr;
    m_driver = -1;
    m_requestedDevice = -1;
    log::info("Microphone recording stopped");
}

std::vector<float> MicCapture::poll(bool keep, float gain) {
    std::vector<float> out;
    if (!m_sound) return out;

    auto system = fmodSystem();
    unsigned int pos = 0;
    if (!system || system->getRecordPosition(m_driver, &pos) != FMOD_OK || pos >= m_lengthFrames) return out;

    unsigned int const available = pos >= m_lastPos ? pos - m_lastPos : m_lengthFrames - m_lastPos + pos;
    if (available == 0) return out;

    unsigned int const frameBytes = sizeof(float) * m_channels;
    void* p1 = nullptr;
    void* p2 = nullptr;
    unsigned int len1 = 0;
    unsigned int len2 = 0;
    if (m_sound->lock(m_lastPos * frameBytes, available * frameBytes, &p1, &p2, &len1, &len2) != FMOD_OK) return out;

    // Downmix to mono (a mic is effectively mono; this also handles stereo/array mics).
    m_mono.clear();
    if (keep) {
        m_mono.reserve(available);
        auto addFrames = [&](void const* data, unsigned int bytes) {
            auto const* f = static_cast<float const*>(data);
            unsigned int const frames = bytes / frameBytes;
            for (unsigned int i = 0; i < frames; ++i) {
                float sum = 0.f;
                for (int c = 0; c < m_channels; ++c) sum += f[i * m_channels + c];
                m_mono.push_back(sum / static_cast<float>(m_channels));
            }
        };
        if (p1) addFrames(p1, len1);
        if (p2) addFrames(p2, len2);
    }
    m_sound->unlock(p1, p2, len1, len2);
    m_lastPos = pos;

    if (!keep || m_mono.empty()) {
        if (!keep) m_resamplePos = 0.0;
        return out;
    }

    // Linear resample native rate -> game rate. y = [m_lastSample, mono...], read position in y units.
    double const step = static_cast<double>(m_nativeRate) / m_targetRate;
    size_t const n = m_mono.size();
    out.reserve(static_cast<size_t>(n / step + 2) * 2);
    double p = m_resamplePos;
    while (p < static_cast<double>(n)) {
        auto const i = static_cast<size_t>(p);
        float const frac = static_cast<float>(p - static_cast<double>(i));
        float const a = i == 0 ? m_lastSample : m_mono[i - 1];
        float const b = m_mono[i];
        float const v = (a + (b - a) * frac) * gain;
        out.push_back(v);
        out.push_back(v);
        p += step;
    }
    m_resamplePos = p - static_cast<double>(n);
    m_lastSample = m_mono.back();
    return out;
}

} // namespace rgd
