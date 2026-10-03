// Cutscenes. The character plays as its base, so a cutscene for its team or one of its players is
// picked by the base's name. Where the pack has the character's own of that kind ("<name>_<kind>..."
// in the cutscene dictionary), it's picked by the character's name instead; a kind it has none of
// plays the base's, whose animations fit (the character has the base's skeleton). Its cutscenes run
// the trigger scripts it names, and the triggers it lists.
#include <kamek.h>

#include "framework.h"

extern "C" int strcmp(const char* a, const char* b);
extern "C" unsigned long strlen(const char* s);

class NisPlayer;
class Nis;
enum NisTarget {};
enum NisUseStadiumOffset {};
enum NisUseFilter {};
enum NisWinnerType {};

namespace {

template <typename T> inline T& At(const void* base, unsigned long offset) {
    return *(T*)((char*)base + offset);
}

const unsigned long kDictCount = 0x30, kDict = 0x34, kHeaderSize = 0x1A0, kExtraNameFilter = 0x342B4;  // NisPlayer
const unsigned long kNisHeader = 0x28;  // Nis::mHeader, its name first

bool s_own = true;  // during a pick of the character's own cutscene

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool StartsWith(const char* text, const char* prefix) {
    for (; *prefix; ++text, ++prefix)
        if (Lower(*text) != Lower(*prefix)) return false;
    return true;
}

bool Contains(const char* text, const char* part) {
    for (; *text; ++text)
        if (StartsWith(text, part)) return true;
    return false;
}

// Whether the dictionary has a cutscene whose name starts with `prefix` (not a sidekick companion's).
bool InDictionary(const NisPlayer* player, const char* prefix) {
    const int count = At<int>(player, kDictCount);
    for (int i = 0; i < count && i < 512; ++i) {
        const char* name = (const char*)player + kDict + i * kHeaderSize;
        if (StartsWith(name, prefix) && !Contains(name, "_same") && !Contains(name, "_other")) return true;
    }
    return false;
}

// `name` ("x.nis" or "x") is `pattern`, where '*' stands for any text.
bool Matches(const char* pattern, const char* name) {
    if (*pattern == '*') {
        for (const char* at = name;; ++at) {
            if (Matches(pattern + 1, at)) return true;
            if (*at == 0) return false;
        }
    }
    if (*pattern == 0) return *name == 0 || (name[0] == '.' && Lower(name[1]) == 'n' && Lower(name[2]) == 'i' &&
                                             Lower(name[3]) == 's' && name[4] == 0);
    return Lower(*pattern) == Lower(*name) && Matches(pattern + 1, name + 1);
}

void Append(char* to, int size, const char* text) {
    int at = strlen(to);
    while (*text && at + 1 < size) to[at++] = *text++;
    to[at] = 0;
}

}  // namespace

asm static void GetTargetFilter_Original() {
    nofralloc
    stwu r1, -16(r1)
    lis r12, 0x8027
    ori r12, r12, 0xF9D8
    mtctr r12
    bctr
}

typedef const char* (*GetTargetFilterFn)(const NisPlayer*, NisTarget, NisWinnerType);

// A mod character by its name (a cutscene filter), or -1.
int CharacterNamed(const char* name) {
    for (int c = 0; name && c < kModCharacterCount; ++c)
        if (strcmp(name, kModCharacters[c].name) == 0) return c;
    return -1;
}

// const char* NisPlayer::GetTargetFilter(NisTarget, NisWinnerType) const: the name a cutscene is
// picked by, the actor's. A mod character's for a kind it has a cutscene of; its base's otherwise.
static const char* GetTargetFilter(const NisPlayer* player, NisTarget target, NisWinnerType winner) {
    const char* filter = ((GetTargetFilterFn)GetTargetFilter_Original)(player, target, winner);
    const int c = CharacterNamed(filter);
    if (c >= 0 && !s_own) return BaseName(c);
    return filter;
}
kmBranch(0x8027F9D4, GetTargetFilter);

asm static void Load_Original() {
    nofralloc
    stwu r1, -464(r1)
    lis r12, 0x8027
    ori r12, r12, 0xFD08
    mtctr r12
    bctr
}

// void NisPlayer::Load(const char* kind, NisTarget, NisUseStadiumOffset, NisUseFilter,
// NisWinnerType, int, int): picks and plays a cutscene.
static void Load(NisPlayer* player, const char* kind, NisTarget target, NisUseStadiumOffset offset, NisUseFilter useFilter,
                 NisWinnerType winner, int a, int b) {
    bool own = true;
    if (kind) {
        const int c = CharacterNamed(((GetTargetFilterFn)GetTargetFilter_Original)(player, target, winner));
        if (c >= 0) {
            char prefix[192] = "";
            Append(prefix, sizeof(prefix), kModCharacters[c].name);
            Append(prefix, sizeof(prefix), "_");
            Append(prefix, sizeof(prefix), kind);
            if (useFilter) {
                Append(prefix, sizeof(prefix), "_");
                Append(prefix, sizeof(prefix), (const char*)player + kExtraNameFilter);
            }
            own = InDictionary(player, prefix);
        }
    }
    s_own = own;
    ((void (*)(NisPlayer*, const char*, NisTarget, NisUseStadiumOffset, NisUseFilter, NisWinnerType, int, int))Load_Original)(
        player, kind, target, offset, useFilter, winner, a, b);
    s_own = true;
}
kmBranch(0x8027FD04, Load);

asm static void LoadTriggers_Original() {
    nofralloc
    stwu r1, -48(r1)
    lis r12, 0x8027
    ori r12, r12, 0xD714
    mtctr r12
    bctr
}

// The triggers the characters list for this cutscene, added as its script adds its own.
static void AddListedTriggers(Nis* nis, const char* name) {
    struct TriggerParams {  // Nis::TriggerParams
        float float1;
        unsigned long param1, param2, param3, param4;
    };
    const unsigned long kNumTriggers = 0x184, kMaxTriggers = 48;  // Nis::mNumTriggers, MAX_NUM_TRIGGERS
    typedef void (*AddTriggerFn)(Nis*, int, float, const char*, const char*, TriggerParams*);
    const AddTriggerFn AddTrigger = (AddTriggerFn)0x80282390;  // Nis::AddTrigger
    for (int c = 0; name && c < kModCharacterCount; ++c) {
        for (int i = 0; i < kModCharacters[c].nisTriggerCount; ++i) {
            const ModNisTrigger& t = kModCharacters[c].nisTriggers[i];
            if (!Matches(t.cutscene, name) || At<int>(nis, kNumTriggers) >= (int)kMaxTriggers) continue;
            TriggerParams params = {-1.0f, (unsigned long)-1, (unsigned long)-1, (unsigned long)-1, (unsigned long)-1};
            if (t.type == kNisEffect) params.param1 = 0;  // the effect belongs to the cutscenes: they end it
            if (t.type == kNisTimeDilation) params.float1 = t.value;
            if (t.type == kNisDepthOfFieldOff) params.param1 = 1;
            AddTrigger(nis, t.type, t.frame, t.effect ? t.effect : "", t.target ? t.target : "", &params);
        }
    }
}

// void NisPlayer::LoadTriggers(Nis&): runs the cutscene's trigger script, the one named after it. A
// character's cutscene aliased to another's script runs that one: the name is the alias's for the call.
// Then the triggers the characters list for it.
static void LoadTriggers(NisPlayer* player, Nis* nis) {
    char* name = nis ? At<char*>(nis, kNisHeader) : 0;
    const char* alias = 0;
    for (int c = 0; name && c < kModCharacterCount && !alias; ++c) {
        for (int i = 0; i < kModCharacters[c].triggerAliasCount; ++i) {
            const char* from = kModCharacters[c].triggerAliases[i][0];
            const int length = strlen(from);
            if (StartsWith(name, from) && (name[length] == 0 || name[length] == '.')) {
                alias = kModCharacters[c].triggerAliases[i][1];
                break;
            }
        }
    }
    if (alias == 0 || strlen(alias) + 5 > 64) {
        ((void (*)(NisPlayer*, Nis*))LoadTriggers_Original)(player, nis);
        AddListedTriggers(nis, name);
        return;
    }
    char saved[64];
    for (int i = 0; i < 64; ++i) saved[i] = name[i];
    name[0] = 0;
    Append(name, 64, alias);
    Append(name, 64, ".nis");
    ((void (*)(NisPlayer*, Nis*))LoadTriggers_Original)(player, nis);
    for (int i = 0; i < 64; ++i) name[i] = saved[i];
    AddListedTriggers(nis, name);
}
kmBranch(0x8027D710, LoadTriggers);

// bool Nis::IsLoading(). A cutscene that animates a character it has no animation for loads
// "Art/Animation/<skeleton>/<anim>.sanim"; when the disc has no such file the load never starts and
// never calls back, its entry stays "loading" and the cutscene waits for it forever. That froze
// matches with a fixed team at the walk-out and after goals: partners' walk-outs and cheers played on
// the character. As the original, except that an entry whose load never started is dropped (its
// request back in the pool). (Strikers Recharged does this natively.)
static bool NisIsLoading(unsigned char* nis) {
    const unsigned long kScriptStarted = 0xBAC, kPending = 0x864, kPendingSize = 0x1C;
    unsigned long* const kPoolFreeList = (unsigned long*)(0x8057AB80 + 0x0C);  // SlotPool<PendingAnimationRequest>
    if (nis[kScriptStarted] == 0) return true;
    bool loading = false;
    for (int i = 0; i < 10; ++i) {
        unsigned char* entry = nis + kPending + i * kPendingSize;  // {name, character, loaded, load, data, size, request}
        if (*(unsigned long*)entry == 0 || *(int*)(entry + 4) == -1 || entry[8] != 0) continue;
        if (*(unsigned long*)(entry + 12) != 0) {
            loading = true;
            continue;
        }
        if (unsigned char* request = *(unsigned char**)(entry + 24)) {
            request[4] = 0;  // inactive
            *(unsigned long*)request = *kPoolFreeList;
            *kPoolFreeList = (unsigned long)request;
        }
        *(unsigned long*)entry = 0;
        *(int*)(entry + 4) = -1;
        *(unsigned long*)(entry + 24) = 0;
    }
    return loading;
}
kmBranch(0x80283930, NisIsLoading);
