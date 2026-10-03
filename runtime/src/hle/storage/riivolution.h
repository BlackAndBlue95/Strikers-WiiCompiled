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
    // How the config file names it: the option's id, or its section name + option name.
    std::string configId;
    std::vector<std::string> choices;
    // 1-based choice in effect since launch; 0 is off.
    uint32_t selected = 0;
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
// Dolphin use, keeping its other options. Takes effect at the next launch. Empty on success,
// else why it failed.
std::string SaveOptionChoice(const Pack& pack, const PackOption& option, uint32_t choice);

} // namespace RuntimeRiivolution
