#include "Settings.hpp"
#include "Output.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

namespace rgd {

namespace {

bool looksUnusable(std::filesystem::path const& p) {
    if (p.empty() || p.is_relative()) return true;
    // An unexpanded placeholder like "{gd_save_dir}" should never be used as a real path.
    using Ch = std::filesystem::path::value_type;
    return p.native().find(static_cast<Ch>('{')) != std::filesystem::path::string_type::npos;
}

} // namespace

RecordSettings RecordSettings::load() {
    auto mod = Mod::get();
    RecordSettings s;
    s.enabled = mod->getSettingValue<bool>("enabled");
    s.resolution = mod->getSettingValue<std::string>("resolution");
    s.fps = static_cast<int>(mod->getSettingValue<int64_t>("fps"));
    s.bitrateMbps = static_cast<int>(mod->getSettingValue<int64_t>("bitrate"));
    s.hardwareEncoding = mod->getSettingValue<bool>("hardware-encoding");
    s.audio = mod->getSettingValue<bool>("audio");
    s.tailSeconds = mod->getSettingValue<double>("tail-seconds");
    s.preRollSeconds = mod->getSettingValue<double>("pre-roll-seconds");
    s.recordPractice = mod->getSettingValue<bool>("record-practice");
    s.recordStartPos = mod->getSettingValue<bool>("record-startpos");
    s.firstCompletionOnly = mod->getSettingValue<bool>("first-completion-only");
    s.notify = mod->getSettingValue<bool>("notify");
    s.askBeforeSaving = mod->getSettingValue<bool>("ask-before-saving");
    s.microphone = mod->getSettingValue<bool>("microphone");
    s.micVolume = static_cast<float>(mod->getSettingValue<double>("mic-volume"));
    s.micDevice = static_cast<int>(mod->getSettingValue<int64_t>("mic-device"));

    // Geode 6 folder settings hold an asp::fs::Path.
    auto out = toStdPath(mod->getSettingValue<asp::fs::Path>("output-folder"));
    s.outputDir = looksUnusable(out) ? modSaveDir() / "recordings" : out;

    if (s.fps < 1) s.fps = 60;
    if (s.bitrateMbps < 1) s.bitrateMbps = 20;
    return s;
}

} // namespace rgd
