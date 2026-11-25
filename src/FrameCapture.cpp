#include "FrameCapture.hpp"
#include "Encoder.hpp"

#include <cstring>

using namespace geode::prelude;

namespace rgd {

namespace {

// Saves and restores the GL bindings we touch, so cocos2d (and other mods) never notice us.
struct GLStateGuard {
    bool fbo;
    bool pbo;
    GLint readFbo = 0;
    GLint drawFbo = 0;
    GLint packBuffer = 0;
    GLint packAlign = 4;

    GLStateGuard(bool fbo, bool pbo) : fbo(fbo), pbo(pbo) {
        if (fbo) {
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        }
        if (pbo) glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
        glGetIntegerv(GL_PACK_ALIGNMENT, &packAlign);
    }

    ~GLStateGuard() {
        glPixelStorei(GL_PACK_ALIGNMENT, packAlign);
        if (pbo) glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
        if (fbo) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
        }
    }
};

void clearGLErrors() {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
}

} // namespace

void FrameCapture::detectSupport() {
    if (m_checked) return;
    m_checked = true;
    // These are GLEW function pointers; null means the driver doesn't expose them.
    m_canBlit = glGenFramebuffers && glDeleteFramebuffers && glBindFramebuffer && glBlitFramebuffer &&
                glGenRenderbuffers && glDeleteRenderbuffers && glBindRenderbuffer && glRenderbufferStorage &&
                glFramebufferRenderbuffer && glCheckFramebufferStatus;
    m_canPbo = glGenBuffers && glDeleteBuffers && glBindBuffer && glBufferData && glMapBuffer && glUnmapBuffer;
    log::info("Frame capture: GPU scaling {}, async readback {}", m_canBlit ? "on" : "off", m_canPbo ? "on" : "off");
}

bool FrameCapture::supportsScaling() {
    detectSupport();
    return m_canBlit;
}

void FrameCapture::release() {
    if (m_canPbo) {
        for (auto& slot : m_slots) {
            if (slot.pbo) glDeleteBuffers(1, &slot.pbo);
            slot = Slot{};
        }
    }
    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    if (m_rbo) glDeleteRenderbuffers(1, &m_rbo);
    m_fbo = 0;
    m_rbo = 0;
    m_width = 0;
    m_height = 0;
    m_next = 0;
}

bool FrameCapture::prepare(int width, int height) {
    detectSupport();
    if (width <= 0 || height <= 0) return false;

    if (width == m_width && height == m_height) {
        reset();
        return true;
    }

    release();
    m_width = width;
    m_height = height;

    GLStateGuard guard(m_canBlit, m_canPbo);
    GLint prevRbo = 0;
    if (m_canBlit) glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRbo);

    if (m_canBlit) {
        glGenRenderbuffers(1, &m_rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, m_rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);

        glGenFramebuffers(1, &m_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_rbo);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            log::warn("Capture framebuffer incomplete; falling back to native-resolution capture");
            glDeleteFramebuffers(1, &m_fbo);
            glDeleteRenderbuffers(1, &m_rbo);
            m_fbo = 0;
            m_rbo = 0;
            m_canBlit = false;
        }
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(prevRbo));
    }

    if (m_canPbo) {
        for (auto& slot : m_slots) {
            glGenBuffers(1, &slot.pbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(frameBytes()), nullptr, GL_STREAM_READ);
        }
    }

    clearGLErrors();
    return true;
}

void FrameCapture::reset() {
    for (auto& slot : m_slots) {
        slot.pending = false;
        slot.repeats = 0;
    }
    m_next = 0;
}

void FrameCapture::pushFlipped(uint64_t id, int repeats, uint8_t const* src) {
    // OpenGL rows are bottom-up; the encoder input is declared top-down. Pixels are BGRX (Media Foundation RGB32).
    auto& encoder = Encoder::get();
    auto buf = encoder.takeBuffer(frameBytes());
    size_t const stride = static_cast<size_t>(m_width) * 4;
    for (int y = 0; y < m_height; ++y) {
        std::memcpy(buf.data() + y * stride, src + (m_height - 1 - y) * stride, stride);
    }
    encoder.push(FrameCmd{id, std::move(buf), repeats});
}

void FrameCapture::pushMapped(Slot& slot) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
    auto* src = static_cast<uint8_t const*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));
    if (src) {
        pushFlipped(slot.id, slot.repeats, src);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    } else {
        log::warn("glMapBuffer failed; dropping a frame");
    }
    slot.pending = false;
}

bool FrameCapture::capture(uint64_t attemptId, int repeats) {
    if (m_width <= 0 || m_height <= 0 || repeats <= 0) return false;

    auto const& frame = CCEGLView::get()->getFrameSize();
    int const srcW = static_cast<int>(frame.width);
    int const srcH = static_cast<int>(frame.height);
    if (srcW <= 0 || srcH <= 0) return false;

    GLStateGuard guard(m_fbo != 0, m_canPbo);

    if (m_fbo) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);
        // Blits are clipped by the scissor test, which cocos2d sometimes leaves enabled.
        GLboolean const scissor = glIsEnabled(GL_SCISSOR_TEST);
        if (scissor) glDisable(GL_SCISSOR_TEST);
        bool const sameSize = srcW == m_width && srcH == m_height;
        glBlitFramebuffer(0, 0, srcW, srcH, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT,
                          sameSize ? GL_NEAREST : GL_LINEAR);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    } else {
        // No GPU scaling: only possible while the window still matches the size we started with.
        if (srcW != m_width || srcH != m_height) return false;
        glReadBuffer(GL_BACK);
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    if (m_canPbo) {
        Slot& slot = m_slots[m_next];
        if (slot.pending) pushMapped(slot); // oldest slot: its copy finished frames ago
        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
        glReadPixels(0, 0, m_width, m_height, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        slot.pending = true;
        slot.repeats = repeats;
        slot.id = attemptId;
        m_next = (m_next + 1) % kSlots;
    } else {
        m_scratch.resize(frameBytes());
        glReadPixels(0, 0, m_width, m_height, GL_BGRA, GL_UNSIGNED_BYTE, m_scratch.data());
        pushFlipped(attemptId, repeats, m_scratch.data());
    }
    return true; // guard restores the FBO / PBO bindings
}

void FrameCapture::flush() {
    if (!m_canPbo) return;
    GLStateGuard guard(false, true);
    for (int i = 0; i < kSlots; ++i) {
        Slot& slot = m_slots[(m_next + i) % kSlots]; // oldest first, to keep frame order
        if (slot.pending) pushMapped(slot);
    }
    m_next = 0;
}

} // namespace rgd
