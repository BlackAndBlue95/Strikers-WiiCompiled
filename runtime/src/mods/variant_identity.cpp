// Team identity for mod characters (mods/variants.h): wherever the match names a team, a team led by
// a variant answers to the variant's name (HUD icon, goal and results overlays, match loading, stadium
// banners, crowd, cutscenes, super ability, newspaper), and anything its mod doesn't supply falls back
// to its base's (front-end images and slides, team textures, effects, cutscenes, files).
//
// Team ids only say which captain, not which side, and a variant shares its base's id, so the id is
// tagged at its source: GameInfoManager::GetTeam, for the callers listed below (each checked to use
// the id only for name lookups and compares against 0 and 4), returns id | tag | side for a side led
// by a variant. GetCharacterIndexFromCaptain turns the tag into a pseudo character index and
// GetCharacterInfo (variants.cpp) into the variant's row. Every other caller gets the plain id.
#include "mods/variants.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "hle/msc_guest.h"
#include "hle_stubs.h"
#include "mods/mod_catalogs.h"
#include "mods/mod_registry.h"
#include "runtime_log.h"

bool DVDPathExistsForRuntime(const char* dvdPath);  // storage/dvd.cpp
extern "C" void func_801EE6A8(CpuContext* ctx);   // PausePostGameScene::BuildStoryArticle
extern "C" void func_801F79C8(CpuContext* ctx);   // StrikerTimesOverlay::SceneCreated
extern "C" void func_8027FD04(CpuContext* ctx);   // NisPlayer::Load(type, target, ...)
extern "C" void func_8027F9D4(CpuContext* ctx);   // NisPlayer::GetTargetFilter(target, winner)
extern "C" void func_80301E6C(CpuContext* ctx);   // TLComponent::SetActiveSlide(hash, bool, bool)
extern "C" void func_80301DA0(CpuContext* ctx);   // TLComponent::SetActiveSlide(const char*, bool, bool)
extern "C" void func_8030677C(CpuContext* ctx);   // FEFindInstance(FEPresentation*, 6 name hashes)
extern "C" void func_803068F8(CpuContext* ctx);   // FEFindInstanceRecursive(TLInstance*, 6 name hashes)
extern "C" void func_801CF6E8(CpuContext* ctx);   // MatchLoadingScene::SetTeamLogo(int side, CharacterInfo)
extern "C" void func_8000A790(CpuContext* ctx);   // CharacterLoader_8056B290::fn_8000A790 (ExtraTextures loaded)

using namespace Mods;
using namespace Mods::Variants;

namespace {

constexpr uint32_t kGameInfos = 0x80, kCurrentMode = 0x11C;  // GameInfoManager: mGameInfo[mode], mCurrentMode

// GetTeam's return addresses (call + 4) whose id only names the team (the team-id lookup audit):
// super ability overlay, crowd list, match loading names and logos, HUD captain icons, final-score
// winner, goal info, match-end overlay, winner title, cup-win overlay, stadium NPC and world banners,
// NIS target filters, NIS mega cone colour. Not the loader's (it works by class).
constexpr uint32_t kNamingCallers[] = {
    0x800C8EB0, 0x801A4248, 0x801A4260, 0x801CE350, 0x801CE360, 0x801E9928, 0x801EC2D8, 0x801F1EC4,
    0x801F2C14, 0x801F2C38, 0x801F2C88, 0x801F2CCC, 0x801F3E5C, 0x801F4780, 0x802768BC, 0x802768D4,
    0x80279C10, 0x80279C20, 0x8027FA10, 0x8027FA2C, 0x8027FC28, 0x8027FC78, 0x8027FC9C, 0x802811EC,
    // Front end: FECharacterPDAComponent::TintInstanceForCaptain (the stats panel's team colour; it
    // tells the sides apart with captain == team0, right in a mirror once both carry the tag).
    0x801E0DDC, 0x801E0DEC,
};

bool NamingCaller(uint32_t lr) {
    return std::find(std::begin(kNamingCallers), std::end(kNamingCallers), lr) != std::end(kNamingCallers);
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The side is led by a variant of the captain it has.
bool LedByVariant(int side, uint32_t team) {
    const CharacterDef* def = SelectedCaptain(side);
    return def != nullptr && static_cast<uint32_t>(def->baseIndex) == team;
}

uint32_t CurrentGameInfo() {
    const uint32_t manager = Memory::Read32(MscGuest::kGameInfoManager);
    return manager ? Memory::Read32(manager + kGameInfos + Memory::Read32(manager + kCurrentMode) * 4) : 0;
}

}  // namespace

// int GameInfoManager::GetTeam(short side) const: the side's team (captain) id, or -1. As the original;
// for the naming callers, a variant-led side's id is tagged.
extern "C" void MSC_GameInfoManagerGetTeam_800FC674(CpuContext* ctx)
{
    const uint32_t lr = ctx->lr;  // read here, in the body: the translator then keeps callers' LR stores
    const uint32_t manager = ctx->gpr[3];
    const int side = static_cast<int16_t>(ctx->gpr[4] & 0xFFFF);
    const uint32_t info = Memory::Read32(manager + kGameInfos + Memory::Read32(manager + kCurrentMode) * 4);
    if (info == 0) {
        ctx->gpr[3] = 0xFFFFFFFFu;
        return;
    }
    uint32_t team = Memory::Read32(info + static_cast<uint32_t>(side) * 4);
    if ((side == 0 || side == 1) && NamingCaller(lr) && LedByVariant(side, team)) team = TagTeam(team, side);
    ctx->gpr[3] = team;
}
PPC_NATIVE_OVERRIDE_VOID(800FC674, MSC_GameInfoManagerGetTeam_800FC674, (CpuContext* ctx), (ctx));

// int GetCharacterIndexFromCaptain(int captain): the character row of a captain (team) id, or -1. As the
// original; a tagged id gives the pseudo index of the variant leading that side.
extern "C" void MSC_GetCharacterIndexFromCaptain_800FBD94(CpuContext* ctx)
{
    const uint32_t captain = ctx->gpr[3];
    if (IsTaggedTeam(captain)) {
        ctx->gpr[3] = static_cast<uint32_t>(kPseudoCharacterIndex + TaggedSide(captain));
        return;
    }
    for (uint32_t i = 0; i < MscGuest::kCharacterInfoRows - 1; ++i) {
        const uint32_t row = MscGuest::kCharacterInfo + i * MscGuest::kCharacterInfoSize;
        if (Memory::Read32(row + 0x10) == captain) {  // mCaptainId
            ctx->gpr[3] = Memory::Read32(row);        // mIndex
            return;
        }
    }
    ctx->gpr[3] = 0xFFFFFFFFu;
}
PPC_NATIVE_OVERRIDE_VOID(800FBD94, MSC_GetCharacterIndexFromCaptain_800FBD94, (CpuContext* ctx), (ctx));

// The newspaper articles read the team ids straight from the game info, not through GetTeam: they're
// tagged there for the call.
static void WithTaggedTeams(CpuContext* ctx, void (*original)(CpuContext*))
{
    const uint32_t info = CurrentGameInfo();
    uint32_t saved[2] = {};
    for (int side = 0; side < 2 && info; ++side) {
        saved[side] = Memory::Read32(info + static_cast<uint32_t>(side) * 4);
        if (LedByVariant(side, saved[side])) Memory::Write32(info + static_cast<uint32_t>(side) * 4, TagTeam(saved[side], side));
    }
    original(ctx);
    for (int side = 0; side < 2 && info; ++side) Memory::Write32(info + static_cast<uint32_t>(side) * 4, saved[side]);
}
static void BuildStoryArticle(CpuContext* ctx) { WithTaggedTeams(ctx, &func_801EE6A8); }
static void StrikerTimesSceneCreated(CpuContext* ctx) { WithTaggedTeams(ctx, &func_801F79C8); }
PPC_NATIVE_WRAP(801EE6A8, BuildStoryArticle);
PPC_NATIVE_WRAP(801F79C8, StrikerTimesSceneCreated);

// --- Cutscenes ------------------------------------------------------------------------------------
// A cutscene is picked from the NIS dictionary by name, "<filter>_<type>[_<extra>]", the filter being
// the target's name (the variant's, for its team or as the scorer). With none of the variant's own for
// that type, its base's play: its animations fit, the variant having its base's skeleton.
namespace {
thread_local bool t_nisBaseFilter = false;
constexpr uint32_t kDictSize = 0x30, kDict = 0x34, kNisHeaderSize = 0x1A0, kExtraNameFilter = 0x342B4;  // NisPlayer

bool DictHasPrefix(uint32_t player, const std::string& prefix) {
    const int32_t count = static_cast<int32_t>(Memory::Read32(player + kDictSize));
    for (int32_t i = 0; i < count && i < 512; ++i) {
        const std::string name = MscGuest::CString(player + kDict + static_cast<uint32_t>(i) * kNisHeaderSize, 64);
        if (name.size() < prefix.size() || name.find("_same") != std::string::npos || name.find("_other") != std::string::npos)
            continue;
        if (std::equal(prefix.begin(), prefix.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }))
            return true;
    }
    return false;
}
}  // namespace

// void NisPlayer::Load(const char* type, NisTarget, NisUseStadiumOffset, NisUseFilter, NisWinnerType,
// int, int): picks and plays a cutscene. As the original; when the target is a variant without one of
// that type, its base's name is the filter for this pick.
static void NisLoad(CpuContext* ctx)
{
    const uint32_t player = ctx->gpr[3], type = ctx->gpr[4], target = ctx->gpr[5], useFilter = ctx->gpr[7], winner = ctx->gpr[8];
    // The filter as the original will see it (the translated GetTargetFilter itself: no plugin hooks
    // for this look), with the caller's registers kept for the real call.
    uint32_t saved[32];
    std::memcpy(saved, ctx->gpr, sizeof(saved));
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[4] = target;
    ctx->gpr[5] = winner;
    func_8027F9D4(ctx);
    const std::string filter = MscGuest::CString(ctx->gpr[3]);
    std::memcpy(ctx->gpr, saved, sizeof(saved));
    ctx->lr = savedLr;
    bool base = false;
    if (FindCharacterNamed(filter)) {
        std::string name = filter + "_" + MscGuest::CString(type);
        if (useFilter != 0) name += "_" + MscGuest::CString(player + kExtraNameFilter, 128);
        base = !DictHasPrefix(player, name);
    }
    t_nisBaseFilter = base;
    func_8027FD04(ctx);
    t_nisBaseFilter = false;
}
PPC_NATIVE_WRAP(8027FD04, NisLoad);

// const char* NisPlayer::GetTargetFilter(NisTarget, NisWinnerType) const (also a plugin wrap point):
// the original, or the base's name during a pick that falls back to it.
static void TargetFilter(CpuContext* ctx)
{
    func_8027F9D4(ctx);
    if (!t_nisBaseFilter) return;
    if (const CharacterDef* def = FindCharacterNamed(MscGuest::CString(ctx->gpr[3])))
        ctx->gpr[3] = Memory::Read32(MscGuest::CharacterInfo(static_cast<uint32_t>(def->baseIndex)) + 0x04);  // its mName
}
PPC_NATIVE_WRAP(8027F9D4, TargetFilter);

// void NisPlayer::LoadTriggers(Nis&): runs the cutscene's trigger script (the function named after it,
// else "all_<rest>"). As the original; a mod's cutscene aliased to another's script (catalogs/nis/
// triggers.txt) runs that one: the name is the alias's for the call.
extern "C" void func_8027D710(CpuContext* ctx);
static void NisLoadTriggers(CpuContext* ctx)
{
    const uint32_t header = Memory::Read32(ctx->gpr[4] + 0x28);  // Nis::mHeader, its name first
    std::string name = header ? MscGuest::CString(header, 64) : std::string();
    const size_t dot = name.rfind('.');
    const std::string alias = Catalogs::NisTriggerAlias(dot == std::string::npos ? name : name.substr(0, dot));
    if (alias.empty() || alias.size() + 5 > 64) {
        func_8027D710(ctx);
        return;
    }
    uint8_t saved[64];
    for (uint32_t i = 0; i < 64; ++i) saved[i] = Memory::Read8(header + i);
    const std::string aliased = alias + ".nis";
    for (uint32_t i = 0; i < 64; ++i) Memory::Write8(header + i, i < aliased.size() ? static_cast<uint8_t>(aliased[i]) : 0);
    func_8027D710(ctx);
    for (uint32_t i = 0; i < 64; ++i) Memory::Write8(header + i, saved[i]);
}
PPC_NATIVE_WRAP(8027D710, NisLoadTriggers);

// void NisPlayer::PrepareNisCue(ulong cue): the cutscene's audio (STREAM_GEN_NIS). As the original; a
// mod's cutscene aliased to another's cue (catalogs/nis/cues.txt) plays that one.
extern "C" void func_8027EDCC(CpuContext* ctx);
static void NisPrepareCue(CpuContext* ctx)
{
    if (const uint32_t alias = Catalogs::NisCueAlias(ctx->gpr[4])) ctx->gpr[4] = alias;
    func_8027EDCC(ctx);
}
PPC_NATIVE_WRAP(8027EDCC, NisPrepareCue);

// --- Front-end textures --------------------------------------------------------------------------
// A copy of a front-end texture resource showing the mod texture `name` ("fe/<path>", added by a
// catalog), sized as the model; 0 when the mod has no such texture.
namespace {
// A mod's front-end texture, loaded if the screen hasn't (only the textures a screen's layout uses
// come in with it): read from the mod's .gxt into guest memory kept for the session, and added to the
// front end's resource pool by its name, as AsyncImage adds an image (glTextureAdd).
// The handle a mod texture is loaded under: its own (not the name's hash, which a screen's bundle may
// still have registered from an earlier screen, with its data gone).
uint32_t ModTextureHandle(const std::string& name) { return MscGuest::LowerHash("mods/" + name); }

bool EnsureFeTexture(CpuContext* ctx, const std::string& name) {
    constexpr uint32_t kTextureLoaded = 0x802CDC34u;  // glTextureLoad(ulong)
    constexpr uint32_t kTextureAdd = 0x802CDC8Cu;     // glTextureAdd(ulong, const void*, ulong, pool)
    constexpr uint32_t kFeResources = 0x806E206Cu;    // nlSingleton<FEResourceManager>::s_pInstance
    constexpr uint32_t kResourcePool = 0x802FDD84u;   // FEResourceManager::GetResourcePool()
    static std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> s_data;  // name -> guest copy, size
    const uint32_t hash = ModTextureHandle(name);
    if (MscGuest::Call(ctx, kTextureLoaded, {hash}) & 0xFF) return true;
    auto& [data, size] = s_data[name];
    if (data == 0) {
        std::ifstream file(Catalogs::FeTextureFile(name), std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        if (bytes.size() <= 0x20 || (data = AllocPersistent(static_cast<uint32_t>(bytes.size()), 32)) == 0) return false;
        size = static_cast<uint32_t>(bytes.size());
        for (uint32_t i = 0; i < size; ++i) Memory::Write8(data + i, bytes[i]);
    }
    const uint32_t manager = Memory::Read32(kFeResources);
    if (manager == 0) return false;
    const uint32_t pool = MscGuest::Call(ctx, kResourcePool, {manager});
    MscGuest::Call(ctx, kTextureAdd, {hash, data, size, pool});
    const bool loaded = (MscGuest::Call(ctx, kTextureLoaded, {hash}) & 0xFF) != 0;
    if (!loaded) RT_LOG(RT_TAG_MODS) << "front-end texture " << name << ": the resource pool refused it" << std::endl;
    return loaded;
}

std::unordered_map<std::string, uint32_t> g_feTextures;
uint32_t FeTexture(CpuContext* ctx, uint32_t model, const std::string& name) {
    if (model == 0 || !Catalogs::HasFeTexture(name) || !EnsureFeTexture(ctx, name)) return 0;
    uint32_t& copy = g_feTextures[name];
    if (copy == 0 && (copy = MscGuest::Alloc(ctx, 0x20, 8)) != 0) {
        for (uint32_t i = 0; i < 0x20; i += 4) Memory::Write32(copy + i, Memory::Read32(model + i));
        const uint32_t hash = ModTextureHandle(name);
        Memory::Write32(copy + 0x00, 0);     // m_next
        Memory::Write32(copy + 0x04, 0);     // m_prev
        Memory::Write32(copy + 0x0C, hash);  // m_hashID
        Memory::Write8(copy + 0x10, 1);      // m_bValid
        Memory::Write32(copy + 0x18, hash);  // m_glTextureHandle
    }
    return copy;
}
}  // namespace

// --- Front-end slides and instances ---------------------------------------------------------------
// Scenes find slides and image instances by name ("<team>", "<team>_alt", "logos_TEAM_<team>", ...).
// A variant's missing one is its base's.
namespace {
struct Alias { uint32_t base; const CharacterDef* def; };
const std::unordered_map<uint32_t, Alias>& SlideAliases() {
    static const std::unordered_map<uint32_t, Alias> aliases = [] {
        std::unordered_map<uint32_t, Alias> map;
        static const char* const kPatterns[] = {"%s", "%s_alt", "logos_TEAM_%s", "LOGOS_TEAMS_%s_knockout",
                                                "lowerthird_%s", "attributes_%s", "positions_%s", "captain_%s_s",
                                                "%s_right", "%s_left", "ability_%s", "%s_super_image"};
        for (const Package* package : ActivePackages()) {
            for (const CharacterDef& c : package->characters) {
                const char* base = BaseCharacterName(c.baseIndex);
                if (!base) continue;
                for (const char* pattern : kPatterns) {
                    char mine[96], theirs[96];
                    std::snprintf(mine, sizeof(mine), pattern, c.name.c_str());
                    std::snprintf(theirs, sizeof(theirs), pattern, base);
                    map.emplace(MscGuest::LowerHash(mine), Alias{MscGuest::LowerHash(theirs), &c});
                }
            }
        }
        return map;
    }();
    return aliases;
}
}  // namespace

namespace {
// The base's slides are the game's own and shown for the base too, so what ApplyVariantToSlide
// changes in one is remembered (address, original word) and put back when it's shown for its own
// character.
std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> g_changedSlides;

void RestoreSlide(uint32_t slide) {
    const auto it = g_changedSlides.find(slide);
    if (it == g_changedSlides.end()) return;
    for (auto change = it->second.rbegin(); change != it->second.rend(); ++change) Memory::Write32(change->first, change->second);
    g_changedSlides.erase(it);
}

void Change(uint32_t slide, uint32_t addr, uint32_t value) {
    g_changedSlides[slide].push_back({addr, Memory::Read32(addr)});
    Memory::Write32(addr, value);
}

// The slide a component is about to show, by its name hash (0: none).
uint32_t SlideNamed(uint32_t component, uint32_t hash) {
    return component ? MscGuest::FE::FindItem(Memory::Read32(component + MscGuest::FE::kComponentSlides), hash, MscGuest::FE::kSlideHash) : 0;
}

// A slide of the base's, shown for a variant: its name text ("NAMES", the base's name) says the
// variant's, and its images of the base's named textures, or named after the base themselves (the
// post-match summary's "captain_<base>_s" shows another texture), show the variant's where its mod
// adds them (fe/screens/images/attributes_<name>, logos_TEAM_<name>, captain_<name>_s, ...).
void ApplyVariantToSlide(CpuContext* ctx, uint32_t slide, const CharacterDef* def)
{
    constexpr uint32_t kTextStringId = 0x90, kTextOverloadFlags = 0xA0, kTextStringIdSet = 0x8;  // TLTextInstance
    if (slide == 0 || def == nullptr) return;
    RestoreSlide(slide);  // shown for another variant before
    const char* base = BaseCharacterName(def->baseIndex);
    if (!base) return;
    static const char* const kImages[] = {"attributes_%s", "logos_TEAM_%s", "captain_%s_s", "captain_%s_ds",
                                          "%s_right", "%s_left", "lowerthird_%s"};
    const uint32_t children = Memory::Read32(slide + MscGuest::FE::kSlideChildren);
    for (uint32_t child = children ? Memory::Read32(children) : 0, guard = 0; child != 0 && guard < 256; ++guard) {
        const uint32_t type = Memory::Read32(child + MscGuest::FE::kInstanceType);
        if (type == 3 && Memory::Read32(child + MscGuest::FE::kInstanceHash) == MscGuest::LowerHash("NAMES") &&
            !def->displayName.empty() && Catalogs::HasLocKey(def->displayName)) {
            Change(slide, child + kTextStringId, MscGuest::LowerHash(def->displayName));
            Change(slide, child + kTextOverloadFlags, Memory::Read32(child + kTextOverloadFlags) | kTextStringIdSet);
        } else if (type == 2) {
            const uint32_t resource = Memory::Read32(child + MscGuest::FE::kImageTexture);
            const uint32_t hash = resource ? Memory::Read32(resource + 0x0C) : 0;  // FETextureResource::m_hashID
            const uint32_t instance = Memory::Read32(child + MscGuest::FE::kInstanceHash);
            for (const char* pattern : kImages) {
                char theirs[96], mine[96];
                std::snprintf(theirs, sizeof(theirs), pattern, base);
                if (hash != MscGuest::LowerHash(std::string("fe/screens/images/") + theirs) && instance != MscGuest::LowerHash(theirs)) continue;
                std::snprintf(mine, sizeof(mine), pattern, def->name.c_str());
                if (const uint32_t texture = FeTexture(ctx, resource, std::string("fe/screens/images/") + mine))
                    Change(slide, child + MscGuest::FE::kImageTexture, texture);
                break;
            }
        }
        if (child == children) break;
        child = Memory::Read32(child);
    }
}
}  // namespace

// void TLComponent::SetActiveSlide(ulong hash, bool, bool). As the original; a variant's slide the
// component doesn't have becomes its base's, shown as the variant's.
static void SetActiveSlideOrBase(CpuContext* ctx)
{
    const auto& aliases = SlideAliases();
    const uint32_t component = ctx->gpr[3];
    const CharacterDef* shown = nullptr;
    if (!aliases.empty()) {
        if (const auto it = aliases.find(ctx->gpr[4]); it != aliases.end() && component != 0 && SlideNamed(component, it->first) == 0) {
            ctx->gpr[4] = it->second.base;
            shown = it->second.def;
        }
    }
    if (!shown && !g_changedSlides.empty()) RestoreSlide(SlideNamed(component, ctx->gpr[4]));
    func_80301E6C(ctx);
    if (shown) ApplyVariantToSlide(ctx, Memory::Read32(component + MscGuest::FE::kComponentActiveSlide), shown);
}
PPC_NATIVE_WRAP(80301E6C, SetActiveSlideOrBase);

// void TLComponent::SetActiveSlide(const char* name, bool, bool) (the loading screen's team names,
// ...). As the original; a slide named after a variant that the component lacks is its base's, and
// its name text ("NAMES", the base's name) says the variant's.
static void SetActiveSlideByNameOrBase(CpuContext* ctx)
{
    const uint32_t component = ctx->gpr[3];
    const std::string name = MscGuest::CString(ctx->gpr[4], 64);
    std::string base;
    if (component != 0 && ToBaseName(name, base) &&
        MscGuest::FE::FindItem(Memory::Read32(component + MscGuest::FE::kComponentSlides), MscGuest::LowerHash(name),
                               MscGuest::FE::kSlideHash) == 0) {
        ctx->gpr[4] = MscGuest::LowerHash(base);  // the same pick by hash
        func_80301E6C(ctx);
        ApplyVariantToSlide(ctx, Memory::Read32(component + MscGuest::FE::kComponentActiveSlide), FindCharacterNamed(Lower(name)));
        return;
    }
    if (!g_changedSlides.empty()) RestoreSlide(SlideNamed(component, MscGuest::LowerHash(name)));
    func_80301DA0(ctx);
}
PPC_NATIVE_WRAP(80301DA0, SetActiveSlideByNameOrBase);

// FEFindInstance / FEFindInstanceRecursive (every FEFinder<...>::Find): an instance by its path of up to
// six name hashes (r4-r9). As the original; not found, a path naming a variant is tried as its base's.
static void FindWithBase(CpuContext* ctx, void (*original)(CpuContext*))
{
    uint32_t args[6];
    for (int i = 0; i < 6; ++i) args[i] = ctx->gpr[4 + i];
    const uint32_t root = ctx->gpr[3];
    original(ctx);
    const auto& aliases = SlideAliases();
    if (ctx->gpr[3] != 0 || aliases.empty()) return;
    bool any = false;
    for (int i = 0; i < 6; ++i) {
        const auto it = aliases.find(args[i]);
        if (it != aliases.end()) {
            args[i] = it->second.base;
            any = true;
        }
    }
    if (!any) return;
    ctx->gpr[3] = root;
    for (int i = 0; i < 6; ++i) ctx->gpr[4 + i] = args[i];
    original(ctx);
}
static void FindInstanceWithBase(CpuContext* ctx) { FindWithBase(ctx, &func_8030677C); }
static void FindInstanceRecursiveWithBase(CpuContext* ctx) { FindWithBase(ctx, &func_803068F8); }
PPC_NATIVE_WRAP(8030677C, FindInstanceWithBase);
PPC_NATIVE_WRAP(803068F8, FindInstanceRecursiveWithBase);

// void MatchLoadingScene::SetTeamLogo(int side, CharacterInfo character): the side's portrait, copied
// from the art instance "logos_TEAM_<name>". As the original (a variant's falls back to its base's art);
// then a variant's own portrait where its mod adds one (fe/screens/images/logos_TEAM_<name>).
static void MatchLoadingSetTeamLogo(CpuContext* ctx)
{
    const uint32_t scene = ctx->gpr[3];
    const int side = static_cast<int>(ctx->gpr[4]);
    const std::string name = MscGuest::CString(Memory::Read32(ctx->gpr[5] + 0x04));  // character.mName
    func_801CF6E8(ctx);
    if (!FindCharacterNamed(name)) return;
    const char* const path[] = {"Slide1", "Layer", side == 0 ? "logos_TEAM_LUIGI" : "logos_TEAM_MARIO"};
    const uint32_t image = MscGuest::FE::FindInPresentation(Memory::Read32(scene + 0x14), path, 3);  // mPresentation
    if (image == 0) return;
    const uint32_t texture = FeTexture(ctx, Memory::Read32(image + MscGuest::FE::kImageTexture), "fe/screens/images/logos_TEAM_" + name);
    if (texture) Memory::Write32(image + MscGuest::FE::kImageTexture, texture);
}
PPC_NATIVE_WRAP(801CF6E8, MatchLoadingSetTeamLogo);

// --- Team textures --------------------------------------------------------------------------------
// A captain's ExtraTextures.rlt holds its team's logo, banners and Mega Strike art, named after it
// ("<name>/<name>_logo", "<name>/mega_hand", ...). A variant whose mod has none loads its base's
// (the ExtraTextures override in msc_game.cpp); here, before they're registered, they're renamed to
// the variant's names so the lookups by its name find them (and its base's own, if it plays too,
// stay its base's).
namespace {
// The base's names whose textures were loaded under a variant's (hash of "<base>/<texture>" ->
// "<variant>/<texture>"): the base's effects still ask by the base's (glx_GetTex below).
std::unordered_map<uint32_t, uint32_t> g_renamedTextures;
} // namespace

static void EndExtraTextures(CpuContext* ctx)
{
    constexpr uint32_t kLoader = 0x8056B290u, kCurrent = 0xCC, kExtraData = 0x110;
    const uint32_t entry = Memory::Read32(kLoader + kCurrent);
    const uint32_t data = Memory::Read32(kLoader + kExtraData);
    if (entry != 0 && data != 0 && Memory::Read8(entry + 0x11) == 0) {  // not a goalie
        const int side = static_cast<int>(Memory::Read32(entry) & 1);
        const int player = static_cast<int>(Memory::Read32(entry + 0x08));
        const int cc = static_cast<int>(Memory::Read32(entry + 0x0C));
        const Variant* v = ForSlot(ctx, side, player, cc);
        if (v && !DVDPathExistsForRuntime(("art/characters/" + v->def->name + "/ExtraTextures.rlt").c_str()) &&
            Memory::Read32(data) == 0x50544C47u) {  // 'PTLG'
            const std::string b = BaseCharacterName(v->def->baseIndex), n = v->def->name;
            static const char* const kSuffixes[] = {"%s_logo", "%s_banners", "%s_banners_alt", "mega_gameplay_bg",
                                                    "mega_cone_colour", "mega_hand", "mega_hand1", "mega_hand_alt",
                                                    "mega_hand1_alt", "%s_eye1", "%s_eye2"};
            std::unordered_map<uint32_t, uint32_t> rename;
            for (const char* suffix : kSuffixes) {
                char mine[96], theirs[96];
                std::snprintf(theirs, sizeof(theirs), suffix, b.c_str());
                std::snprintf(mine, sizeof(mine), suffix, n.c_str());
                rename.emplace(MscGuest::StringHash(b + "/" + theirs), MscGuest::StringHash(n + "/" + mine));
            }
            const uint32_t count = Memory::Read32(data + 4);
            for (uint32_t i = 0; i < count && i < 256; ++i) {
                const uint32_t at = data + 0x10 + i * 16;
                if (const auto it = rename.find(Memory::Read32(at)); it != rename.end()) {
                    Memory::Write32(at, it->second);
                    g_renamedTextures[it->first] = it->second;  // the base's own name still finds it
                }
            }
        }
    }
    func_8000A790(ctx);
}
PPC_NATIVE_WRAP(8000A790, EndExtraTextures);

// --- Effects --------------------------------------------------------------------------------------
// PlatTexture* glx_GetTex(ulong handle): a texture by name hash, from every resource pool. As the
// original; a base texture a variant loaded under its own name (EndExtraTextures) is found by the
// base's name too, so the base's effects (Mega Strike hands, cones, backgrounds) keep their textures.
extern "C" void func_802D064C(CpuContext* ctx);
static void GetTextureOrRenamed(CpuContext* ctx)
{
    const uint32_t handle = ctx->gpr[3];
    func_802D064C(ctx);
    if (ctx->gpr[3] != 0 || g_renamedTextures.empty()) return;
    if (const auto it = g_renamedTextures.find(handle); it != g_renamedTextures.end()) {
        ctx->gpr[3] = it->second;
        func_802D064C(ctx);
    }
}
PPC_NATIVE_WRAP(802D064C, GetTextureOrRenamed);

// EffectsGroup* EmissionManager::GetEffectsGroup(const char* name). As the original; an effect named
// after a mod character ("superteam_megastrike_home_3_gameplay", "superteam/dirt", ...) that no effects
// bundle has is its base's ("waluigi_..."), which its base's bundle (loaded for it) does.
extern "C" void func_802E7CDC(CpuContext* ctx);
static void GetEffectsGroupOrBase(CpuContext* ctx)
{
    const uint32_t manager = ctx->gpr[3], nameAt = ctx->gpr[4];
    func_802E7CDC(ctx);
    if (ctx->gpr[3] != 0 || nameAt == 0) return;
    const std::string name = MscGuest::CString(nameAt, 127);
    const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    for (const Package* package : ActivePackages()) {
        for (const CharacterDef& c : package->characters) {
            const char* base = BaseCharacterName(c.baseIndex);
            size_t at = name.find(c.name);
            while (at != std::string::npos && ((at > 0 && word(name[at - 1])) ||
                                               (at + c.name.size() < name.size() && word(name[at + c.name.size()]))))
                at = name.find(c.name, at + 1);
            if (base == nullptr || at == std::string::npos) continue;
            const std::string theirs = name.substr(0, at) + base + name.substr(at + c.name.size());
            // The name in a stack frame of ours for the call.
            const uint32_t sp = ctx->gpr[1], frame = sp - 0x110;
            Memory::Write32(frame, sp);
            for (size_t i = 0; i <= theirs.size() && i < 0x100; ++i)
                Memory::Write8(frame + 8 + static_cast<uint32_t>(i), i < theirs.size() ? static_cast<uint8_t>(theirs[i]) : 0);
            const uint32_t savedLr = ctx->lr;
            ctx->gpr[1] = frame;
            ctx->gpr[3] = manager;
            ctx->gpr[4] = frame + 8;
            func_802E7CDC(ctx);
            ctx->gpr[1] = sp;
            ctx->lr = savedLr;
            static std::set<std::string> s_reported;
            if (s_reported.insert(name).second)
                RT_LOG(RT_TAG_MODS) << "effect " << name << ": not in any loaded bundle, " << theirs
                                    << (ctx->gpr[3] != 0 ? " used" : " missing too") << std::endl;
            return;
        }
    }
}
PPC_NATIVE_WRAP(802E7CDC, GetEffectsGroupOrBase);

// EmissionController* fn_802E7DC4(EmissionManager*, const char* name, int view, bool addToEnd, ushort id):
// starts an effect by name (cutscene triggers). As the original; an effect no loaded bundle has is
// reported once (a mod's cutscene naming effects it didn't load).
extern "C" void func_802E7DC4(CpuContext* ctx);
static void CreateEffectReported(CpuContext* ctx)
{
    const uint32_t nameAt = ctx->gpr[4];
    func_802E7DC4(ctx);
    if (ctx->gpr[3] != 0) return;
    static std::set<std::string> s_reported;
    if (const std::string name = MscGuest::CString(nameAt, 64); s_reported.insert(name).second)
        RT_LOG(RT_TAG_MODS) << "effect " << name << ": not in any loaded bundle" << std::endl;
}
PPC_NATIVE_WRAP(802E7DC4, CreateEffectReported);
