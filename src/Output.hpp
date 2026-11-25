#pragma once

#include "Types.hpp"

#include <Geode/ui/Notification.hpp>
#include <asp/fs/Path.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace rgd {

// Geode 6 uses asp::fs::Path; the file I/O here uses std::filesystem.
inline std::filesystem::path toStdPath(asp::fs::Path const& p) {
    return std::filesystem::path(p.native());
}

std::filesystem::path modSaveDir();

// Folder for in-progress attempt files. Wiped on startup to remove leftovers from crashes.
std::filesystem::path tempDir();
void cleanupTempDir();

// UTF-8 representation of a path (safe for logging and for passing to FFmpeg).
std::string toUtf8(std::filesystem::path const& p);

std::string sanitizeFileName(std::string_view name);
std::string makeBaseName(LevelMeta const& meta);

// Returns `desired`, or "name (2).ext", "name (3).ext", ... if it already exists.
std::filesystem::path uniquePath(std::filesystem::path const& desired);

// rename(), falling back to copy + remove when moving across drives.
bool moveFile(std::filesystem::path const& from, std::filesystem::path const& to);

bool fileNonEmpty(std::filesystem::path const& p);
void removeQuietly(std::filesystem::path const& p);

void writeSidecar(std::filesystem::path const& path, LevelMeta const& meta, VideoInfo const& info);

// Thread-safe: the notification is shown on the main thread.
void notify(std::string text, geode::NotificationIcon icon, float seconds = 3.f);

} // namespace rgd
