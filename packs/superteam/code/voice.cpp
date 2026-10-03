// Captain select: a team picking the character hears its voice, as picking a captain plays theirs.
// Its sound bank (the base's, which identity.cpp turns into the character's) loads into the team's
// voice slot (1 home, 5 away); once it's in, its "selected" cue plays. Before the match loads its
// own voices into those slots, partner select empties them.
#include <kamek.h>

#include "framework.h"

class GameAudio;
class AudioResourceLoadOwner;
extern GameAudio* g_pAudioSystem;
void LoadSoundBank(GameAudio* audio, int bank, unsigned long slot, void (*callback)(AudioResourceLoadOwner*, void*), void* param);
class AudioBankTable {
public:
    void UnloadBank(unsigned int slot);
};
class FEAudio {
public:
    static void PlaySound(int slot, unsigned long cue, const void* debugName, void* context);
    static void StopAnimAudioEvent(unsigned long cue, void* context);
};

namespace {

template <typename T> inline T& At(const void* base, unsigned long offset) {
    return *(T*)((char*)base + offset);
}

const unsigned long kSelectedCue = 0x270203ED;

int s_loaded[2] = {-1, -1};  // the character whose voice is in each team's slot
bool s_pending[2];           // play once it's in

unsigned long Slot(int side) {
    return side == 0 ? 1 : 5;
}

bool SlotLoaded(int side) {
    void* table = VoiceBankTable();
    if (table == 0) return false;
    unsigned char* source = At<unsigned char*>(table, 0x0C) + Slot(side) * 0x18;  // AudioResourceSource
    unsigned char* owner = At<unsigned char*>(source, 0x10);
    return owner && At<unsigned char>(owner, 0x08);  // AudioResourceLoadOwner::m_Completed
}

void NoCallback(AudioResourceLoadOwner*, void*) {}

}  // namespace

void PlayPickVoice(int side, int c) {
    if (side < 0 || side > 1 || c < 0 || !kModCharacters[c].voiceBank || g_pAudioSystem == 0) return;
    if (s_loaded[side] != c) {
        if (s_loaded[side] >= 0) {
            if (!SlotLoaded(side)) return;  // the other's still loading
            ((AudioBankTable*)VoiceBankTable())->UnloadBank(Slot(side));
        }
        LoadSoundBank(g_pAudioSystem, BaseVoiceBank(c), Slot(side), NoCallback, 0);
        s_loaded[side] = c;
    }
    s_pending[side] = true;
    UpdatePickVoices();
}

void UpdatePickVoices() {
    for (int side = 0; side < 2; ++side) {
        if (!s_pending[side] || !SlotLoaded(side)) continue;
        FEAudio::PlaySound((int)Slot(side), kSelectedCue, 0, 0);
        s_pending[side] = false;
    }
}

// Partner select's virtual Update: the pick voices' banks out of the teams' slots (once loaded).
class ChooseSidekicksSceneV2 {
public:
    void Update(float dt);
};

static void PartnerSelectUpdate(ChooseSidekicksSceneV2* scene, float dt) {
    for (int side = 0; side < 2; ++side) {
        if (s_loaded[side] < 0 || !SlotLoaded(side)) continue;
        ((AudioBankTable*)VoiceBankTable())->UnloadBank(Slot(side));
        s_loaded[side] = -1;
        s_pending[side] = false;
    }
    scene->ChooseSidekicksSceneV2::Update(dt);
}
kmWritePointer(0x8051D590, PartnerSelectUpdate);  // __vt__22ChooseSidekicksSceneV2: Update
