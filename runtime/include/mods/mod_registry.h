// Mod packages: discovery, manifests (mod.toml) and enable state. A package is a folder in the mods
// directory (RuntimeConfigFile::ModsDirectory(), <data>/Mods by default):
//
//   <id>/mod.toml   the manifest (docs/modding/manifest.md)
//   <id>/files/     disc-shaped files, added to the game's file table at boot (storage/dvd.cpp)
//   <id>/catalogs/  entries merged into the game's shared files at boot (mods/mod_catalogs.h)
//
// The registry is read once, at first use (before the disc file table is built), and does not change
// for the rest of the session: enabling or disabling a mod takes effect on the next launch.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Mods {

// The manifest format this runtime reads; a manifest asking for a newer one is refused.
constexpr int kFrameworkVersion = 1;

// A character the mod adds ([[character]]): a named variant of a base character.
struct CharacterDef {
    std::string name;         // internal name: every name-keyed asset (paths, textures, cutscenes, menus)
    std::string kind;         // "captain" ("partner" is read but not used yet)
    std::string base;         // base character's internal name (gameplay: skeleton, animations, ability)
    int baseIndex = -1;       // its CharacterInfo row
    std::string displayName;  // localisation key for its name
    std::string voiceBank;    // sound bank name (audio/<bank>.resbun + .nlxwb)
    std::string goalieKit;    // goalie kit name (art/characters/<kit>/<kit>.rlt)
    std::string stats;        // tweaks INI path (disc-relative)
    std::optional<uint32_t> primaryColour, alternateColour;  // 0xRRGGBB
    std::optional<std::array<float, 4>> statsBars;  // captain select's bars: movement, shooting, passing, defense (0-1)
    int role = -1;                                  // offensive, defensive, playmaker, power, balanced (0-4)
    bool noAbility = false;  // ability = "none": its team is never given a captain's super ability
    // deke: "base" (its base's), "plain" (its base's without the teleport), "default" (the generic
    // directional deke Mario, Luigi and Yoshi use; needs deke_300/600/1200 of its own in its animations).
    enum class Deke { Base, Plain, Default } deke = Deke::Base;
    std::vector<std::string> teammates;  // default teammates (character names)
    // fixed_team: its team is always it and its teammates (copies of itself unless `teammates` says
    // otherwise; filled in so), as SMS's Super Team, whatever partner select picks.
    bool fixedTeam = false;
    // extra_effects: effects bundles (disc paths, "art/effects/<x>.bun"; "<x>nonres.bun.zlib" with it if
    // there is one) loaded with its own, for effects its base's bundle doesn't have.
    std::vector<std::string> extraEffects;
};

struct Package {
    std::string id;           // folder-independent identity, [a-z0-9._-]
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> authors;
    int framework = 0;
    int loadOrder = 0;        // higher loads later, so its files win
    std::filesystem::path root;
    std::filesystem::path pluginLibrary;  // this platform's native plugin (absolute), empty if none
    std::vector<CharacterDef> characters;
    std::vector<std::string> errors;      // the package can't load
    std::vector<std::string> warnings;
    bool enabledSetting = true;           // what Config.toml asks for ([mod_packages])
    bool pluginSetting = false;           // whether its native plugin may load ([mod_plugins])
    bool active = false;                  // loaded this session: enabled and without errors

    bool HasPlugin() const { return !pluginLibrary.empty(); }
    std::filesystem::path FilesRoot() const { return root / "files"; }
};

// Every package found in the mods directory, in load order (load_order, then id). Read on first call.
const std::vector<Package>& Packages();

// The packages loaded this session, in load order.
std::vector<const Package*> ActivePackages();

// Change a package's switches in Config.toml. Takes effect on the next launch.
void SetEnabled(const std::string& id, bool enabled);
void SetPluginAllowed(const std::string& id, bool allowed);

// True when a switch was changed this session (the running game still uses the old state).
bool RestartRequired();

// What a package's files/ folder did to the disc file table (recorded by the DVD layer at boot).
struct MountStats {
    int added = 0;      // new files
    int replaced = 0;   // disc files replaced: global overrides
    int conflicts = 0;  // files another package also ships (this one won)
};
void SetMountStats(const std::string& id, MountStats stats);
MountStats GetMountStats(const std::string& id);

// The mods directory, created on first use so players have somewhere to put mods.
std::filesystem::path Directory();

// Internal names of the game's characters, by CharacterInfo row (captains 0-11, partners 12-19).
const char* BaseCharacterName(int index);
int BaseCharacterIndex(const std::string& name);

}  // namespace Mods
