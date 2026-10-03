// The menus: fast menus and skip intro. As Strikers Recharged had them natively.
#include <kamek.h>

#include "tweaks.h"

extern "C" void OSReport(const char* format, ...);

// ---- Fast menus
//
// Menu transitions are skipped, outside matches. 2D: one-shot slides of the front end (panels
// sliding in and out, button states, a board's screen flickering on) jump to their end; long ones are
// content (credits) and looping ones idle animation, so those keep their pace, as do the boot screens
// (the notices, the studio logo). 3D: while the front end's presentation runs a transition script (the
// zoom from the main menu), the game's time dilation runs the time-dilated tasks (camera moves, the
// front end's scenes, effects) 30 times faster, so they're over within a frame or two, and each
// scripted wait ends as it starts; audio keeps real time.

namespace {

const float kDilation = 30.0f;
const float kLongestTransition = 3.0f;    // seconds: a longer one-shot slide is content
const float kLongestPresentation = 6.0f;  // seconds: a screen's main slide (its text fading in)
const unsigned long kTaskManager = 0x806E1DA0;  // nlTaskManager::m_pInstance: mTimeDilation at +0x00
const unsigned long kAudioTask = 0x8056F870;    // audioUpdateTask: nlTask::mTimeDilated at +0x1C

unsigned long s_frame;  // game frames
bool s_dilating;

// Slides set again this frame (SetActiveSlide resets a slide with Update(0)) take this frame's step as
// they would: partner select sets its pointers and its slots every frame, holding them on their first
// frame, and jumping those to their end showed the wrong state each time. A one-shot slide set once
// jumps to its end on its next step.
const int kMaxResets = 256;
unsigned long s_resets[kMaxResets];
int s_resetCount;
unsigned long s_resetFrame;

void NoteReset(unsigned long slide) {
    if (s_resetFrame != s_frame) {
        s_resetFrame = s_frame;
        s_resetCount = 0;
    }
    if (s_resetCount < kMaxResets) s_resets[s_resetCount++] = slide;
}

bool ResetThisFrame(unsigned long slide) {
    if (s_resetFrame != s_frame) return false;
    for (int i = 0; i < s_resetCount; ++i)
        if (s_resets[i] == slide) return true;
    return false;
}

bool InMenus() {
    return TweakOn(kFastMenus) && !InMatch();
}

// The boot screens (BootLoadingScene): the notices are meant to be read, and the studio logo's jingle
// is unloaded as its slide ends, which under the playing jingle crashes the sound update.
bool BootScreens() {
    const unsigned long top = TopScene();
    return top != 0 && At<unsigned long>(top, 0) == 0x805207D8;  // BootLoadingScene's vtable
}

}  // namespace

// Once a frame: the 3D side (and real time back when the tweak goes off mid-transition).
void FastMenusFrame() {
    ++s_frame;
    bool transition = false;
    unsigned long presentation = 0;
    const unsigned long manager = *(unsigned long*)kSceneManager;
    if (InMenus() && manager != 0 && At<unsigned long>(manager, 0x04) != 0) {  // the front end is up
        presentation = ((unsigned long (*)())0x801FEEAC)();                    // FrontEndPresentation::GetInstance()
        transition = presentation != 0 && ((bool (*)(unsigned long))0x801FF168)(presentation);  // IsActive()
    }
    const unsigned long tasks = *(unsigned long*)kTaskManager;
    if (transition && tasks != 0) {
        At<float>(tasks, 0x00) = kDilation;
        At<unsigned char>(kAudioTask, 0x1C) = 0;
        s_dilating = true;
        if (At<float>(presentation, 0xA8) > 0.0f) At<float>(presentation, 0xA8) = 0.0f;  // a wait started: over
    } else if (s_dilating) {
        if (tasks != 0) At<float>(tasks, 0x00) = 1.0f;
        At<unsigned char>(kAudioTask, 0x1C) = 1;
        s_dilating = false;
    }
}

asm static void SlideUpdate_Original() {
    nofralloc
    stwu r1, -48(r1)
    lis r12, 0x802F
    ori r12, r12, 0xFCD8
    mtctr r12
    bctr
}

// void TLSlide::Update(float dt): the slide's time advances by dt (looping, or stopping at its end),
// its animations take that time and its children dt. A one-shot slide no longer than a transition
// goes straight to its end: its time put dt short of it. Only on a real step: SetActiveSlide resets a
// slide with Update(0), and a scene holding a slide on its first frame (the partner screen while its
// portraits load) must keep it there; nor in the frame the slide was set.
static void SlideUpdate(unsigned long slide, float dt) {
    const bool paused = At<unsigned char>(slide, 0x44) != 0;
    if (InMenus() && !paused) {
        if (dt == 0.0f) {
            NoteReset(slide);
        } else if (dt > 0.0f) {
            const long mode = At<long>(slide, 0x1C);  // 0 stops at the end, 1 loops, 2 runs on past it
            const float duration = At<float>(slide, 0x14);
            if ((mode == 0 || mode == 2) && duration <= kLongestTransition && !ResetThisFrame(slide) && !BootScreens())
                At<float>(slide, 0x18) = At<float>(slide, 0x10) + duration - dt;  // time: start + duration - dt
        }
    }
    ((void (*)(unsigned long, float))SlideUpdate_Original)(slide, dt);
}
kmBranch(0x802FFCD4, SlideUpdate);

asm static void PresentationUpdate_Original() {
    nofralloc
    lwz r4, 4(r3)
    lis r12, 0x802F
    ori r12, r12, 0xBC50
    mtctr r12
    bctr
}

// void FEPresentation::Update(float dt): a screen's presentation keeps its own clock (m_fadeDuration)
// and sets its slide's time from it. A one-shot slide short enough to be the screen coming in starts
// at its end, as the game itself does for pop-ups and the pause menu (m_fadeDuration = 999.9).
static void PresentationUpdate(unsigned long presentation, float dt) {
    const unsigned long slide = At<unsigned long>(presentation, 0x04);
    if (slide != 0 && dt > 0.0f && InMenus() && !BootScreens()) {
        const long mode = At<long>(slide, 0x1C);
        const float duration = At<float>(slide, 0x14);
        if ((mode == 0 || mode == 2) && duration <= kLongestPresentation)
            At<float>(presentation, 0x08) = At<float>(slide, 0x10) + duration - dt;  // m_fadeDuration
    }
    ((void (*)(unsigned long, float))PresentationUpdate_Original)(presentation, dt);
}
kmBranch(0x802FBC4C, PresentationUpdate);

asm static void PopupSetPositions_Original() {
    nofralloc
    stwu r1, -400(r1)
    lis r12, 0x801C
    ori r12, r12, 0x9D3C
    mtctr r12
    bctr
}

// void FEPopupMenu::SetPositions(): lays out the message and options and hides the text until Update
// shows it, once, when the popup's slide is 1 s in. The first SetPositions can't measure the text yet
// and runs again the next frame; a slide already past 1 s on its first frame (fast menus start it at
// its end) had the text shown before that second one hid it for good. So it's shown again after.
static void PopupSetPositions(unsigned long popup) {
    ((void (*)(unsigned long))PopupSetPositions_Original)(popup);
    if (InMenus() && popup != 0 && At<unsigned char>(popup, 0x99C)) At<unsigned char>(popup, 0x99D) = 0;
}
kmBranch(0x801C9D38, PopupSetPositions);

// ---- Skip intro
//
// The game boots straight to the main menu: the boot screens end at once (the studio logo keeps its few
// seconds), the intro movie is skipped as if A were pressed, and the title screen is passed as if its
// controller had pressed A, so the game's own way to the main menu runs. Once a launch: back on the
// title screen later, it waits for A as usual.

namespace {

bool s_titlePassed;

bool SkipIntro() {
    return !s_titlePassed && TweakOn(kSkipIntro);
}

}  // namespace

asm static void BootUpdate_Original() {
    nofralloc
    stwu r1, -48(r1)
    lis r12, 0x8026
    ori r12, r12, 0x3A58
    mtctr r12
    bctr
}

// void BootLoadingScene::Update(float dt): its slides run on dt (the strap screen times out on it too),
// so a long step ends each one at once. Not the studio logo's: its jingle's bank is unloaded as the
// slide ends, which under the playing jingle crashes the sound update.
static void BootUpdate(unsigned long scene, float dt) {
    if (SkipIntro() && scene != 0 && At<unsigned long>(scene, 0x28) != 3 && dt < 30.0f) dt = 30.0f;  // phase 3: the logo
    ((void (*)(unsigned long, float))BootUpdate_Original)(scene, dt);
}
kmBranch(0x80263A54, BootUpdate);

asm static void MovieAbort_Original() {
    nofralloc
    lwz r3, -5000(r13)
    lis r12, 0x801D
    ori r12, r12, 0x97B4
    mtctr r12
    bctr
}

// bool MoviePlayerScene::CheckMoviePlayerAbort(): A pressed on any controller skips the movie playing.
static bool MovieAbort() {
    const bool abort = ((bool (*)())MovieAbort_Original)();
    return abort || SkipIntro();
}
kmBranch(0x801D97B0, MovieAbort);

asm static void TitleUpdate_Original() {
    nofralloc
    stwu r1, -512(r1)
    lis r12, 0x801D
    ori r12, r12, 0x1358
    mtctr r12
    bctr
}

// void TitleScene::Update(float dt): once it takes input (1.5 s in), the press of A by the controller
// the front end listens to, unless the scene was left this frame.
static void TitleUpdate(unsigned long scene, float dt) {
    ((void (*)(unsigned long, float))TitleUpdate_Original)(scene, dt);
    if (!SkipIntro() || scene == 0) return;
    if (!At<unsigned char>(scene, 0xDE) || At<unsigned char>(scene, 0xDC)) return;  // mInitialized, mStartedDemo
    if (TopScene() != scene) return;
    s_titlePassed = true;
    OSReport("[Strikers Tweaks] skip intro: title screen passed\n");
    const unsigned long controller = *(unsigned long*)0x806E18B0;  // gFEControllerIndex
    ((void (*)(unsigned long, unsigned long, void*))0x801D22C8)(scene, controller, 0);  // OnControllerPointerPress
}
kmBranch(0x801D1354, TitleUpdate);
