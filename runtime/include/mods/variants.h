// Mod characters in matches: variants of a base character (docs/modding/README.md, "Characters").
//
// A variant keeps its base's class and team id, so everything that plays (skeleton, animations,
// physics, ability, AI) is the base's. What it looks and sounds like comes from its own name: the
// runtime gives it a CharacterInfo row of its own (name, display name, colours, voice bank), its own
// character template (model, textures, effects, stats; each falling back to the base's file when the
// mod doesn't ship one) and its own goalie kit. While the character loader works on one of the
// variant's players, the game's lookups (GetCharacterTemplate, GetCharacterTemplateInfo,
// GetGoalieTemplateInfo, GetCharacterInfo) answer with the variant's; once the player exists, its
// CharacterInfo pointer (cCharacter +0x11C) is the variant's row, so every later name lookup for it
// does too. A variant and its base can be on one team or face each other.
#pragma once

#include <cstdint>
#include <string>

struct CpuContext;

namespace Mods {
struct CharacterDef;
struct Package;

namespace Variants {

struct Variant {
    const CharacterDef* def = nullptr;
    const Package* package = nullptr;
    uint32_t row = 0;             // CharacterInfo (0x5C)
    uint32_t templateInfo = 0;    // tCharacterTemplateInfo (0x5C), its loaded flag reset per match
    uint32_t templateObject = 0;  // tCharacterTemplate* for the current match (0 = not created)
    uint32_t goalieInfo = 0;      // tGoalieTemplateInfo (0x10), 0 without a goalie kit
    uint32_t goalieRow = 0;       // CharacterInfo of its goalie (name = the kit)
    uint32_t bankRecord = 0;      // AudioResourceName {0, name} of its voice bank, 0 without one
    int bankIndex = -1;           // the virtual bank index its row carries (kFirstVirtualBank + n)
};

constexpr int kFirstVirtualBank = 0x1000;

// The variant playing `player` (0 captain, 1-3 teammates, 4 goalie) for `side`, given the class the
// loader put in that slot; nullptr for the base character. Guest data is built on first use.
const Variant* ForSlot(CpuContext* ctx, int side, int player, int characterClass);

// The variant of a mod character (built on first use), or nullptr if it isn't a valid variant.
const Variant* ForCharacter(CpuContext* ctx, const CharacterDef* def);

// Captain select: the mod character a side's stats panel shows (hovered or picked), which the
// side's pseudo index stands for there until the pick is made; nullptr to clear.
void SetMenuCharacter(int side, const CharacterDef* def);
const CharacterDef* MenuCharacter(int side);

// The variant whose template info is `templateInfo`, or nullptr.
const Variant* ForTemplateInfo(uint32_t templateInfo);

// The variant whose row (or goalie row) is `characterInfo`, or nullptr.
const Variant* ForRow(uint32_t characterInfo);

// The variant carrying a virtual voice bank index, or nullptr.
const Variant* ForBank(int bankIndex);

// The character the player picked for a side on captain select's mod pages (captain kind), or nullptr:
// Config.toml's [mod_selection] home / away, which captain select keeps.
const CharacterDef* SelectedCaptain(int side);

// A side led by a fixed-team character (CharacterDef::fixedTeam; the side's team is its base's), or
// nullptr. Its players 1-3 are its teammates: TeammateClass gives the character class each one's
// loader entry takes (a mod character's base; ForSlot then makes it the mod character).
const CharacterDef* FixedTeamCaptain(int side);
int TeammateClass(const CharacterDef* captain, int player);

// Captain select: a mod captain picked says its own line, the voice-preview cue of its voice bank
// (0x270203ED, the cue the game previews a captain's voice with), loaded into the side's captain voice
// slot (1 home, 5 away). False when it has no voice bank: play its base's accept sound instead.
bool PlayMenuVoice(CpuContext* ctx, int side, const CharacterDef* def);
// Once a frame while captain select is up: plays a line once its bank has loaded.
void UpdateMenuVoices(CpuContext* ctx);
// Captain select gone: the banks PlayMenuVoice loaded are unloaded (the match loader expects its
// slots empty). Call every frame until it returns true (a bank still loading is unloaded once in).
bool ReleaseMenuVoices(CpuContext* ctx);

// Team identity (variant_identity.cpp). At the match's team-naming sites, GameInfoManager::GetTeam
// returns a tagged id for a side led by a variant; GetCharacterIndexFromCaptain turns it into a
// pseudo character index and GetCharacterInfo into the variant's row.
constexpr uint32_t kTeamTag = 0x100;            // tagged id: id | kTeamTag | side << 9
constexpr int kPseudoCharacterIndex = 0x60;      // + side: the variant leading that side
inline bool IsTaggedTeam(uint32_t id) { return (id & ~0x3FFu) == 0 && (id & kTeamTag) != 0; }
inline int TaggedSide(uint32_t id) { return static_cast<int>((id >> 9) & 1); }
inline uint32_t TagTeam(uint32_t id, int side) { return id | kTeamTag | (static_cast<uint32_t>(side & 1) << 9); }

// Guest memory kept for the session (the framework's reserved region), zeroed; 0 when it's used up.
uint32_t AllocPersistent(uint32_t size, uint32_t alignment = 8);

// The active mod character with this internal name, or nullptr.
const CharacterDef* FindCharacterNamed(const std::string& name);

// `name` with every mod character's name in it (as a whole word: between '_', '/', '.', or the
// ends) replaced by its base's. False when there was none.
bool ToBaseName(const std::string& name, std::string& base);

// Native code calling into the character loader's steps (outside its own code range) marks itself
// so the loader-time lookups still answer for the current entry.
class LoaderScope {
public:
    LoaderScope();
    ~LoaderScope();
    LoaderScope(const LoaderScope&) = delete;
    LoaderScope& operator=(const LoaderScope&) = delete;
};

}  // namespace Variants
}  // namespace Mods
