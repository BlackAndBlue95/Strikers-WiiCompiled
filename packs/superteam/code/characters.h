// Mod characters: characters a pack adds, each a team and character of its own after the game's 12
// captains (slots.cpp), built on one of them (its base: the skeleton and animations its model uses,
// and the cutscenes it has none of). The framework (framework.cpp) puts them on pages of their own on
// captain select. A pack lists its characters in kModCharacters.
#ifndef MOD_CHARACTERS_H
#define MOD_CHARACTERS_H

// A texture the game asks for by the character's name ("fe/screens/images/logos_TEAM_superteam"),
// which its bundles don't have: a .gxt in the pack.
struct ModTexture {
    const char* game;  // e.g. "fe/screens/images/attributes_waluigi"
    const char* file;  // e.g. "/mods/superteam/attributes_superteam.gxt"
};

// A trigger in one of its cutscenes, as the game's trigger scripts add them (Nis::AddTrigger): for its
// cutscenes the game's scripts don't know. At a frame of the cutscene (30 a second).
const int kNisEffect = 0, kNisTimeDilation = 1, kNisDepthOfFieldOff = 8;  // NisTriggerType
struct ModNisTrigger {
    const char* cutscene;  // its name without ".nis"; '*' stands for any text ("superteam_megastrike_*_2")
    float frame;
    int type;
    const char* effect;  // kNisEffect: an effects group
    const char* target;  // kNisEffect: where it plays: "bip01" (the character), "ball", a prop of the cutscene
    float value;         // kNisTimeDilation: the game's speed from then on
};

struct ModCharacter {
    // Internal name, e.g. "superteam". Its files are named after it.
    const char* name;
    // The captain it's built on: CharacterInfo row 0-11 (mario 0, bowser 1, daisy 2, dk 3, luigi 4,
    // peach 5, waluigi 6, wario 7, yoshi 8, bowserjr 9, diddykong 10, petey 11). Its row, template
    // and goalie kit start as the base's.
    int base;
    // Disc paths in its character template; 0 keeps the base's.
    const char* model;          // .rlg (its model id must be nlStringHash("<name>/<name>"))
    const char* textures;       // .rlt
    const char* animations;     // .sanim.zlib
    const char* stats;          // /ini/characters/<x>.ini
    const char* statsGameplay;  // /Game/Gameplay/ini/chars/<x>
    // Captain select portraits (.gxt): lit, and greyed (taken by the other team). 0 uses the base's.
    const char* portrait;
    const char* portraitTaken;
    // Its name as the game shows it (UTF-16, as the game's strings), under a string key of its own.
    const unsigned short* displayName;
    const char* nameKey;
    // Team colour (0xRRGGBB, 0: the base's), captain select's four stat bars (all 0: the base's), role
    // (0 offensive, 1 defensive, 2 playmaker, 3 power, 4 balanced; -1: the base's), and whether it
    // has no super ability.
    unsigned long colour;
    float statsBars[4];
    int role;
    bool noAbility;
    // Goalie kit: its name and texture bundle (0: the base's).
    const char* goalieKit;
    const char* goalieTextures;
    // Voice: a sound bank, audio/<name>.resbun and .nlxwb (0: the base's).
    const char* voiceBank;
    // Its textures the game asks for by its name (HUD icon, logos, menu art, the Striker Times'
    // photos). Several can be one file.
    const ModTexture* ownTextures;
    int ownTextureCount;
    // Its team is always itself, three times over, whatever partner select picked (SMS's Super Team:
    // three robots): captains in the partner slots, as the game's own captain-only teams switch has them.
    bool fixedTeam;
    // An effects bundle of its own (art/effects/<x>.bun, and <x>nonres.bun.zlib beside it if there's
    // one), loaded with it for every match: its cutscenes' effects. 0: none.
    const char* extraEffects;
    // Its cutscenes are "<name>_<kind>..." in the cutscene dictionary (Art/nis/nis_dict.txt); a kind
    // it has none of plays its base's. Each runs the trigger script named after it, or another's:
    // pairs of {cutscene, script it runs}.
    const char* const (*triggerAliases)[2];
    int triggerAliasCount;
    // Its deke: the generic directional one (Mario's, the game's default; SMS's spin) instead of its
    // base's.
    bool defaultDeke;
    // Triggers its cutscenes add: effects and time dilation in cutscenes of its own the game's trigger
    // scripts don't know (its Mega Strike).
    const ModNisTrigger* nisTriggers;
    int nisTriggerCount;
};

extern const ModCharacter kModCharacters[];
extern const int kModCharacterCount;

#endif
