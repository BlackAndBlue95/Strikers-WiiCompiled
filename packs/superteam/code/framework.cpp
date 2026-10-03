// Mod characters on captain select and in matches.
//
// Captain select gets pages after the captains, - and + switching between them (the grid slides out
// and back in, as partner select swaps its rows). Each mod character sits on its base captain's
// button on the first page where that button is free. Picking one picks its own team
// (kFirstModTeam + its index, slots.cpp), which no grid button is: its base stays free for the other
// team.
//
// The game's own handlers do the captains page. On a mod page the grid's hover and press handlers
// are muted and this does what they do, as ChooseCaptainsSceneV2::OnCaptainPointerPress would.
#include <kamek.h>

#include "Game/SH/SHChooseCaptains.h"
#include "Game/FE/tlComponent.h"
#include "Game/FE/feInput.h"
#include "Game/FE/FEAudio.h"
#include "Game/FE/feHelpFuncs_decl.h"
#include "Game/FE/feTextureResource.h"
#include "Game/FE/feResourceManager.h"
#include "NL/nlFile.h"
#include "NL/gl/glTexture.h"

#include "framework.h"

extern "C" void* memcpy(void* dst, const void* src, unsigned long n);
void* nlMalloc(unsigned long size, unsigned int alignment, bool fromEnd);
extern FEInput* g_pFEInput;
extern float gFEPointerPositions[4][2];

namespace {

// ---- the game's objects, by offset where the decomp has no field for it ----

template <typename T> inline T& At(const void* base, unsigned long offset) {
    return *(T*)((char*)base + offset);
}

const unsigned long kInstanceComponent = 0x0C, kInstanceVisible = 0x8E, kImageTexture = 0x90;  // TLInstance
const unsigned long kComponentActiveSlide = 0x7C;                                              // TLComponent
const unsigned long kSlideDuration = 0x14, kSlideTime = 0x18, kSlideHash = 0x40;               // TLSlide
const unsigned long kHandlerPresentation = 0x14, kHandlerScene = 0x18, kSceneState = 0x74;     // BaseSceneHandler, FEScene
const unsigned long kCallbacks[4] = {0x0C, 0x14, 0x1C, 0x24};  // FEPointerListener: enter, leave, inside, press
const unsigned long kPointerDisabled = 0x80, kPointerStates = 0xA0;  // FEPointerListener

const int kButtonSelect = 0x1E, kButtonMinus = 0x31, kButtonPlus = 0x30;  // FE buttons: A, -, +
const int kGridCaptains[12] = {0, 5, 3, 6, 4, 7, 1, 8, 2, 9, 10, 11};    // grid button -> captain
const int kMaxPages = 4;

TLComponent* ComponentOf(void* instance) {
    return instance ? At<TLComponent*>(instance, kInstanceComponent) : 0;
}

void SetSlide(void* instance, const char* slide) {
    if (TLComponent* component = ComponentOf(instance)) component->SetActiveSlide(slide, true, false);
}

void SetVisible(void* instance, bool visible) {
    if (instance) At<unsigned char>(instance, kInstanceVisible) = visible ? 1 : 0;
}

bool Loaded(ChooseCaptainsSceneV2* scene) {
    void* feScene = At<void*>(scene, kHandlerScene);
    return feScene && At<int>(feScene, kSceneState) == 6 && At<void*>(scene, kHandlerPresentation);
}

bool Online() {
    // GameInfoManager::mIsOnlineMode
    void* info = *(void**)0x806E0F54;
    return info && At<unsigned char>(info, 0x120) != 0;
}

bool JustPressed(int pad, int button) {
    return g_pFEInput && g_pFEInput->JustPressed((eFEINPUT_PAD)pad, button, true, 0);
}

void PlaySound(unsigned long cue) {
    FEAudio::PlayAnimAudioEvent(cue, 0, 0, true);
}

int GridButtonOf(int captain) {
    for (int i = 0; i < 12; ++i)
        if (kGridCaptains[i] == captain) return i;
    return -1;
}

// ---- the pages: each mod character on its base's button on the first page where it's free ----

int s_pages = 0;
signed char s_layout[kMaxPages][12];  // mod character index, -1 = none

void LayOutPages() {
    if (s_pages != 0) return;
    for (int p = 0; p < kMaxPages; ++p)
        for (int b = 0; b < 12; ++b) s_layout[p][b] = -1;
    for (int c = 0; c < kModCharacterCount; ++c) {
        const int own = GridButtonOf(kModCharacters[c].base);
        if (own < 0) continue;
        bool placed = false;
        for (int p = 0; p < s_pages && !placed; ++p)
            if (s_layout[p][own] < 0) { s_layout[p][own] = (signed char)c; placed = true; }
        if (!placed && s_pages < kMaxPages) s_layout[s_pages++][own] = (signed char)c;
    }
}

// The character on `button` of page `page` (1 = the first mod page), or -1.
int CharacterAt(int page, int button) {
    return page >= 1 && page <= s_pages && button >= 0 && button < 12 ? s_layout[page - 1][button] : -1;
}

// ---- state ----

ChooseCaptainsSceneV2* s_scene;
bool s_fresh;            // created, not yet looked at
int s_page;              // shown page: 0 = captains
int s_swapTo = -1;       // the page the grid comes back with once it has slid out
bool s_muted;
unsigned long s_saved[12][4][2];  // the grid buttons' muted handlers (Function2: tag, functor)
int s_hover[4] = {-1, -1, -1, -1};
int s_selected[2] = {-1, -1};     // the mod character leading each side's team, -1 = none
bool s_wasConfirmed[2];

// ---- portraits ----

// A captain select texture resource for a pack texture: a captain portrait's, pointed at it.
unsigned char s_resources[16][2][0x20];
unsigned long s_portraitHashes[16][2];
bool s_portraitsLoaded[16];

FETextureResource* Portrait(ChooseCaptainsSceneV2* scene, int c, bool taken) {
    if (c < 0 || c >= 16) return 0;
    FETextureResource* model = scene->mCaptainTextures[0][1];
    if (!s_portraitsLoaded[c] && model) {
        s_portraitsLoaded[c] = true;
        for (int t = 0; t < 2; ++t) {
            const unsigned long hash = PackTexture(t ? kModCharacters[c].portraitTaken : kModCharacters[c].portrait);
            if (hash == 0) continue;
            FETextureResource* resource = (FETextureResource*)s_resources[c][t];
            memcpy(resource, model, 0x20);
            At<unsigned long>(resource, 0x00) = 0;     // m_next
            At<unsigned long>(resource, 0x04) = 0;     // m_prev
            At<unsigned long>(resource, 0x0C) = hash;  // m_hashID
            At<unsigned char>(resource, 0x10) = 1;     // m_bValid
            resource->m_glTextureHandle = hash;
            s_portraitHashes[c][t] = hash;
        }
    }
    const int t = taken ? 1 : 0;
    if (s_portraitHashes[c][t] && EnsurePackTexture(s_portraitHashes[c][t])) return (FETextureResource*)s_resources[c][t];
    const int button = GridButtonOf(kModCharacters[c].base);  // else its base's
    return button >= 0 ? scene->mCaptainTextures[button][taken ? 0 : 1] : 0;
}

// Taken by `side`'s opponent: the same character (its base is free: it's a team of its own).
bool TakenFor(ChooseCaptainsSceneV2* scene, int side, int c) {
    const int other = side ^ 1;
    return scene->mConfirmed[other] && scene->mCaptainIds[other] == kFirstModTeam + c;
}

bool TakenForEither(ChooseCaptainsSceneV2* scene, int c) {
    return TakenFor(scene, 0, c) || TakenFor(scene, 1, c);
}

void ShowPortraits(ChooseCaptainsSceneV2* scene) {
    if (s_page == 0) return;
    for (int b = 0; b < 12; ++b) {
        const int c = CharacterAt(s_page, b);
        TLImageInstance* image = scene->mCaptainImages[b];
        if (c < 0 || image == 0) continue;
        if (FETextureResource* portrait = Portrait(scene, c, TakenForEither(scene, c)))
            At<FETextureResource*>(image, kImageTexture) = portrait;
    }
}

// ---- the grid ----

bool ButtonUsed(int button) {
    return s_page == 0 || CharacterAt(s_page, button) >= 0;
}

// Muted: the grid's hover and press handlers saved and emptied (a mod page handles its picks here).
// Either way, buttons the page doesn't use are off and hidden.
void MuteGrid(ChooseCaptainsSceneV2* scene, bool mute) {
    for (int i = 0; i < 12; ++i) {
        FEPointerButton* button = &scene->mCaptainButtons[i];
        if (mute != s_muted) {
            for (int k = 0; k < 4; ++k) {
                unsigned long* callback = &At<unsigned long>(button, kCallbacks[k]);
                if (mute) {
                    s_saved[i][k][0] = callback[0];
                    s_saved[i][k][1] = callback[1];
                    callback[0] = 0;  // FUNCTION_EMPTY
                } else {
                    callback[0] = s_saved[i][k][0];
                    callback[1] = s_saved[i][k][1];
                }
            }
        }
        for (int p = 0; p < 4; ++p) At<int>(button, kPointerStates + p * 4) = 0;
        const bool unused = mute && !ButtonUsed(i);
        At<unsigned char>(button, kPointerDisabled) = unused ? 1 : 0;
        SetVisible(scene->mCaptainInstances[i], !unused);
    }
    s_muted = mute;
}

int SideOf(ChooseCaptainsSceneV2* scene, int pad) {
    for (int s = 1; s >= 0; --s)
        if (scene->mSidePads[s] == pad) return s;
    return -1;
}

// The mod character on `button` leads `side`'s team, its own (kFirstModTeam + c), picked as
// OnCaptainPointerPress picks a captain. No grid button is the team's, so none is taken.
void Pick(ChooseCaptainsSceneV2* scene, int side, int pad, int button) {
    const int c = CharacterAt(s_page, button);
    if (c < 0 || TakenFor(scene, side, c)) return;
    const int team = kFirstModTeam + c;
    scene->mConfirmed[side] = true;
    scene->mSidePads[side] = -1;
    scene->mCaptainIds[side] = team;
    scene->mSelectedCaptains[side] = -1;
    SetSlide(scene->mSelectDisplays[side], "off");
    At<int>(&scene->mSelectButtons[side], kPointerStates + pad * 4) = 0;
    for (int p = 0; p < 4; ++p) At<int>(&scene->mCaptainButtons[button], kPointerStates + p * 4) = 0;
    SetVisible(scene->mGreenArrows[side], false);

    if (!scene->mChangeTextShown[side]) scene->UpdateSelectText(side);
    SetVisible(scene->mSelectButtonInstances[side], true);
    s_selected[side] = c;
    s_wasConfirmed[side] = true;
    s_hover[pad] = -1;
    SetActiveCharacter(c);
    scene->mCaptainComponents[side].SetCaptainInfo(team, pad, 1);
    PlayPickVoice(side, c);
}

// The character in play: a team's pick, else one hovered on its page by a team still choosing (its
// stats on the panel), unless the other team has its base.
int CharacterInPlay(ChooseCaptainsSceneV2* scene) {
    if (s_selected[0] >= 0) return s_selected[0];
    if (s_selected[1] >= 0) return s_selected[1];
    for (int pad = 0; s_page != 0 && pad < 4; ++pad) {
        const int side = SideOf(scene, pad), c = CharacterAt(s_page, s_hover[pad]);
        if (side >= 0 && c >= 0 && !scene->mConfirmed[side] && !TakenFor(scene, side, c)) return c;
    }
    return -1;
}

void UpdatePages(ChooseCaptainsSceneV2* scene) {
    if (scene != s_scene) {  // a captain select this didn't see created
        s_scene = scene;
        s_fresh = true;
    }
    if (scene->mSceneType != ChooseCaptainsSceneV2::ST_DOMINATION || Online() || kModCharacterCount == 0) {
        s_selected[0] = s_selected[1] = -1;  // Striker Cup and online: the captains only
        SetActiveCharacter(-1);
        return;
    }
    if (!Loaded(scene) || !scene->mCaptainButtonsInitialized || scene->mState != 1 || scene->mInputSuppressed)
        return;
    LayOutPages();

    if (s_fresh) {  // coming back with the teams chosen keeps a mod character; anything else drops it
        s_fresh = false;
        for (int side = 0; side < 2; ++side) {
            const int c = s_selected[side];
            if (c >= 0 && !(scene->mConfirmed[side] && scene->mCaptainIds[side] == kFirstModTeam + c))
                s_selected[side] = -1;
            s_wasConfirmed[side] = scene->mConfirmed[side];
        }
    }
    for (int side = 0; side < 2; ++side) {  // choosing again, or a captain picked from the grid
        const bool confirmed = scene->mConfirmed[side];
        if (!confirmed || !s_wasConfirmed[side]) s_selected[side] = -1;
        s_wasConfirmed[side] = confirmed;
    }
    SetActiveCharacter(CharacterInPlay(scene));
    UpdatePickVoices();

    // Changing pages: the grid slides out, the page changes, and it slides back in.
    void* layer = scene->mCaptainsLayer;
    if (s_swapTo >= 0) {
        TLComponent* component = ComponentOf(layer);
        void* slide = component ? At<void*>(component, kComponentActiveSlide) : 0;
        if (slide == 0 || At<unsigned long>(slide, kSlideHash) != nlStringLowerHash("out") ||
            At<float>(slide, kSlideTime) >= At<float>(slide, kSlideDuration)) {
            s_page = s_swapTo;
            s_swapTo = -1;
            MuteGrid(scene, s_page != 0);
            if (s_page == 0) scene->RefreshCaptainImages();
            SetVisible(layer, true);
            SetSlide(layer, "in");
            PlaySound(0xDF52130F);
        }
        return;
    }
    if (!(scene->mConfirmed[0] && scene->mConfirmed[1])) {
        for (int pad = 0; pad < 4; ++pad) {
            const int step = JustPressed(pad, kButtonPlus) ? 1 : JustPressed(pad, kButtonMinus) ? -1 : 0;
            // Only a pad choosing a captain: before that the grid isn't up.
            if (step == 0 || SideOf(scene, pad) < 0) continue;
            s_swapTo = (s_page + step + s_pages + 1) % (s_pages + 1);
            MuteGrid(scene, true);  // nothing to pick while it's away
            for (int i = 0; i < 4; ++i) s_hover[i] = -1;
            SetSlide(layer, "out");
            PlaySound(0x0B8C09FA);
            return;
        }
    }
    if (s_page == 0) return;

    ShowPortraits(scene);
    for (int pad = 0; pad < 4; ++pad) {
        const int side = SideOf(scene, pad);
        int hit = -1;
        if (side >= 0) {
            nlVector2 position;
            position.x = gFEPointerPositions[pad][0];
            position.y = gFEPointerPositions[pad][1];
            for (int i = 0; i < 12 && hit < 0; ++i)
                if (ButtonUsed(i) && scene->mCaptainButtons[i].FEPointerRegion::ContainsPoint(position)) hit = i;
        }
        // The pad's pointer state on each button, as the muted handlers would set it (what
        // UpdatePointerCursors keeps lit).
        for (int i = 0; i < 12; ++i) At<int>(&scene->mCaptainButtons[i], kPointerStates + pad * 4) = i == hit ? 1 : 0;
        if (hit != s_hover[pad]) {
            s_hover[pad] = hit;
            if (hit >= 0) {
                if (!scene->mConfirmed[side]) {  // its stats, as hovering a captain shows theirs
                    SetActiveCharacter(CharacterInPlay(scene));
                    scene->mCaptainComponents[side].SetCaptainInfo(kFirstModTeam + CharacterAt(s_page, hit), pad, 1);
                }
                SetSlide(scene->mCaptainInstances[hit], "over");
                scene->mCaptainButtons[hit].PlayHoverFeedback(pad);
                PlaySound(0xA183DBCD + pad);
            }
        }
        if (side >= 0 && hit >= 0 && JustPressed(pad, kButtonSelect)) Pick(scene, side, pad, hit);
    }
}

}  // namespace

// ---- hooks: captain select's virtual SceneCreated and Update ----

static void SceneCreated(ChooseCaptainsSceneV2* scene) {
    scene->ChooseCaptainsSceneV2::SceneCreated();
    LoadPackTextures();
    s_scene = scene;
    s_fresh = true;
    s_page = 0;
    s_swapTo = -1;
    s_muted = false;
    for (int i = 0; i < 4; ++i) s_hover[i] = -1;
}

static void Update(ChooseCaptainsSceneV2* scene, float dt) {
    scene->ChooseCaptainsSceneV2::Update(dt);
    UpdatePages(scene);
}

kmWritePointer(0x8051D18C, SceneCreated);  // __vt__21ChooseCaptainsSceneV2: SceneCreated
kmWritePointer(0x8051D174, Update);        // __vt__21ChooseCaptainsSceneV2: Update
