#pragma once

#include "FrameCapture.hpp"
#include "Settings.hpp"
#include "Types.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace rgd {

// Main-thread state machine. One *session* (one encoder, one temp file) runs across all attempts in a
// level; start() at a fresh attempt just marks where that attempt begins in the session.
//
//   Idle --start()--> Recording --complete()--> Tail --(tail time elapsed)--> finalize() --> Idle
//          ^   start() again = new attempt,        |
//          |   same session                   finalize() early (quit / replay / Save button)
//       discard() (quit, practice, settings change, session too long)
//
// finalize() cuts [attempt start - pre-roll, end] out of the session, so the video can open on the last death.
//
// With "Ask Before Saving", finalize() finishes the file but the encoder keeps it in temp ("held")
// until decide() is called from the end-screen button; leaving the level without deciding drops it.
//
// All heavy work (encoding, file I/O) happens on the Encoder thread.
class RunRecorder {
public:
    enum class State { Idle, Recording, Tail };

    static RunRecorder& get();

    bool start();
    void discard();
    void complete(LevelMeta meta);
    void finalize();

    // A completed run exists that the player hasn't saved or discarded yet (ask mode only).
    bool awaitingDecision() const;
    LevelMeta const& pendingMeta() const { return m_meta; }
    // Stops capturing now (so a popup isn't recorded); the run stays pending.
    void stopForDecision();
    void decide(bool save);
    // Drops an unanswered run (the player moved on without choosing).
    void abandonPending() { dropUndecided(true); }

    // Leaving the level: saves/drops as appropriate and releases the microphone.
    void onLevelExit();

    // Called once per presented frame (from the CCEGLView::swapBuffers hook).
    void onSwap();

    State state() const { return m_state; }
    bool isActive() const { return m_state != State::Idle; }
    bool isRecording() const { return m_state == State::Recording; }
    bool inTail() const { return m_state == State::Tail; }

private:
    RunRecorder() = default;
    void drainAudio();
    void pollMic(bool keep);
    void dropUndecided(bool tellPlayer);
    bool targetSize(int& w, int& h);

    using Clock = std::chrono::steady_clock;

    State m_state = State::Idle;
    uint64_t m_id = 0;
    uint64_t m_nextId = 0;
    RecordSettings m_settings;
    bool m_audio = false;   // an audio track is being recorded (game audio and/or microphone)
    bool m_mic = false;     // microphone is mixed into it
    uint64_t m_heldId = 0;  // finished run waiting for the player's decision (0 = none)

    FrameCapture m_capture;
    size_t m_maxQueuedFrames = 16;

    // Constant-FPS pacing: video time advances with real time, but only while the level is visible
    // and unpaused. Each presented frame is written as many times as needed to keep up.
    std::optional<Clock::time_point> m_lastTick;
    double m_recordedSeconds = 0.0;
    uint64_t m_emittedFrames = 0;
    int m_carryRepeats = 0;
    uint64_t m_droppedFrames = 0;

    uint64_t m_attemptStartFrame = 0;  // session frame where the current attempt began
    double m_attemptStartSeconds = 0.0;

    double m_tailLeft = 0.0;
    LevelMeta m_meta;
};

} // namespace rgd
