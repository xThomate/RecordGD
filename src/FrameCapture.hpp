#pragma once

#include <Geode/Geode.hpp>

#include <cstdint>
#include <vector>

namespace rgd {

// Reads the finished frame from the back buffer just before it's presented.
//
// Fast path (any GL 3.0 GPU): blit the back buffer into an FBO at the output resolution
// (GPU downscale), then glReadPixels into a ring of pixel-buffer objects. Each PBO is mapped
// a few frames later, when the GPU has finished the copy, so the game never waits on the GPU.
//
// Fallback: synchronous glReadPixels at native resolution.
class FrameCapture {
public:
    // Whether output can differ from the window size. Valid after the GL context exists.
    bool supportsScaling();

    // (Re)allocates GL resources for this output size and clears pending frames.
    bool prepare(int width, int height);

    // Grabs the current back buffer. The image covers `repeats` video frames.
    // Returns false if nothing could be captured (caller carries the repeats forward).
    bool capture(uint64_t attemptId, int repeats);

    // Pushes all frames still sitting in PBOs to the encoder (call before finalizing).
    void flush();

    // Drops frames still in PBOs (call on discard).
    void reset();

    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    struct Slot {
        GLuint pbo = 0;
        bool pending = false;
        int repeats = 0;
        uint64_t id = 0;
    };
    static constexpr int kSlots = 3;

    void detectSupport();
    void release();
    void pushMapped(Slot& slot);
    void pushFlipped(uint64_t id, int repeats, uint8_t const* src);
    size_t frameBytes() const { return static_cast<size_t>(m_width) * m_height * 4; }

    bool m_checked = false;
    bool m_canBlit = false;
    bool m_canPbo = false;

    int m_width = 0;
    int m_height = 0;
    GLuint m_fbo = 0;
    GLuint m_rbo = 0;
    Slot m_slots[kSlots]{};
    int m_next = 0;
    std::vector<uint8_t> m_scratch;
};

} // namespace rgd
