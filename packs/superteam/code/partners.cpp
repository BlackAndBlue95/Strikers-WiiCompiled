// Partner select: a character whose team is always itself (fixedTeam) fills its board's three partner
// slots whatever partner select picked: its picture in each, as the board's captain spot shows it
// (the base's picture, which is the character's while it's in play), and its role.
#include <kamek.h>

#include "Game/FE/feCaptainComponent.h"
#include "Game/FE/fePresentation.h"
#include "Game/FE/feFinder.h"
#include "Game/FE/tlComponent.h"
#include "NL/nlString.h"

#include "framework.h"


namespace {

template <typename T> inline T& At(const void* base, unsigned long offset) {
    return *(T*)((char*)base + offset);
}

const unsigned long kInstanceComponent = 0x0C, kInstanceRotation = 0x48, kInstanceOverloadFlags = 0x84,
                    kInstanceVisible = 0x8E, kImageTexture = 0x90;  // TLInstance, TLImageInstance
const unsigned long kComponentActiveSlide = 0x7C;                  // TLComponent
const unsigned long kHandlerPresentation = 0x14;                   // BaseSceneHandler
const unsigned long kRowSize = 0x5C, kRowRole = 0x48;              // CharacterInfo

// The presentation of the scene on top of the game's scene stack: partner select, while a board loads.
FEPresentation* TopPresentation() {
    void* manager = *(void**)0x806E1838;  // nlSingleton<GameSceneManager>::s_pInstance
    const int depth = manager ? At<int>(manager, 0x04) : 0;
    void* scene = depth >= 1 && depth <= 32 ? At<void*>(manager, 0x84 + depth * 4) : 0;
    return scene ? At<FEPresentation*>(scene, kHandlerPresentation) : 0;
}

void Join(char* out, unsigned long size, const char* a, const char* b) {
    unsigned long n = 0;
    for (; *a && n + 1 < size; ++a) out[n++] = *a;
    for (; *b && n + 1 < size; ++b) out[n++] = *b;
    out[n] = 0;
}

// FECaptainComponent::FindCaptainImage(captain, left), which the game inlines: the captain's picture
// in partner select's art, "<name>_right" for the right board, else "positions_<name>".
TLInstance* CaptainImage(const char* name, bool right) {
    FEPresentation* presentation = TopPresentation();
    if (presentation == 0 || name == 0) return 0;
    const unsigned long art = nlStringLowerHash("art"), layer = nlStringLowerHash("Layer");
    char picture[48];
    void* found = 0;
    if (right) {
        Join(picture, sizeof(picture), name, "_right");
        found = FEFindInstance(presentation, art, layer, nlStringLowerHash(picture), 0, 0, 0);
    }
    if (found == 0) {
        Join(picture, sizeof(picture), "positions_", name);
        found = FEFindInstance(presentation, art, layer, nlStringLowerHash(picture), 0, 0, 0);
    }
    return (TLInstance*)found;
}

// An instance by name path in a component's active slide.
TLInstance* FindInActiveSlide(TLComponentInstance* instance, const char* first, const char* second) {
    TLComponent* component = instance ? At<TLComponent*>(instance, kInstanceComponent) : 0;
    TLSlide* slide = component ? At<TLSlide*>(component, kComponentActiveSlide) : 0;
    TLInstance* child = slide ? FindItemByHashID(slide->pChildren, nlStringLowerHash(first)) : 0;
    return child ? (TLInstance*)FEFindInstanceRecursive(child, nlStringLowerHash(second), 0, 0, 0, 0, 0) : 0;
}

}  // namespace

// The end of FECaptainComponent::LoadSlotImages: its UpdateOverallSlides (each position's role, from
// the captain's and the partners' CharacterInfo), then the character's slots.
static void UpdateSlides(FECaptainComponent* board) {
    board->UpdateOverallSlides();
    const int c = ModCharacterOfTeam(board->mCaptain);
    if (c < 0 || !kModCharacters[c].fixedTeam) return;
    static const char* const kStates[] = {"off", "over", "down"};
    static const char* const kOveralls[] = {"overall_1", "overall_2", "overall_3"};
    static const char* const kRoles[] = {"offensive", "defensive", "playmaker", "power", "balanced"};
    TLInstance* picture = CaptainImage(kModCharacters[c].name, board->mSide != 0);
    void* texture = picture ? At<void*>(picture, kImageTexture) : 0;
    const int role = At<int>(ModCharacterRow(c), kRowRole);
    // The captain's role too (the board took it from a row the game doesn't have).
    if (TLInstance* overall = FindInActiveSlide(board->mPositions, "positions", "overall_0"))
        if (TLComponent* component = At<TLComponent*>(overall, kInstanceComponent))
            if (role >= 0 && role < 5) component->SetActiveSlide(kRoles[role], false, false);
    for (int slot = 0; slot < 3; ++slot) {
        for (int s = 0; texture && s < 3; ++s) {
            if (TLImageInstance* image = board->FindPositionImage(slot, kStates[s])) {
                At<void*>(image, kImageTexture) = texture;
                At<unsigned long>(image, kInstanceOverloadFlags) |= 2;  // its own rotation: none
                for (int k = 0; k < 3; ++k) At<float>(image, kInstanceRotation + k * 4) = 0.0f;
            }
        }
        TLInstance* overall = FindInActiveSlide(board->mPositions, "positions", kOveralls[slot]);
        if (overall && role >= 0 && role < 5) {
            At<unsigned char>(overall, kInstanceVisible) = 1;
            if (TLComponent* component = At<TLComponent*>(overall, kInstanceComponent))
                component->SetActiveSlide(kRoles[role], false, false);
        }
    }
}
kmCall(0x801DC808, UpdateSlides);
