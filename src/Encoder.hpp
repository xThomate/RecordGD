#pragma once

#include "Types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace rgd {

// ---- Commands sent from the main thread to the encoder thread (processed strictly in order) ----
//
// One encoder *session* (one temp .mp4) spans many attempts in the same level, so the footage
// before the winning attempt (e.g. the last death) is still there when the level is beaten.
// On save, the run is cut out of the session without re-encoding.

struct StartCmd {
    uint64_t id;
    int width;
    int height;
    int fps;
    int bitrateMbps;
    bool hardware;              // try the GPU encoder first
    int sampleRate;             // 0 = no audio track
    bool gameAudio;             // false = game sound muted in the video (track kept for the microphone)
    std::filesystem::path tempDir;
};

struct FrameCmd {
    uint64_t id;
    std::vector<uint8_t> pixels; // BGRX, top-down, width*height*4 bytes
    int repeats;                 // how many video frames this image covers (constant-FPS pacing)
};

struct AudioCmd {
    uint64_t id;
    std::vector<float> samples;  // interleaved stereo float
};

struct MicCmd {
    uint64_t id;
    std::vector<float> samples;  // interleaved stereo float, already resampled to the game's sample rate
};

// A new attempt starts with the next frame: ask the encoder for a keyframe so the cut is clean.
struct KeyframeCmd {
    uint64_t id;
};

struct DiscardCmd {
    uint64_t id;
};

struct FinalizeCmd {
    uint64_t id;
    LevelMeta meta;
    std::filesystem::path outputDir;
    bool notify;
    uint64_t droppedFrames;
    uint64_t trimStartFrame;     // first session frame to keep (attempt start minus pre-roll)
    bool hold;                   // finish the file but keep it in temp until a ResolveHeldCmd arrives
};

// The player's answer for a held run (end-screen "Save Run" button).
struct ResolveHeldCmd {
    uint64_t id;
    bool save;
};

using Command = std::variant<StartCmd, FrameCmd, AudioCmd, MicCmd, KeyframeCmd, DiscardCmd, FinalizeCmd, ResolveHeldCmd>;

// Owns the worker thread that encodes with Windows Media Foundation (H.264 video + AAC audio,
// written straight into an .mp4). All Media Foundation code lives in Encoder.cpp.
class Encoder {
public:
    static Encoder& get();

    void push(Command cmd);

    // Frames currently waiting in the queue (used for back-pressure on the main thread).
    size_t queuedFrames() const { return m_queuedFrames.load(std::memory_order_relaxed); }

    // Marks every attempt up to and including `id` as discarded, so the worker skips
    // their queued frames (and doesn't even start their encoder) instead of encoding them for nothing.
    void markDiscarded(uint64_t id);

    // Reusable frame buffers, to avoid allocating ~8 MB per frame.
    std::vector<uint8_t> takeBuffer(size_t bytes);
    void recycleBuffer(std::vector<uint8_t>&& buf);

    struct Attempt; // worker-thread state of one attempt (defined in Encoder.cpp)

private:
    Encoder();
    void run();

    void handleStart(Attempt*& cur, StartCmd& cmd);
    void handleFrame(Attempt* cur, FrameCmd& cmd);
    void handleAudio(Attempt* cur, AudioCmd& cmd);
    void handleMic(Attempt* cur, MicCmd& cmd);
    void handleKeyframe(Attempt* cur, KeyframeCmd& cmd);
    void handleResolve(ResolveHeldCmd& cmd);
    void handleDiscard(Attempt*& cur, DiscardCmd& cmd);
    void handleFinalize(Attempt*& cur, FinalizeCmd& cmd);
    void destroyAttempt(Attempt*& cur);
    bool isDiscarded(uint64_t id) const;

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<Command> m_queue;
    std::atomic<size_t> m_queuedFrames{0};
    std::atomic<uint64_t> m_discardedThrough{0};

    std::mutex m_poolMutex;
    std::vector<std::vector<uint8_t>> m_pool;

    // Worker-thread only.
    struct Held;
    Held* m_held = nullptr;      // finished run waiting for the player's decision
    bool m_mfReady = false;
    bool m_hardwareBroken = false; // hardware encoder failed once this session: go straight to software
    bool m_warnedInitFailure = false;
};

} // namespace rgd
