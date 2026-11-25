#include "Output.hpp"

#include <Geode/Geode.hpp>

#include <cstring>
#include <fstream>

using namespace geode::prelude;

namespace rgd {

namespace {

std::string formatLocalTime(std::time_t t, char const* format) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64]{};
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

std::string jsonEscape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) out += fmt::format("\\u{:04x}", c);
                else out += static_cast<char>(c);
        }
    }
    return out;
}

} // namespace

std::filesystem::path modSaveDir() {
    return toStdPath(Mod::get()->getSaveDir());
}

std::filesystem::path tempDir() {
    return modSaveDir() / "temp";
}

void cleanupTempDir() {
    std::error_code ec;
    auto dir = tempDir();
    if (!std::filesystem::exists(dir, ec)) return;
    std::filesystem::remove_all(dir, ec);
    if (ec) log::warn("Couldn't clear temp folder {}: {}", toUtf8(dir), ec.message());
}

std::string toUtf8(std::filesystem::path const& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<char const*>(u8.data()), u8.size());
}

std::string sanitizeFileName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c < 32 || c > 126 || std::strchr("<>:\"/\\|?*", c)) out += '_';
        else out += static_cast<char>(c);
    }
    // Windows doesn't allow trailing dots/spaces.
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.size() > 80) out.resize(80);
    if (out.empty()) out = "Unnamed";
    return out;
}

std::string makeBaseName(LevelMeta const& meta) {
    std::string name = sanitizeFileName(meta.levelName);
    if (meta.levelID > 0) name += fmt::format(" ({})", meta.levelID);
    name += " " + formatLocalTime(meta.completedAt, "%Y-%m-%d %H-%M-%S");
    name += fmt::format(" {}att", meta.attempts);
    if (meta.practice) name += " [practice]";
    if (meta.startPos) name += " [startpos]";
    return name;
}

std::filesystem::path uniquePath(std::filesystem::path const& desired) {
    std::error_code ec;
    if (!std::filesystem::exists(desired, ec)) return desired;
    auto stem = desired.stem();
    auto ext = desired.extension();
    auto dir = desired.parent_path();
    for (int i = 2; i < 1000; ++i) {
        auto candidate = dir / (toUtf8(stem) + fmt::format(" ({})", i) + toUtf8(ext));
        if (!std::filesystem::exists(candidate, ec)) return candidate;
    }
    return desired;
}

bool moveFile(std::filesystem::path const& from, std::filesystem::path const& to) {
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (!ec) return true;

    ec.clear();
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        log::error("Couldn't move {} to {}: {}", toUtf8(from), toUtf8(to), ec.message());
        return false;
    }
    std::filesystem::remove(from, ec);
    return true;
}

bool fileNonEmpty(std::filesystem::path const& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec) && std::filesystem::file_size(p, ec) > 0 && !ec;
}

void removeQuietly(std::filesystem::path const& p) {
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

void writeSidecar(std::filesystem::path const& path, LevelMeta const& meta, VideoInfo const& info) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        log::warn("Couldn't write {}", toUtf8(path));
        return;
    }
    out << "{\n";
    out << fmt::format("  \"level\": \"{}\",\n", jsonEscape(meta.levelName));
    out << fmt::format("  \"levelID\": {},\n", meta.levelID);
    out << fmt::format("  \"attempts\": {},\n", meta.attempts);
    out << fmt::format("  \"practice\": {},\n", meta.practice ? "true" : "false");
    out << fmt::format("  \"startPos\": {},\n", meta.startPos ? "true" : "false");
    out << fmt::format("  \"runSeconds\": {:.3f},\n", meta.runSeconds);
    out << fmt::format("  \"completedAt\": \"{}\",\n", formatLocalTime(meta.completedAt, "%Y-%m-%dT%H:%M:%S"));
    out << fmt::format("  \"codec\": \"{}\",\n", jsonEscape(info.codec));
    out << fmt::format("  \"width\": {},\n", info.width);
    out << fmt::format("  \"height\": {},\n", info.height);
    out << fmt::format("  \"fps\": {},\n", info.fps);
    out << fmt::format("  \"frames\": {},\n", info.frames);
    out << fmt::format("  \"droppedFrames\": {},\n", info.droppedFrames);
    out << fmt::format("  \"audio\": {},\n", info.hasAudio ? "true" : "false");
    out << fmt::format("  \"microphone\": {},\n", info.hasMic ? "true" : "false");
    out << fmt::format("  \"mod\": \"{}\"\n", Mod::get()->getVersion().toVString());
    out << "}\n";
}

void notify(std::string text, NotificationIcon icon, float seconds) {
    queueInMainThread([text = std::move(text), icon, seconds] {
        Notification::create(text, icon, seconds)->show();
    });
}

} // namespace rgd
