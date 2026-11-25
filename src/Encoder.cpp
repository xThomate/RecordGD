#include "Encoder.hpp"
#include "Output.hpp"

#include <Geode/Geode.hpp>

// Windows Media Foundation: ships with Windows, uses the GPU encoder when available.
#include <Windows.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h> // ICodecAPI
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <thread>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using namespace geode::prelude;
using Microsoft::WRL::ComPtr;

// Bail out of a function returning HRESULT on the first failure.
#define RGD_TRY(expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) return hr_; } while (0)

namespace rgd {

struct Encoder::Attempt {
    uint64_t id = 0;
    ComPtr<IMFSinkWriter> writer;
    ComPtr<ICodecAPI> codecApi;      // encoder controls (keyframes); may be null if the encoder doesn't expose it
    DWORD videoStream = 0;
    DWORD audioStream = 0;
    bool hasAudio = false;
    bool hardware = false;
    int width = 0;
    int height = 0;
    int fps = 60;
    int sampleRate = 0;
    std::filesystem::path videoPath;
    uint64_t framesWritten = 0;
    uint64_t audioFramesWritten = 0; // stereo sample frames
    std::vector<int16_t> pcm;        // conversion scratch
    bool gameAudio = true;
    std::vector<float> mic;          // microphone FIFO (stereo), consumed as game audio arrives
    size_t micRead = 0;
    bool micUsed = false;
    bool failed = false;
};

// A finished .mp4 still in the temp folder, waiting for "Save" or "Discard" on the end screen.
struct Encoder::Held {
    uint64_t id = 0;
    std::filesystem::path videoPath;
    LevelMeta meta;
    VideoInfo info;
    std::filesystem::path outputDir;
    bool notify = true;
};

namespace {

constexpr size_t kMaxPooledBuffers = 8;
constexpr uint64_t kHnsPerSecond = 10'000'000; // Media Foundation time unit: 100 ns
constexpr UINT32 kAacBytesPerSecond = 24000;    // 192 kbps (valid values: 12000, 16000, 20000, 24000)

std::string hrString(HRESULT hr) {
    return fmt::format("0x{:08X}", static_cast<uint32_t>(hr));
}

HRESULT makeVideoTypes(int w, int h, int fps, int bitrateMbps, ComPtr<IMFMediaType>& out, ComPtr<IMFMediaType>& in) {
    auto const bitrate = static_cast<UINT32>(std::min<int64_t>(int64_t(bitrateMbps) * 1'000'000, 0xFFFFFFFFll));

    RGD_TRY(MFCreateMediaType(&out));
    RGD_TRY(out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RGD_TRY(out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    RGD_TRY(out->SetUINT32(MF_MT_AVG_BITRATE, bitrate));
    RGD_TRY(out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RGD_TRY(out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High));
    RGD_TRY(MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, w, h));
    RGD_TRY(MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1));
    RGD_TRY(MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    RGD_TRY(out->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, static_cast<UINT32>(fps))); // a keyframe every second

    RGD_TRY(MFCreateMediaType(&in));
    RGD_TRY(in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RGD_TRY(in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)); // BGRX in memory
    RGD_TRY(in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RGD_TRY(in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(w * 4))); // positive stride = top-down rows
    RGD_TRY(MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, w, h));
    RGD_TRY(MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1));
    RGD_TRY(MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    return S_OK;
}

HRESULT makeAudioTypes(int sampleRate, ComPtr<IMFMediaType>& out, ComPtr<IMFMediaType>& in) {
    RGD_TRY(MFCreateMediaType(&out));
    RGD_TRY(out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    RGD_TRY(out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
    RGD_TRY(out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    RGD_TRY(out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate));
    RGD_TRY(out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
    RGD_TRY(out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAacBytesPerSecond));

    // The AAC encoder only takes 16-bit PCM, so FMOD's float samples are converted before writing.
    RGD_TRY(MFCreateMediaType(&in));
    RGD_TRY(in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    RGD_TRY(in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    RGD_TRY(in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    RGD_TRY(in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate));
    RGD_TRY(in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2));
    RGD_TRY(in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4));
    RGD_TRY(in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * 4));
    return S_OK;
}

HRESULT makeBuffer(void const* data, size_t bytes, ComPtr<IMFMediaBuffer>& buffer) {
    RGD_TRY(MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &buffer));
    BYTE* dst = nullptr;
    RGD_TRY(buffer->Lock(&dst, nullptr, nullptr));
    std::memcpy(dst, data, bytes);
    buffer->Unlock();
    return buffer->SetCurrentLength(static_cast<DWORD>(bytes));
}

HRESULT writeSample(IMFSinkWriter* writer, DWORD stream, IMFMediaBuffer* buffer, LONGLONG time, LONGLONG duration) {
    ComPtr<IMFSample> sample;
    RGD_TRY(MFCreateSample(&sample));
    RGD_TRY(sample->AddBuffer(buffer));
    RGD_TRY(sample->SetSampleTime(time));
    RGD_TRY(sample->SetSampleDuration(duration));
    return writer->WriteSample(stream, sample.Get());
}

HRESULT createWriter(Encoder::Attempt& a, int bitrateMbps, bool hardware, bool withAudio) {
    a.codecApi.Reset();
    a.writer.Reset();
    a.hasAudio = false;

    ComPtr<IMFAttributes> attrs;
    RGD_TRY(MFCreateAttributes(&attrs, 3));
    RGD_TRY(attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE));
    RGD_TRY(attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
    RGD_TRY(attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));

    ComPtr<IMFSinkWriter> writer;
    std::wstring const url = a.videoPath.wstring();
    RGD_TRY(MFCreateSinkWriterFromURL(url.c_str(), nullptr, attrs.Get(), &writer));

    ComPtr<IMFMediaType> vOut, vIn;
    RGD_TRY(makeVideoTypes(a.width, a.height, a.fps, bitrateMbps, vOut, vIn));
    RGD_TRY(writer->AddStream(vOut.Get(), &a.videoStream));
    RGD_TRY(writer->SetInputMediaType(a.videoStream, vIn.Get(), nullptr));

    // Keyframe every second (so a run can be cut out of the session cleanly), plus on demand.
    // Not every encoder exposes ICodecAPI; without it we rely on MF_MT_MAX_KEYFRAME_SPACING.
    a.codecApi.Reset();
    if (SUCCEEDED(writer->GetServiceForStream(a.videoStream, GUID_NULL, __uuidof(ICodecAPI),
                                               reinterpret_cast<void**>(a.codecApi.GetAddressOf())))) {
        VARIANT gop{};
        gop.vt = VT_UI4;
        gop.ulVal = static_cast<ULONG>(a.fps);
        if (FAILED(a.codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &gop))) log::debug("Encoder ignored GOP size");
    }

    if (withAudio) {
        ComPtr<IMFMediaType> aOut, aIn;
        RGD_TRY(makeAudioTypes(a.sampleRate, aOut, aIn));
        RGD_TRY(writer->AddStream(aOut.Get(), &a.audioStream));
        RGD_TRY(writer->SetInputMediaType(a.audioStream, aIn.Get(), nullptr));
    }

    RGD_TRY(writer->BeginWriting());

    a.writer = std::move(writer);
    a.hasAudio = withAudio;
    a.hardware = hardware;
    return S_OK;
}

// Adds queued microphone audio onto a block of game audio (both interleaved stereo at the same rate).
// The two come from different clocks, so the queue is kept short: if the mic runs ahead, old samples
// are dropped; if it runs behind, the missing part is simply silent.
void mixMicrophone(Encoder::Attempt& a, std::vector<float>& block) {
    size_t const available = a.mic.size() - a.micRead;
    size_t const n = std::min(available, block.size()) & ~size_t(1);
    for (size_t i = 0; i < n; ++i) block[i] += a.mic[a.micRead + i];
    a.micRead += n;

    size_t const maxQueued = static_cast<size_t>(a.sampleRate) * 2 * 3 / 10; // 300 ms
    size_t const keepQueued = static_cast<size_t>(a.sampleRate) * 2 / 10;    // 100 ms
    if (a.mic.size() - a.micRead > maxQueued) a.micRead = (a.mic.size() - keepQueued) & ~size_t(1);

    if (a.micRead > 0 && a.micRead * 2 >= a.mic.size()) {
        a.mic.erase(a.mic.begin(), a.mic.begin() + static_cast<std::ptrdiff_t>(a.micRead));
        a.micRead = 0;
    }
}

// Moves a finished temp video into the output folder and writes its sidecar.
bool saveVideo(std::filesystem::path const& video, LevelMeta const& meta, VideoInfo const& info,
               std::filesystem::path const& outputDir, bool notifyUser) {
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) log::warn("Couldn't create {}: {}", toUtf8(outputDir), ec.message());

    auto out = uniquePath(outputDir / (makeBaseName(meta) + ".mp4"));
    if (!moveFile(video, out)) {
        notify("RecordGD: couldn't write the video to the output folder.", NotificationIcon::Error);
        removeQuietly(video);
        return false;
    }

    auto sidecar = out;
    sidecar.replace_extension(".json");
    writeSidecar(sidecar, meta, info);

    log::info("Saved {} ({} frames, {} dropped, audio: {}, mic: {})", toUtf8(out), info.frames, info.droppedFrames,
              info.hasAudio, info.hasMic);
    if (notifyUser) notify(fmt::format("Saved: {}", toUtf8(out.filename())), NotificationIcon::Success, 4.f);
    return true;
}

// Copies everything from the first keyframe at/before `start` (100 ns units) to the end of `in` into `out`,
// without re-encoding, with timestamps rebased to 0. Used to cut the winning run out of a session.
HRESULT remuxFrom(std::filesystem::path const& in, std::filesystem::path const& out, LONGLONG start) {
    ComPtr<IMFSourceReader> reader;
    RGD_TRY(MFCreateSourceReaderFromURL(in.wstring().c_str(), nullptr, &reader));

    ComPtr<IMFAttributes> attrs;
    RGD_TRY(MFCreateAttributes(&attrs, 2));
    RGD_TRY(attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
    RGD_TRY(attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4));
    ComPtr<IMFSinkWriter> writer;
    RGD_TRY(MFCreateSinkWriterFromURL(out.wstring().c_str(), nullptr, attrs.Get(), &writer));

    constexpr DWORD kMaxStreams = 8;
    DWORD writerStream[kMaxStreams]{};
    bool used[kMaxStreams]{};
    int videoIndex = -1;
    int streamCount = 0;
    for (DWORD i = 0; i < kMaxStreams; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(i, 0, &type))) break;
        GUID major{};
        type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        bool const isVideo = major == MFMediaType_Video;
        if (!isVideo && major != MFMediaType_Audio) {
            reader->SetStreamSelection(i, FALSE);
            continue;
        }
        // Same type in and out = passthrough: compressed samples are copied, nothing is re-encoded.
        RGD_TRY(reader->SetStreamSelection(i, TRUE));
        RGD_TRY(reader->SetCurrentMediaType(i, nullptr, type.Get()));
        RGD_TRY(writer->AddStream(type.Get(), &writerStream[i]));
        RGD_TRY(writer->SetInputMediaType(writerStream[i], type.Get(), nullptr));
        used[i] = true;
        ++streamCount;
        if (isVideo && videoIndex < 0) videoIndex = static_cast<int>(i);
    }
    if (videoIndex < 0) return E_FAIL;

    if (start > 0) {
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = start;
        HRESULT const hr = reader->SetCurrentPosition(GUID_NULL, pos); // lands on the keyframe at/before
        PropVariantClear(&pos);
        RGD_TRY(hr);
    }

    RGD_TRY(writer->BeginWriting());

    LONGLONG base = -1; // time of the first keyframe we keep
    std::vector<std::pair<DWORD, ComPtr<IMFSample>>> earlyAudio; // audio read before that keyframe
    bool ended[kMaxStreams]{};
    int open = streamCount;

    auto writeRebased = [&](DWORD index, IMFSample* sample) -> HRESULT {
        LONGLONG t = 0;
        RGD_TRY(sample->GetSampleTime(&t));
        if (t < base) return S_OK; // audio from before the cut
        RGD_TRY(sample->SetSampleTime(t - base));
        return writer->WriteSample(writerStream[index], sample);
    };

    while (open > 0) {
        DWORD index = 0;
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        RGD_TRY(reader->ReadSample(MF_SOURCE_READER_ANY_STREAM, 0, &index, &flags, &ts, &sample));
        if (index >= kMaxStreams || !used[index]) continue;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (!ended[index]) {
                ended[index] = true;
                --open;
            }
            continue;
        }
        if (!sample.Get()) continue;

        if (base < 0) {
            if (static_cast<int>(index) != videoIndex) {
                earlyAudio.emplace_back(index, sample);
                continue;
            }
            UINT32 keyframe = 0;
            sample->GetUINT32(MFSampleExtension_CleanPoint, &keyframe);
            if (!keyframe) continue;
            base = ts;
            for (auto& [i, s] : earlyAudio) RGD_TRY(writeRebased(i, s.Get()));
            earlyAudio.clear();
        }
        RGD_TRY(writeRebased(index, sample.Get()));
    }

    if (base < 0) return E_FAIL;
    return writer->Finalize();
}

HRESULT writeAudio(Encoder::Attempt& a, float const* samples, size_t count) {
    size_t const frames = count / 2;
    if (frames == 0) return S_OK;

    a.pcm.resize(frames * 2);
    for (size_t i = 0; i < frames * 2; ++i) {
        float const v = std::clamp(samples[i], -1.f, 1.f);
        a.pcm[i] = static_cast<int16_t>(v * 32767.f);
    }

    ComPtr<IMFMediaBuffer> buffer;
    RGD_TRY(makeBuffer(a.pcm.data(), a.pcm.size() * sizeof(int16_t), buffer));
    auto const sr = static_cast<uint64_t>(a.sampleRate);
    auto const t0 = static_cast<LONGLONG>(a.audioFramesWritten * kHnsPerSecond / sr);
    auto const t1 = static_cast<LONGLONG>((a.audioFramesWritten + frames) * kHnsPerSecond / sr);
    RGD_TRY(writeSample(a.writer.Get(), a.audioStream, buffer.Get(), t0, t1 - t0));
    a.audioFramesWritten += frames;
    return S_OK;
}

} // namespace

// ---------------------------------------------------------------------------------------------

Encoder& Encoder::get() {
    // Intentionally leaked: the worker thread is detached and must never see a destroyed Encoder.
    static Encoder* instance = new Encoder();
    return *instance;
}

Encoder::Encoder() {
    std::thread([this] { run(); }).detach();
}

void Encoder::push(Command cmd) {
    bool isFrame = std::holds_alternative<FrameCmd>(cmd);
    {
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back(std::move(cmd));
    }
    if (isFrame) m_queuedFrames.fetch_add(1, std::memory_order_relaxed);
    m_queueCv.notify_one();
}

void Encoder::markDiscarded(uint64_t id) {
    uint64_t prev = m_discardedThrough.load();
    while (prev < id && !m_discardedThrough.compare_exchange_weak(prev, id)) {}
}

bool Encoder::isDiscarded(uint64_t id) const {
    return id <= m_discardedThrough.load();
}

std::vector<uint8_t> Encoder::takeBuffer(size_t bytes) {
    std::vector<uint8_t> buf;
    {
        std::lock_guard lock(m_poolMutex);
        if (!m_pool.empty()) {
            buf = std::move(m_pool.back());
            m_pool.pop_back();
        }
    }
    buf.resize(bytes);
    return buf;
}

void Encoder::recycleBuffer(std::vector<uint8_t>&& buf) {
    std::lock_guard lock(m_poolMutex);
    if (m_pool.size() < kMaxPooledBuffers) m_pool.push_back(std::move(buf));
}

void Encoder::run() {
    // Media Foundation needs COM on this thread.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
        hr = MFStartup(MF_VERSION);
        m_mfReady = SUCCEEDED(hr);
    }
    if (!m_mfReady) log::error("Media Foundation failed to start ({}); recording is unavailable", hrString(hr));

    Attempt* cur = nullptr;
    while (true) {
        Command cmd;
        {
            std::unique_lock lock(m_queueMutex);
            m_queueCv.wait(lock, [this] { return !m_queue.empty(); });
            cmd = std::move(m_queue.front());
            m_queue.pop_front();
        }

        if (auto* c = std::get_if<StartCmd>(&cmd)) handleStart(cur, *c);
        else if (auto* c = std::get_if<FrameCmd>(&cmd)) {
            m_queuedFrames.fetch_sub(1, std::memory_order_relaxed);
            handleFrame(cur, *c);
            recycleBuffer(std::move(c->pixels));
        }
        else if (auto* c = std::get_if<AudioCmd>(&cmd)) handleAudio(cur, *c);
        else if (auto* c = std::get_if<MicCmd>(&cmd)) handleMic(cur, *c);
        else if (auto* c = std::get_if<KeyframeCmd>(&cmd)) handleKeyframe(cur, *c);
        else if (auto* c = std::get_if<ResolveHeldCmd>(&cmd)) handleResolve(*c);
        else if (auto* c = std::get_if<DiscardCmd>(&cmd)) handleDiscard(cur, *c);
        else if (auto* c = std::get_if<FinalizeCmd>(&cmd)) handleFinalize(cur, *c);
    }
}

// ---------------------------------------------------------------------------------------------

void Encoder::handleStart(Attempt*& cur, StartCmd& cmd) {
    if (cur) destroyAttempt(cur); // shouldn't happen, but never leak a stray attempt

    // Already discarded before we got to it (e.g. instant death / restart spam): skip the expensive init.
    if (isDiscarded(cmd.id)) return;

    std::error_code ec;
    std::filesystem::create_directories(cmd.tempDir, ec);

    auto* a = new Attempt();
    a->id = cmd.id;
    a->width = cmd.width;
    a->height = cmd.height;
    a->fps = cmd.fps;
    a->videoPath = cmd.tempDir / fmt::format("attempt_{}.mp4", cmd.id);
    a->gameAudio = cmd.gameAudio;
    // The AAC encoder only supports 44.1 kHz and 48 kHz.
    a->sampleRate = (cmd.sampleRate == 44100 || cmd.sampleRate == 48000) ? cmd.sampleRate : 0;
    if (cmd.sampleRate > 0 && a->sampleRate == 0) {
        log::warn("Unsupported audio sample rate {} Hz; recording without audio", cmd.sampleRate);
    }

    bool ok = false;
    if (m_mfReady) {
        bool const tryHardware = cmd.hardware && !m_hardwareBroken;
        // Most capable first; each fallback drops one feature.
        struct Option { bool hardware; bool audio; };
        Option const options[] = {{true, true}, {false, true}, {true, false}, {false, false}};
        for (auto const& opt : options) {
            if (opt.hardware && !tryHardware) continue;
            if (opt.audio && a->sampleRate == 0) continue;
            removeQuietly(a->videoPath);
            HRESULT const hr = createWriter(*a, cmd.bitrateMbps, opt.hardware, opt.audio);
            if (SUCCEEDED(hr)) {
                ok = true;
                break;
            }
            log::warn("Encoder setup failed (hardware: {}, audio: {}): {}", opt.hardware, opt.audio, hrString(hr));
        }
        // Hardware failed but software worked: skip hardware for the rest of the session.
        if (ok && tryHardware && !a->hardware) m_hardwareBroken = true;
    }

    if (!ok) {
        a->failed = true;
        if (!m_warnedInitFailure) {
            m_warnedInitFailure = true;
            notify("RecordGD: couldn't start the video encoder. Check the logs.", NotificationIcon::Error, 5.f);
        }
    } else {
        log::debug("Attempt {}: encoder ready ({}x{} @ {} fps, {}, audio: {})", a->id, a->width, a->height, a->fps,
                   a->hardware ? "hardware" : "software", a->hasAudio);
    }

    cur = a;
}

void Encoder::handleFrame(Attempt* cur, FrameCmd& cmd) {
    if (!cur || cur->id != cmd.id || cur->failed || !cur->writer.Get()) return;
    if (isDiscarded(cmd.id)) return;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = makeBuffer(cmd.pixels.data(), cmd.pixels.size(), buffer);
    // The same image is written `repeats` times; the buffer is shared and never modified afterwards.
    for (int i = 0; SUCCEEDED(hr) && i < cmd.repeats; ++i) {
        auto const t0 = static_cast<LONGLONG>(cur->framesWritten * kHnsPerSecond / cur->fps);
        auto const t1 = static_cast<LONGLONG>((cur->framesWritten + 1) * kHnsPerSecond / cur->fps);
        hr = writeSample(cur->writer.Get(), cur->videoStream, buffer.Get(), t0, t1 - t0);
        if (SUCCEEDED(hr)) ++cur->framesWritten;
    }
    if (FAILED(hr)) {
        log::error("Writing video frame failed: {}", hrString(hr));
        cur->failed = true;
    }
}

void Encoder::handleAudio(Attempt* cur, AudioCmd& cmd) {
    if (!cur || cur->id != cmd.id || cur->failed || !cur->writer.Get() || !cur->hasAudio) return;
    if (isDiscarded(cmd.id)) return;

    if (!cur->gameAudio) std::fill(cmd.samples.begin(), cmd.samples.end(), 0.f); // keep the clock, drop the sound
    if (cur->micUsed) mixMicrophone(*cur, cmd.samples);

    if (HRESULT const hr = writeAudio(*cur, cmd.samples.data(), cmd.samples.size()); FAILED(hr)) {
        // Losing audio shouldn't lose the run: keep writing video.
        log::warn("Writing audio failed ({}); continuing without audio", hrString(hr));
        cur->hasAudio = false;
    }
}

void Encoder::handleMic(Attempt* cur, MicCmd& cmd) {
    if (!cur || cur->id != cmd.id || cur->failed || !cur->hasAudio) return;
    if (isDiscarded(cmd.id)) return;
    cur->mic.insert(cur->mic.end(), cmd.samples.begin(), cmd.samples.end());
    cur->micUsed = true;
}

void Encoder::handleKeyframe(Attempt* cur, KeyframeCmd& cmd) {
    if (!cur || cur->id != cmd.id || cur->failed || !cur->codecApi.Get()) return;
    VARIANT force{};
    force.vt = VT_UI4;
    force.ulVal = 1;
    if (FAILED(cur->codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &force))) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            log::debug("Encoder doesn't support forced keyframes; cuts use the 1-second keyframe interval");
        }
    }
}

void Encoder::destroyAttempt(Attempt*& cur) {
    cur->codecApi.Reset();
    cur->writer.Reset(); // without Finalize(): the file is incomplete and gets deleted
    removeQuietly(cur->videoPath);
    delete cur;
    cur = nullptr;
}

void Encoder::handleDiscard(Attempt*& cur, DiscardCmd& cmd) {
    if (cur && cur->id == cmd.id) destroyAttempt(cur);
}

void Encoder::handleFinalize(Attempt*& cur, FinalizeCmd& cmd) {
    if (!cur || cur->id != cmd.id) {
        log::warn("Finalize for attempt {} which isn't active", cmd.id);
        notify("RecordGD: the run wasn't recorded, nothing to save.", NotificationIcon::Error);
        return;
    }

    Attempt& a = *cur;
    if (a.failed || !a.writer.Get() || a.framesWritten == 0) {
        notify("RecordGD: recording failed, nothing was saved.", NotificationIcon::Error);
        destroyAttempt(cur);
        return;
    }

    // An audio stream that never received a sample can make Finalize() fail: give it a moment of silence.
    if (a.hasAudio && a.audioFramesWritten == 0) {
        std::vector<float> silence(2048, 0.f);
        if (FAILED(writeAudio(a, silence.data(), silence.size()))) a.hasAudio = false;
    }

    HRESULT const hr = a.writer->Finalize();
    a.writer.Reset();
    if (FAILED(hr)) {
        log::error("Finalizing the video failed: {}", hrString(hr));
        notify("RecordGD: couldn't finish writing the video.", NotificationIcon::Error);
        destroyAttempt(cur);
        return;
    }

    // The session holds earlier attempts too: cut out the run (attempt start minus pre-roll).
    auto video = a.videoPath;
    a.videoPath.clear(); // from here on we manage the file ourselves
    uint64_t keptFrames = a.framesWritten;
    if (cmd.trimStartFrame > 0 && cmd.trimStartFrame < a.framesWritten) {
        auto trimmed = video;
        trimmed.replace_filename(fmt::format("run_{}.mp4", a.id));
        // Aim half a frame past the cut so rounding can't land on the keyframe before it.
        auto const start = static_cast<LONGLONG>((cmd.trimStartFrame * 2 + 1) * kHnsPerSecond / (2 * static_cast<uint64_t>(a.fps)));
        HRESULT const rhr = remuxFrom(video, trimmed, start);
        if (SUCCEEDED(rhr) && fileNonEmpty(trimmed)) {
            removeQuietly(video);
            video = trimmed;
            keptFrames = a.framesWritten - cmd.trimStartFrame;
        } else {
            log::warn("Cutting the run out of the session failed ({}); keeping the whole session", hrString(rhr));
            removeQuietly(trimmed);
            notify("RecordGD: couldn't cut the run, the video includes earlier attempts.", NotificationIcon::Warning, 4.f);
        }
    }

    VideoInfo info;
    info.codec = a.hardware ? "H.264 (hardware, Media Foundation)" : "H.264 (software, Media Foundation)";
    info.width = a.width;
    info.height = a.height;
    info.fps = a.fps;
    info.frames = keptFrames;
    info.droppedFrames = cmd.droppedFrames;
    info.hasAudio = a.hasAudio && (a.gameAudio || a.micUsed);
    info.hasMic = a.hasAudio && a.micUsed;

    if (cmd.hold) {
        // Keep the finished file in temp until the player answers on the end screen.
        if (m_held) {
            removeQuietly(m_held->videoPath);
            delete m_held;
        }
        m_held = new Held{a.id, video, cmd.meta, info, cmd.outputDir, cmd.notify};
        destroyAttempt(cur);
        log::debug("Attempt {}: finished, waiting for save decision", m_held->id);
        return;
    }

    destroyAttempt(cur);
    saveVideo(video, cmd.meta, info, cmd.outputDir, cmd.notify);
}

void Encoder::handleResolve(ResolveHeldCmd& cmd) {
    if (!m_held || m_held->id != cmd.id) return;
    if (cmd.save) {
        saveVideo(m_held->videoPath, m_held->meta, m_held->info, m_held->outputDir, m_held->notify);
    } else {
        removeQuietly(m_held->videoPath);
        log::debug("Attempt {}: discarded by player", m_held->id);
    }
    delete m_held;
    m_held = nullptr;
}

} // namespace rgd
