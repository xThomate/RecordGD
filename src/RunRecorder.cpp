#include "RunRecorder.hpp"
#include "AudioCapture.hpp"
#include "Encoder.hpp"
#include "MicCapture.hpp"
#include "Output.hpp"

#include <Geode/Geode.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

namespace rgd {

namespace {

int targetHeightFor(std::string const& resolution, int nativeHeight) {
    if (resolution == "1440p") return 1440;
    if (resolution == "1080p") return 1080;
    if (resolution == "720p") return 720;
    if (resolution == "480p") return 480;
    return nativeHeight; // "native"
}

// Keep the in-flight frame queue under ~512 MB regardless of resolution.
constexpr size_t kQueueMemoryBudget = 512ull * 1024 * 1024;

// A session is restarted at the next attempt once it holds this much footage, to bound the temp file
// (about 750 MB at 20 Mbps). The attempt right after a restart has no pre-roll.
constexpr double kMaxSessionSeconds = 300.0;

} // namespace

RunRecorder& RunRecorder::get() {
    static RunRecorder instance;
    return instance;
}

bool RunRecorder::targetSize(int& w, int& h) {
    auto const& frame = CCEGLView::get()->getFrameSize();
    int const srcW = static_cast<int>(std::lround(frame.width));
    int const srcH = static_cast<int>(std::lround(frame.height));
    if (srcW < 16 || srcH < 16) return false;

    w = srcW;
    h = srcH;
    if (m_capture.supportsScaling()) {
        h = std::min(targetHeightFor(m_settings.resolution, srcH), srcH);
        w = static_cast<int>(std::lround(static_cast<double>(srcW) * h / srcH));
    }
    // H.264 with 4:2:0 chroma needs even dimensions.
    w &= ~1;
    h &= ~1;
    return true;
}

bool RunRecorder::start() {
    // A new attempt means the player left the end screen without choosing.
    dropUndecided(true);
    if (m_state == State::Tail) finalize(); // callers normally do this already

    auto settings = RecordSettings::load();
    if (!settings.enabled) {
        discard();
        MicCapture::get().stop();
        return false;
    }

    // Same level, same encoding settings, session not too long: keep the session running and just
    // mark where this attempt begins, so the previous attempt's ending is available as pre-roll.
    if (m_state == State::Recording && settings.sameEncoding(m_settings) &&
        m_recordedSeconds < kMaxSessionSeconds + settings.preRollSeconds) {
        auto const previous = m_settings;
        m_settings = settings;
        int w = 0;
        int h = 0;
        if (targetSize(w, h) && w == m_capture.width() && h == m_capture.height()) {
            m_capture.flush(); // frames still in GPU readback belong to the previous attempt
            m_attemptStartFrame = m_emittedFrames;
            m_attemptStartSeconds = m_recordedSeconds;
            m_meta = LevelMeta{};
            Encoder::get().push(KeyframeCmd{m_id}); // so the cut can start exactly here
            log::debug("Session {}: new attempt at frame {}", m_id, m_attemptStartFrame);
            return true;
        }
        m_settings = previous; // window size changed: fall through to a new session
    }

    discard();
    m_settings = settings;

    int w = 0;
    int h = 0;
    if (!targetSize(w, h)) return false;

    if (!m_capture.prepare(w, h)) {
        log::error("Couldn't prepare frame capture at {}x{}", w, h);
        return false;
    }

    m_id = ++m_nextId;

    // The game-audio tap also acts as the audio clock, so it's needed for the microphone too.
    int sampleRate = 0;
    m_audio = false;
    m_mic = false;
    if (m_settings.audio || m_settings.microphone) {
        auto& audio = AudioCapture::get();
        if (audio.install()) {
            audio.setGate(false);
            audio.clear();
            sampleRate = audio.sampleRate();
            m_audio = true;
        }
    }

    auto& mic = MicCapture::get();
    if (m_settings.microphone && m_audio) {
        bool const wasRunning = mic.running();
        m_mic = mic.start(m_settings.micDevice, sampleRate);
        if (m_mic) {
            mic.poll(false, 0.f); // drop anything recorded before this attempt
            if (!wasRunning && m_settings.notify) {
                notify(fmt::format("Recording microphone: {}", mic.deviceName()), NotificationIcon::Info, 2.f);
            }
        } else {
            static bool warned = false;
            if (!warned) {
                warned = true;
                notify("RecordGD: couldn't open the microphone. Check the logs.", NotificationIcon::Warning, 4.f);
            }
        }
    } else {
        mic.stop();
    }

    size_t const frameBytes = static_cast<size_t>(w) * h * 4;
    m_maxQueuedFrames = std::clamp<size_t>(kQueueMemoryBudget / frameBytes, 4, 120);

    Encoder::get().push(StartCmd{
        m_id, w, h, m_settings.fps, m_settings.bitrateMbps, m_settings.hardwareEncoding,
        sampleRate, m_settings.audio, tempDir(),
    });

    m_lastTick.reset();
    m_recordedSeconds = 0.0;
    m_emittedFrames = 0;
    m_carryRepeats = 0;
    m_droppedFrames = 0;
    m_tailLeft = 0.0;
    m_attemptStartFrame = 0;
    m_attemptStartSeconds = 0.0;
    m_meta = LevelMeta{};
    m_state = State::Recording;

    log::debug("Session {}: recording at {}x{} @ {} fps (audio: {}, mic: {})", m_id, w, h, m_settings.fps, m_audio, m_mic);
    return true;
}

void RunRecorder::discard() {
    if (m_state == State::Idle) return;

    m_capture.reset();
    if (m_audio) {
        AudioCapture::get().setGate(false);
        AudioCapture::get().clear();
    }
    Encoder::get().markDiscarded(m_id);
    Encoder::get().push(DiscardCmd{m_id});
    m_state = State::Idle;

    log::debug("Session {}: discarded", m_id);
}

void RunRecorder::complete(LevelMeta meta) {
    if (m_state != State::Recording) return;

    m_meta = std::move(meta);
    m_meta.runSeconds = m_recordedSeconds - m_attemptStartSeconds;
    m_tailLeft = std::max(0.0, m_settings.tailSeconds);
    m_state = State::Tail;

    log::debug("Session {}: level complete after {:.2f}s, recording {:.1f}s tail", m_id, m_meta.runSeconds, m_tailLeft);
    if (m_tailLeft <= 0.0) finalize();
}

void RunRecorder::finalize() {
    if (m_state == State::Recording) {
        // Only a completed run (Tail state) can be saved.
        discard();
        return;
    }
    if (m_state != State::Tail) return;

    if (m_audio) AudioCapture::get().setGate(false);
    drainAudio();
    pollMic(true);
    m_capture.flush();

    bool const hold = m_settings.askBeforeSaving;
    if (hold) {
        dropUndecided(false); // only one run can wait for a decision
        m_heldId = m_id;
    }

    // Where the saved video starts: the attempt start, minus the pre-roll (but never before the session).
    auto const preRollFrames = static_cast<uint64_t>(std::lround(std::max(0.0, m_settings.preRollSeconds) * m_settings.fps));
    uint64_t const trimStart = m_attemptStartFrame > preRollFrames ? m_attemptStartFrame - preRollFrames : 0;

    Encoder::get().push(FinalizeCmd{
        m_id, m_meta, m_settings.outputDir, m_settings.notify, m_droppedFrames, trimStart, hold,
    });
    m_state = State::Idle;

    if (!hold && m_settings.notify) notify("Saving run...", NotificationIcon::Loading, 1.5f);
    log::debug("Session {}: finalizing from frame {}{}", m_id, trimStart, hold ? " (waiting for save decision)" : "");
}

bool RunRecorder::awaitingDecision() const {
    if (!m_settings.askBeforeSaving) return false;
    return m_state == State::Tail || m_heldId != 0;
}

void RunRecorder::stopForDecision() {
    if (m_state == State::Tail) finalize();
}

void RunRecorder::decide(bool save) {
    stopForDecision();
    if (m_heldId == 0) return;

    Encoder::get().push(ResolveHeldCmd{m_heldId, save});
    m_heldId = 0;
    if (save && m_settings.notify) notify("Saving run...", NotificationIcon::Loading, 1.5f);
}

void RunRecorder::dropUndecided(bool tellPlayer) {
    if (m_heldId == 0) return;
    Encoder::get().push(ResolveHeldCmd{m_heldId, false});
    m_heldId = 0;
    if (tellPlayer && m_settings.notify) notify("Run not saved", NotificationIcon::Info, 1.5f);
}

void RunRecorder::onLevelExit() {
    if (m_state == State::Tail) finalize(); // auto mode: saves it; ask mode: it becomes undecided...
    else discard();
    dropUndecided(true);                     // ...and an undecided run is dropped here
    MicCapture::get().stop();
}

void RunRecorder::drainAudio() {
    if (!m_audio) return;
    auto samples = AudioCapture::get().take();
    if (!samples.empty()) Encoder::get().push(AudioCmd{m_id, std::move(samples)});
}

void RunRecorder::pollMic(bool keep) {
    auto& mic = MicCapture::get();
    if (!mic.running()) return;
    bool const use = keep && m_mic && m_state != State::Idle;
    auto samples = mic.poll(use, m_settings.micVolume);
    if (use && !samples.empty()) Encoder::get().push(MicCmd{m_id, std::move(samples)});
}

void RunRecorder::onSwap() {
    auto playLayer = PlayLayer::get();
    if (!playLayer) {
        // Left the level by some path we didn't hook.
        if (m_state != State::Idle || m_heldId != 0 || MicCapture::get().running()) onLevelExit();
        return;
    }

    if (m_state == State::Idle) {
        pollMic(false); // keep the mic ring buffer drained between attempts
        return;
    }

    bool const inTransition = typeinfo_cast<CCTransitionScene*>(CCDirector::get()->getRunningScene()) != nullptr;
    bool const capturable = !playLayer->m_isPaused && !inTransition;

    if (m_audio) AudioCapture::get().setGate(capturable);
    drainAudio();
    pollMic(capturable);

    auto const now = Clock::now();
    if (!capturable) {
        m_lastTick.reset(); // freeze the video clock while paused / transitioning
        return;
    }

    double dt = 0.0;
    if (m_lastTick) dt = std::chrono::duration<double>(now - *m_lastTick).count();
    m_lastTick = now;
    m_recordedSeconds += dt;

    // Frame N of the video shows time N / fps. Frame 0 is emitted on the first visible swap.
    auto const target = static_cast<uint64_t>(std::floor(m_recordedSeconds * m_settings.fps)) + 1;
    if (target > m_emittedFrames) {
        int const repeats = static_cast<int>(std::min<uint64_t>(target - m_emittedFrames, 1'000'000)) + m_carryRepeats;
        m_emittedFrames = target;

        bool const encoderBusy = Encoder::get().queuedFrames() >= m_maxQueuedFrames;
        if (encoderBusy || !m_capture.capture(m_id, repeats)) {
            // Can't take a new image right now: the next one will cover these frames too.
            m_carryRepeats = repeats;
            ++m_droppedFrames;
        } else {
            m_carryRepeats = 0;
        }
    }

    if (m_state == State::Tail) {
        m_tailLeft -= dt;
        if (m_tailLeft <= 0.0) finalize();
    }
}

} // namespace rgd
