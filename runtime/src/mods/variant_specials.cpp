// Special moves for mod characters (CharacterDef::deke, ability; docs/modding/manifest.md). A mod
// character plays as its base, specials included; these switch the base's off.
//
// ability = "none": its CharacterInfo row has no captain item type (+0x14 = -1, as a partner's row;
// Variants::Build), so its team is never awarded the captain item and the ability never starts.
//
// deke: the deke is picked by class (cFielder::fn_800447C0) and its teleport (Waluigi's, Daisy's, Dry
// Bones') comes from the "disappear" / "reappear" triggers on the deke animations. "plain" keeps the
// base's deke without the teleport; "default" dekes as Mario does (the switch's default case, the
// directional spin Luigi, Yoshi and the partners use too), which needs deke_300 / deke_600 / deke_1200
// animations of the character's own. r3 = the player (cFielder) in each hook below.
#include "mods/variants.h"

#include <unordered_map>
#include "hle_stubs.h"
#include "memory.h"
#include "mods/mod_registry.h"

using namespace Mods;
using namespace Mods::Variants;

namespace {
constexpr uint32_t kCharacterClass = 0x24, kCharacterRow = 0x11C, kActionState = 0x430, kDekeAction = 1;  // cCharacter, cFielder
constexpr uint32_t kDefaultDekeClass = 0;  // Mario: the switch's default case (also Luigi's, Yoshi's, ...)

CharacterDef::Deke DekeOf(uint32_t player) {
    const Variant* v = player != 0 ? ForRow(Memory::Read32(player + kCharacterRow)) : nullptr;
    return v != nullptr ? v->def->deke : CharacterDef::Deke::Base;
}

std::unordered_map<uint32_t, uint32_t> g_dekeClass;  // player -> its own class, while it dekes as another
bool g_startingDeke = false;
} // namespace

// The deke animation's "disappear" trigger (fn_800395C0: hides the player and ball and moves them, for
// every class but Boo's, Shy Guy's and Monty's) does nothing for such a character's player during a
// deke, and "reappear" then has nothing to do: its base's triggers (Waluigi's teleport) stay on its
// animations of those names.
extern "C" void func_800395C0(CpuContext* ctx);
static void DekeDisappear(CpuContext* ctx)
{
    const uint32_t player = ctx->gpr[3];
    if (player != 0 && Memory::Read32(player + kActionState) == kDekeAction && DekeOf(player) != CharacterDef::Deke::Base) return;
    func_800395C0(ctx);
}
PPC_NATIVE_WRAP(800395C0, DekeDisappear);

// bool cFielder::fn_800447C0(ushort direction) ("InitActionDeke"): starts a deke, its animation and
// direction picked by class. A "default" deke character's player dekes as Mario: its class reads as
// his from here until the deke action ends (CleanUpAction below), since the deke's update also
// switches on it. As the original otherwise.
extern "C" void func_800447C0(CpuContext* ctx);
static void DekeStart(CpuContext* ctx)
{
    const uint32_t player = ctx->gpr[3];
    if (const auto it = g_dekeClass.find(player); it != g_dekeClass.end() && Memory::Read32(player + kCharacterClass) != kDefaultDekeClass)
        g_dekeClass.erase(it);  // a player since freed (a new one at its address)
    if (DekeOf(player) != CharacterDef::Deke::Default || g_dekeClass.count(player) != 0) {
        func_800447C0(ctx);
        return;
    }
    const uint32_t own = Memory::Read32(player + kCharacterClass);
    g_dekeClass[player] = own;
    Memory::Write32(player + kCharacterClass, kDefaultDekeClass);
    g_startingDeke = true;  // it sets the deke action itself (CleanUpAction of the action before)
    func_800447C0(ctx);
    g_startingDeke = false;
    if ((ctx->gpr[3] & 0xFF) == 0 || Memory::Read32(player + kActionState) != kDekeAction) {  // no deke
        Memory::Write32(player + kCharacterClass, own);
        g_dekeClass.erase(player);
    }
}
PPC_NATIVE_WRAP(800447C0, DekeStart);

// void cFielder::CleanUpAction(eFielderActionState next): ends the current action. As the original;
// when a player that dekes as another class ends its deke, its own class comes back (after the
// original, which cleans up the deke as the class that started it).
extern "C" void func_800349DC(CpuContext* ctx);
static void DekeCleanUp(CpuContext* ctx)
{
    const uint32_t player = ctx->gpr[3];
    const bool deking = Memory::Read32(player + kActionState) == kDekeAction;
    func_800349DC(ctx);
    if (!deking || g_startingDeke) return;
    if (const auto it = g_dekeClass.find(player); it != g_dekeClass.end()) {
        Memory::Write32(player + kCharacterClass, it->second);
        g_dekeClass.erase(it);
    }
}
PPC_NATIVE_WRAP(800349DC, DekeCleanUp);
