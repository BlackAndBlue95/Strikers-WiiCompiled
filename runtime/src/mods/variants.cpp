// Mod characters in matches (mods/variants.h): the variants' guest data, which variant plays where,
// and the character loader's lookups answering for them.
#include "mods/variants.h"

#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "hle/msc_guest.h"
#include "hle_stubs.h"
#include "mods/mod_catalogs.h"
#include "mods/mod_registry.h"
#include "runtime_config.h"
#include "runtime_log.h"

extern "C" uint32_t g_modDataReservedBase;
extern "C" uint32_t g_modDataReservedSize;
bool DVDPathExistsForRuntime(const char* dvdPath);  // storage/dvd.cpp
extern "C" void func_8000BA00(CpuContext* ctx);   // translated CharacterLoader_8056B290::fn_8000BA00
extern "C" void func_80026370(CpuContext* ctx);   // translated DestroyCharacters

namespace Mods::Variants {
namespace {

// CharacterLoader_8056B290 (the match's character loader) and its Entry.
constexpr uint32_t kLoader = 0x8056B290u;
constexpr uint32_t kLoaderCurrent = 0xCC, kLoaderCaptains = 0xD8;
constexpr uint32_t kEntryTeam = 0x00, kEntryCharIdx = 0x04, kEntryPlayer = 0x08, kEntryClass = 0x0C,
                   kEntryGoalie = 0x11, kEntrySidekick = 0x12;
constexpr uint32_t kLoaderCodeBegin = 0x80009B34u, kLoaderCodeEnd = 0x8000C300u;  // tu_80009B34.cpp
// Templates (CharacterTemplate.cpp).
constexpr uint32_t kTemplateInfos = 0x804F7F18u;      // g_aCharacterTemplateInfo[20]
constexpr uint32_t kGoalieTemplateInfo = 0x804F8808u; // g_GoalieTemplateInfo
constexpr uint32_t kGoalieKits = 0x804F8DA0u;         // g_GoalieTextureInfo[12]
constexpr uint32_t kTemplates = 0x8056B828u;          // g_aCharacterTemplates[20]
constexpr uint32_t kGoalieTemplate = 0x806E0C38u;     // g_GoalieTemplate
constexpr uint32_t kTemplateSize = 0x34, kTemplateAnimHash = 0x10, kTemplateAnims = 0x1C;
constexpr uint32_t kInfoSize = 0x5C, kInfoModel = 0x04, kInfoShockModel = 0x08, kInfoLowPolyModel = 0x0C,
                   kInfoShadowModel = 0x10, kInfoTextures = 0x14, kInfoAltTextures = 0x18, kInfoTriggers = 0x1C,
                   kInfoAnimations = 0x34, kInfoEffects = 0x3C, kInfoTweaks = 0x44, kInfoTweakPath = 0x48,
                   kInfoLoaded = 0x58;
constexpr uint32_t kKitSize = 0x10, kKitName = 0x00, kKitTextures = 0x04, kKitAltTextures = 0x08, kKitLoaded = 0x0C;
// CharacterInfo.
constexpr uint32_t kRowName = 0x04, kRowDisplayName = 0x08, kRowCaptainItem = 0x14, kRowVoiceBank = 0x1C,
                   kRowStatsBars = 0x38, kRowRole = 0x48, kRowColourMask = 0x4C, kRowColourRank = 0x50,
                   kRowPrimary = 0x54, kRowAlternate = 0x58;
constexpr int kFirstGoalie = 20;
// The match's characters.
constexpr uint32_t kCharacters = 0x8056B800u;  // g_pCharacters[10]
constexpr uint32_t kCharacterRow = 0x11C;      // cCharacter::mUnidentified11C (its CharacterInfo)
// Audio.
constexpr uint32_t kBankSources = 0x0C, kBankNames = 0x14, kBankSourceSize = 0x18;  // AudioBankTable
constexpr uint32_t kSourceResource = 0x08, kSourceOwner = 0x10, kSourceLoaded = 0x14;
constexpr uint32_t kLoadAudioResource = 0x802ED498u;  // LoadAudioResource(owner, name, callback, param, allocator)

thread_local int t_loaderScope = 0;
uint32_t g_bankTable = 0;  // the sound map's AudioBankTable (seen by the LoadBank override)
std::vector<std::unique_ptr<Variant>> g_variants;  // built on first use, kept for the session
uint32_t g_dataNext = 0;

// The framework's reserved guest memory (system_bridge.cpp), handed out once.
uint32_t DataAlloc(uint32_t size, uint32_t alignment = 8) {
    if (g_dataNext == 0) g_dataNext = g_modDataReservedBase;
    const uint32_t addr = (g_dataNext + alignment - 1) & ~(alignment - 1);
    if (g_modDataReservedBase == 0 || addr + size > g_modDataReservedBase + g_modDataReservedSize) {
        RT_LOG(RT_TAG_MODS) << "out of reserved guest memory for mod characters" << std::endl;
        return 0;
    }
    g_dataNext = addr + size;
    for (uint32_t i = 0; i < size; i += 4) Memory::Write32(addr + i, 0);
    return addr;
}
uint32_t DataString(const std::string& s) {
    const uint32_t addr = DataAlloc(static_cast<uint32_t>(s.size() + 1));
    if (addr == 0) return 0;
    for (size_t i = 0; i < s.size(); ++i) Memory::Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
    Memory::Write8(addr + static_cast<uint32_t>(s.size()), 0);
    return addr;
}
void Copy(uint32_t to, uint32_t from, uint32_t size) {
    for (uint32_t i = 0; i < size; i += 4) Memory::Write32(to + i, Memory::Read32(from + i));
}
bool Exists(const std::string& path) { return DVDPathExistsForRuntime(path.c_str()); }
std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const CharacterDef* FindCharacter(const std::string& name, const Package** owner = nullptr) {
    if (name.empty()) return nullptr;
    for (const Package* package : ActivePackages())
        for (const CharacterDef& c : package->characters)
            if (c.name == name) {
                if (owner) *owner = package;
                return &c;
            }
    return nullptr;
}

// A path slot of the template info: the variant's own file when the mod ships it.
void Prefer(uint32_t info, uint32_t field, const std::string& path, std::string& used) {
    if (!Exists(path)) return;
    Memory::Write32(info + field, DataString(path));
    used += (used.empty() ? "" : ", ") + path.substr(path.rfind('/') + 1);
}

Variant* Build(const Package* package, const CharacterDef* def) {
    auto v = std::make_unique<Variant>();
    v->def = def;
    v->package = package;
    const std::string& n = def->name;
    const std::string dir = "art/characters/" + n + "/";
    std::string own;

    v->row = DataAlloc(MscGuest::kCharacterInfoSize);
    v->templateInfo = DataAlloc(kInfoSize);
    if (v->row == 0 || v->templateInfo == 0) return nullptr;
    Copy(v->row, MscGuest::CharacterInfo(static_cast<uint32_t>(def->baseIndex)), MscGuest::kCharacterInfoSize);
    Memory::Write32(v->row + kRowName, DataString(n));
    if (!def->displayName.empty()) {  // only a key its mod gave a string (an unknown key would show as itself)
        if (Mods::Catalogs::HasLocKey(def->displayName)) Memory::Write32(v->row + kRowDisplayName, DataString(def->displayName));
        else RT_LOG(RT_TAG_MODS) << package->id << ": " << n << ": no string for " << def->displayName << " (catalogs/loc)" << std::endl;
    }
    if (def->primaryColour) {
        Memory::Write32(v->row + kRowPrimary, *def->primaryColour);
        // Its own colour: none of its base's clash categories (the colour mask), or a match against its
        // base would always see a clash and put both teams in the home/away colours.
        Memory::Write32(v->row + kRowColourMask, 0);
    }
    if (def->alternateColour) Memory::Write32(v->row + kRowAlternate, *def->alternateColour);
    if (def->statsBars)
        for (uint32_t i = 0; i < 4; ++i) Memory::WriteFloat32(v->row + kRowStatsBars + i * 4, (*def->statsBars)[i]);
    if (def->role >= 0) Memory::Write32(v->row + kRowRole, static_cast<uint32_t>(def->role));
    // No super ability: no captain item type, as a partner's row (variant_specials.cpp).
    if (def->noAbility) Memory::Write32(v->row + kRowCaptainItem, 0xFFFFFFFFu);

    Copy(v->templateInfo, kTemplateInfos + static_cast<uint32_t>(def->baseIndex) * kInfoSize, kInfoSize);
    Prefer(v->templateInfo, kInfoModel, dir + n + ".rlg", own);
    Prefer(v->templateInfo, kInfoShockModel, dir + n + "_shock.rlg", own);
    Prefer(v->templateInfo, kInfoLowPolyModel, dir + n + "_lowpoly.rlg", own);
    Prefer(v->templateInfo, kInfoShadowModel, dir + n + "_shadow.rlg", own);
    Prefer(v->templateInfo, kInfoTextures, dir + n + ".rlt", own);
    // Its own animations (the base's set and skeleton; the same names) and their triggers.
    Prefer(v->templateInfo, kInfoAnimations, "art/animation/" + n + ".sanim.zlib", own);
    Prefer(v->templateInfo, kInfoTriggers, "art/animation/" + n + ".trg", own);
    // The away kit is the variant's or none: the base's would only bring textures named after the base.
    Memory::Write32(v->templateInfo + kInfoAltTextures, DataString(dir + n + "_alt.rlt"));
    if (Exists("art/effects/" + n + "Effects.bun")) Memory::Write32(v->templateInfo + kInfoEffects, DataString(n));
    if (!def->stats.empty()) {
        const std::string stats = def->stats[0] == '/' ? def->stats : "/" + def->stats;
        if (Exists(stats)) {
            Memory::Write32(v->templateInfo + kInfoTweaks, DataString(stats));
            Memory::Write32(v->templateInfo + kInfoTweakPath, DataString("/Game/Gameplay/ini/chars/" + n));
            own += (own.empty() ? "" : ", ") + stats.substr(stats.rfind('/') + 1);
        } else {
            RT_LOG(RT_TAG_MODS) << package->id << ": " << n << ": stats " << stats << " not found" << std::endl;
        }
    }
    Memory::Write8(v->templateInfo + kInfoLoaded, 0);

    if (const std::string& kit = def->goalieKit; !kit.empty() && def->kind == "captain") {
        const std::string kitPath = "art/characters/" + kit + "/" + kit + ".rlt";
        if (Exists(kitPath)) {
            v->goalieInfo = DataAlloc(kKitSize);
            v->goalieRow = DataAlloc(MscGuest::kCharacterInfoSize);
            if (v->goalieInfo && v->goalieRow) {
                Memory::Write32(v->goalieInfo + kKitName, DataString(kit));
                Memory::Write32(v->goalieInfo + kKitTextures, DataString(kitPath));
                Memory::Write32(v->goalieInfo + kKitAltTextures, DataString("art/characters/" + kit + "_alt/" + kit + "_alt.rlt"));
                Copy(v->goalieRow, MscGuest::CharacterInfo(static_cast<uint32_t>(kFirstGoalie + def->baseIndex)), MscGuest::kCharacterInfoSize);
                Memory::Write32(v->goalieRow + kRowName, DataString(kit));
                own += (own.empty() ? "" : ", ") + kit + " goalie";
            }
        } else {
            RT_LOG(RT_TAG_MODS) << package->id << ": " << n << ": goalie kit " << kitPath << " not found" << std::endl;
        }
    }

    if (const std::string& bank = def->voiceBank; !bank.empty()) {
        if (Exists("audio/" + bank + ".resbun") && Exists("audio/" + bank + ".nlxwb")) {
            v->bankRecord = DataAlloc(8);
            if (v->bankRecord) {
                Memory::Write32(v->bankRecord + 4, DataString(bank));
                v->bankIndex = kFirstVirtualBank + static_cast<int>(g_variants.size());
                Memory::Write32(v->row + kRowVoiceBank, static_cast<uint32_t>(v->bankIndex));
                own += (own.empty() ? "" : ", ") + bank;
            }
        } else {
            RT_LOG(RT_TAG_MODS) << package->id << ": " << n << ": voice bank audio/" << bank << " not found" << std::endl;
        }
    }

    RT_LOG(RT_TAG_MODS) << package->id << ": " << n << " (base " << def->base << ") uses its own "
                        << (own.empty() ? "nothing yet (all " + def->base + "'s)" : own) << std::endl;
    g_variants.push_back(std::move(v));
    return g_variants.back().get();
}

Variant* Get(const CharacterDef* def) {
    if (def == nullptr || def->baseIndex < 0) return nullptr;
    for (auto& v : g_variants)
        if (v->def == def) return v.get();
    const Package* owner = nullptr;
    FindCharacter(def->name, &owner);
    return owner ? Build(owner, def) : nullptr;
}

struct Entry {
    uint32_t addr = 0;
    int side = 0, player = 0, cc = -1, charIdx = 0;
    bool goalie = false, sidekick = false;
};

bool InLoader(const CpuContext* ctx) {
    return t_loaderScope > 0 || (ctx->lr >= kLoaderCodeBegin && ctx->lr < kLoaderCodeEnd);
}

bool CurrentEntry(Entry& e) {
    e.addr = Memory::Read32(kLoader + kLoaderCurrent);
    if (e.addr == 0) return false;
    e.side = static_cast<int>(Memory::Read32(e.addr + kEntryTeam) & 1);
    e.charIdx = static_cast<int>(Memory::Read32(e.addr + kEntryCharIdx));
    e.player = static_cast<int>(Memory::Read32(e.addr + kEntryPlayer));
    e.cc = static_cast<int>(Memory::Read32(e.addr + kEntryClass));
    e.goalie = Memory::Read8(e.addr + kEntryGoalie) != 0;
    e.sidekick = Memory::Read8(e.addr + kEntrySidekick) != 0;
    return e.charIdx >= 0 && e.charIdx < 10;
}

int LoaderCaptain(int side) { return static_cast<int>(Memory::Read32(kLoader + kLoaderCaptains + static_cast<uint32_t>(side) * 4)); }

// The variant for the loader's current entry, when the caller is the loader.
const Variant* LoaderVariant(CpuContext* ctx, Entry& e) {
    if (!InLoader(ctx) || !CurrentEntry(e)) return nullptr;
    return ForSlot(ctx, e.side, e.player, e.cc);
}

}  // namespace

const CharacterDef* FindCharacterNamed(const std::string& name) { return FindCharacter(name); }

uint32_t AllocPersistent(uint32_t size, uint32_t alignment) { return DataAlloc(size, alignment); }

namespace {
const CharacterDef* g_menuCharacters[2] = {};
}
void SetMenuCharacter(int side, const CharacterDef* def) {
    if (side == 0 || side == 1) g_menuCharacters[side] = def;
}
const CharacterDef* MenuCharacter(int side) { return side == 0 || side == 1 ? g_menuCharacters[side] : nullptr; }

bool ToBaseName(const std::string& name, std::string& base) {
    base = name;
    bool changed = false;
    const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    for (const Package* package : ActivePackages()) {
        for (const CharacterDef& c : package->characters) {
            const char* baseName = BaseCharacterName(c.baseIndex);
            if (!baseName) continue;
            for (size_t at = 0; (at = Lower(base).find(c.name, at)) != std::string::npos;) {
                const size_t end = at + c.name.size();
                if ((at > 0 && word(base[at - 1])) || (end < base.size() && word(base[end]))) {
                    at = end;
                    continue;
                }
                base.replace(at, c.name.size(), baseName);
                at += std::strlen(baseName);
                changed = true;
            }
        }
    }
    return changed;
}

const CharacterDef* SelectedCaptain(int side) {
    const CharacterDef* def = FindCharacter(RuntimeConfigFile::ModSelection(side));
    return def && def->kind == "captain" ? def : nullptr;
}

const CharacterDef* FixedTeamCaptain(int side) {
    const CharacterDef* def = SelectedCaptain(side);
    if (def == nullptr || !def->fixedTeam || side < 0 || side > 1) return nullptr;
    const uint32_t info = Memory::Read32(MscGuest::kGameInfoManager);
    const uint32_t game = info ? Memory::Read32(info + 0x80 + Memory::Read32(info + 0x11C) * 4) : 0;  // GetTeam(side)
    return game != 0 && static_cast<int>(Memory::Read32(game + static_cast<uint32_t>(side) * 4)) == def->baseIndex ? def : nullptr;
}

int TeammateClass(const CharacterDef* captain, int player) {
    if (captain == nullptr || player < 1 || player > 3 || static_cast<size_t>(player - 1) >= captain->teammates.size()) return -1;
    const std::string& name = captain->teammates[static_cast<size_t>(player - 1)];
    if (const CharacterDef* mate = FindCharacter(name)) return mate->baseIndex;
    return BaseCharacterIndex(name);
}

const Variant* ForSlot(CpuContext*, int side, int player, int characterClass) {
    const CharacterDef* captain = SelectedCaptain(side);
    if (captain == nullptr || LoaderCaptain(side) != captain->baseIndex) return nullptr;  // the team isn't the base's
    if (player == 0) return characterClass == captain->baseIndex ? Get(captain) : nullptr;
    if (player == 4) {
        const Variant* v = Get(captain);
        return v && v->goalieInfo ? v : nullptr;
    }
    if (player < 1 || player > 3 || static_cast<size_t>(player - 1) >= captain->teammates.size()) return nullptr;
    const CharacterDef* mate = FindCharacter(captain->teammates[static_cast<size_t>(player - 1)]);
    return mate && mate->baseIndex == characterClass ? Get(mate) : nullptr;
}

const Variant* ForCharacter(CpuContext*, const CharacterDef* def) { return Get(def); }

const Variant* ForTemplateInfo(uint32_t templateInfo) {
    for (const auto& v : g_variants)
        if (templateInfo != 0 && v->templateInfo == templateInfo) return v.get();
    return nullptr;
}

const Variant* ForRow(uint32_t characterInfo) {
    for (const auto& v : g_variants)
        if (characterInfo != 0 && (v->row == characterInfo || v->goalieRow == characterInfo)) return v.get();
    return nullptr;
}

const Variant* ForBank(int bankIndex) {
    for (const auto& v : g_variants)
        if (v->bankIndex == bankIndex) return v.get();
    return nullptr;
}

namespace {
constexpr uint32_t kVoicePreviewCue = 0x270203EDu;
constexpr uint32_t kLoadSoundBank = 0x800EBB04u;   // LoadSoundBank(GameAudio*, bank, slot, callback, param)
constexpr uint32_t kUnloadBank = 0x802EBC9Cu;      // AudioBankTable::UnloadBank(slot)
constexpr uint32_t kFePlaySound = 0x801CBC54u;     // FEAudio::PlaySound(slot, cue, const void*, void*)
constexpr uint32_t kNoCallback = 0x801CBCE4u;      // FEAudio::StopAnimAudioEvent: an empty function
constexpr uint32_t kOwnerCompleted = 0x08;         // AudioResourceLoadOwner::m_Completed
struct MenuVoice {
    const Variant* variant = nullptr;
    bool pending = false;  // play once loaded
};
MenuVoice g_menuVoices[2];

uint32_t VoiceSlot(int side) { return side == 0 ? 1u : 5u; }
bool SlotLoaded(int side) {
    if (g_bankTable == 0) return false;
    const uint32_t owner = Memory::Read32(Memory::Read32(g_bankTable + kBankSources) + VoiceSlot(side) * kBankSourceSize + kSourceOwner);
    return owner != 0 && Memory::Read8(owner + kOwnerCompleted) != 0;
}
}  // namespace

bool PlayMenuVoice(CpuContext* ctx, int side, const CharacterDef* def) {
    const Variant* v = Get(def);
    if (v == nullptr || v->bankRecord == 0 || side < 0 || side > 1) return false;
    MenuVoice& voice = g_menuVoices[side];
    if (voice.variant != v) {
        if (voice.variant != nullptr && g_bankTable != 0 && SlotLoaded(side))
            MscGuest::Call(ctx, kUnloadBank, {g_bankTable, VoiceSlot(side)});
        const uint32_t audio = Memory::Read32(ctx->gpr[13] - 5028);  // g_pAudioSystem
        MscGuest::Call(ctx, kLoadSoundBank, {audio, static_cast<uint32_t>(v->bankIndex), VoiceSlot(side), kNoCallback, 0});
        voice.variant = v;
    }
    voice.pending = true;
    UpdateMenuVoices(ctx);
    return true;
}

void UpdateMenuVoices(CpuContext* ctx) {
    for (int side = 0; side < 2; ++side) {
        MenuVoice& voice = g_menuVoices[side];
        if (!voice.pending || !SlotLoaded(side)) continue;
        MscGuest::Call(ctx, kFePlaySound, {VoiceSlot(side), kVoicePreviewCue, 0, 0});
        voice.pending = false;
    }
}

bool ReleaseMenuVoices(CpuContext* ctx) {
    bool done = true;
    for (int side = 0; side < 2; ++side) {
        MenuVoice& voice = g_menuVoices[side];
        if (voice.variant == nullptr) continue;
        if (!SlotLoaded(side)) {  // still loading: unload once it's in
            done = false;
            continue;
        }
        MscGuest::Call(ctx, kUnloadBank, {g_bankTable, VoiceSlot(side)});
        voice = MenuVoice{};
    }
    return done;
}

LoaderScope::LoaderScope() { ++t_loaderScope; }
LoaderScope::~LoaderScope() { --t_loaderScope; }

}  // namespace Mods::Variants

using namespace Mods::Variants;

// tCharacterTemplate* GetCharacterTemplate(int index, bool* created): the class's template, made on
// first use. As the original; for a variant's player, while the loader works on it, the variant's own.
extern "C" void MSC_GetCharacterTemplate_80025F5C(CpuContext* ctx)
{
    const int32_t index = static_cast<int32_t>(ctx->gpr[3]);
    const uint32_t created = ctx->gpr[4];
    Memory::Write8(created, 0);
    Entry e;
    if (index < 20) {
        if (const Variant* v = LoaderVariant(ctx, e); v && !e.goalie && e.cc == index) {
            auto* own = const_cast<Variant*>(v);
            if (own->templateObject == 0) {
                own->templateObject = MscGuest::Alloc(ctx, kTemplateSize, 8);
                Memory::Write8(created, 1);
            }
            ctx->gpr[3] = own->templateObject;
            return;
        }
    }
    const uint32_t slot = index < 20 ? kTemplates + static_cast<uint32_t>(index) * 4 : kGoalieTemplate;
    if (Memory::Read32(slot) == 0) {
        Memory::Write32(slot, MscGuest::Alloc(ctx, kTemplateSize, 8));
        Memory::Write8(created, 1);
    }
    ctx->gpr[3] = Memory::Read32(slot);
}
PPC_NATIVE_OVERRIDE_VOID(80025F5C, MSC_GetCharacterTemplate_80025F5C, (CpuContext* ctx), (ctx));

// tCharacterTemplateInfo* GetCharacterTemplateInfo(eCharacterClass cc): the class's file names. As the
// original; for a variant's player, while the loader works on it, the variant's.
extern "C" void MSC_GetCharacterTemplateInfo_8002600C(CpuContext* ctx)
{
    const int32_t cc = static_cast<int32_t>(ctx->gpr[3]);
    Entry e;
    if (cc < 20) {
        if (const Variant* v = LoaderVariant(ctx, e); v && !e.goalie && e.cc == cc) {
            ctx->gpr[3] = v->templateInfo;
            return;
        }
    }
    ctx->gpr[3] = cc < 20 ? kTemplateInfos + static_cast<uint32_t>(cc) * kInfoSize : kGoalieTemplateInfo;
}
PPC_NATIVE_OVERRIDE_VOID(8002600C, MSC_GetCharacterTemplateInfo_8002600C, (CpuContext* ctx), (ctx));

// tGoalieTemplateInfo* GetGoalieTemplateInfo(int goalie): a team's goalie kit. As the original; for a
// variant captain's goalie, while the loader works on it, the variant's kit.
extern "C" void MSC_GetGoalieTemplateInfo_80025F48(CpuContext* ctx)
{
    const int32_t goalie = static_cast<int32_t>(ctx->gpr[3]);
    Entry e;
    if (const Variant* v = LoaderVariant(ctx, e); v && e.goalie && e.cc - kFirstGoalie == goalie && v->goalieInfo) {
        ctx->gpr[3] = v->goalieInfo;
        return;
    }
    ctx->gpr[3] = kGoalieKits + static_cast<uint32_t>(goalie) * kKitSize;
}
PPC_NATIVE_OVERRIDE_VOID(80025F48, MSC_GetGoalieTemplateInfo_80025F48, (CpuContext* ctx), (ctx));

// const CharacterInfo& GetCharacterInfo(int index). As the original (row 32 when out of range); while
// the loader works on a variant's player, that class answers with the variant's row (its goalie row
// for a goalie), and its team's captain class with the variant captain's row: the loader builds
// partner kit names (<partner>_<captain>) from it, so a team led by a variant gets the variant's kits
// when the mod ships them and the base's otherwise.
extern "C" void MSC_GetCharacterInfo_800FBD60(CpuContext* ctx)
{
    const int32_t index = static_cast<int32_t>(ctx->gpr[3]);
    if (index == kPseudoCharacterIndex || index == kPseudoCharacterIndex + 1) {  // a tagged team (variant_identity.cpp)
        const int side = index - kPseudoCharacterIndex;
        const Mods::CharacterDef* def = g_menuCharacters[side] ? g_menuCharacters[side] : SelectedCaptain(side);
        const Variant* v = def ? Get(def) : nullptr;
        ctx->gpr[3] = v ? v->row : MscGuest::CharacterInfo(def ? static_cast<uint32_t>(def->baseIndex) : 32u);
        return;
    }
    // FEModelManager::FinishLoadModel naming a hologram (its impostor) after its template's class:
    // a variant's hologram is named after the variant. Holograms' render targets go by name, so a
    // variant and its base (Super Team vs Waluigi on choose sides) would otherwise share one, and
    // destroying the second frees it twice. Here r27 is the model being finished (its template info
    // at +0x20).
    constexpr uint32_t kFinishLoadModelNameCall = 0x801C2424u;
    if (ctx->lr == kFinishLoadModelNameCall) {
        if (const Variant* v = ForTemplateInfo(Memory::Read32(ctx->gpr[27] + 0x20))) {
            ctx->gpr[3] = v->row;
            return;
        }
    }
    Entry e;
    if (InLoader(ctx) && CurrentEntry(e)) {
        if (const Variant* v = ForSlot(ctx, e.side, e.player, e.cc); v && index == e.cc) {
            ctx->gpr[3] = e.goalie ? v->goalieRow : v->row;
            return;
        }
        const int captain = LoaderCaptain(e.side);
        if (index == captain && index != e.cc) {
            if (const Variant* v = ForSlot(ctx, e.side, 0, captain)) {
                bool use = true;
                if (e.sidekick && e.cc >= 12 && e.cc < 20) {  // a partner's kit: only if the mod has it
                    const std::string partner = Mods::BaseCharacterName(e.cc);
                    const std::string prefix = e.cc == 13 ? "hammer" : partner;
                    use = DVDPathExistsForRuntime(("art/characters/" + partner + "/" + prefix + "_" + v->def->name + ".rlt").c_str());
                }
                if (use) {
                    ctx->gpr[3] = v->row;
                    return;
                }
            }
        }
    }
    ctx->gpr[3] = MscGuest::CharacterInfo(static_cast<uint32_t>(index));
}
PPC_NATIVE_OVERRIDE_VOID(800FBD60, MSC_GetCharacterInfo_800FBD60, (CpuContext* ctx), (ctx));

// cAnimInventory* FindDuplicateAnimInventory(int index, ulong hash): another template that already
// loaded the same animation file, to share it. As the original, plus variant templates: a variant
// without animations of its own shares its base's (and the base the variant's).
extern "C" void MSC_FindDuplicateAnimInventory_80026034(CpuContext* ctx)
{
    const int32_t index = static_cast<int32_t>(ctx->gpr[3]);
    const uint32_t hash = ctx->gpr[4];
    Entry e;
    uint32_t requester = index >= 0 && index < 20 ? Memory::Read32(kTemplates + static_cast<uint32_t>(index) * 4) : 0;
    if (const Variant* v = LoaderVariant(ctx, e); v && !e.goalie && e.cc == index) requester = v->templateObject;
    auto match = [&](uint32_t t) { return t != 0 && t != requester && Memory::Read32(t + kTemplateAnimHash) == hash; };
    for (uint32_t i = 0; i < 20; ++i) {
        const uint32_t t = Memory::Read32(kTemplates + i * 4);
        if (match(t)) {
            ctx->gpr[3] = Memory::Read32(t + kTemplateAnims);
            return;
        }
    }
    for (const auto& v : g_variants) {
        if (match(v->templateObject)) {
            ctx->gpr[3] = Memory::Read32(v->templateObject + kTemplateAnims);
            return;
        }
    }
    ctx->gpr[3] = 0;
}
PPC_NATIVE_OVERRIDE_VOID(80026034, MSC_FindDuplicateAnimInventory_80026034, (CpuContext* ctx), (ctx));

// void CharacterLoader_8056B290::fn_8000BA00(): creates the current entry's player. As the original;
// a variant's player then carries the variant's CharacterInfo, which every name lookup for it reads.
static void CreateLoaderCharacter(CpuContext* ctx)
{
    Entry e;
    const Variant* v = CurrentEntry(e) ? ForSlot(ctx, e.side, e.player, e.cc) : nullptr;
    func_8000BA00(ctx);
    if (v == nullptr) return;
    if (const uint32_t character = Memory::Read32(kCharacters + static_cast<uint32_t>(e.charIdx) * 4))
        Memory::Write32(character + kCharacterRow, e.goalie ? v->goalieRow : v->row);
}
PPC_NATIVE_WRAP(8000BA00, CreateLoaderCharacter);

// void DestroyCharacters(): frees the match's players and templates. As the original, then the same
// for the variants' templates (run through it again from the emptied template slots), and their
// loaded flags reset like the game's own.
static void DestroyCharactersAndVariants(CpuContext* ctx)
{
    func_80026370(ctx);
    std::vector<Variant*> made;
    for (const auto& v : g_variants)
        if (v->templateObject != 0) made.push_back(v.get());
    for (size_t first = 0; first < made.size(); first += 20) {
        for (size_t i = first; i < made.size() && i < first + 20; ++i)
            Memory::Write32(kTemplates + static_cast<uint32_t>(i - first) * 4, made[i]->templateObject);
        func_80026370(ctx);
    }
    for (const auto& v : g_variants) {
        v->templateObject = 0;
        Memory::Write8(v->templateInfo + kInfoLoaded, 0);
        if (v->goalieInfo) Memory::Write8(v->goalieInfo + kKitLoaded, 0);
    }
}
PPC_NATIVE_WRAP(80026370, DestroyCharactersAndVariants);

// --- Extra effects bundles (CharacterDef::extraEffects) --------------------------------------------
// Loaded with a character's own effects bundle, as the loader loads that one (fn_8000A418 starts the
// loads, fn_8000A5D8 waits for them and registers the bundle in the current resource pool), so they
// go when the match's effects go. Each once a match.
namespace {
constexpr uint32_t kLoaderCurrentIndex = 0xC8;
constexpr uint32_t kLoadEntireFileAsync = 0x802B396Cu;      // nlLoadEntireFileAsync(path, cb, param, align, type, buffer, size, allocator)
constexpr uint32_t kLoadCompressedFileAsync = 0x802B3E94u;  // nlLoadCompressedFileAsync(path, cb, param, align, type, chunk, buf0, buf1, p, size, allocator)
constexpr uint32_t kStoreLoaded = 0x8000A410u;              // the loader's callback: *param = data
constexpr uint32_t kLoadEffectsBundle = 0x802E67E0u;        // EmissionManager::LoadBundle(data, nonres, pool, type)
struct ExtraEffects {
    std::string path;
    uint32_t slots = 0;  // guest {data, nonres}, filled by the loads
    bool data = false, nonres = false;
};
std::vector<ExtraEffects> g_extraEffects;    // started for the current character
std::set<std::string> g_extraEffectsLoaded;  // this match's
std::map<std::string, std::pair<uint32_t, uint32_t>> g_extraEffectsGuest;  // path -> {slots, path string}
} // namespace

extern "C" void func_8000A418(CpuContext* ctx);
static void StartEffects(CpuContext* ctx)
{
    func_8000A418(ctx);
    g_extraEffects.clear();
    if (Memory::Read32(kLoader + kLoaderCurrentIndex) == 0) g_extraEffectsLoaded.clear();  // a match's first character
    Entry e;
    if (!CurrentEntry(e) || e.goalie) return;
    const Variant* v = ForSlot(ctx, e.side, e.player, e.cc);
    if (v == nullptr) return;
    for (const std::string& path : v->def->extraEffects) {
        if (!Exists(path)) {
            RT_LOG(RT_TAG_MODS) << v->def->name << ": extra effects " << path << " not found" << std::endl;
            continue;
        }
        if (!g_extraEffectsLoaded.insert(path).second) continue;
        std::string nonres = path;
        if (nonres.size() > 4 && nonres.compare(nonres.size() - 4, 4, ".bun") == 0) nonres.insert(nonres.size() - 4, "nonres");
        nonres += ".zlib";
        auto& guest = g_extraEffectsGuest[path];
        if (guest.first == 0) guest = {DataAlloc(8), DataString(path)};
        if (guest.first == 0 || guest.second == 0) continue;
        ExtraEffects x{path, guest.first};
        Memory::Write32(x.slots, 0);
        Memory::Write32(x.slots + 4, 0);
        x.data = MscGuest::Call(ctx, kLoadEntireFileAsync, {guest.second, kStoreLoaded, x.slots, 0x20, 0, 0, 0, 0}) != 0;
        if (Exists(nonres)) {
            auto& nonresGuest = g_extraEffectsGuest[nonres];
            if (nonresGuest.second == 0) nonresGuest.second = DataString(nonres);
            // Arguments 9-11 (none) in a stack frame of ours, as the loader passes them.
            const uint32_t sp = ctx->gpr[1], frame = sp - 0x40;
            Memory::Write32(frame, sp);
            for (uint32_t k = 4; k < 0x40; k += 4) Memory::Write32(frame + k, 0);
            ctx->gpr[1] = frame;
            x.nonres = (MscGuest::Call(ctx, kLoadCompressedFileAsync, {nonresGuest.second, kStoreLoaded, x.slots + 4, 0x20, 1, 0x20000, 0, 0}) & 0xFF) != 0;
            ctx->gpr[1] = sp;
        }
        g_extraEffects.push_back(x);
    }
}
PPC_NATIVE_WRAP(8000A418, StartEffects);

extern "C" void func_8000A5D8(CpuContext* ctx);
static void FinishEffects(CpuContext* ctx)
{
    for (const ExtraEffects& x : g_extraEffects) {
        if ((x.data && Memory::Read32(x.slots) == 0) || (x.nonres && Memory::Read32(x.slots + 4) == 0)) {
            ctx->gpr[3] = 0;  // still loading
            return;
        }
    }
    func_8000A5D8(ctx);
    if ((ctx->gpr[3] & 0xFF) == 0 || g_extraEffects.empty()) return;
    const uint32_t pool = Memory::Read32(ctx->gpr[13] - 5352);  // glGetCurrentResourcePool()
    for (const ExtraEffects& x : g_extraEffects) {
        MscGuest::Call(ctx, kLoadEffectsBundle, {Memory::Read32(x.slots), Memory::Read32(x.slots + 4), pool, 1});
        RT_LOG(RT_TAG_MODS) << "extra effects " << x.path << " loaded" << std::endl;
    }
    g_extraEffects.clear();
    ctx->gpr[3] = 1;
}
PPC_NATIVE_WRAP(8000A5D8, FinishEffects);

// void AudioBankTable::LoadBank(int bank, ulong slot, callback, param, allocator): loads a sound bank
// (by its index in the bank table) into a slot. As the original; a variant's virtual bank index loads
// the variant's bank by name.
extern "C" void MSC_AudioBankTableLoadBank_802EBBF0(CpuContext* ctx)
{
    const uint32_t table = ctx->gpr[3];
    g_bankTable = table;
    const int32_t bank = static_cast<int32_t>(ctx->gpr[4]);
    const uint32_t slot = ctx->gpr[5];
    uint32_t resource = Memory::Read32(table + kBankNames) + static_cast<uint32_t>(bank) * 8;
    if (bank >= kFirstVirtualBank) {
        const Variant* v = ForBank(bank);
        resource = v ? v->bankRecord : Memory::Read32(table + kBankNames);  // never a wild index
    }
    const uint32_t source = Memory::Read32(table + kBankSources) + slot * kBankSourceSize;
    Memory::Write32(source + kSourceResource, resource);
    Memory::Write8(source + kSourceLoaded, 1);
    const uint32_t callback = ctx->gpr[6], param = ctx->gpr[7], allocator = ctx->gpr[8];
    MscGuest::Call(ctx, kLoadAudioResource, {Memory::Read32(source + kSourceOwner), Memory::Read32(resource + 4), callback, param, allocator});
}
PPC_NATIVE_OVERRIDE_VOID(802EBBF0, MSC_AudioBankTableLoadBank_802EBBF0, (CpuContext* ctx), (ctx));

// Partner team kits: a partner wears its captain's colours from "art/characters/<partner>/<prefix>_<captain>
// [_alt].rlt" (texture "<prefix>_<captain>[_alt]/<same>", swapped in for the stand-in "<partner>/<prefix>_mario";
// prefix "hammer" for Hammer Bro). The two loader steps below are the originals (fn_8000B3E0 starts loading
// the kit, fn_8000B6C4 installs it), except:
// - <captain> is a mod captain's name when its mod ships that kit, else its base's;
// - the away team's partners wear _alt kits in a mirror match only when both captains are the same
//   character (a mod captain and its base aren't: Super Team vs Waluigi);
// - an _alt kit that doesn't exist falls back to the normal one rather than the stand-in (only a mod
//   captain against its base gets that far: the game itself only asks for _alt kits that exist).
namespace {
constexpr uint32_t kLoaderSidekickData = 0x178, kLoaderSidekickSize = 0x17C;
constexpr uint32_t kSidekickKitLoaded = 0x8000B3CCu;     // fn_8000B3CC (load callback)
constexpr uint32_t kSetOriginalTexture = 0x80022DACu;    // cCharacter::fn_80022DAC(ulong)
constexpr uint32_t kSetReplacementTexture = 0x80022DE8u; // cCharacter::fn_80022DE8(ulong)
constexpr uint32_t kTextureLoaded = 0x802CDC34u;         // glTextureLoad(ulong)
constexpr uint32_t kBeginLoadTextures = 0x802C8204u;     // glBeginLoadTextureBundle(path, callback, param, pool)
constexpr uint32_t kEndLoadTextures = 0x802CDD78u;       // glEndLoadTextureBundle(data, size, pool, bool)
constexpr uint32_t kCurrentResourcePool = 0x802CC094u;   // glGetCurrentResourcePool()
constexpr uint32_t kNlFree = 0x802AA800u;                // nlFree(void*)

struct PartnerKit { std::string bundle, texture, standIn; };

// The row naming a side's captain for its partners' kits, and whether it's a mod character.
uint32_t CaptainRow(CpuContext* ctx, int side, const Variant** variant) {
    const int captain = LoaderCaptain(side);
    *variant = ForSlot(ctx, side, 0, captain);
    return *variant ? (*variant)->row : MscGuest::CharacterInfo(static_cast<uint32_t>(captain));
}

PartnerKit KitFor(CpuContext* ctx, const Entry& e) {
    const std::string name = MscGuest::CString(Memory::Read32(MscGuest::CharacterInfo(static_cast<uint32_t>(e.cc)) + kRowName));
    const std::string prefix = e.cc == 13 ? "hammer" : name;
    const Variant* mine = nullptr;
    const Variant* theirs = nullptr;
    const uint32_t myRow = CaptainRow(ctx, e.side, &mine);
    const uint32_t theirRow = CaptainRow(ctx, e.side ^ 1, &theirs);
    std::string captain = MscGuest::CString(Memory::Read32(myRow + kRowName));
    auto bundle = [&](const std::string& cap, bool alt) { return "art/characters/" + name + "/" + prefix + "_" + cap + (alt ? "_alt" : "") + ".rlt"; };
    if (mine && !Exists(bundle(captain, false)) && !Exists(bundle(captain, true)))
        captain = Mods::BaseCharacterName(mine->def->baseIndex);  // the mod has no kits: its base's
    // Which team wears the alternate kit (GetAlternateCaptain on the two captains' rows).
    const int captain0 = LoaderCaptain(0), captain1 = LoaderCaptain(1);
    bool alt;
    if (captain0 == captain1) {
        alt = mine == theirs && e.side == 1;  // a true mirror: the away team
    } else {
        const uint32_t row0 = e.side == 0 ? myRow : theirRow, row1 = e.side == 0 ? theirRow : myRow;
        const uint32_t mask0 = Memory::Read32(row0 + kRowColourMask), mask1 = Memory::Read32(row1 + kRowColourMask);
        const int32_t rank0 = static_cast<int32_t>(Memory::Read32(row0 + kRowColourRank));
        const int32_t rank1 = static_cast<int32_t>(Memory::Read32(row1 + kRowColourRank));
        const int altSide = (mask0 & mask1) == 0 ? -1 : rank0 < rank1 ? 1 : 0;
        alt = altSide == e.side;
    }
    if (alt && !Exists(bundle(captain, true))) alt = false;
    const std::string kit = prefix + "_" + captain + (alt ? "_alt" : "");
    return {bundle(captain, alt), kit + "/" + kit, name + "/" + prefix + "_mario"};
}

void SetKitTextures(CpuContext* ctx, uint32_t character, uint32_t original, uint32_t replacement) {
    MscGuest::Call(ctx, kSetOriginalTexture, {character, original});
    MscGuest::Call(ctx, kSetReplacementTexture, {character, replacement});
}
}  // namespace

// bool CharacterLoader_8056B290::fn_8000B3E0(): a partner's team kit: already loaded (install it, false),
// loading started (true), or none (no swap, false).
extern "C" void MSC_CharacterLoaderPartnerKit_8000B3E0(CpuContext* ctx)
{
    const uint32_t loader = ctx->gpr[3];
    Entry e;
    if (!CurrentEntry(e)) {
        ctx->gpr[3] = 0;
        return;
    }
    const PartnerKit kit = KitFor(ctx, e);
    Memory::Write32(loader + kLoaderSidekickData, 0);
    Memory::Write32(loader + kLoaderSidekickSize, 0);
    const uint32_t character = Memory::Read32(kCharacters + static_cast<uint32_t>(e.charIdx) * 4);
    const uint32_t texture = MscGuest::StringHash(kit.texture);  // glGetTexture
    if (MscGuest::Call(ctx, kTextureLoaded, {texture}) & 0xFF) {
        SetKitTextures(ctx, character, MscGuest::StringHash(kit.standIn), texture);
        ctx->gpr[3] = 0;
        return;
    }
    // The bundle path lives in a stack frame of ours for the call, as the original's szArtPath[64].
    const uint32_t savedLr = ctx->lr, sp = ctx->gpr[1], frame = sp - 0x90;
    Memory::Write32(frame, sp);
    for (size_t i = 0; i <= kit.bundle.size() && i < 0x7F; ++i)
        Memory::Write8(frame + 8 + static_cast<uint32_t>(i), i < kit.bundle.size() ? static_cast<uint8_t>(kit.bundle[i]) : 0);
    ctx->gpr[1] = frame;
    const uint32_t pool = MscGuest::Call(ctx, kCurrentResourcePool, {});
    const uint32_t started = MscGuest::Call(ctx, kBeginLoadTextures, {frame + 8, kSidekickKitLoaded, e.addr, pool}) & 0xFF;
    ctx->gpr[1] = sp;
    ctx->lr = savedLr;
    if (!started) SetKitTextures(ctx, character, 0xFFFFFFFFu, 0xFFFFFFFFu);
    ctx->gpr[3] = started ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE_VOID(8000B3E0, MSC_CharacterLoaderPartnerKit_8000B3E0, (CpuContext* ctx), (ctx));

// bool CharacterLoader_8056B290::fn_8000B6C4(): installs a partner's team kit once loaded.
extern "C" void MSC_CharacterLoaderPartnerKitLoaded_8000B6C4(CpuContext* ctx)
{
    const uint32_t loader = ctx->gpr[3];
    const uint32_t data = Memory::Read32(loader + kLoaderSidekickData);
    Entry e;
    if (data == 0 || !CurrentEntry(e)) {
        ctx->gpr[3] = 0;
        return;
    }
    const uint32_t pool = MscGuest::Call(ctx, kCurrentResourcePool, {});
    MscGuest::Call(ctx, kEndLoadTextures, {data, Memory::Read32(loader + kLoaderSidekickSize), pool, 0});
    MscGuest::Call(ctx, kNlFree, {data});
    Memory::Write32(loader + kLoaderSidekickData, 0);
    const PartnerKit kit = KitFor(ctx, e);
    SetKitTextures(ctx, Memory::Read32(kCharacters + static_cast<uint32_t>(e.charIdx) * 4),
                   MscGuest::StringHash(kit.standIn), MscGuest::StringHash(kit.texture));
    ctx->gpr[3] = 1;
}
PPC_NATIVE_OVERRIDE_VOID(8000B6C4, MSC_CharacterLoaderPartnerKitLoaded_8000B6C4, (CpuContext* ctx), (ctx));
