#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace rgd {

// Everything we know about a completed run, captured on the main thread at levelComplete().
struct LevelMeta {
    std::string levelName;
    int levelID = 0;
    int attempts = 0;
    bool practice = false;
    bool startPos = false;
    double runSeconds = 0.0;       // recorded gameplay time (pauses excluded), filled in by RunRecorder
    std::time_t completedAt = 0;
};

// Technical details about the produced video, written into the sidecar JSON.
struct VideoInfo {
    std::string codec;
    int width = 0;
    int height = 0;
    int fps = 0;
    uint64_t frames = 0;
    uint64_t droppedFrames = 0;
    bool hasAudio = false;
    bool hasMic = false;
};

} // namespace rgd
