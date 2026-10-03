// The mod characters' own slots, after the game's: character i is team 12 + i, character class
// kFirstModClass + i and goalie class kFirstModGoalie + i, with a CharacterInfo row, a character
// template and a goalie kit of its own. The game finds all of these through a handful of lookups,
// which this extends; its own tables stay as they are. A character is built on a captain (its base:
// the skeleton, animations and cutscenes it shares), whose row and template it starts from.
#include <kamek.h>

#include "Game/CharacterTemplate.h"
#include "Game/FE/feCaptainComponent.h"
#include "NL/nlMath.h"
#include "NL/nlMemory.h"
#include "NL/nlString.h"

#include "framework.h"

extern "C" void* memcpy(void* dst, const void* src, unsigned long n);
extern "C" int stricmp(const char* a, const char* b);
extern unsigned char sCharacterInfo[];  // CharacterInfo[33], 0x5C each

namespace {

template <typename T> inline T& At(const void* base, unsigned long offset) {
    return *(T*)((char*)base + offset);
}

const unsigned long kRowSize = 0x5C;
const unsigned long kRowIndex = 0x00, kRowName = 0x04, kRowNameKey = 0x08, kRowCaptainId = 0x10, kRowCaptainItem = 0x14,
                    kRowVoiceBank = 0x1C, kRowStatsBars = 0x38, kRowRole = 0x48, kRowColourMask = 0x4C,
                    kRowColour = 0x54, kRowAltColour = 0x58;
const int kMaxCharacters = 4;

unsigned char s_rows[kMaxCharacters][kRowSize];
unsigned char s_goalieRows[kMaxCharacters][kRowSize];
tCharacterTemplateInfo s_info[kMaxCharacters];
tGoalieTemplateInfo s_goalieInfo[kMaxCharacters];
tCharacterTemplate* s_templates[kMaxCharacters];
int s_rules[kMaxCharacters][3];  // its partners, as GameInfoManager keeps the captains'
bool s_built;

}  // namespace

// The class whose row of the front end's per-captain hologram sizes (lbl_80515D18[12]) a class uses:
// its own for the captains, its base's for a mod character.
unsigned char s_hologramClass[64] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                     16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
                                     32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
                                     48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63};

namespace {

unsigned char* GameRow(int index) {
    return sCharacterInfo + (index < 0 || index >= 32 ? 32 : index) * kRowSize;
}
tCharacterTemplateInfo* GameTemplateInfo(int cc) {
    return cc < 20 ? (tCharacterTemplateInfo*)0x804F7F18 + cc : (tCharacterTemplateInfo*)0x804F8808;
}
tGoalieTemplateInfo* GameGoalieInfo(int index) {
    return (tGoalieTemplateInfo*)0x804F8DA0 + index;  // g_GoalieTextureInfo[12]
}
const int* DefaultRules(int team) {
    return (const int*)0x804DC558 + team * 3;  // kDefaultRules[12][3]
}

void Build() {
    if (s_built) return;
    s_built = true;
    for (int c = 0; c < kModCharacterCount && c < kMaxCharacters; ++c) {
        const ModCharacter& m = kModCharacters[c];
        unsigned char* row = s_rows[c];
        memcpy(row, GameRow(m.base), kRowSize);
        At<int>(row, kRowIndex) = kFirstModClass + c;
        At<const char*>(row, kRowName) = m.name;
        At<const char*>(row, kRowNameKey) = m.nameKey;
        At<int>(row, kRowCaptainId) = kFirstModTeam + c;
        if (m.noAbility) At<int>(row, kRowCaptainItem) = -1;
        if (m.voiceBank) At<int>(row, kRowVoiceBank) = kFirstModBank + c;
        if (m.statsBars[0] || m.statsBars[1] || m.statsBars[2] || m.statsBars[3])
            for (int i = 0; i < 4; ++i) At<float>(row, kRowStatsBars + i * 4) = m.statsBars[i];
        if (m.role >= 0) At<int>(row, kRowRole) = m.role;
        if (m.colour) {
            At<unsigned long>(row, kRowColour) = m.colour;
            At<unsigned long>(row, kRowAltColour) = m.colour;
            At<unsigned long>(row, kRowColourMask) = 0;  // clashes with nobody's
        }

        unsigned char* goalie = s_goalieRows[c];
        memcpy(goalie, GameRow(m.base + 20), kRowSize);
        At<int>(goalie, kRowIndex) = kFirstModGoalie + c;
        if (m.goalieKit) At<const char*>(goalie, kRowName) = m.goalieKit;  // its kit's textures are named after it
        if (m.colour) {
            At<unsigned long>(goalie, kRowColour) = m.colour;
            At<unsigned long>(goalie, kRowAltColour) = m.colour;
            At<unsigned long>(goalie, kRowColourMask) = 0;
        }

        tCharacterTemplateInfo& info = s_info[c];
        memcpy(&info, GameTemplateInfo(m.base), sizeof(info));
        info.mUnidentified00 = (eCharacterClass)(kFirstModClass + c);
        if (m.model) info.szModelFilename = m.model;
        if (m.textures) info.szTextureFilename = info.pUnidentified18 = m.textures;  // no away kit
        if (m.animations) info.szAnimFilename = m.animations;
        if (m.stats) info.szTweaksFilename = m.stats;
        if (m.statsGameplay) info.pUnidentified48 = m.statsGameplay;
        info.bUnidentified58 = 0;

        tGoalieTemplateInfo& kit = s_goalieInfo[c];
        memcpy(&kit, GameGoalieInfo(m.base), sizeof(kit));
        if (m.goalieKit && m.goalieTextures) {
            kit.szCharName = m.goalieKit;
            kit.szTextureFilename = kit.pUnidentified08 = m.goalieTextures;
        }
        kit.bLoaded = 0;

        for (int i = 0; i < 3; ++i) s_rules[c][i] = DefaultRules(m.base)[i];
        s_hologramClass[kFirstModClass + c] = (unsigned char)m.base;
    }
}

int Count() {
    return kModCharacterCount < kMaxCharacters ? kModCharacterCount : kMaxCharacters;
}

}  // namespace

int ModCharacterOfTeam(int team) {
    const int c = team - kFirstModTeam;
    return c >= 0 && c < Count() ? c : -1;
}

int ModCharacterOfClass(int cc) {
    const int c = cc - kFirstModClass;
    return c >= 0 && c < Count() ? c : -1;
}

const unsigned char* ModCharacterRow(int c) {
    Build();
    return s_rows[c];
}

// ---- the lookups ----

// const CharacterInfo& GetCharacterInfo(int index)
static const unsigned char* GetCharacterInfo_(int index) {
    Build();
    if (ModCharacterOfClass(index) >= 0) return s_rows[index - kFirstModClass];
    const int g = index - kFirstModGoalie;
    if (g >= 0 && g < Count()) return s_goalieRows[g];
    return GameRow(index);
}
kmBranch(0x800FBD60, GetCharacterInfo_);

// int GetCharacterIndexFromCaptain(int captain)
static int GetCharacterIndexFromCaptain_(int captain) {
    if (ModCharacterOfTeam(captain) >= 0) return kFirstModClass + (captain - kFirstModTeam);
    for (int i = 0; i < 32; ++i)
        if (At<int>(GameRow(i), kRowCaptainId) == captain) return At<int>(GameRow(i), kRowIndex);
    return -1;
}
kmBranch(0x800FBD94, GetCharacterIndexFromCaptain_);

// int GetCharacterIndexFromName(const char* name)
static int GetCharacterIndexFromName_(const char* name) {
    for (int c = 0; c < Count(); ++c)
        if (name && stricmp(name, kModCharacters[c].name) == 0) return kFirstModClass + c;
    for (int i = 0; i < 32; ++i)
        if (name && stricmp(name, At<const char*>(GameRow(i), kRowName)) == 0) return At<int>(GameRow(i), kRowIndex);
    return -1;
}
kmBranch(0x800FBE24, GetCharacterIndexFromName_);

// int GetGoalieCharacterIndex(const CharacterInfo& info)
static int GetGoalieCharacterIndex_(const unsigned char* info) {
    const int index = At<int>(info, kRowIndex);
    if (index >= 0 && index < 12) return index + 20;
    if (ModCharacterOfClass(index) >= 0) return kFirstModGoalie + (index - kFirstModClass);
    return -1;
}
kmBranch(0x800FBED0, GetGoalieCharacterIndex_);

// tCharacterTemplateInfo* GetCharacterTemplateInfo(eCharacterClass cc)
static tCharacterTemplateInfo* GetCharacterTemplateInfo_(int cc) {
    Build();
    if (ModCharacterOfClass(cc) >= 0) return &s_info[cc - kFirstModClass];
    return GameTemplateInfo(cc);
}
kmBranch(0x8002600C, GetCharacterTemplateInfo_);

// tCharacterTemplate* GetCharacterTemplate(int index, bool* created)
static tCharacterTemplate* GetCharacterTemplate_(int index, bool* created) {
    *created = false;
    tCharacterTemplate** slot;
    if (ModCharacterOfClass(index) >= 0) slot = &s_templates[index - kFirstModClass];
    else if (index < 20) slot = (tCharacterTemplate**)0x8056B828 + index;  // g_aCharacterTemplates[20]
    else slot = (tCharacterTemplate**)0x806E0C38;                           // g_GoalieTemplate
    if (*slot == 0) {
        *slot = (tCharacterTemplate*)nlMalloc(sizeof(tCharacterTemplate), 8, false);
        *created = true;
    }
    return *slot;
}
kmBranch(0x80025F5C, GetCharacterTemplate_);

// tGoalieTemplateInfo* GetGoalieTemplateInfo(int goalie): goalie class - 20.
static tGoalieTemplateInfo* GetGoalieTemplateInfo_(int index) {
    Build();
    const int c = index - (kFirstModGoalie - 20);
    if (c >= 0 && c < Count()) return &s_goalieInfo[c];
    return GameGoalieInfo(index);
}
kmBranch(0x80025F48, GetGoalieTemplateInfo_);

// void DestroyCharacters(): the game frees its templates after a match. Ours go in its table's free
// places for it to free with its own.
asm static void DestroyCharacters_Original() {
    nofralloc
    stwu r1, -224(r1)
    lis r12, 0x8002
    ori r12, r12, 0x6374
    mtctr r12
    bctr
}

static void DestroyCharacters_() {
    tCharacterTemplate** table = (tCharacterTemplate**)0x8056B828;
    for (int c = 0; c < Count(); ++c) {
        if (s_templates[c] == 0) continue;
        for (int i = 0; i < 20; ++i)
            if (table[i] == 0) { table[i] = s_templates[c]; s_templates[c] = 0; break; }
    }
    ((void (*)())DestroyCharacters_Original)();
    for (int c = 0; c < Count(); ++c) {
        s_info[c].bUnidentified58 = 0;
        s_goalieInfo[c].bLoaded = 0;
    }
}
kmBranch(0x80026370, DestroyCharacters_);

// ---- the team: its partners and its sound ----

// eTeamID's accept sound (FECharacterSound::GetCaptainAcceptSound): none for ours (its pick voice
// plays instead).
static unsigned long GetCaptainAcceptSound_(int team) {
    return team >= 0 && team < 12 ? ((const unsigned long*)0x804E8318)[team] : 0;
}
kmBranch(0x801CC090, GetCaptainAcceptSound_);

namespace {

int* RulesOf(void* info, int team) {
    const int c = ModCharacterOfTeam(team);
    if (c >= 0) return s_rules[c];
    if (team < 0 || team >= 12) team = 0;
    return &At<int>(info, 0x298 + team * 0xC);  // GameInfoManager::mRulesTable[12]
}

void* GameInfo() {
    return *(void**)0x806E0F54;
}

int TeamOfBoard(FECaptainComponent* board) {
    void* info = GameInfo();
    if (At<int>(info, 0x11C) == 3) return At<int>(*(void**)0x806E0F90, 0x8A28);  // cup: its pending team
    return At<int>(At<void*>(info, 0x80 + At<int>(info, 0x11C) * 4), board->mSide * 4);  // GetTeam(side)
}

}  // namespace

// void GameInfoManager::ResetRules(int team)
static void ResetRules_(void* info, int team) {
    Build();
    const int c = ModCharacterOfTeam(team);
    const int* defaults = DefaultRules(c >= 0 ? kModCharacters[c].base : (team >= 0 && team < 12 ? team : 0));
    int* rules = RulesOf(info, team);
    for (int i = 0; i < 3; ++i) rules[i] = defaults[i];
}
kmBranch(0x800FDD24, ResetRules_);

// void FECaptainComponent::ReloadSidekicks()
static void ReloadSidekicks_(FECaptainComponent* board) {
    Build();
    const int* rules = RulesOf(GameInfo(), TeamOfBoard(board));
    for (int i = 0; i < 3; ++i) board->mSidekicks[i] = rules[i];
}
kmBranch(0x801DCCEC, ReloadSidekicks_);

// void FECaptainComponent::RandomizeSidekicks(): for our teams, into their own rules.
asm static void RandomizeSidekicks_Original() {
    nofralloc
    stwu r1, -96(r1)
    lis r12, 0x801D
    ori r12, r12, 0xCB2C
    mtctr r12
    bctr
}

static void RandomizeSidekicks_(FECaptainComponent* board) {
    const int c = ModCharacterOfTeam(board->mCaptain);
    if (c < 0) {
        ((void (*)(FECaptainComponent*))RandomizeSidekicks_Original)(board);
        return;
    }
    for (int i = 0; i < 3; ++i) board->mSidekicks[i] = s_rules[c][i] = nlRandom(8, &nlDefaultSeed);
}
kmBranch(0x801DCB28, RandomizeSidekicks_);

// void ChooseSidekicksSceneV2::OnSidekickPointerPress(int pad, void* button): its last act is to
// write the pick into the side's rules. For our teams, a captain's rules take the write and give it
// back.
asm static void OnSidekickPointerPress_Original() {
    nofralloc
    stwu r1, -48(r1)
    lis r12, 0x8022
    ori r12, r12, 0xAB6C
    mtctr r12
    bctr
}

static void OnSidekickPointerPress_(void* scene, int pad, void* button) {
    int side = -1;
    for (int s = 1; s >= 0; --s)
        if (At<int>(scene, 0x20 + s * 4) == pad) side = s;  // mSidePads
    int* team = side >= 0 ? &At<int>(scene, 0x40 + side * 4) : 0;  // mTeams
    const int c = team ? ModCharacterOfTeam(*team) : -1;
    if (c < 0) {
        ((void (*)(void*, int, void*))OnSidekickPointerPress_Original)(scene, pad, button);
        return;
    }
    int* proxy = RulesOf(GameInfo(), 0);
    int saved[3] = {proxy[0], proxy[1], proxy[2]};
    proxy[0] = s_rules[c][0], proxy[1] = s_rules[c][1], proxy[2] = s_rules[c][2];
    const int own = *team;
    *team = 0;
    ((void (*)(void*, int, void*))OnSidekickPointerPress_Original)(scene, pad, button);
    *team = own;
    for (int i = 0; i < 3; ++i) s_rules[c][i] = proxy[i], proxy[i] = saved[i];
}
kmBranch(0x8022AB68, OnSidekickPointerPress_);

// ---- the front end's holograms ----
// FEModelManager sizes a captain's hologram from a 12-row table, by the class in its template
// (row = class * 20): a mod character's class takes its base's row. Each "mulli rX, rX, 20" there
// becomes a call to one of these, which does it with the class mapped (touching only rX).
asm static void HologramRow_r0() {
    nofralloc
    stwu r1, -16(r1)
    stw r12, 8(r1)
    lis r12, s_hologramClass@ha
    addi r12, r12, s_hologramClass@l
    lbzx r0, r12, r0
    mulli r0, r0, 20
    lwz r12, 8(r1)
    addi r1, r1, 16
    blr
}

asm static void HologramRow_r4() {
    nofralloc
    stwu r1, -16(r1)
    stw r12, 8(r1)
    lis r12, s_hologramClass@ha
    addi r12, r12, s_hologramClass@l
    lbzx r4, r12, r4
    mulli r4, r4, 20
    lwz r12, 8(r1)
    addi r1, r1, 16
    blr
}

kmCall(0x801C12A0, HologramRow_r0);
kmCall(0x801C12D4, HologramRow_r0);
kmCall(0x801C2490, HologramRow_r4);
kmCall(0x801C256C, HologramRow_r0);
