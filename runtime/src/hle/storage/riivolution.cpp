// Riivolution overlay loading (host-IO shim). See riivolution.h.
//
// Ported behavior from Dolphin Emulator's DiscIO/RiivolutionParser and
// RiivolutionPatcher (https://github.com/dolphin-emu/dolphin).
// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/storage/riivolution.h"

#include "hle/dvd_contract.h"
#include "hle/riivolution_contract.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"
#include "nand_path.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

// An option whose choices only add files (PackOption::live).
struct RiivoLiveOption {
    size_t pack = 0, option = 0;                        // packs[pack].options[option]
    std::vector<std::vector<std::string>> choiceFiles;  // by choice (0: the first), the files it adds (lookup form)
    uint32_t current = 0;
    bool live = true;
};

struct RiivoState {
    std::vector<RuntimeRiivolution::Overlay> overlays;
    std::optional<RuntimeRiivolution::SaveRedirect> saveRedirect;
    std::vector<RuntimeRiivolution::Pack> packs;

    std::vector<RiivoLiveOption> liveOptions;
    std::vector<RuntimeRiivolution::LiveFile> liveFiles;
    // What the other options add, to tell whether a live option's files are its own: files (lookup
    // form), and folders ("/folder/", and where their files come from).
    std::unordered_set<std::string> otherFiles;
    std::vector<std::pair<std::string, fs::path>> otherFolders;
    // F10 changes a live option's choice while the game looks files up.
    std::mutex liveMutex;
    std::unordered_set<std::string> hidden;  // the live options' files no current choice adds
    std::atomic<bool> anyHidden{false};
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

std::string RiivoLookupPath(const std::string& discPath) {
    return DvdFstContract::NormalizeLookupPath(discPath.empty() || discPath[0] != '/' ? "/" + discPath : discPath);
}

// The files of each choice of an option whose choices only add whole files (<file create="true">, no
// code, folders, memory patches or saves), with their contents; nullopt for any other option. Each
// choice's patches are generated as if it alone were chosen, so they're what the pack applies.
std::optional<std::vector<std::vector<RuntimeRiivolution::Mapping>>> RiivoLiveChoices(
    const RiivolutionContract::Disc& disc, size_t sectionIndex, size_t optionIndex, const std::string& gameId,
    const std::string& sdRootGeneric, const std::string& xmlDirGeneric) {
    const RiivolutionContract::Option& option = disc.sections[sectionIndex].options[optionIndex];
    if (option.choices.empty()) return std::nullopt;
    for (const auto& choice : option.choices) {
        for (const auto& reference : choice.patchReferences) {
            for (const auto& patch : disc.patches) {
                if (patch.id != reference.id) continue;
                if (!patch.folderPatches.empty() || !patch.savegamePatches.empty() || !patch.memoryPatches.empty())
                    return std::nullopt;
                for (const auto& file : patch.filePatches) {
                    std::string module;
                    if (!file.create || file.disc.empty() || file.offset != 0 || file.fileoffset != 0 || file.length != 0 ||
                        RuntimeRiivolution::IsCodeModuleDiscPath(file.disc, module))
                        return std::nullopt;
                }
            }
        }
    }
    std::vector<std::vector<RuntimeRiivolution::Mapping>> choices;
    for (uint32_t choice = 1; choice <= option.choices.size(); ++choice) {
        RiivolutionContract::Disc alone = disc;
        for (auto& section : alone.sections)
            for (auto& other : section.options) other.selectedChoice = 0;
        alone.sections[sectionIndex].options[optionIndex].selectedChoice = choice;
        RuntimeRiivolution::PatchSet set;
        for (const auto& patch : alone.GeneratePatches(gameId)) RiivoCollectMappings(patch, sdRootGeneric, xmlDirGeneric, set);
        choices.push_back(std::move(set.mappings));
    }
    return choices;
}

// The live options' files that no current choice adds. With liveMutex held.
void RiivoUpdateHidden(RiivoState& state) {
    std::unordered_set<std::string> hidden, shown;
    for (const RiivoLiveOption& option : state.liveOptions) {
        if (!option.live) continue;
        for (size_t choice = 0; choice < option.choiceFiles.size(); ++choice)
            for (const std::string& file : option.choiceFiles[choice]) (choice + 1 == option.current ? shown : hidden).insert(file);
    }
    for (const std::string& file : shown) hidden.erase(file);
    state.hidden = std::move(hidden);
    state.anyHidden = !state.hidden.empty();
}

// With liveMutex held.
void RiivoNotLive(RiivoState& state, size_t liveIndex, const char* why) {
    RiivoLiveOption& option = state.liveOptions[liveIndex];
    if (!option.live) return;
    option.live = false;
    RuntimeRiivolution::PackOption& entry = state.packs[option.pack].options[option.option];
    entry.live = false;
    RT_LOG(RT_TAG_RIIVOLUTION) << entry.configId << ": " << why << ", so it applies at the next launch" << std::endl;
    RiivoUpdateHidden(state);
}

// Writes riivolution/config/<GameID4>.xml as Riivolution and Dolphin do. Empty on success, else why
// it failed.
std::string RiivoWriteConfig(const fs::path& configXml, const std::vector<RiivolutionContract::ConfigOption>& options) {
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
    return {};
}

// The tweaks that became the Strikers Tweaks pack were [mods] settings in Config.toml: each of the
// pack's options the config file doesn't name yet takes its old setting, written to the file, so the
// tweaks a player had on stay on (from this launch) and F10 > Mods changes them from then on.
void RiivoCarryOverTweaks(const RiivolutionContract::Disc& disc, std::optional<RiivolutionContract::Config>& config,
                          const fs::path& configXml) {
    std::vector<RiivolutionContract::ConfigOption> added;
    for (const auto& section : disc.sections) {
        for (const auto& option : section.options) {
            const std::string id = option.id.empty() ? section.name + option.name : option.id;
            const auto legacy = RuntimeConfigFile::LegacyTweak(id);
            if (!legacy) continue;
            if (config && std::any_of(config->options.begin(), config->options.end(),
                                      [&](const RiivolutionContract::ConfigOption& entry) { return entry.id == id; }))
                continue;
            added.push_back({id, *legacy ? 1u : 0u});
        }
    }
    if (added.empty()) return;
    if (!config) config = RiivolutionContract::Config{};
    for (const auto& entry : added) {
        config->options.push_back(entry);
        RT_LOG(RT_TAG_RIIVOLUTION) << "Config.toml's setting carried over: " << entry.id << " = " << entry.defaultChoice
                                   << std::endl;
    }
    if (const std::string error = RiivoWriteConfig(configXml, config->options); !error.empty()) {
        RT_LOG(RT_TAG_RIIVOLUTION) << "WARNING: " << error << std::endl;
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

        RiivoCarryOverTweaks(*disc, config, configXml);
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
                entry.description = option.description;
                entry.configId = option.id.empty() ? section.name + option.name : option.id;
                for (const auto& choice : option.choices) {
                    entry.choices.push_back(choice.name);
                }
                entry.selected = option.selectedChoice;
                for (const auto& choice : option.choices) {
                    for (const auto& reference : choice.patchReferences) {
                        for (const auto& patch : disc->patches) {
                            if (patch.id != reference.id) continue;
                            for (const auto& file : patch.filePatches) {
                                std::string module;
                                entry.addsCode |= RuntimeRiivolution::IsCodeModuleDiscPath(file.disc, module);
                            }
                        }
                    }
                }
                const size_t sectionIndex = static_cast<size_t>(&section - disc->sections.data());
                const size_t optionIndex = static_cast<size_t>(&option - section.options.data());
                if (auto choices = RiivoLiveChoices(*disc, sectionIndex, optionIndex, gameId, sdRootGeneric, xmlDirGeneric)) {
                    RiivoLiveOption live;
                    live.pack = state.packs.size();
                    live.option = pack.options.size();
                    live.current = option.selectedChoice;
                    for (const auto& mappings : *choices) {
                        std::vector<std::string> files;
                        for (const auto& mapping : mappings) {
                            files.push_back(RiivoLookupPath(mapping.discPath));
                            state.liveFiles.push_back({mapping.discPath, mapping.hostPath, state.liveOptions.size()});
                        }
                        live.choiceFiles.push_back(std::move(files));
                    }
                    entry.live = true;
                    entry.liveIndex = state.liveOptions.size();
                    state.liveOptions.push_back(std::move(live));
                } else {
                    for (const auto& choice : option.choices) {
                        for (const auto& reference : choice.patchReferences) {
                            for (const auto& patch : disc->patches) {
                                if (patch.id != reference.id) continue;
                                for (const auto& file : patch.filePatches) state.otherFiles.insert(RiivoLookupPath(file.disc));
                                const std::string patchRoot =
                                    RiivolutionContract::ResolvePatchRoot(sdRootGeneric, xmlDirGeneric, patch.root);
                                for (const auto& folder : patch.folderPatches) {
                                    if (folder.disc.empty()) continue;  // by name: only replaces the disc's files
                                    const auto external =
                                        RiivolutionContract::MakeAbsoluteFromRelative(sdRootGeneric, patchRoot, folder.external);
                                    if (!external) continue;
                                    std::string prefix = RiivoLookupPath(folder.disc);
                                    if (prefix.back() != '/') prefix += '/';
                                    state.otherFolders.emplace_back(std::move(prefix), PathFromUtf8(*external));
                                }
                            }
                        }
                    }
                }
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

    // A live option's files are its own: one another option adds too, as a file or in a folder, doesn't
    // come and go with it.
    std::lock_guard<std::mutex> lock(g_riivoState.liveMutex);
    for (size_t index = 0; index < g_riivoState.liveOptions.size(); ++index) {
        for (const auto& files : g_riivoState.liveOptions[index].choiceFiles) {
            for (const std::string& file : files) {
                bool shared = g_riivoState.otherFiles.count(file) != 0;
                for (const auto& [prefix, folder] : g_riivoState.otherFolders) {
                    std::error_code ec;
                    shared = shared || (file.compare(0, prefix.size(), prefix) == 0 &&
                                        fs::exists(folder / PathFromUtf8(file.substr(prefix.size())), ec));
                }
                if (shared) RiivoNotLive(g_riivoState, index, "another option adds its files too");
            }
        }
    }
    auto& liveFiles = g_riivoState.liveFiles;
    liveFiles.erase(std::remove_if(liveFiles.begin(), liveFiles.end(),
                                   [](const RuntimeRiivolution::LiveFile& file) {
                                       return !g_riivoState.liveOptions[file.option].live;
                                   }),
                    liveFiles.end());
    RiivoUpdateHidden(g_riivoState);
    if (!g_riivoState.liveOptions.empty()) {
        RT_LOG(RT_TAG_RIIVOLUTION) << std::count_if(g_riivoState.liveOptions.begin(), g_riivoState.liveOptions.end(),
                                                    [](const RiivoLiveOption& option) { return option.live; })
                                   << " option(s) apply at once (they only add files), " << g_riivoState.liveFiles.size()
                                   << " file(s)" << std::endl;
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

    if (const std::string error = RiivoWriteConfig(configXml, options); !error.empty()) {
        return error;
    }
    bool now = false;
    {
        std::lock_guard<std::mutex> lock(g_riivoState.liveMutex);
        if (option.live && option.liveIndex < g_riivoState.liveOptions.size() &&
            g_riivoState.liveOptions[option.liveIndex].live) {
            g_riivoState.liveOptions[option.liveIndex].current = choice;
            RiivoUpdateHidden(g_riivoState);
            now = true;
        }
    }
    RT_LOG(RT_TAG_RIIVOLUTION) << PathToUtf8(configXml) << ": " << option.configId << " = " << choice
                               << (now ? " (now)" : " (from the next launch)") << std::endl;
    return {};
}

const std::vector<LiveFile>& LiveFiles() {
    std::call_once(g_riivoOnce, RiivoInitialize);
    return g_riivoState.liveFiles;
}

void NotLive(size_t liveIndex) {
    std::lock_guard<std::mutex> lock(g_riivoState.liveMutex);
    if (liveIndex < g_riivoState.liveOptions.size()) RiivoNotLive(g_riivoState, liveIndex, "its files replace the disc's");
}

bool LiveFileHidden(const std::string& discPath) {
    if (!g_riivoState.anyHidden.load(std::memory_order_acquire)) return false;
    const std::string file = RiivoLookupPath(discPath);
    std::lock_guard<std::mutex> lock(g_riivoState.liveMutex);
    return g_riivoState.hidden.count(file) != 0;
}

} // namespace RuntimeRiivolution
