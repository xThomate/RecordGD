#pragma once

#include <filesystem>
#include <string>

namespace rgd {

// Snapshot of the mod settings. Loaded on the main thread when an attempt starts,
// so a settings change never affects a recording that is already running.
struct RecordSettings {
    bool enabled = true;
    std::string resolution = "1080p";
    int fps = 60;
    int bitrateMbps = 20;
    bool hardwareEncoding = true;
    bool audio = true;
    double tailSeconds = 3.0;
    double preRollSeconds = 0.0;
    bool recordPractice = false;
    bool recordStartPos = false;
    bool firstCompletionOnly = false;
    bool notify = true;
    bool askBeforeSaving = true;
    bool microphone = false;
    float micVolume = 1.f;
    int micDevice = 0; // 0 = system default, N = Nth FMOD record driver (1-based)
    std::filesystem::path outputDir;

    static RecordSettings load();

    // Settings that require a new encoder session when they change between attempts.
    bool sameEncoding(RecordSettings const& o) const {
        return resolution == o.resolution && fps == o.fps && bitrateMbps == o.bitrateMbps &&
               hardwareEncoding == o.hardwareEncoding && audio == o.audio && microphone == o.microphone &&
               micDevice == o.micDevice;
    }
};

} // namespace rgd
