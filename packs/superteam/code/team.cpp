// A fixed team: the character in play's side is the character four times over (the captain and
// players 1-3), whatever partner select picked. Entered when the match's character loader starts,
// as captains in partner slots, the way the game's own captain-only teams switch enters them; the
// character's model, textures and voice come with its base's identity (identity.cpp).
#include <kamek.h>

#include "Game/CharacterLoader.h"
#include "Game/DB/CharacterInfo.h"

#include "framework.h"

extern "C" void OSReport(const char* format, ...);
class GameAudio;
class AudioResourceLoadOwner;
extern GameAudio* g_pAudioSystem;
extern bool gAudioEnabled;
void LoadSoundBank(GameAudio* audio, int bank, unsigned long slot, void (*callback)(AudioResourceLoadOwner*, void*), void* param);

// The loader moving to its next entry, from AsyncLoadingManager::DoFunctionCall: once it's at the
// first, players 1-3 of a side the character leads (its captain entry the base) become the character.
static bool s_fixedTeamInPlay;  // this match has a fixed team

static bool NextEntry(CharacterLoader_8056B290* loader) {
    const bool more = loader->fn_80009EFC();
    if (loader->mCurrentIndex != 0) return more;
    s_fixedTeamInPlay = false;
    for (int side = 0; side < 2; ++side) {
        int captain = -1;
        for (int i = 0; i < 10; ++i) {
            const CharacterLoader_8056B290::Entry& entry = loader->mEntries[i];
            if (!entry.bGoalie && entry.nPlayerID == 0 && (entry.nTeamID & 1) == side) captain = (int)entry.cc;
        }
        const int c = ModCharacterOfClass(captain);
        if (c < 0 || !kModCharacters[c].fixedTeam) continue;
        OSReport("[superteam] %s's fixed team on side %d\n", kModCharacters[c].name, side);
        s_fixedTeamInPlay = true;
        for (int i = 0; i < 10; ++i) {
            CharacterLoader_8056B290::Entry& entry = loader->mEntries[i];
            if (entry.bGoalie || entry.nPlayerID < 1 || entry.nPlayerID > 3 || (entry.nTeamID & 1) != side) continue;
            entry.cc = (eCharacterClass)captain;
            entry.bCaptain = true;
            entry.bSidekick = false;
            loader->sidekick[side][entry.nPlayerID - 1] = (eCharacterClass)captain;
        }
    }
    return more;
}
kmCall(0x80118524, NextEntry);

// void CharacterLoader_8056B290::fn_8000C130(): a captain's voice bank, into its team's captain slot
// (1 home, 5 away). A captain in a partner's place (a fixed team's) goes in its own player's slot,
// as a partner's bank does: in the team captain's again, its load would never finish and the match
// would wait for it forever. (Strikers Recharged does this natively.)
static void CaptainAudio(CharacterLoader_8056B290* loader) {
    const CharacterLoader_8056B290::Entry* entry = loader->mCurrent;
    const int bank = GetCharacterInfo((int)entry->cc).unknown_0x1C;  // its voice bank
    loader->mAudioRequestCount += gAudioEnabled;
    const unsigned long slot = (entry->nTeamID == 0 ? 1 : 5) + entry->nPlayerID;  // + 0 for the team captain
    LoadSoundBank(g_pAudioSystem, bank, slot, (void (*)(AudioResourceLoadOwner*, void*))0x8000C0EC,  // fn_8000C0EC
                  (void*)loader->mAudioRequestCount);
}
kmBranch(0x8000C130, CaptainAudio);

// void NisPlayer::fn_8028041C(nis, "same"/"other", target, ...): a cutscene's companion, the partners'
// part ("<nis>_<partner>_same.nis": the walk-out, goals). A fixed team's partners are the character,
// not the partners picked, so a companion would wait forever for actors who aren't on the pitch:
// skipped while one is in play. (Strikers Recharged skips them natively.)
asm static void Companion_Original() {
    nofralloc
    stwu r1, -176(r1)
    lis r12, 0x8028
    ori r12, r12, 0x0420
    mtctr r12
    bctr
}

static void Companion(void* player, const char* nis, const char* side, int target, int stadiumOffset, int winner,
                      bool mirrored, int param6) {
    if (s_fixedTeamInPlay) return;
    ((void (*)(void*, const char*, const char*, int, int, int, bool, int))Companion_Original)(
        player, nis, side, target, stadiumOffset, winner, mirrored, param6);
}
kmBranch(0x8028041C, Companion);
