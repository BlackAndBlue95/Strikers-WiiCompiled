#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <toml.hpp>
#include "platform/host_platform.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

// Sounds with a volume of their own in F10 > Audio: the game's music, sound effects and voices (its
// own Options sliders' groups), plus its menu sounds and cutscenes.
enum class SoundCategory { Music, Effects, Voices, Menus, Cutscenes };
inline constexpr std::array<std::string_view, 5> kSoundCategoryKeys{"music_volume", "effects_volume", "voice_volume",
                                                                    "menu_volume", "cutscene_volume"};

// The frame rates the game can run at (F10 > Graphics > Frame rate); 0 matches the display's refresh.
inline constexpr std::array<uint32_t, 7> kFrameRates{30, 60, 120, 144, 160, 165, 0};
inline bool IsSupportedFrameRate(uint64_t value) {
    return std::find(kFrameRates.begin(), kFrameRates.end(), value) != kFrameRates.end();
}

struct RuntimeUserConfig {
    std::optional<bool> widescreen;
    std::optional<bool> forceAspect169;
    std::optional<int32_t> windowPosX;
    std::optional<int32_t> windowPosY;
    std::optional<uint32_t> windowWidth;
    std::optional<uint32_t> windowHeight;
    std::optional<float> resolutionMultiplier;
    std::optional<std::string> graphicsApi;
    std::optional<std::string> displayMode;
    std::optional<uint32_t> frameRate;
    std::optional<bool> skipUnreadyPipelines;
    std::optional<bool> disableCopyFilter;
    std::optional<bool> scaledEfbCopy;
    std::optional<bool> textureReplacements;
    std::optional<bool> textureDumps;
    std::optional<bool> showFps;
    std::optional<float> audioVolume;
    // Per sound category (SoundCategory above), on top of the game's own Music / SFX / Voice options.
    std::array<std::optional<float>, kSoundCategoryKeys.size()> soundCategoryVolumes;
    std::optional<bool> audioMuted;
    std::optional<bool> audioMixWorker;
    // Real Wii Remotes (with or without Nunchuk / Classic Controller) and Wii U Pro
    // Controllers paired over Bluetooth, driven by SDL's HIDAPI Wii driver. The driver
    // is opt-in on SDL's side, so this decides whether the runtime turns it on.
    std::optional<bool> wiiRemotes;
    // Keep re-enumerating Bluetooth HID devices while no Wii controller is connected
    // (Dolphin's "continuous scanning"), so a remote that dropped or was switched on
    // after launch shows up without restarting.
    std::optional<bool> wiiContinuousScan;
    // The sensor bar (or DolphinBar) sits above the screen rather than below it. KPAD aims the
    // pointer relative to it (KPADCalibrateDPD), like the Wii's sensor bar position setting.
    std::optional<bool> sensorBarAbove;
    std::optional<bool> rumbleEnabled;     // the game's vibration, on remotes and controllers
    std::optional<int32_t> irSensitivity;  // the Wii's IR sensitivity setting, 1-5, for real remotes
    std::optional<double> pointerSpeed;    // the stick-driven pointer's speed (controllers), x1
    // Accelerometer zero-point correction for the Bluetooth Wii Remote, in g and in
    // SDL's sensor frame (x right, y out of the button face, z towards the user).
    // SDL's Wii driver falls back to a nominal zero point when its read of the
    // remote's calibration block times out (common over Bluetooth), so this is
    // measured in the overlay with the remote at rest.
    std::optional<double> wiiAccelOffsetX;
    std::optional<double> wiiAccelOffsetY;
    std::optional<double> wiiAccelOffsetZ;
    // Debugging aid: append every KPAD sample of the Bluetooth remote (raw and
    // corrected accelerometer, buttons) to wii_accel_trace.csv next to Config.toml.
    std::optional<bool> wiiAccelTrace;
    std::optional<bool> networkEnabled;
    std::optional<bool> discordPresenceEnabled;
    // The ID of a Discord application to show Rich Presence under (none by default).
    std::optional<std::string> discordClientId;
    std::optional<std::string> nandRoot;
    std::optional<std::string> dvdRoot;
    // Riivolution packs: each root is laid out like a Wii SD card (riivolution/*.xml and the pack
    // folders). <data>/Riivolution is used too when it exists (hle/storage/riivolution.cpp).
    std::vector<std::string> overlayRoots;
    // Mod packages (runtime/src/mods): [paths] mods_dir, and per mod id whether it is enabled
    // ([mod_packages]) and whether its native plugin may load ([mod_plugins]).
    std::optional<std::string> modsDirectory;
    std::map<std::string, bool> modPackagesEnabled;
    std::map<std::string, bool> modPluginsEnabled;
    std::map<std::string, std::map<std::string, bool>> modSettings;  // [mod_settings."<id>"]: plugin settings
    std::optional<std::string> modSelection[2];  // [mod_selection] home / away: a mod character leading the side
    // Controller mappings use Wii/GameCube button names as keys and up to two
    // comma-separated SDL-style physical button names ("south", or
    // "dpad_up,left_shoulder") as values; pressing either bound button counts.
    std::array<std::optional<std::string>, 12> controllerButtons;
    std::optional<int32_t> muteHotkey;
    // [mods]: controller-friendly changes to the game, all on by default.
    std::optional<bool> modMenuNavigation;
    std::optional<bool> modSelectionBadge;
    std::optional<bool> modNoMegaStrikes;
    std::optional<bool> modUnlockEverything;
    std::optional<bool> modWinByTwo;
    std::optional<bool> modNkFix;
    std::optional<bool> modFastStadiums;
    std::optional<bool> modShotCounter;
    std::optional<bool> modBluePeach;
    std::optional<bool> modKitChoice;
    std::optional<bool> modAllCaptains;
    std::optional<bool> modFastMenus;
    std::optional<bool> modSkipIntro;
    std::optional<std::string> modCaptainTeammates[2];  // "a,b,c": character per sidekick slot, -1 = the team's leader
    std::optional<int> modCaptainSpot[2];  // character in the captain spot: a partner (12-19), -1 = the captain
    std::map<std::string, std::string> controllerExpressions;
};

namespace RuntimeConfigFile {

// Narrow path strings are UTF-8 everywhere in the runtime; string() and the
// char path constructor would use the ANSI codepage on Windows, which drops
// characters the codepage cannot represent.
inline std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline constexpr const char* kConfigFileName = "Config.toml";
inline constexpr const char* kApplicationDirectoryName = "MSCRecomp"; // MSC: keep separate from a real WiiCompiled install

// Portable layout. A directory holding kPortableMarkerFileName is a portable root; every piece of
// runtime user state (Config.toml, NAND, Cache, Logs) lives in <root>/UserData instead of
// %LOCALAPPDATA%. The marker is searched for from the executable's directory upwards, which is what
// makes an installation survive being moved or carried on removable media.
inline constexpr const char* kPortableMarkerFileName = "portable.txt";
inline constexpr const char* kPortableUserDataDirectoryName = "UserData";

// The installed layout puts products two levels below the root (<root>/Install/Base/game.exe). The
// bound is deliberately small so an unrelated marker far up a drive can never capture an ordinary
// installation.
inline constexpr int kPortableSearchDepth = 4;

inline std::string Trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

inline std::string RemoveComment(std::string_view line) {
    bool inSingle = false;
    bool inDouble = false;
    bool escaped = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inDouble && ch == '\\' && !escaped) {
            escaped = true;
            continue;
        }
        if (ch == '\'' && !inDouble) {
            inSingle = !inSingle;
        } else if (ch == '"' && !inSingle && !escaped) {
            inDouble = !inDouble;
        } else if (ch == '#' && !inSingle && !inDouble) {
            return std::string(line.substr(0, i));
        }
        escaped = false;
    }
    return std::string(line);
}

inline bool IsSupportedResolutionMultiplier(float value) {
    static constexpr std::array values{0.0f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
    return std::find(values.begin(), values.end(), value) != values.end();
}

// Must stay in step with the backend table in main.cpp, which is what actually
// maps these to AuroraBackend.
inline bool IsSupportedGraphicsApi(std::string_view value) {
#if defined(__APPLE__)
    static constexpr std::array<std::string_view, 2> values{"auto", "metal"};
// only vulkan for linux
#elif defined(__linux__)
    static constexpr std::array<std::string_view, 2> values{"auto", "vulkan"};
#elif defined(_WIN32)
    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};
#endif
    return std::find(values.begin(), values.end(), value) != values.end();
}

inline bool IsSupportedDisplayMode(std::string_view value) {
    static constexpr std::array<std::string_view, 3> values{
        "windowed", "borderless", "exclusive",
    };
    return std::find(values.begin(), values.end(), value) != values.end();
}

inline std::optional<std::filesystem::path> ExecutableDirectory() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    return RuntimePlatform::ExecutableDirectory();
#else
    // /proc/self/exe is a Linux-specific magic symlink to the running executable; readlink()
    // does not NUL-terminate and silently truncates if the buffer is too small, so this grows
    // the buffer until the result no longer fills it completely, the same doubling strategy as
    // the Windows branch above uses for GetModuleFileNameW.
    std::string buffer(256, '\0');
    for (;;) {
        const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::nullopt;
        }
        if (static_cast<size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<size_t>(length));
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

// The portable root this executable lives under, or nullopt for a normal installation. The answer
// cannot change while the process runs, so it is resolved exactly once: every user-state path
// derives from it and they must not disagree with each other.
inline const std::optional<std::filesystem::path>& PortableRootDirectory() {
    static const std::optional<std::filesystem::path> root = []() -> std::optional<std::filesystem::path> {
        const auto executableDirectory = ExecutableDirectory();
        if (!executableDirectory) {
            return std::nullopt;
        }
        std::filesystem::path current = *executableDirectory;
        for (int level = 0; level <= kPortableSearchDepth; ++level) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(current / kPortableMarkerFileName, ec)) {
                return current;
            }
            const auto parent = current.parent_path();
            if (parent.empty() || parent == current) {
                break;
            }
            current = parent;
        }
        return std::nullopt;
    }();
    return root;
}

inline std::filesystem::path ApplicationDataDirectory() {
    if (const auto& portableRoot = PortableRootDirectory()) {
        return *portableRoot / kPortableUserDataDirectoryName;
    }
#ifdef _WIN32
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath)) && rawPath) {
        const std::filesystem::path directory = std::filesystem::path(rawPath) / kApplicationDirectoryName;
        CoTaskMemFree(rawPath);
        return directory;
    }
#elif defined(__APPLE__)
    return RuntimePlatform::ApplicationDataDirectory(kApplicationDirectoryName);
#else
    // XDG Base Directory spec equivalent of FOLDERID_LocalAppData: $XDG_DATA_HOME if set and
    // non-empty, otherwise its default of $HOME/.local/share.
    if (const char* xdgDataHome = std::getenv("XDG_DATA_HOME"); xdgDataHome && *xdgDataHome) {
        return std::filesystem::path(xdgDataHome) / kApplicationDirectoryName;
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / kApplicationDirectoryName;
    }
#endif
    return std::filesystem::current_path() / kApplicationDirectoryName;
}

inline std::filesystem::path ResolveConfigPath() {
    return ApplicationDataDirectory() / kConfigFileName;
}

inline void EnsureConfigFile() {
    const std::filesystem::path path = ResolveConfigPath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec || std::filesystem::exists(path, ec)) {
        return;
    }

    std::ofstream output(path);
    if (!output) {
        return;
    }
    output << "# Strikers-WiiCompiled user configuration. The F10 bar in the game writes most of\n"
              "# these; anything it doesn't show can be set here.\n\n"
              "[video]\n"
              "widescreen = true\n"
              "force_16_9 = false\n"
              "resolution_multiplier = 1.0\n"
              "# Native frame rate: 30, 60, 120, 144, 160 or 165, or 0 to match the display. Above 60\n"
              "# needs a high-refresh display and costs battery.\n"
              "frame_rate = 60\n"
              "display_mode = \"windowed\"\n"
              "graphics_api = \"auto\"\n"
              "skip_unready_pipelines = true\n"
              "disable_copy_filter = true\n"
              "# EFB copies at the internal resolution (Dolphin's Scaled EFB Copy). Off: native size,\n"
              "# so blur, bloom and depth of field look as at native resolution.\n"
              "scaled_efb_copy = true\n"
              "show_fps = true\n"
              "# Dolphin-style custom textures. When enabled, the renderer indexes\n"
              "# texture_replacements/ next to this file at startup and substitutes\n"
              "# any tex1_<W>x<H>_<hash>[_<tlut hash>]_<format>.dds or .png it finds\n"
              "# there for the matching game texture. texture_dumps writes every\n"
              "# unmatched texture to Cache/texture_dumps under the name a\n"
              "# replacement would need. Both are read once, at startup.\n"
              "texture_replacements = false\n"
              "texture_dumps = false\n\n"
              "[audio]\n"
              "volume = 1.0\n"
              "muted = false\n"
              "# Each kind of sound's volume (0-1), on top of the game's own Options sliders.\n"
              "music_volume = 1.0\n"
              "effects_volume = 1.0\n"
              "voice_volume = 1.0\n"
              "menu_volume = 1.0\n"
              "cutscene_volume = 1.0\n"
              "# Runs the AX/DSP voice mix on its own thread, joined before the\n"
              "# guest can observe it. Set to false to mix inline on the guest thread.\n"
              "mix_worker = true\n\n"
              "[mods]\n"
              "# Controller-friendly changes (also in the F10 bar, Tweaks menu).\n"
              "menu_navigation = true\n"
              "selection_badge = false\n"
              "no_mega_strikes = true\n"
              "# Captain select: X / Y (Wii Remote - / 2) switch the home / away team's kit.\n"
              "kit_choice = true\n"
              "# Gameplay extras (off by default).\n"
              "unlock_everything = false\n"
              "win_by_two = false\n"
              "fast_stadiums = false\n"
              "shot_counter = false\n"
              "blue_peach = false\n"
              "# Fixes the NK bug (shots passing through Kritter after an interrupted deke/teleport).\n"
              "nk_fix = true\n\n"
              "[network]\n"
              "enabled = true\n\n"
              "[discord]\n"
              "# Rich Presence talks only to a locally running Discord client, and\n"
              "# needs the ID of a Discord application to show the game under.\n"
              "enabled = false\n"
              "# client_id = \"123456789012345678\"\n\n"
              "[paths]\n"
              "# Relative paths are relative to this file.\n"
              "# dvd_root = \"Game\"\n"
              "# nand_root = \"NAND\"\n"
              "# mods_dir = \"Mods\"\n"
              "# Riivolution packs, each laid out like a Wii SD card (riivolution/*.xml\n"
              "# plus the pack folders). A Riivolution folder next to this file is used too.\n"
              "# overlay_roots = [\"D:\\\\Riivolution\"]\n";
}

template <typename T>
inline std::optional<T> FindConfigValue(
    const toml::value& document, std::string_view section, std::string_view key) {
    try {
        return toml::find<T>(document, std::string(section), std::string(key));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline std::optional<uint32_t> FindConfigUint(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < 0 || static_cast<uint64_t>(*value) > UINT32_MAX) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(*value);
}

inline std::optional<int32_t> FindConfigInt(
    const toml::value& document, std::string_view section, std::string_view key) {
    const auto value = FindConfigValue<int64_t>(document, section, key);
    if (!value || *value < INT32_MIN || *value > INT32_MAX) {
        return std::nullopt;
    }
    return static_cast<int32_t>(*value);
}

inline std::optional<float> FindConfigFloat(
    const toml::value& document, std::string_view section, std::string_view key) {
    std::optional<double> value = FindConfigValue<double>(document, section, key);
    if (!value) {
        if (const auto integer = FindConfigValue<int64_t>(document, section, key)) {
            value = static_cast<double>(*integer);
        }
    }
    if (!value || !std::isfinite(*value) ||
        *value < -static_cast<double>(std::numeric_limits<float>::max()) ||
        *value > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::nullopt;
    }
    return static_cast<float>(*value);
}

inline void AppendOverlayRoots(RuntimeUserConfig& config, const std::string& roots) {
    size_t begin = 0;
    while (begin < roots.size()) {
        const size_t end = roots.find(';', begin);
        std::string root = Trim(std::string_view(roots).substr(begin, end - begin));
        if (!root.empty()) {
            config.overlayRoots.push_back(std::move(root));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

// Reads every supported setting out of a parsed Config.toml document.
inline RuntimeUserConfig ParseConfigDocument(const toml::value& document) {
    RuntimeUserConfig config;

    static constexpr std::array<std::string_view, 12> buttonKeys = {
        "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
    };
    for (size_t index = 0; index < buttonKeys.size(); ++index) {
        config.controllerButtons[index] =
            FindConfigValue<std::string>(document, "controller", buttonKeys[index]);
    }

    config.modMenuNavigation = FindConfigValue<bool>(document, "mods", "menu_navigation");
    config.modSelectionBadge = FindConfigValue<bool>(document, "mods", "selection_badge");
    config.modNoMegaStrikes = FindConfigValue<bool>(document, "mods", "no_mega_strikes");
    config.modUnlockEverything = FindConfigValue<bool>(document, "mods", "unlock_everything");
    config.modWinByTwo = FindConfigValue<bool>(document, "mods", "win_by_two");
    config.modNkFix = FindConfigValue<bool>(document, "mods", "nk_fix");
    config.modFastStadiums = FindConfigValue<bool>(document, "mods", "fast_stadiums");
    config.modShotCounter = FindConfigValue<bool>(document, "mods", "shot_counter");
    config.modBluePeach = FindConfigValue<bool>(document, "mods", "blue_peach");
    config.modAllCaptains = FindConfigValue<bool>(document, "mods", "all_captains");
    config.modFastMenus = FindConfigValue<bool>(document, "mods", "fast_menus");
    config.modSkipIntro = FindConfigValue<bool>(document, "mods", "skip_intro");
    config.modCaptainTeammates[0] = FindConfigValue<std::string>(document, "mods", "captain_teammates_home");
    config.modCaptainTeammates[1] = FindConfigValue<std::string>(document, "mods", "captain_teammates_away");
    if (auto value = FindConfigInt(document, "mods", "captain_spot_home")) config.modCaptainSpot[0] = static_cast<int>(*value);
    if (auto value = FindConfigInt(document, "mods", "captain_spot_away")) config.modCaptainSpot[1] = static_cast<int>(*value);
    config.modKitChoice = FindConfigValue<bool>(document, "mods", "kit_choice");
    if (auto value = FindConfigInt(document, "audio", "mute_key")) {
        config.muteHotkey = *value;
    }

    if (const auto* section = document.contains("controller") ? &document.at("controller") : nullptr;
        section != nullptr && section->is_table()) {
        for (const auto& [key, value] : section->as_table()) {
            if (key.rfind("expr_", 0) == 0 && value.is_string()) {
                config.controllerExpressions[key] = value.as_string();
            }
        }
    }

    for (const auto& [tableName, target] : {std::pair<const char*, std::map<std::string, bool>*>{"mod_packages", &config.modPackagesEnabled},
                                            std::pair<const char*, std::map<std::string, bool>*>{"mod_plugins", &config.modPluginsEnabled}}) {
        if (const auto* table = document.contains(tableName) ? &document.at(tableName) : nullptr;
            table != nullptr && table->is_table()) {
            for (const auto& [key, value] : table->as_table()) {
                if (value.is_boolean()) (*target)[key] = value.as_boolean();
            }
        }
    }

    if (const auto* settings = document.contains("mod_settings") ? &document.at("mod_settings") : nullptr;
        settings != nullptr && settings->is_table()) {
        for (const auto& [id, table] : settings->as_table()) {
            if (!table.is_table()) continue;
            for (const auto& [key, value] : table.as_table())
                if (value.is_boolean()) config.modSettings[id][key] = value.as_boolean();
        }
    }

    config.widescreen = FindConfigValue<bool>(document, "video", "widescreen");
    config.forceAspect169 = FindConfigValue<bool>(document, "video", "force_16_9");
    config.windowPosX = FindConfigInt(document, "video", "window_x");
    config.windowPosY = FindConfigInt(document, "video", "window_y");
    if (auto value = FindConfigUint(document, "video", "window_width"); value && *value != 0) {
        config.windowWidth = *value;
    }
    if (auto value = FindConfigUint(document, "video", "window_height"); value && *value != 0) {
        config.windowHeight = *value;
    }
    if (auto value = FindConfigFloat(document, "video", "resolution_multiplier");
        value && IsSupportedResolutionMultiplier(*value)) {
        config.resolutionMultiplier = *value;
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "graphics_api")) {
        if (IsSupportedGraphicsApi(*value)) {
            config.graphicsApi = *value;
        } else {
            std::cerr << "[runtime] Unknown video.graphics_api=\"" << *value
                      << "\", using the automatic backend" << std::endl;
        }
    }
    if (auto value = FindConfigValue<std::string>(document, "video", "display_mode");
        value && IsSupportedDisplayMode(*value)) {
        config.displayMode = *value;
    }
    if (auto value = FindConfigUint(document, "video", "frame_rate")) {
        if (IsSupportedFrameRate(*value)) {
            config.frameRate = static_cast<uint32_t>(*value);
        }
    }
    config.skipUnreadyPipelines = FindConfigValue<bool>(document, "video", "skip_unready_pipelines");
    config.disableCopyFilter = FindConfigValue<bool>(document, "video", "disable_copy_filter");
    config.scaledEfbCopy = FindConfigValue<bool>(document, "video", "scaled_efb_copy");
    config.showFps = FindConfigValue<bool>(document, "video", "show_fps");
    config.textureReplacements = FindConfigValue<bool>(document, "video", "texture_replacements");
    config.textureDumps = FindConfigValue<bool>(document, "video", "texture_dumps");

    auto readVolume = [&](std::string_view key) -> std::optional<float> {
        auto value = FindConfigFloat(document, "audio", key);
        return value && *value >= 0.0f && *value <= 1.0f ? value : std::nullopt;
    };
    config.audioVolume = readVolume("volume");
    for (size_t i = 0; i < kSoundCategoryKeys.size(); ++i) config.soundCategoryVolumes[i] = readVolume(kSoundCategoryKeys[i]);
    config.audioMuted = FindConfigValue<bool>(document, "audio", "muted");
    config.audioMixWorker = FindConfigValue<bool>(document, "audio", "mix_worker");
    config.wiiRemotes = FindConfigValue<bool>(document, "controller", "wii_remotes");
    config.wiiContinuousScan = FindConfigValue<bool>(document, "controller", "wii_continuous_scan");
    config.sensorBarAbove = FindConfigValue<bool>(document, "controller", "sensor_bar_above");
    config.rumbleEnabled = FindConfigValue<bool>(document, "controller", "rumble");
    if (auto value = FindConfigInt(document, "controller", "ir_sensitivity"); value && *value >= 1 && *value <= 5)
        config.irSensitivity = *value;
    if (auto value = FindConfigFloat(document, "controller", "pointer_speed"); value && *value >= 0.25f && *value <= 4.0f)
        config.pointerSpeed = *value;
    config.wiiAccelOffsetX = FindConfigValue<double>(document, "controller", "wii_accel_offset_x");
    config.wiiAccelOffsetY = FindConfigValue<double>(document, "controller", "wii_accel_offset_y");
    config.wiiAccelOffsetZ = FindConfigValue<double>(document, "controller", "wii_accel_offset_z");
    config.wiiAccelTrace = FindConfigValue<bool>(document, "controller", "wii_accel_trace");
    config.networkEnabled = FindConfigValue<bool>(document, "network", "enabled");
    config.discordPresenceEnabled = FindConfigValue<bool>(document, "discord", "enabled");
    config.discordClientId = FindConfigValue<std::string>(document, "discord", "client_id");

    config.nandRoot = FindConfigValue<std::string>(document, "paths", "nand_root");
    config.dvdRoot = FindConfigValue<std::string>(document, "paths", "dvd_root");
    config.modsDirectory = FindConfigValue<std::string>(document, "paths", "mods_dir");
    config.modSelection[0] = FindConfigValue<std::string>(document, "mod_selection", "home");
    config.modSelection[1] = FindConfigValue<std::string>(document, "mod_selection", "away");
    if (auto roots = FindConfigValue<std::vector<std::string>>(document, "paths", "overlay_roots")) {
        for (auto& root : *roots) {
            root = Trim(root);
            if (!root.empty()) {
                config.overlayRoots.push_back(std::move(root));
            }
        }
    } else if (auto roots = FindConfigValue<std::string>(document, "paths", "overlay_roots")) {
        AppendOverlayRoots(config, *roots);
    }

    return config;
}

inline RuntimeUserConfig ParseConfig(std::istream& input, std::string sourceName = "Config.toml") {
    try {
        return ParseConfigDocument(toml::parse(input, std::move(sourceName)));
    } catch (const std::exception& exception) {
        std::cerr << "[runtime-config] Invalid TOML; using built-in defaults: "
                  << exception.what() << std::endl;
        return {};
    }
}

inline RuntimeUserConfig LoadConfigFile() {
    EnsureConfigFile();
    std::ifstream file(ResolveConfigPath(), std::ios::binary);
    return file ? ParseConfig(file, PathToUtf8(ResolveConfigPath())) : RuntimeUserConfig{};
}

inline const RuntimeUserConfig& Get() {
    static RuntimeUserConfig config = LoadConfigFile();
    return config;
}

inline RuntimeUserConfig& Mutable() {
    return const_cast<RuntimeUserConfig&>(Get());
}

inline constexpr std::array<std::string_view, 12> kControllerButtonKeys = {
    "a", "b", "x", "y", "start", "z", "l", "r", "up", "down", "left", "right",
};

inline const std::optional<std::string>& ControllerButton(size_t index) {
    static const std::optional<std::string> empty;
    return index < Get().controllerButtons.size() ? Get().controllerButtons[index] : empty;
}

// Update one TOML value without discarding comments, unrelated settings, or
// user-specific paths. This is used by the in-game F10 settings bar.
inline bool WriteSetting(std::string_view section, std::string_view key, std::string_view value) {
    const auto path = ResolveConfigPath();
    std::vector<std::string> lines;
    std::error_code existsError;
    if (std::filesystem::exists(path, existsError)) {
        // A file that's there but can't be read is not an empty one: rewriting it from nothing
        // would throw away every other setting.
        std::ifstream input(path);
        std::string line;
        while (input && std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(std::move(line));
        }
        if (!input.eof()) {
            std::cerr << "[runtime-config] Unable to read " << PathToUtf8(path) << "; not saving "
                      << key << std::endl;
            return false;
        }
    }

    const std::string normalizedSection = Trim(section);
    const std::string normalizedKey = Trim(key);
    size_t sectionStart = lines.size();
    size_t sectionEnd = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = Trim(RemoveComment(lines[i]));
        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string found = Trim(std::string_view(trimmed).substr(1, trimmed.size() - 2));
            if (sectionStart != lines.size()) {
                sectionEnd = i;
                break;
            }
            if (found == normalizedSection) {
                sectionStart = i;
            }
        }
    }

    const std::string replacement = normalizedKey + " = " + std::string(value);
    if (sectionStart == lines.size()) {
        if (!lines.empty() && !lines.back().empty()) {
            lines.emplace_back();
        }
        lines.emplace_back("[" + normalizedSection + "]");
        lines.push_back(replacement);
    } else {
        bool replaced = false;
        for (size_t i = sectionStart + 1; i < sectionEnd; ++i) {
            const std::string uncommented = Trim(RemoveComment(lines[i]));
            const size_t equals = uncommented.find('=');
            if (equals != std::string::npos && Trim(std::string_view(uncommented).substr(0, equals)) == normalizedKey) {
                lines[i] = replacement;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            // Append after the section's last real line rather than after the blank line that
            // separates it from the next header: this file is edited by hand as well, and a key
            // parked below the separator reads as if it belonged to the next section.
            size_t insertAt = sectionEnd;
            while (insertAt > sectionStart + 1 && Trim(lines[insertAt - 1]).empty()) {
                --insertAt;
            }
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), replacement);
        }
    }

    // Written beside the file and renamed over it, so a crash or a full disk mid-write leaves the
    // old file whole rather than a truncated one.
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        for (const auto& outputLine : lines) {
            output << outputLine << '\n';
        }
        output.close();
        if (!output) {
            std::cerr << "[runtime-config] Unable to write " << PathToUtf8(temporary) << std::endl;
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::cerr << "[runtime-config] Unable to replace " << PathToUtf8(path) << ": " << ec.message()
                  << std::endl;
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

inline std::string FormatString(std::string_view value) {
    return toml::format(toml::value(std::string(value)));
}

inline bool SetResolutionMultiplier(float value) {
    Mutable().resolutionMultiplier = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("video", "resolution_multiplier", formatted.str());
}

inline bool SetWindowSize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return false;
    }
    Mutable().windowWidth = width;
    Mutable().windowHeight = height;
    const bool wroteWidth = WriteSetting("video", "window_width", std::to_string(width));
    const bool wroteHeight = WriteSetting("video", "window_height", std::to_string(height));
    return wroteWidth && wroteHeight;
}

inline bool SetWindowPosition(int32_t x, int32_t y) {
    Mutable().windowPosX = x;
    Mutable().windowPosY = y;
    const bool wroteX = WriteSetting("video", "window_x", std::to_string(x));
    const bool wroteY = WriteSetting("video", "window_y", std::to_string(y));
    return wroteX && wroteY;
}

inline bool SetFrameRate(uint32_t value) {
    if (!IsSupportedFrameRate(value)) {
        return false;
    }
    Mutable().frameRate = value;
    return WriteSetting("video", "frame_rate", std::to_string(value));
}

inline bool SetDisplayMode(std::string value) {
    if (!IsSupportedDisplayMode(value)) {
        return false;
    }
    Mutable().displayMode = value;
    return WriteSetting("video", "display_mode", FormatString(value));
}

inline bool SetSkipUnreadyPipelines(bool value) {
    Mutable().skipUnreadyPipelines = value;
    return WriteSetting("video", "skip_unready_pipelines", value ? "true" : "false");
}

inline bool SetDisableCopyFilter(bool value) {
    Mutable().disableCopyFilter = value;
    return WriteSetting("video", "disable_copy_filter", value ? "true" : "false");
}

inline bool SetScaledEfbCopy(bool value) {
    Mutable().scaledEfbCopy = value;
    return WriteSetting("video", "scaled_efb_copy", value ? "true" : "false");
}

inline bool SetShowFps(bool value) {
    Mutable().showFps = value;
    return WriteSetting("video", "show_fps", value ? "true" : "false");
}

inline bool SetControllerButton(size_t index, std::string value) {
    if (index >= kControllerButtonKeys.size()) {
        return false;
    }
    Mutable().controllerButtons[index] = value;
    return WriteSetting("controller", kControllerButtonKeys[index], FormatString(value));
}

inline std::string ControllerExpression(const std::string& key) {
    const auto it = Get().controllerExpressions.find(key);
    return it == Get().controllerExpressions.end() ? std::string() : it->second;
}

inline bool SetControllerExpression(const std::string& key, const std::string& value) {
    Mutable().controllerExpressions[key] = value;
    return WriteSetting("controller", key, FormatString(value));
}

// Mods (F10 > Mods). Each is on unless disabled.
inline bool ModMenuNavigation() { return Get().modMenuNavigation.value_or(true); }
inline bool ModSelectionBadge() { return Get().modSelectionBadge.value_or(false); }
inline bool ModNoMegaStrikes() { return Get().modNoMegaStrikes.value_or(true); }
inline bool SetModMenuNavigation(bool value) {
    Mutable().modMenuNavigation = value;
    return WriteSetting("mods", "menu_navigation", value ? "true" : "false");
}
inline bool SetModSelectionBadge(bool value) {
    Mutable().modSelectionBadge = value;
    return WriteSetting("mods", "selection_badge", value ? "true" : "false");
}
inline bool SetModNoMegaStrikes(bool value) {
    Mutable().modNoMegaStrikes = value;
    return WriteSetting("mods", "no_mega_strikes", value ? "true" : "false");
}
// Gameplay extras, off unless enabled.
inline bool ModUnlockEverything() { return Get().modUnlockEverything.value_or(false); }
inline bool ModWinByTwo() { return Get().modWinByTwo.value_or(false); }
inline bool SetModUnlockEverything(bool value) {
    Mutable().modUnlockEverything = value;
    return WriteSetting("mods", "unlock_everything", value ? "true" : "false");
}
inline bool ModKitChoice() { return Get().modKitChoice.value_or(true); }
inline bool SetModKitChoice(bool value) {
    Mutable().modKitChoice = value;
    return WriteSetting("mods", "kit_choice", value ? "true" : "false");
}
inline bool ModFastMenus() { return Get().modFastMenus.value_or(false); }
inline bool SetModFastMenus(bool value) {
    Mutable().modFastMenus = value;
    return WriteSetting("mods", "fast_menus", value ? "true" : "false");
}
inline bool ModSkipIntro() { return Get().modSkipIntro.value_or(false); }
inline bool SetModSkipIntro(bool value) {
    Mutable().modSkipIntro = value;
    return WriteSetting("mods", "skip_intro", value ? "true" : "false");
}
inline bool ModAllCaptains() { return Get().modAllCaptains.value_or(false); }
inline bool SetModAllCaptains(bool value) {
    Mutable().modAllCaptains = value;
    return WriteSetting("mods", "all_captains", value ? "true" : "false");
}
// Captain-only teams: the character in each sidekick slot of a side (0 home, 1 away): a captain
// (0-11) or a partner (12-19, CharacterInfo order), -1 = the team's leader (its captain, or the
// partner in its captain spot).
inline std::array<int, 3> ModCaptainTeammates(int side) {
    std::array<int, 3> slots{-1, -1, -1};
    const std::string text = Get().modCaptainTeammates[side & 1].value_or("");
    size_t pos = 0;
    for (int& slot : slots) {
        if (pos > text.size()) break;
        const size_t comma = text.find(',', pos);
        const std::string item = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        char* end = nullptr;
        const long value = std::strtol(item.c_str(), &end, 10);
        if (end != item.c_str() && value >= 0 && value <= 19) slot = static_cast<int>(value);  // captains 0-11, partners 12-19
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return slots;
}
// Captain-only teams: a partner (12-19) playing in a side's captain spot, the team keeping its
// captain's colours, banners and goalie; -1 = the captain plays.
inline int ModCaptainSpot(int side) {
    const int value = Get().modCaptainSpot[side & 1].value_or(-1);
    return value >= 12 && value <= 19 ? value : -1;
}
inline bool SetModCaptainSpot(int side, int character) {
    Mutable().modCaptainSpot[side & 1] = character;
    return WriteSetting("mods", side == 0 ? "captain_spot_home" : "captain_spot_away", std::to_string(character));
}
inline bool SetModCaptainTeammates(int side, const std::array<int, 3>& slots) {
    const std::string text = std::to_string(slots[0]) + "," + std::to_string(slots[1]) + "," + std::to_string(slots[2]);
    Mutable().modCaptainTeammates[side & 1] = text;
    return WriteSetting("mods", side == 0 ? "captain_teammates_home" : "captain_teammates_away", FormatString(text));
}
inline bool ModBluePeach() { return Get().modBluePeach.value_or(false); }
inline bool SetModBluePeach(bool value) {
    Mutable().modBluePeach = value;
    return WriteSetting("mods", "blue_peach", value ? "true" : "false");
}
inline bool ModShotCounter() { return Get().modShotCounter.value_or(false); }
inline bool SetModShotCounter(bool value) {
    Mutable().modShotCounter = value;
    return WriteSetting("mods", "shot_counter", value ? "true" : "false");
}
inline bool ModFastStadiums() { return Get().modFastStadiums.value_or(false); }
inline bool SetModFastStadiums(bool value) {
    Mutable().modFastStadiums = value;
    return WriteSetting("mods", "fast_stadiums", value ? "true" : "false");
}
inline bool ModNkFix() { return Get().modNkFix.value_or(true); }
inline bool SetModNkFix(bool value) {
    Mutable().modNkFix = value;
    return WriteSetting("mods", "nk_fix", value ? "true" : "false");
}
inline bool SetModWinByTwo(bool value) {
    Mutable().modWinByTwo = value;
    return WriteSetting("mods", "win_by_two", value ? "true" : "false");
}

inline int32_t MuteHotkey(int32_t fallback) {
    return Get().muteHotkey.value_or(fallback);
}

inline bool SetMuteHotkey(int32_t value) {
    Mutable().muteHotkey = value;
    return WriteSetting("audio", "mute_key", std::to_string(value));
}

inline bool SetAudioVolume(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().audioVolume = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", "volume", formatted.str());
}

// The volume of one sound category, 0-1 (1 by default).
inline float SoundCategoryVolume(SoundCategory category) {
    const auto index = static_cast<size_t>(category);
    return index < kSoundCategoryKeys.size() ? std::clamp(Get().soundCategoryVolumes[index].value_or(1.0f), 0.0f, 1.0f) : 1.0f;
}
inline bool SetSoundCategoryVolume(SoundCategory category, float value) {
    const auto index = static_cast<size_t>(category);
    if (index >= kSoundCategoryKeys.size()) return false;
    value = std::clamp(value, 0.0f, 1.0f);
    Mutable().soundCategoryVolumes[index] = value;
    std::ostringstream formatted;
    formatted << value;
    return WriteSetting("audio", kSoundCategoryKeys[index], formatted.str());
}

inline bool SetAudioMuted(bool value) {
    Mutable().audioMuted = value;
    return WriteSetting("audio", "muted", value ? "true" : "false");
}

inline bool SetAudioMixWorker(bool value) {
    Mutable().audioMixWorker = value;
    return WriteSetting("audio", "mix_worker", value ? "true" : "false");
}

inline bool WidescreenEnabled(bool fallback = false) {
    return Get().widescreen.value_or(fallback);
}

inline bool ForceAspect169Enabled(bool fallback = false) {
    return Get().forceAspect169.value_or(fallback);
}

inline bool SetForceAspect169(bool value) {
    Mutable().forceAspect169 = value;
    return WriteSetting("video", "force_16_9", value ? "true" : "false");
}

inline bool WindowPosition(int32_t& x, int32_t& y) {
    if (!Get().windowPosX || !Get().windowPosY) {
        return false;
    }
    x = *Get().windowPosX;
    y = *Get().windowPosY;
    return true;
}

inline uint32_t WindowWidth(uint32_t fallback) {
    return Get().windowWidth.value_or(fallback);
}

inline uint32_t WindowHeight(uint32_t fallback) {
    return Get().windowHeight.value_or(fallback);
}

inline float ResolutionMultiplier(float fallback = 1.0f) {
    return std::max(0.0f, Get().resolutionMultiplier.value_or(fallback));
}

inline float AudioVolume(float fallback = 1.0f) {
    return std::clamp(Get().audioVolume.value_or(fallback), 0.0f, 1.0f);
}

inline bool AudioMuted(bool fallback = false) {
    return Get().audioMuted.value_or(fallback);
}

// Off-thread AX/DSP mix. Default on; false restores the fully synchronous mix.
inline bool AudioMixWorkerEnabled(bool fallback = true) {
    return Get().audioMixWorker.value_or(fallback);
}

// Bluetooth Wii Remotes / Wii U Pro Controllers. Read once before SDL's joystick
// subsystem comes up, so a change only takes effect on the next launch.
inline bool WiiRemotesEnabled(bool fallback = true) {
    return Get().wiiRemotes.value_or(fallback);
}

// Persists the Bluetooth Wii Remote driver switch.
inline bool SetWiiRemotesEnabled(bool value) {
    Mutable().wiiRemotes = value;
    return WriteSetting("controller", "wii_remotes", value ? "true" : "false");
}

// Whether to keep rescanning Bluetooth while no Wii controller is connected.
inline bool WiiContinuousScanEnabled(bool fallback = false) {
    return Get().wiiContinuousScan.value_or(fallback);
}

// Persists the continuous scanning switch.
inline bool SetWiiContinuousScanEnabled(bool value) {
    Mutable().wiiContinuousScan = value;
    return WriteSetting("controller", "wii_continuous_scan", value ? "true" : "false");
}

// Whether the sensor bar sits above the screen (default: below).
inline bool SensorBarAbove() { return Get().sensorBarAbove.value_or(false); }

// The game's vibration (on unless turned off).
inline bool RumbleEnabled() { return Get().rumbleEnabled.value_or(true); }
inline bool SetRumbleEnabled(bool value) {
    Mutable().rumbleEnabled = value;
    return WriteSetting("controller", "rumble", value ? "true" : "false");
}

// The Wii's IR sensitivity (1-5, default 3): how bright a dot real remotes' cameras report.
inline int32_t IrSensitivity() { return Get().irSensitivity.value_or(3); }
inline bool SetIrSensitivity(int32_t value) {
    value = std::clamp(value, 1, 5);
    Mutable().irSensitivity = value;
    return WriteSetting("controller", "ir_sensitivity", std::to_string(value));
}

// How fast a controller's stick moves the pointer, as a multiple of the normal speed.
inline double PointerSpeed() { return Get().pointerSpeed.value_or(1.0); }
inline bool SetPointerSpeed(double value) {
    value = std::clamp(value, 0.25, 4.0);
    Mutable().pointerSpeed = value;
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(2) << value;
    return WriteSetting("controller", "pointer_speed", formatted.str());
}

// Persists the sensor bar position.
inline bool SetSensorBarAbove(bool value) {
    Mutable().sensorBarAbove = value;
    return WriteSetting("controller", "sensor_bar_above", value ? "true" : "false");
}

// Wii Remote accelerometer zero-point correction (g, SDL sensor frame); all zero
// when the remote has not been calibrated.
inline std::array<double, 3> WiiAccelOffset() {
    const RuntimeUserConfig& config = Get();
    return {config.wiiAccelOffsetX.value_or(0.0), config.wiiAccelOffsetY.value_or(0.0),
            config.wiiAccelOffsetZ.value_or(0.0)};
}

// Whether to write the per-frame accelerometer trace (off unless asked for).
inline bool WiiAccelTraceEnabled(bool fallback = false) {
    return Get().wiiAccelTrace.value_or(fallback);
}

// True while a non-zero correction is stored ("Clear calibration" writes zeros).
inline bool HasWiiAccelOffset() {
    const std::array<double, 3> offset = WiiAccelOffset();
    return offset[0] != 0.0 || offset[1] != 0.0 || offset[2] != 0.0;
}

// Persists the accelerometer correction measured by the overlay's calibration.
inline bool SetWiiAccelOffset(const std::array<double, 3>& offset) {
    Mutable().wiiAccelOffsetX = offset[0];
    Mutable().wiiAccelOffsetY = offset[1];
    Mutable().wiiAccelOffsetZ = offset[2];
    bool ok = true;
    const char* keys[3] = {"wii_accel_offset_x", "wii_accel_offset_y", "wii_accel_offset_z"};
    for (size_t i = 0; i < 3; ++i) {
        // Always a float literal, so a whole-number offset does not come back as a TOML integer.
        std::ostringstream formatted;
        formatted << std::fixed << std::setprecision(4) << offset[i];
        ok = WriteSetting("controller", keys[i], formatted.str()) && ok;
    }
    return ok;
}

// The game's native frame rate: 30, 60, 120, 144, 160 or 165 frames per second, or 0 for the
// display's refresh rate (hle/vi.cpp: VI_HLE_FrameRate gives the rate in effect).
inline uint32_t FrameRate(uint32_t fallback = 60) {
    return Get().frameRate.value_or(fallback);
}

// Whether to skip draws whose graphics pipeline has not finished compiling yet.
inline bool SkipUnreadyPipelines(bool fallback = true) {
    return Get().skipUnreadyPipelines.value_or(fallback);
}

inline bool DisableCopyFilter(bool fallback = true) {
    return Get().disableCopyFilter.value_or(fallback);
}

inline bool ScaledEfbCopy(bool fallback = true) {
    return Get().scaledEfbCopy.value_or(fallback);
}

inline bool ShowFps(bool fallback = true) {
    return Get().showFps.value_or(fallback);
}

inline bool TextureReplacements(bool fallback = false) {
    return Get().textureReplacements.value_or(fallback);
}

// Dumping only produces the names a replacement would need, so main.cpp
// gates it on TextureReplacements() as well.
inline bool TextureDumps(bool fallback = false) {
    return Get().textureDumps.value_or(fallback);
}

// Both apply on the next launch: the renderer indexes the folder once, at startup.
inline bool SetTextureReplacements(bool value) {
    Mutable().textureReplacements = value;
    return WriteSetting("video", "texture_replacements", value ? "true" : "false");
}
inline bool SetTextureDumps(bool value) {
    Mutable().textureDumps = value;
    return WriteSetting("video", "texture_dumps", value ? "true" : "false");
}

inline std::string GraphicsApi(std::string fallback = "auto") {
    return Get().graphicsApi.value_or(std::move(fallback));
}

inline std::string DisplayMode(std::string fallback = "windowed") {
    return Get().displayMode.value_or(std::move(fallback));
}

inline bool NetworkEnabled(bool fallback = true) {
    return Get().networkEnabled.value_or(fallback);
}

inline std::string NandRoot(std::string fallback = "") {
    return Get().nandRoot.value_or(std::move(fallback));
}

inline std::string DvdRoot(std::string fallback = "") {
    return Get().dvdRoot.value_or(std::move(fallback));
}

// The one resolver for configured paths. A relative value means the same thing
// everywhere it can be configured: relative to the config file that named it,
// never to the process working directory.
inline std::filesystem::path ResolveRelativeTo(const std::filesystem::path& base,
                                               const std::string& value) {
    std::filesystem::path path = PathFromUtf8(value);
    if (path.is_relative()) {
        path = base / path;
    }
    return path.lexically_normal();
}

inline std::filesystem::path ResolveRelativeToConfig(const std::string& value) {
    return ResolveRelativeTo(ResolveConfigPath().parent_path(), value);
}

// The extracted DATA directory. Empty when nothing is configured.
inline std::filesystem::path ResolvedDvdRoot() {
    const std::string configured = DvdRoot();
    return configured.empty() ? std::filesystem::path{} : ResolveRelativeToConfig(configured);
}

inline bool DiscordPresenceEnabled(bool fallback = false) {
    return Get().discordPresenceEnabled.value_or(fallback);
}

inline std::string DiscordClientId(std::string fallback = "") {
    return Get().discordClientId.value_or(std::move(fallback));
}

inline const std::vector<std::string>& OverlayRoots() {
    return Get().overlayRoots;
}

// Where mod packages are installed: [paths] mods_dir (relative to Config.toml), else <data>/Mods.
inline std::filesystem::path ModsDirectory() {
    if (const auto& configured = Get().modsDirectory; configured && !Trim(*configured).empty()) {
        return ResolveRelativeToConfig(Trim(*configured));
    }
    return ResolveConfigPath().parent_path() / "Mods";
}

// A mod's switches, or nullopt when Config.toml doesn't mention it.
inline std::optional<bool> ModPackageEnabled(const std::string& id) {
    const auto& table = Get().modPackagesEnabled;
    const auto it = table.find(id);
    return it == table.end() ? std::nullopt : std::optional<bool>(it->second);
}
inline std::optional<bool> ModPluginEnabled(const std::string& id) {
    const auto& table = Get().modPluginsEnabled;
    const auto it = table.find(id);
    return it == table.end() ? std::nullopt : std::optional<bool>(it->second);
}
inline bool SetModPackageEnabled(const std::string& id, bool enabled) {
    Mutable().modPackagesEnabled[id] = enabled;
    return WriteSetting("mod_packages", FormatString(id), enabled ? "true" : "false");
}
inline bool SetModPluginEnabled(const std::string& id, bool enabled) {
    Mutable().modPluginsEnabled[id] = enabled;
    return WriteSetting("mod_plugins", FormatString(id), enabled ? "true" : "false");
}
inline std::optional<bool> ModSetting(const std::string& id, const std::string& key) {
    const auto& settings = Get().modSettings;
    const auto mod = settings.find(id);
    if (mod == settings.end()) return std::nullopt;
    const auto it = mod->second.find(key);
    return it == mod->second.end() ? std::nullopt : std::optional<bool>(it->second);
}
inline bool SetModSetting(const std::string& id, const std::string& key, bool value) {
    Mutable().modSettings[id][key] = value;
    const bool bare = !key.empty() && std::all_of(key.begin(), key.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    });
    return WriteSetting("mod_settings." + FormatString(id), bare ? key : FormatString(key), value ? "true" : "false");
}
// The mod character (internal name) leading a side, "" for the game's own captain.
inline std::string ModSelection(int side) { return Trim(Get().modSelection[side & 1].value_or("")); }
inline bool SetModSelection(int side, const std::string& name) {
    Mutable().modSelection[side & 1] = name;
    return WriteSetting("mod_selection", side == 0 ? "home" : "away", FormatString(name));
}

inline void LogLoadedConfig() {
    static const bool logged = [] {
        const auto& config = Get();
        const auto configPath = ResolveConfigPath();
        std::cout << "[runtime-config] " << PathToUtf8(configPath);
        if (!std::filesystem::exists(configPath)) {
            std::cout << " not found; using built-in defaults";
        } else {
            std::cout << " loaded";
            if (config.widescreen) {
                std::cout << " widescreen=" << (*config.widescreen ? "true" : "false");
            }
            if (config.windowWidth || config.windowHeight) {
                std::cout << " window=" << config.windowWidth.value_or(0) << "x"
                          << config.windowHeight.value_or(0);
            }
            if (config.resolutionMultiplier) {
                std::cout << " resolution_multiplier=" << *config.resolutionMultiplier;
            }
            if (config.dvdRoot) {
                std::cout << " dvd_root=" << *config.dvdRoot;
            }
            if (config.graphicsApi) {
                std::cout << " graphics_api=" << *config.graphicsApi;
            }
            if (config.skipUnreadyPipelines) {
                std::cout << " skip_unready_pipelines=" << (*config.skipUnreadyPipelines ? "true" : "false");
            }
            if (config.disableCopyFilter) {
                std::cout << " disable_copy_filter=" << (*config.disableCopyFilter ? "true" : "false");
            }
            if (config.scaledEfbCopy) {
                std::cout << " scaled_efb_copy=" << (*config.scaledEfbCopy ? "true" : "false");
            }
            if (config.showFps) {
                std::cout << " show_fps=" << (*config.showFps ? "true" : "false");
            }
            if (config.textureReplacements) {
                std::cout << " texture_replacements=" << (*config.textureReplacements ? "true" : "false");
            }
            if (config.textureDumps) {
                std::cout << " texture_dumps=" << (*config.textureDumps ? "true" : "false");
            }
            if (config.audioVolume) {
                std::cout << " audio_volume=" << *config.audioVolume;
            }
            if (config.audioMuted) {
                std::cout << " audio_muted=" << (*config.audioMuted ? "true" : "false");
            }
            if (config.networkEnabled) {
                std::cout << " network_enabled=" << (*config.networkEnabled ? "true" : "false");
            }
            if (config.discordPresenceEnabled) {
                std::cout << " discord_enabled=" << (*config.discordPresenceEnabled ? "true" : "false");
            }
            if (config.nandRoot) {
                std::cout << " nand_root=" << *config.nandRoot;
            }
        }
        std::cout << std::endl;
        return true;
    }();
    (void)logged;
}

} // namespace RuntimeConfigFile
