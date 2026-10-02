// Catalogs: entries mod packages add to the game's shared files instead of replacing them. The DVD
// layer runs this at boot, after mounting the packages' files: each catalog file the mods touch is
// rebuilt (the game's or a package's version plus the additions) into the cache folder and takes
// that file's place in the file table.
//
// Front-end textures: <package>/catalogs/fe/<bundle>/<fe path>.gxt adds a texture to Art/fe/<bundle>
// (e.g. mainui.dmn), named "fe/<fe path>" (catalogs/fe/mainui.dmn/screens/images/captain_x_s.gxt is
// fe/screens/images/captain_x_s). A .gxt is a texture as the bundles store it: the 32-byte GX texture
// header, then the image data. A name the bundle already has is replaced.
//
// Strings: <package>/catalogs/loc/all.txt (every language) and <language>.txt (english, french, ...;
// overriding all.txt), UTF-8 lines "KEY = text", go into Art/fe/<language>.loc and <language>_game.loc.
//
// INI: <package>/catalogs/ini/<path>.ini is appended to ini/<path>.ini. Per-character tables (hologram
// framing) get the base's section copied for each mod character that has none.
//
// Per-character files the game opens by name (crowd lists) that a mod character lacks are its base's,
// registered under its name.
//
// Streamed sounds: <package>/catalogs/streams/<bank>/<cue>.idsp adds the cue <cue> to the stream bank
// audio/<bank>.resbun + .nlxwb (STREAM_GEN_NIS: cutscene audio, STREAM_GEN_Music, STREAM_GEN_Crowd,
// FE_GEN_Music). A .idsp is a stream as the bank's .nlxwb stores it: "IDSP", the bank's interleave, two
// 0x60-byte DSP-ADPCM channel headers, then the channels' blocks interleaved. catalogs/streams/<bank>/
// cues.txt: "<other cue> = <cue>" lines name more cues for a stream, "template = <cue>" picks the
// bank's cue whose settings (category, volume group) the new ones copy (default: the bank's first).
// The merged .resbun replaces the bank's; the streams are appended to its .nlxwb (AppendFile).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Mods::Catalogs {

using HostPathOf = std::function<std::optional<std::filesystem::path>(const std::string& dvdPath)>;
using RegisterFile = std::function<void(const std::string& dvdPath, const std::filesystem::path& hostPath, uint32_t size)>;
// Appends a host file's `size` bytes to a disc file; the offset they start at, or nullopt.
using AppendFile = std::function<std::optional<uint32_t>(const std::string& dvdPath, const std::filesystem::path& hostPath, uint32_t size)>;

// Rebuilds every catalog file the active packages add to. Returns the number of files rebuilt.
int Merge(const HostPathOf& hostPathOf, const RegisterFile& registerFile, const AppendFile& appendFile);

// Cutscenes: <package>/catalogs/nis/nis_dict.txt is appended to Art/nis/nis_dict.txt (entries for the
// mod's .nis files); catalogs/nis/triggers.txt and cues.txt ("nis_name = other_name" lines) make a
// cutscene use another's trigger script or audio cue. These look the aliases up (names without
// ".nis"; cues by nlStringLowerHash, "<name>" and "<name>_nocrowd" both mapped). Empty / 0: none.
std::string NisTriggerAlias(const std::string& nis);
uint32_t NisCueAlias(uint32_t cueHash);

// Whether a mod added the localisation key `key` (in any language). Valid once Merge has run.
bool HasLocKey(const std::string& key);

// Whether a mod added the front-end texture named `name` ("fe/screens/images/captain_x_s"). Valid once
// Merge has run (at boot).
bool HasFeTexture(const std::string& name);

// The mod file a front-end texture came from (its .gxt), or empty.
std::filesystem::path FeTextureFile(const std::string& name);

// A front-end bundle (.res/.dmn: header {0x20, count, 1, data start / 32}, entries {hash, offset / 32,
// size}, data) with `additions` (hash, texture) appended, replacing same-hash entries. Empty on a
// malformed bundle.
std::vector<uint8_t> MergeFeBundle(const std::vector<uint8_t>& bundle,
                                   const std::vector<std::pair<uint32_t, std::vector<uint8_t>>>& additions);

// A stream bank (.resbun: NL chunk tree {SoundMap 0x80023000, ResourceBundle 0x80023300, Sources
// 0x80023200}) with `streams` added: each new stream gets a voice, sequence, sound event and source
// copying the template cue's, its source at `nlxwbSize` (32-aligned) + its place in `append`, and a
// cue per name. False with `error` set on a malformed bank, a blob not in the bank's format or a name
// the bank already has.
struct NewStream {
    std::vector<std::string> names;  // the cue names (nlStringLowerHash'd), the first the source's
    std::vector<uint8_t> blob;       // the .idsp
};
bool MergeStreamBank(const std::vector<uint8_t>& resbun, uint32_t nlxwbSize, const std::vector<NewStream>& streams,
                     const std::string& templateCue, std::vector<uint8_t>& merged, std::vector<uint8_t>& append,
                     std::string& error);

}  // namespace Mods::Catalogs
