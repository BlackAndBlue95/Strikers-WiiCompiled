// Riivolution packs: host-IO shim around hle/riivolution_contract.h. Discovers the overlay roots
// (Config.toml [paths] overlay_roots, then <data>/Riivolution), parses each root's XMLs, applies the
// remembered option choices, and exposes the resulting disc mappings (dvd.cpp) and savegame redirect
// (nand_fs.cpp). Memory patches are not applied: see riivolution_contract.h.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RuntimeRiivolution {

struct Mapping {
    enum class Kind {
        File,         // discPath is a file path
        Folder,       // discPath is a directory prefix
        FolderByName, // no disc path: replace disc files matching by filename
    };

    Kind kind = Kind::File;
    std::string discPath;
    std::filesystem::path hostPath;
    bool recursive = true;
    // Riivolution 'create': when false, only files already present on the disc
    // may be replaced; nothing new is added.
    bool create = false;
    // A <file> patch over part of a disc file (offset / fileoffset / length): the
    // external's bytes from fileOffset (length of them, 0: to its end) written at
    // `offset` in the disc file. dvd.cpp applies the case a pack uses to grow a
    // file, appending at its end.
    bool partial = false;
    uint32_t offset = 0;
    uint32_t fileOffset = 0;
    uint32_t length = 0;
};

struct PatchSet {
    // In Dolphin's application order (per active patch: files, then folders);
    // a later mapping for the same disc path wins.
    std::vector<Mapping> mappings;
    uint32_t skippedExternals = 0; // externals that do not exist on the host
};

struct Overlay {
    std::filesystem::path root;
    // nullopt: the root carries no Riivolution XML and is a plain disc-shaped
    // overlay directory.
    std::optional<PatchSet> patches;
};

struct SaveRedirect {
    std::filesystem::path hostDirectory;
    bool clone = false;
};

// Discovered overlay roots in precedence order (highest priority first), each
// with its active Riivolution mappings. Loaded once, thread-safe.
const std::vector<Overlay>& Overlays();

// The active <savegame> redirect from the highest-priority overlay that has
// one, resolved to a host directory (Dolphin semantics: the whole NAND
// /title/<id>/data directory is redirected there).
const std::optional<SaveRedirect>& GetSaveRedirect();

// One option of a pack, for F10 > Mods.
struct PackOption {
    std::string section;
    std::string name;
    std::string description;  // the option's description attribute, if it has one
    // How the config file names it: the option's id, or its section name + option name.
    std::string configId;
    std::vector<std::string> choices;
    // 1-based choice in effect since launch; 0 is off.
    uint32_t selected = 0;
    // A choice of it adds a code mod (a disc-root sml_*.bin): changing it takes a build.
    bool addsCode = false;
    // Its choices only add files the disc doesn't have (each of the Strikers Tweaks: a file its code
    // looks for): a change applies at once, the files coming and going while the game runs.
    bool live = false;
    size_t liveIndex = 0;  // its live state, while live
};

// A pack XML in an overlay root, as loaded at launch.
struct Pack {
    std::filesystem::path xml;
    std::filesystem::path root; // the overlay root (SD card) holding riivolution/<pack>.xml
    std::vector<PackOption> options;
    size_t mappings = 0;                  // file and folder patches it applies
    std::vector<std::string> codeModules; // disc-root sml_*.bin files it adds (Kamek code mods)
};

// Whether a disc path names a code mod: a file at the disc root called sml_*.bin (case-insensitive),
// where the Strikers Mod Loader looks on a console. name is the lower-case file name. The translator's
// RiivolutionCodeModules applies the same rule when build.sh picks the modules to build in.
bool IsCodeModuleDiscPath(std::string_view discPath, std::string& name);

// Every pack XML for this game, in overlay precedence order.
const std::vector<Pack>& Packs();

// Remembers a choice in the root's riivolution/config/<GameID4>.xml, the file Riivolution and
// Dolphin use, keeping its other options. A live option's choice applies at once, the others' at the
// next launch. Empty on success, else why it failed.
std::string SaveOptionChoice(const Pack& pack, const PackOption& option, uint32_t choice);

// A file a live option's choice adds. dvd.cpp puts every one on the disc at launch, the current
// choices' and the others', and the game doesn't see the others (LiveFileHidden).
struct LiveFile {
    std::string discPath;
    std::filesystem::path hostPath;
    size_t option = 0;  // the live option's liveIndex
};
const std::vector<LiveFile>& LiveFiles();

// dvd.cpp, at launch: a live option one of whose files the disc has would replace a game file, which
// can't come and go, so its changes apply at the next launch like other options'.
void NotLive(size_t liveIndex);

// Whether a file on the disc (by its path) is a live option's file that no current choice adds.
bool LiveFileHidden(const std::string& discPath);

} // namespace RuntimeRiivolution
