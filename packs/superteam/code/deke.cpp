// The character's deke. "defaultDeke" is the game's generic directional one (Mario's, its default
// case; SMS's spin) instead of its base's (Waluigi's teleport): the character's players deke as Mario,
// their class reading as his from the deke's start until the deke action ends (the deke's update also
// switches on it), and the base's "disappear" animation trigger does nothing meanwhile (its trigger
// script stays on the character's animations of those names).
#include <kamek.h>

#include "framework.h"

class cFielder;
enum eFielderActionState {};

namespace {

template <typename T> inline T& At(const void* base, unsigned long offset) {
    return *(T*)((char*)base + offset);
}

const unsigned long kClass = 0x24, kActionState = 0x430;  // cCharacter, cFielder
const int kDekeAction = 1, kDefaultDekeClass = 0;          // Mario: the deke switch's default case

struct Deking {
    cFielder* player;
    int ownClass;
};
Deking s_deking[10];
bool s_starting;  // InitActionDeke sets the deke action itself (CleanUpAction of the one before)

Deking* Find(cFielder* player) {
    for (int i = 0; i < 10; ++i)
        if (s_deking[i].player == player) return &s_deking[i];
    return 0;
}

bool DekesAsDefault(cFielder* player) {
    const int c = ModCharacterOfClass(At<int>(player, kClass));
    return c >= 0 && kModCharacters[c].defaultDeke;
}

}  // namespace

asm static void InitActionDeke_Original() {
    nofralloc
    stwu r1, -32(r1)
    lis r12, 0x8004
    ori r12, r12, 0x47C4
    mtctr r12
    bctr
}

// bool cFielder::fn_800447C0(ushort direction) ("InitActionDeke"): starts a deke, its animation and
// direction picked by class.
static bool InitActionDeke(cFielder* player, unsigned short direction) {
    typedef bool (*Fn)(cFielder*, unsigned short);
    Deking* deking = Find(player);
    if (deking && At<int>(player, kClass) != kDefaultDekeClass) deking->player = 0;  // a player since freed
    if (Find(player) || !DekesAsDefault(player)) return ((Fn)InitActionDeke_Original)(player, direction);
    deking = Find(0);
    if (deking == 0) return ((Fn)InitActionDeke_Original)(player, direction);
    const int own = At<int>(player, kClass);
    deking->player = player;
    deking->ownClass = own;
    At<int>(player, kClass) = kDefaultDekeClass;
    s_starting = true;
    const bool started = ((Fn)InitActionDeke_Original)(player, direction);
    s_starting = false;
    if (!started || At<int>(player, kActionState) != kDekeAction) {  // no deke after all
        At<int>(player, kClass) = own;
        deking->player = 0;
    }
    return started;
}
kmBranch(0x800447C0, InitActionDeke);

asm static void CleanUpAction_Original() {
    nofralloc
    stwu r1, -96(r1)
    lis r12, 0x8003
    ori r12, r12, 0x49E0
    mtctr r12
    bctr
}

// void cFielder::CleanUpAction(eFielderActionState next): ends the current action. A player that
// deked as another class gets its own back when its deke ends (after the deke is cleaned up as the
// class that started it).
static void CleanUpAction(cFielder* player, eFielderActionState next) {
    const bool deking = At<int>(player, kActionState) == kDekeAction;
    ((void (*)(cFielder*, eFielderActionState))CleanUpAction_Original)(player, next);
    if (!deking || s_starting) return;
    if (Deking* d = Find(player)) {
        At<int>(player, kClass) = d->ownClass;
        d->player = 0;
    }
}
kmBranch(0x800349DC, CleanUpAction);

asm static void Disappear_Original() {
    nofralloc
    stwu r1, -128(r1)
    lis r12, 0x8003
    ori r12, r12, 0x95C4
    mtctr r12
    bctr
}

// The deke animations' "disappear" trigger (hides and moves the player and ball): nothing for a
// player deking as another class.
static void Disappear(cFielder* player) {
    if (player && At<int>(player, kActionState) == kDekeAction && Find(player)) return;
    ((void (*)(cFielder*))Disappear_Original)(player);
}
kmBranch(0x800395C0, Disappear);
