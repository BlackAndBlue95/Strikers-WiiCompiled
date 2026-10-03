// Riivolution overlay loading (host-IO shim). See riivolution.h.
//
// Ported behavior from Dolphin Emulator's DiscIO/RiivolutionParser and
// RiivolutionPatcher (https://github.com/dolphin-emu/dolphin).
// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/storage/riivolution.h"

#include "hle/riivolution_contract.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"
#include "nand_path.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

struct RiivoState {
    std::vector<RuntimeRiivolution::Overlay> overlays;
    std::optional<RuntimeRiivolution::SaveRedirect> saveRedirect;
    std::vector<RuntimeRiivolution::Pack> packs;
};

std::once_flag g_riivoOnce;
RiivoState g_riivoState;

std::optional<std::string> RiivoReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Six-character game ID ("R4QE01") from guest low memory, with the same R4QE
// fallback the rest of the storage HLE uses for boots that have not written
// the disc header yet.
std::string RiivoGameId() {
    const uint32_t code = RuntimeHle::CurrentGameCode(0x52345145u); // "R4QE"
    std::string id(6, '\0');
    id[0] = static_cast<char>((code >> 24) & 0xffu);
    id[1] = static_cast<char>((code >> 16) & 0xffu);
    id[2] = static_cast<char>((code >> 8) & 0xffu);
    id[3] = static_cast<char>(code & 0xffu);

    uint16_t maker = 0;
    if (Memory::Contains(0x80000004u, 2u)) {
        maker = Memory::Read16(0x80000004u);
    }
    const char makerHi = static_cast<char>((maker >> 8) & 0xffu);
    const char makerLo = static_cast<char>(maker & 0xffu);
    if (std::isalnum(static_cast<unsigned char>(makerHi)) &&
        std::isalnum(static_cast<unsigned char>(makerLo))) {
        id[4] = makerHi;
        id[5] = makerLo;
    } else {
        id[4] = '0';
        id[5] = '1';
    }
    return id;
}

// Every narrow path string here is UTF-8, including the ones the XML halves of
// resolved paths are concatenated with.
using RuntimeConfigFile::PathFromUtf8;
using RuntimeConfigFile::PathToUtf8;

std::string RiivoGenericText(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

std::string RiivoComparablePath(const fs::path& path) {
    std::string text = RiivoGenericText(path.lexically_normal());
#ifdef _WIN32
    RuntimeHle::LowerInPlace(text);
#endif
    return text;
}

void RiivoAddRoot(std::vector<RuntimeRiivolution::Overlay>& overlays, fs::path root,
                  const char* source) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        RT_LOG(RT_TAG_RIIVOLUTION) << "rejected overlay root (" << (source ? source : "unknown")
                  << "): " << PathToUtf8(root) << " is not a reachable directory" << std::endl;
        return;
    }

    fs::path normalized = fs::weakly_canonical(root, ec);
    if (ec) {
        normalized = root.lexically_normal();
    }

    const std::string comparable = RiivoComparablePath(normalized);
    const auto duplicate =
        std::find_if(overlays.begin(), overlays.end(), [&](const RuntimeRiivolution::Overlay& overlay) {
            return RiivoComparablePath(overlay.root) == comparable;
        });
    if (duplicate != overlays.end()) {
        return;
    }

    RT_LOG(RT_TAG_RIIVOLUTION) << "overlay root (" << (source ? source : "unknown")
              << "): " << PathToUtf8(normalized) << std::endl;
    overlays.push_back({std::move(normalized), std::nullopt});
}

std::vector<RuntimeRiivolution::Overlay> RiivoDiscoverRoots() {
    std::vector<RuntimeRiivolution::Overlay> overlays;

    // Discovery order is precedence order (DVDInit applies the roots in
    // reverse): the roots Config.toml names, then the default folder.
    for (const auto& root : RuntimeConfigFile::OverlayRoots()) {
        // A relative overlay root resolves against the config file that named
        // it, never against whatever working directory the process happened
        // to be started in.
        RiivoAddRoot(overlays, RuntimeNandPath::ResolveConfiguredPath(root), "Config.toml");
    }

    // <data>/Riivolution, laid out like a Wii SD card (riivolution/*.xml plus
    // the pack folders), like Dolphin's Load/Riivolution.
    const fs::path defaultRoot = RuntimeConfigFile::ResolveConfigPath().parent_path() / "Riivolution";
    std::error_code ec;
    if (fs::is_directory(defaultRoot / "riivolution", ec)) {
        RiivoAddRoot(overlays, defaultRoot, "default folder");
    }

    return overlays;
}

// The Riivolution XMLs describing an overlay root, plus the SD-root directory
// that absolute external paths resolve against.
struct RiivoXmlSet {
    fs::path sdRoot;
    std::vector<fs::path> xmlFiles;
};

std::optional<RiivoXmlSet> RiivoFindXmls(const fs::path& overlayRoot) {
    std::error_code ec;

    // Dolphin-style discovery: the overlay root is itself a virtual SD root
    // carrying <root>/riivolution/*.xml.
    const fs::path xmlDirectory = overlayRoot / "riivolution";
    if (fs::is_directory(xmlDirectory, ec)) {
        std::vector<fs::path> xmlFiles;
        for (const auto& entry : fs::directory_iterator(xmlDirectory, ec)) {
            if (ec) {
                break;
            }
            if (entry.is_regular_file(ec) && entry.path().extension() == ".xml") {
                xmlFiles.push_back(entry.path());
            }
        }
        if (!xmlFiles.empty()) {
            // Sort so the same folder always produces the same disc.
            std::sort(xmlFiles.begin(), xmlFiles.end());
            return RiivoXmlSet{overlayRoot, std::move(xmlFiles)};
        }
    }

    return std::nullopt;
}

void RiivoCollectMappings(const RiivolutionContract::Patch& patch, const std::string& sdRootGeneric,
                          const std::string& xmlDirGeneric, RuntimeRiivolution::PatchSet& set) {
    const std::string patchRoot =
        RiivolutionContract::ResolvePatchRoot(sdRootGeneric, xmlDirGeneric, patch.root);
    std::error_code ec;

    // Dolphin applies each patch's file patches first, then its folder
    // patches; with last-registration-wins that means folders shadow files
    // within one patch, and later patches shadow earlier ones.
    for (const auto& file : patch.filePatches) {
        if (file.disc.empty()) {
            ++set.skippedExternals;
            continue;
        }
        const auto resolved =
            RiivolutionContract::MakeAbsoluteFromRelative(sdRootGeneric, patchRoot, file.external);
        if (!resolved) {
            ++set.skippedExternals;
            continue;
        }
        const fs::path hostFile = PathFromUtf8(*resolved);
        if (!fs::is_regular_file(hostFile, ec)) {
            ++set.skippedExternals;
            continue;
        }
        RuntimeRiivolution::Mapping mapping{RuntimeRiivolution::Mapping::Kind::File, file.disc, hostFile, true, file.create};
        mapping.partial = file.offset != 0 || file.fileoffset != 0 || file.length != 0;
        mapping.offset = file.offset;
        mapping.fileOffset = file.fileoffset;
        mapping.length = file.length;
        set.mappings.push_back(std::move(mapping));
    }

    for (const auto& folder : patch.folderPatches) {
        const auto resolved =
            RiivolutionContract::MakeAbsoluteFromRelative(sdRootGeneric, patchRoot, folder.external);
        if (!resolved) {
            ++set.skippedExternals;
            continue;
        }
        const fs::path hostFolder = PathFromUtf8(*resolved);
        if (!fs::is_directory(hostFolder, ec)) {
            ++set.skippedExternals;
            continue;
        }
        const auto kind = folder.disc.empty() ? RuntimeRiivolution::Mapping::Kind::FolderByName
                                              : RuntimeRiivolution::Mapping::Kind::Folder;
        set.mappings.push_back({kind, folder.disc, hostFolder, folder.recursive, folder.create});
    }
}

std::optional<RuntimeRiivolution::PatchSet> RiivoLoadPatchSet(const fs::path& overlayRoot,
                                                              RiivoState& state) {
    const auto xmlSet = RiivoFindXmls(overlayRoot);
    if (!xmlSet) {
        return std::nullopt;
    }

    const std::string gameId = RiivoGameId();
    const std::string sdRootGeneric = RiivoGenericText(xmlSet->sdRoot);

    // The remembered option choices, in the file Riivolution and Dolphin use.
    std::optional<RiivolutionContract::Config> config;
    const fs::path configXml =
        xmlSet->sdRoot / "riivolution" / "config" / (gameId.substr(0, 4) + ".xml");
    if (const auto configText = RiivoReadFile(configXml)) {
        config = RiivolutionContract::ParseConfigString(*configText);
    }

    RuntimeRiivolution::PatchSet set;
    for (const fs::path& xmlFile : xmlSet->xmlFiles) {
        const auto text = RiivoReadFile(xmlFile);
        if (!text) {
            RT_LOG(RT_TAG_RIIVOLUTION) << "WARNING: cannot read " << PathToUtf8(xmlFile)
                      << std::endl;
            continue;
        }

        auto disc = RiivolutionContract::ParseString(*text);
        if (!disc) {
            RT_LOG(RT_TAG_RIIVOLUTION) << "WARNING: " << PathToUtf8(xmlFile)
                      << " is not a valid Riivolution XML (version 1 wiidisc); ignoring it"
                      << std::endl;
            continue;
        }
        if (!disc->IsValidForGame(gameId, std::nullopt, std::nullopt)) {
            RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(xmlFile) << ": not valid for " << gameId
                      << ", skipped" << std::endl;
            continue;
        }

        if (config) {
            RiivolutionContract::ApplyConfigDefaults(*disc, *config);
        }

        const auto activePatches = disc->GeneratePatches(gameId);
        const std::string xmlDirGeneric = RiivoGenericText(xmlFile.parent_path());

        const size_t before = set.mappings.size();
        for (const auto& patch : activePatches) {
            RiivoCollectMappings(patch, sdRootGeneric, xmlDirGeneric, set);
        }

        RuntimeRiivolution::Pack pack;
        pack.xml = xmlFile;
        pack.root = xmlSet->sdRoot;
        pack.mappings = set.mappings.size() - before;
        for (const auto& section : disc->sections) {
            for (const auto& option : section.options) {
                RuntimeRiivolution::PackOption entry;
                entry.section = section.name;
                entry.name = option.name;
                entry.configId = option.id.empty() ? section.name + option.name : option.id;
                for (const auto& choice : option.choices) {
                    entry.choices.push_back(choice.name);
                }
                entry.selected = option.selectedChoice;
                pack.options.push_back(std::move(entry));
            }
        }
        for (size_t index = before; index < set.mappings.size(); ++index) {
            std::string name;
            if (set.mappings[index].kind == RuntimeRiivolution::Mapping::Kind::File &&
                RuntimeRiivolution::IsCodeModuleDiscPath(set.mappings[index].discPath, name)) {
                pack.codeModules.push_back(name);
            }
        }
        state.packs.push_back(std::move(pack));

        // Highest-priority overlay with a <savegame> wins.
        if (!state.saveRedirect) {
            if (const auto* savegame = RiivolutionContract::FindSavegamePatch(activePatches)) {
                if (const auto resolvedSave = RiivolutionContract::MakeAbsoluteFromRelative(
                        sdRootGeneric, xmlDirGeneric, savegame->external)) {
                    state.saveRedirect =
                        RuntimeRiivolution::SaveRedirect{PathFromUtf8(*resolvedSave),
                                                         savegame->clone};
                    RT_LOG(RT_TAG_RIIVOLUTION) << "savegame redirect: "
                              << PathToUtf8(state.saveRedirect->hostDirectory)
                              << (savegame->clone ? " (clone)" : "") << std::endl;
                }
            }
        }

        // A pack whose XML parses but activates nothing is the most confusing
        // failure this layer has: the game boots, plays, and quietly shows
        // vanilla content. Always say what happened.
        RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(xmlFile) << ": " << activePatches.size()
                  << " active patch(es), " << (set.mappings.size() - before) << " mapping(s)"
                  << std::endl;
        if (activePatches.empty()) {
            RT_LOG(RT_TAG_RIIVOLUTION) << "WARNING: " << PathToUtf8(xmlFile)
                      << " has no enabled options for " << gameId << "; check "
                      << sdRootGeneric << "/riivolution/config/" << gameId.substr(0, 4) << ".xml"
                      << std::endl;
        }
    }

    if (set.skippedExternals != 0) {
        RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(overlayRoot) << ": skipped "
                  << set.skippedExternals << " mapping(s) whose external path does not exist"
                  << std::endl;
    }

    return set;
}

void RiivoInitialize() {
    g_riivoState.overlays = RiivoDiscoverRoots();
    for (auto& overlay : g_riivoState.overlays) {
        overlay.patches = RiivoLoadPatchSet(overlay.root, g_riivoState);
    }
}

} // namespace

namespace RuntimeRiivolution {

const std::vector<Overlay>& Overlays() {
    std::call_once(g_riivoOnce, RiivoInitialize);
    return g_riivoState.overlays;
}

const std::optional<SaveRedirect>& GetSaveRedirect() {
    std::call_once(g_riivoOnce, RiivoInitialize);
    return g_riivoState.saveRedirect;
}

bool IsCodeModuleDiscPath(std::string_view discPath, std::string& name) {
    while (!discPath.empty() && discPath.front() == '/') {
        discPath.remove_prefix(1);
    }
    std::string lower(discPath);
    RuntimeHle::LowerInPlace(lower);
    if (lower.find('/') != std::string::npos || lower.find('\\') != std::string::npos || lower.size() <= 8 ||
        lower.compare(0, 4, "sml_") != 0 || lower.compare(lower.size() - 4, 4, ".bin") != 0) {
        return false;
    }
    name = std::move(lower);
    return true;
}

const std::vector<Pack>& Packs() {
    std::call_once(g_riivoOnce, RiivoInitialize);
    return g_riivoState.packs;
}

std::string SaveOptionChoice(const Pack& pack, const PackOption& option, uint32_t choice) {
    const fs::path configXml =
        pack.root / "riivolution" / "config" / (RiivoGameId().substr(0, 4) + ".xml");

    // Keep the other options the file remembers, in their order.
    std::vector<RiivolutionContract::ConfigOption> options;
    if (const auto text = RiivoReadFile(configXml)) {
        if (auto config = RiivolutionContract::ParseConfigString(*text)) {
            options = std::move(config->options);
        }
    }
    const auto existing = std::find_if(options.begin(), options.end(),
        [&](const RiivolutionContract::ConfigOption& entry) { return entry.id == option.configId; });
    if (existing != options.end()) {
        existing->defaultChoice = choice;
    } else {
        options.push_back({option.configId, choice});
    }

    pugi::xml_document document;
    pugi::xml_node root = document.append_child("riivolution");
    root.append_attribute("version") = 2;
    for (const auto& entry : options) {
        pugi::xml_node node = root.append_child("option");
        node.append_attribute("id") = entry.id.c_str();
        node.append_attribute("default") = entry.defaultChoice;
    }

    std::error_code ec;
    fs::create_directories(configXml.parent_path(), ec);
    const fs::path temporary = configXml.string() + ".tmp";
    if (!document.save_file(temporary.c_str(), "\t", pugi::format_default, pugi::encoding_utf8)) {
        return "can't write " + PathToUtf8(temporary);
    }
    fs::rename(temporary, configXml, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return "can't replace " + PathToUtf8(configXml);
    }
    RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(configXml) << ": " << option.configId << " = " << choice
                               << " (from the next launch)" << std::endl;
    return {};
}

} // namespace RuntimeRiivolution
