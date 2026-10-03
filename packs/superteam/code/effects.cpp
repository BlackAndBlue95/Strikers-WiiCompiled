// The character's own effects bundle (its cutscenes' effects): loaded with its captain, once a
// match, alongside the effects the loader loads for every character, and added to the effects system
// when those are. Effects the game names after the character are its own, or its base's.
#include <kamek.h>

#include "Game/CharacterLoader.h"
#include "NL/nlFile.h"

#include "framework.h"

bool nlFileExists(const char* path);
bool nlLoadCompressedFileAsync(const char* path, LoadAsyncCallback callback, void* param, unsigned int alignment,
                               eAllocType type, unsigned int chunk, void* buffer0, void* buffer1, void* p,
                               unsigned long size, MemoryAllocator* allocator);
class GLResourcePool;
GLResourcePool* glGetCurrentResourcePool();
class EffectsGroup;
class EmissionManager {
public:
    static void LoadBundle(void* data, void* nonres, GLResourcePool* pool, int type);
};

namespace {

void* s_data[2];  // the bundle and its non-resident half, as the loads finish
bool s_loading[2];
bool s_pending;   // loads started for this match, not yet added
bool s_thisMatch;
char s_nonresPath[128];

void Store(void* data, unsigned long, void* param) {
    *(void**)param = data;
}

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

}  // namespace

asm static void StartEffects_Original() {
    nofralloc
    stwu r1, -176(r1)
    lis r12, 0x8000
    ori r12, r12, 0xA41C
    mtctr r12
    bctr
}

// void CharacterLoader_8056B290::fn_8000A418(): starts loading the current character's effects.
static void StartEffects(CharacterLoader_8056B290* loader) {
    ((void (*)(CharacterLoader_8056B290*))StartEffects_Original)(loader);
    if (loader->mCurrentIndex == 0) s_thisMatch = false;  // a match's first character
    CharacterLoader_8056B290::Entry* entry = loader->mCurrent;
    const int c = entry ? ModCharacterOfClass((int)entry->cc) : -1;
    if (c < 0 || entry->bGoalie || entry->nPlayerID != 0) return;
    const char* path = kModCharacters[c].extraEffects;
    if (path == 0 || s_thisMatch || !nlFileExists(path)) return;
    s_thisMatch = true;
    s_data[0] = s_data[1] = 0;
    s_loading[0] = nlLoadEntireFileAsync(path, Store, &s_data[0], 0x20, AllocateStart, 0, 0, 0) != 0;
    // Its non-resident half: "<x>nonres.bun.zlib" for "<x>.bun".
    const int length = strlen(path);
    s_loading[1] = false;
    if (length > 4 && length + 12 < (int)sizeof(s_nonresPath)) {
        for (int i = 0; i < length - 4; ++i) s_nonresPath[i] = path[i];
        const char* tail = "nonres.bun.zlib";
        int at = length - 4;
        while (*tail) s_nonresPath[at++] = *tail++;
        s_nonresPath[at] = 0;
        if (nlFileExists(s_nonresPath))
            s_loading[1] = nlLoadCompressedFileAsync(s_nonresPath, Store, &s_data[1], 0x20, AllocateEnd, 0x20000, 0, 0, 0, 0, 0);
    }
    s_pending = true;
}
kmBranch(0x8000A418, StartEffects);

asm static void FinishEffects_Original() {
    nofralloc
    stwu r1, -16(r1)
    lis r12, 0x8000
    ori r12, r12, 0xA5DC
    mtctr r12
    bctr
}

// bool CharacterLoader_8056B290::fn_8000A5D8(): true once the current character's effects are in
// (and added). Not before the character's own are, which are added with them.
static bool FinishEffects(CharacterLoader_8056B290* loader) {
    if (s_pending && ((s_loading[0] && s_data[0] == 0) || (s_loading[1] && s_data[1] == 0))) return false;
    const bool done = ((bool (*)(CharacterLoader_8056B290*))FinishEffects_Original)(loader);
    if (!done || !s_pending) return done;
    s_pending = false;
    if (s_data[0]) EmissionManager::LoadBundle(s_data[0], s_data[1], glGetCurrentResourcePool(), 1);
    return true;
}
kmBranch(0x8000A5D8, FinishEffects);

asm static void GetEffectsGroup_Original() {
    nofralloc
    stwu r1, -16(r1)
    lis r12, 0x802E
    ori r12, r12, 0x7CE0
    mtctr r12
    bctr
}

// EffectsGroup* EmissionManager::GetEffectsGroup(const char* name). The game names some effects
// after the character playing them, by its internal name ("<name>_megastrike_home_3_gameplay", the
// ball's after a Mega Strike, which it starts without checking it exists): a mod character's own if
// its effects have them, else its base's.
static EffectsGroup* GetEffectsGroup(EmissionManager* manager, const char* name) {
    typedef EffectsGroup* (*Lookup)(EmissionManager*, const char*);
    EffectsGroup* group = ((Lookup)GetEffectsGroup_Original)(manager, name);
    if (group != 0 || name == 0) return group;
    for (int c = 0; c < kModCharacterCount; ++c) {
        const char* own = kModCharacters[c].name;
        int n = 0;
        while (own[n] && Lower(name[n]) == Lower(own[n])) ++n;  // effect names hash in lower case
        if (own[n] != 0 || name[n] != '_') continue;
        char baseName[128];
        const char* base = BaseName(c);
        int at = 0;
        while (*base && at < (int)sizeof(baseName) - 1) baseName[at++] = *base++;
        for (const char* rest = name + n; *rest && at < (int)sizeof(baseName) - 1; ++rest) baseName[at++] = *rest;
        baseName[at] = 0;
        return ((Lookup)GetEffectsGroup_Original)(manager, baseName);
    }
    return 0;
}
kmBranch(0x802E7CDC, GetEffectsGroup);

asm static void BallLaunch_Original() {
    nofralloc
    stwu r1, -160(r1)
    lis r12, 0x8001
    ori r12, r12, 0xAD28
    mtctr r12
    bctr
}

// void fn_8001AD24(LiveBallTrail*, cFielder*): a Mega Strike's ball as it leaves the cutscene. Its
// streak is picked by the shooter's character class, the 12 captains' and no other (for any other the
// texture's name is whatever was on the stack): a mod character's is its base's.
static void BallLaunch(void* trail, unsigned char* fielder) {
    typedef void (*Launch)(void*, unsigned char*);
    int& characterClass = *(int*)(fielder + 0x24);  // cCharacter::mUnidentified024.m_eCharacterClass
    const int c = ModCharacterOfClass(characterClass);
    if (c < 0) {
        ((Launch)BallLaunch_Original)(trail, fielder);
        return;
    }
    const int own = characterClass;
    characterClass = kModCharacters[c].base;  // a captain's class is its CharacterInfo row
    ((Launch)BallLaunch_Original)(trail, fielder);
    characterClass = own;
}
kmBranch(0x8001AD24, BallLaunch);
