// Shared between the pack's files.
#ifndef TWEAKS_H
#define TWEAKS_H

// The tweaks, each an option of the pack (riivolution/tweaks.xml).
enum Tweak {
    kSkipIntro,
    kFastMenus,
    kFastStadiums,
    kNoMegaStrikes,
    kUnlockAll,
    kWinByTwo,
    kNkFix,
    kBluePeach,
    kShotCounter,
    kTweakCount
};

// Whether a tweak's option is on (options.cpp).
bool TweakOn(Tweak tweak);

// Once a frame: every half second, the options are read again (options.cpp).
void RefreshTweaks();

template <typename T> inline T& At(unsigned long base, unsigned long offset) {
    return *(T*)(base + offset);
}

// The game's state, by address.
const unsigned long kGame = 0x806E0C94;             // g_pGame: non-zero during a match
const unsigned long kGameInfoManager = 0x806E0F54;  // nlSingleton<GameInfoManager>::s_pInstance
const unsigned long kSceneManager = 0x806E1838;     // GameSceneManager: depth +0x04, handlers +0x88

inline bool InMatch() {
    return *(unsigned long*)kGame != 0;
}

// The scene on top of the front end's stack, or 0.
inline unsigned long TopScene() {
    const unsigned long manager = *(unsigned long*)kSceneManager;
    if (manager == 0) return 0;
    const unsigned long depth = At<unsigned long>(manager, 0x04);
    return depth >= 1 && depth <= 32 ? At<unsigned long>(manager, 0x88 + (depth - 1) * 4) : 0;
}

// Fast menus (menus.cpp), once a frame.
void FastMenusFrame();

#endif
