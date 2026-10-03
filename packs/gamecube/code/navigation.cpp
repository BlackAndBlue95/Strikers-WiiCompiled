// Menu navigation for GameCube controllers, as Strikers Recharged has it (MenuNav in its
// runtime/src/hle/input/pad.cpp). The game's menus are pointer-only, so the D-pad and stick flicks move
// between buttons: the pointer snaps onto the next button (FEPointerRegion) in that direction, so the
// game's own hand marks the selection and its hover and click logic (A to confirm) run unchanged. A menu
// that opens starts on its first button, or on the one last selected there; B goes to the screen's
// back button and presses it (on the pause menu, the pause button); L and R page as - and +; ring
// menus (the main menu) pick the button at the stick's angle.
#include <kamek.h>

#include "gamecube.h"

extern "C" {
double sin(double x);
double cos(double x);
double atan2(double y, double x);
double sqrt(double x);
void* memset(void* to, int value, unsigned long size);
}

namespace {

template <typename T> inline T& At(unsigned long base, unsigned long offset) {
    return *(T*)(base + offset);
}

// Whether `p` points into the console's memory (MEM1 or MEM2): every pointer read from the game's
// structures is checked before it's followed.
bool Valid(unsigned long p) {
    return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000);
}

const unsigned long kPointerManager = 0x806E2030;      // g_pFEPointerManager
const unsigned long kPointerPositions = 0x80578460;    // gFEPointerPositions[4]: centre origin, +y up
const unsigned long kPointerInstances = 0x80578450;    // gFEPointerInstances[4]: the hands
const unsigned long kRegionContainsPoint = 0x8030131C; // FEPointerRegion::ContainsPoint (vtable +0x2C)
const unsigned long kBackButtonVtable = 0x8051D5E4;    // FEBackButton
const unsigned long kFEInput = 0x806E2038;             // g_pFEInput (m_InputLockDepth +0x20)
const unsigned long kSceneManager = 0x806E1838;        // GameSceneManager (depth +0x04, handlers +0x88)
const unsigned long kStadiumSelectVtable = 0x80519C70; // StadiumSelectScene
const unsigned long kScreenInfo = 0x8057F1F8;          // _ScreenInfo {ScreenWidth, ScreenHeight}
const unsigned long kWidescreen = 0x806E1958;          // sWidescreen

// Scenes that keep a character grid's buttons live while the grid is hidden: vtable, the "grid shown"
// flag, the grid's first FEPointerButton (0xB4 bytes each) and their number.
struct HiddenGrid {
    unsigned long vtable, shownFlag, buttons, count;
};
const HiddenGrid kHiddenGrids[] = {
    {0x8051D168, 0x4C, 0xB0, 12},  // ChooseCaptainsSceneV2: mCaptainsShown, mCaptainButtons
    {0x8051D584, 0x4C, 0xE8, 8},   // ChooseSidekicksSceneV2: mSidekicksShown, mSidekickButtons
};

// Frames (navigation runs once a frame): first repeat of a held direction, the next ones, hiding the
// hand while B goes back, and resting off every button before the default is selected.
const int kRepeatFirst = 21, kRepeatNext = 8, kHideHandFrames = 20, kOffButtonFrames = 6;

float Abs(float v) {
    return v < 0.0f ? -v : v;
}
float Length(float x, float y) {
    return (float)sqrt(x * x + y * y);
}
bool Finite(float v) {
    return v == v && v - v == 0.0f;
}
float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

struct Point {
    float x, y;
};
Point MakePoint(float x, float y) {
    Point p;
    p.x = x;
    p.y = y;
    return p;
}
bool Same(Point a, Point b) {
    return a.x == b.x && a.y == b.y;
}

struct Button {
    float minX, maxX, minY, maxY, rot, pivotX, pivotY;
    Point centre;
    Point aim;  // where the pointer goes to select it
    bool back;
    unsigned long listener;
};

// As FEPointerRegion::ContainsPoint.
bool Contains(const Button& b, Point p) {
    if (b.rot != 0.0f) {
        const float lx = p.x - b.pivotX, ly = p.y - b.pivotY;
        const float c = (float)cos(b.rot), s = (float)sin(b.rot);
        p = MakePoint(lx * c + ly * s + b.pivotX, -lx * s + ly * c + b.pivotY);
    }
    return p.x >= b.minX && p.x <= b.maxX && p.y >= b.minY && p.y <= b.maxY;
}

const int kMaxButtons = 64;
Button s_buttons[kMaxButtons];
int s_count;

struct State {
    unsigned long heldDir;
    unsigned long repeatAt;
    Point goal, aim;
    int settleFrames;
    bool backHeld;
    int backFrames;      // B pressed: heading for the back button
    int pressFrames;     // tapping A on it
    int hideHandFrames;  // the hand hidden while B goes back
    int pageFrames, pageDir;
    int pauseFrames;     // tapping the pause button (B on an in-match overlay)
    unsigned long scene;      // top scene last frame, to spot screen changes
    unsigned long memoryKey;  // this screen's key in the remembered selections
    int heldPage;
    bool hadButtons;  // buttons last frame (false: a menu or overlay just appeared)
    bool overlay;     // this menu appeared over an unchanged scene (the pause menu)
    unsigned long sceneWhenEmpty;
    unsigned long selectedListener;  // the button last selected (buttons move: a screen slides out)
    int offButtonFrames;
};
State s_state[4];
unsigned long s_frames[4];
// Player 1 always navigates; other controllers join in once they're used, so an idle one doesn't
// snap its pointer around in menus player 1 is driving.
bool s_engaged[4] = {true, false, false, false};

// Where each controller last was on each screen, so coming back to a screen or closing a pop-up
// returns to that button.
struct Remembered {
    unsigned long key;
    Point at;
};
const int kRemembered = 16;
Remembered s_remembered[4][kRemembered];
int s_rememberNext[4];

const Remembered* Recall(int chan, unsigned long key) {
    for (int i = 0; i < kRemembered; ++i)
        if (s_remembered[chan][i].key == key) return &s_remembered[chan][i];
    return 0;
}
void Remember(int chan, unsigned long key, Point at) {
    Remembered* slot = (Remembered*)Recall(chan, key);
    if (slot == 0) {
        slot = &s_remembered[chan][s_rememberNext[chan]];
        s_rememberNext[chan] = (s_rememberNext[chan] + 1) % kRemembered;
    }
    slot->key = key;
    slot->at = at;
}

// The selected button, for remembering.
Button s_selected[4];
bool s_hasSelected[4];

void SelectAt(int chan, Point centre) {
    for (int i = 0; i < s_count; ++i) {
        if (Same(s_buttons[i].centre, centre)) {
            s_selected[chan] = s_buttons[i];
            s_hasSelected[chan] = true;
            return;
        }
    }
}

// The menu canvas the pointer moves on: GetPointerPosition (feDPD) maps KPAD's pos to it as
// (x * w/2, -y * h/2), w = 854 in widescreen, else the screen's width; h its height.
Point HalfExtents() {
    const float w = *(unsigned char*)kWidescreen ? 854.0f : (float)*(int*)kScreenInfo;
    const float h = (float)*(int*)(kScreenInfo + 4);
    if (w > 100.0f && h > 100.0f) return MakePoint(w * 0.5f, h * 0.5f);
    return MakePoint(320.0f, 240.0f);
}

Point PointerPosition(int chan) {
    return MakePoint(At<float>(kPointerPositions, chan * 8), At<float>(kPointerPositions, chan * 8 + 4));
}

// The scene on top of the game's scene stack, or 0.
unsigned long CurrentScene() {
    const unsigned long manager = *(unsigned long*)kSceneManager;
    if (!Valid(manager)) return 0;
    const unsigned long depth = At<unsigned long>(manager, 0x04);
    if (depth == 0 || depth > 32) return 0;
    const unsigned long scene = At<unsigned long>(manager, 0x88 + (depth - 1) * 4);
    return Valid(scene) ? scene : 0;
}
unsigned long SceneVtable(unsigned long scene) {
    return scene ? At<unsigned long>(scene, 0) : 0;
}

// Every live pointer region (button), in screen space. Scenes feed pointer events only to their own
// buttons, so buttons of screens that are registered but not shown never see one: a button counts as
// live only if its last event for this controller is at the pointer's current position.
void CollectButtons(int chan) {
    s_count = 0;
    const unsigned long manager = *(unsigned long*)kPointerManager;
    if (!Valid(manager)) return;
    const unsigned long input = *(unsigned long*)kFEInput;
    const bool inputLocked = Valid(input) && At<unsigned long>(input, 0x20) != 0;
    const Point at = PointerPosition(chan);
    unsigned long hiddenFrom = 0, hiddenTo = 0;  // the listeners of a hidden character grid
    const unsigned long scene = CurrentScene();
    if (scene) {
        const unsigned long vtable = SceneVtable(scene);
        for (unsigned int i = 0; i < sizeof(kHiddenGrids) / sizeof(kHiddenGrids[0]); ++i) {
            const HiddenGrid& g = kHiddenGrids[i];
            if (vtable == g.vtable && !At<unsigned char>(scene, g.shownFlag)) {
                hiddenFrom = scene + g.buttons;
                hiddenTo = hiddenFrom + g.count * 0xB4;
            }
        }
    }
    // mListeners: {?, head, tail}; entries {next, listener}.
    unsigned long entry = At<unsigned long>(manager, 4);
    for (int guard = 0; Valid(entry) && guard < 512 && s_count < kMaxButtons; ++guard, entry = At<unsigned long>(entry, 0)) {
        const unsigned long listener = At<unsigned long>(entry, 4);
        if (!Valid(listener) || (listener >= hiddenFrom && listener < hiddenTo)) continue;
        const unsigned long vtable = At<unsigned long>(listener, 0);
        if (!Valid(vtable) || At<unsigned long>(vtable, 0x2C) != kRegionContainsPoint) continue;
        if (At<unsigned char>(listener, 0x80)) continue;                  // mDisabled
        if (inputLocked && !At<unsigned char>(listener, 0x81)) continue;  // !mIgnoreInputLock
        const unsigned long last = listener + 0x3C + chan * 0x10;         // mPreviousEvents[chan]
        if (At<int>(last, 0) != chan || Abs(At<float>(last, 4) - at.x) > 1.0f || Abs(At<float>(last, 8) - at.y) > 1.0f)
            continue;
        Button b;
        b.minX = At<float>(listener, 0x84);
        b.maxX = At<float>(listener, 0x88);
        b.maxY = At<float>(listener, 0x8C);
        b.minY = At<float>(listener, 0x90);
        if (!(b.maxX > b.minX && b.maxY > b.minY) || b.maxX - b.minX > 600.0f || b.maxY - b.minY > 420.0f) continue;
        b.rot = At<float>(listener, 0x94);
        b.pivotX = At<float>(listener, 0x98);
        b.pivotY = At<float>(listener, 0x9C);
        b.centre = MakePoint((b.minX + b.maxX) * 0.5f, (b.minY + b.maxY) * 0.5f);
        if (b.rot != 0.0f) {
            const float dx = b.centre.x - b.pivotX, dy = b.centre.y - b.pivotY;
            const float c = (float)cos(b.rot), s = (float)sin(b.rot);
            b.centre = MakePoint(b.pivotX + dx * c - dy * s, b.pivotY + dx * s + dy * c);
        }
        // Where the pointer goes: low in the button, so the hand (drawn below its fingertip) leaves
        // the label visible; small or rotated buttons (ring icons) take the centre.
        b.aim = b.centre;
        if (b.rot == 0.0f && b.maxY - b.minY > 24.0f) b.aim.y = b.minY + (b.maxY - b.minY) * 0.3f;
        b.back = vtable == kBackButtonVtable;
        b.listener = listener;
        if (Finite(b.centre.x) && Finite(b.centre.y)) s_buttons[s_count++] = b;
    }
}

// The next button in the pressed direction. Buttons lined up with the current one (overlapping it
// across the direction of travel: the same row for left/right, the same column for up/down) come first,
// nearest first; otherwise the nearest, weighting sideways distance. Off the end of a row or column it
// wraps to the far end of it. The back button is never a target (B does that).
bool PickInDirection(const Button* current, Point from, unsigned long dir, Point& out) {
    const bool horizontal = (dir & (kNavLeft | kNavRight)) != 0;
    const float dx = (dir & kNavRight) ? 1.0f : (dir & kNavLeft) ? -1.0f : 0.0f;
    const float dy = (dir & kNavUp) ? 1.0f : (dir & kNavDown) ? -1.0f : 0.0f;
    // The span to line up against: the current button, or a small box around the pointer.
    const float lo = current ? (horizontal ? current->minY : current->minX) : (horizontal ? from.y : from.x) - 4.0f;
    const float hi = current ? (horizontal ? current->maxY : current->maxX) : (horizontal ? from.y : from.x) + 4.0f;
    float best = 1e30f, wrapBest = -1.0f;
    Point wrap = MakePoint(0.0f, 0.0f);
    for (int i = 0; i < s_count; ++i) {
        const Button& b = s_buttons[i];
        if (&b == current || b.back) continue;
        const float vx = b.centre.x - from.x, vy = b.centre.y - from.y;
        const float forward = vx * dx + vy * dy;
        const float bLo = horizontal ? b.minY : b.minX, bHi = horizontal ? b.maxY : b.maxX;
        float gap = bLo - hi > lo - bHi ? bLo - hi : lo - bHi;
        if (gap < 0.0f) gap = 0.0f;
        if (forward >= 8.0f) {
            const float score = gap == 0.0f ? forward : 100000.0f + forward + 3.0f * gap;
            if (score < best) {
                best = score;
                out = b.centre;
            }
        } else if (forward <= -8.0f && gap == 0.0f && -forward > wrapBest) {
            wrapBest = -forward;  // the farthest lined-up button behind
            wrap = b.centre;
        }
    }
    if (best < 1e30f) return true;
    if (wrapBest > 0.0f) {
        out = wrap;
        return true;
    }
    return false;
}

// The selection a menu opens on: the first button in reading order (top row first, then left to
// right), past the back button and controls at the screen's edges (page dots, side arrows). With
// preferWidest (stage select) the widest wins instead: PLAY NOW over the arrows.
Point PickDefault(bool preferWidest) {
    const Point half = HalfExtents();
    const Button* best = 0;
    for (int pass = 0; pass < 2 && best == 0; ++pass) {
        for (int i = 0; i < s_count; ++i) {
            const Button& b = s_buttons[i];
            if (b.back) continue;
            if (pass == 0 && (Abs(b.centre.x) > half.x - 50.0f || b.centre.y > half.y - 50.0f)) continue;
            if (best == 0) {
                best = &b;
            } else if (preferWidest) {
                if (b.maxX - b.minX > best->maxX - best->minX) best = &b;
            } else if (b.centre.y > best->centre.y + 20.0f ||
                       (Abs(b.centre.y - best->centre.y) <= 20.0f && b.centre.x < best->centre.x)) {
                best = &b;  // a higher row, or the same row further left
            }
        }
    }
    return best ? best->centre : s_buttons[0].centre;
}

// The hand, hidden while B goes back: scenes show it again every frame, but nothing touches its
// colour, so the hand and its child graphics get a colour override with no alpha, the originals kept
// to put back. TLInstance: m_next +0x00, pChildren +0x08, the overriding colour +0x6D (RGBA),
// m_overloadFlags +0x84 (0x10: colour overridden).
struct SavedColour {
    unsigned long instance, flags;
    unsigned char rgba[4];
};
const int kMaxInstances = 64;
struct HandState {
    unsigned long hand;
    SavedColour saved[kMaxInstances];
    int count;
};
HandState s_hands[4];

int CollectInstances(unsigned long instance, unsigned long* out, int count) {
    for (int guard = 0; Valid(instance) && guard < kMaxInstances && count < kMaxInstances; ++guard) {
        for (int i = 0; i < count; ++i)
            if (out[i] == instance) return count;
        out[count++] = instance;
        count = CollectInstances(At<unsigned long>(instance, 0x08), out, count);
        instance = At<unsigned long>(instance, 0x00);
    }
    return count;
}

void SetHandVisible(int chan, bool visible) {
    HandState& hs = s_hands[chan];
    const unsigned long hand = At<unsigned long>(kPointerInstances, chan * 4);
    if (hand != hs.hand) {  // a new scene and hand: the old instances may be gone
        hs.hand = hand;
        hs.count = 0;
    }
    if (!Valid(hand)) return;
    if (visible) {
        for (int i = 0; i < hs.count; ++i) {
            const SavedColour& c = hs.saved[i];
            At<unsigned long>(c.instance, 0x84) = c.flags;
            for (int k = 0; k < 4; ++k) At<unsigned char>(c.instance, 0x6D + k) = c.rgba[k];
        }
        hs.count = 0;
        return;
    }
    unsigned long tree[kMaxInstances];
    int count = 0;
    tree[count++] = hand;
    count = CollectInstances(At<unsigned long>(hand, 0x08), tree, count);
    for (int t = 0; t < count; ++t) {
        const unsigned long instance = tree[t];
        bool saved = false;
        for (int i = 0; i < hs.count && !saved; ++i) saved = hs.saved[i].instance == instance;
        if (!saved && hs.count < kMaxInstances) {
            SavedColour& c = hs.saved[hs.count++];
            c.instance = instance;
            c.flags = At<unsigned long>(instance, 0x84);
            for (int k = 0; k < 4; ++k) c.rgba[k] = At<unsigned char>(instance, 0x6D + k);
        }
        At<unsigned long>(instance, 0x84) |= 0x10;
        At<unsigned char>(instance, 0x70) = 0;  // alpha
    }
}

// Select the button whose centre is `goal`: the pointer goes to its aim point, which the game reads
// on its next update.
void SnapTo(State& st, Point goal, float pointer[2]) {
    const Point half = HalfExtents();
    st.goal = goal;
    st.aim = goal;
    for (int i = 0; i < s_count; ++i) {
        if (Same(s_buttons[i].centre, goal)) {
            st.aim = s_buttons[i].aim;
            break;
        }
    }
    st.settleFrames = 4;  // until the game has caught up with the new position
    pointer[0] = Clamp(st.aim.x / half.x, -1.0f, 1.0f);
    pointer[1] = Clamp(-st.aim.y / half.y, -1.0f, 1.0f);
}

}  // namespace

NavResult UpdateNavigation(int chan, unsigned long dpad, float stickX, float stickY, float cStickX, float cStickY,
                           bool back, int pageInput, float pointer[2]) {
    NavResult r;
    r.menu = r.pressA = r.pressPause = false;
    r.page = 0;
    State& st = s_state[chan];
    const unsigned long now = ++s_frames[chan];
    // The stronger stick navigates alongside the D-pad.
    Point stick = MakePoint(stickX, stickY);
    if (Length(cStickX, cStickY) > Length(stickX, stickY)) stick = MakePoint(cStickX, cStickY);
    unsigned long dir = dpad;
    if (stick.y > 0.6f) dir |= kNavUp;
    if (stick.y < -0.6f) dir |= kNavDown;
    if (stick.x < -0.6f) dir |= kNavLeft;
    if (stick.x > 0.6f) dir |= kNavRight;

    // A menu is up whenever live pointer buttons exist (none during matches).
    CollectButtons(chan);
    s_hasSelected[chan] = false;
    if (dir || back || Length(stick.x, stick.y) > 0.5f) s_engaged[chan] = true;
    if (!s_engaged[chan]) return r;  // an idle extra controller: its pointer left alone
    if (s_count == 0) {
        const unsigned long scene = CurrentScene();
        memset(&st, 0, sizeof(st));
        st.sceneWhenEmpty = scene;
        return r;
    }
    r.menu = true;
    // One direction at a time; the first press moves at once, holding repeats.
    if (dir & (kNavUp | kNavDown))
        dir &= kNavUp | kNavDown;
    else
        dir &= kNavLeft | kNavRight;
    if (((dir & kNavUp) && (dir & kNavDown)) || ((dir & kNavLeft) && (dir & kNavRight))) dir = 0;
    bool step = false;
    if (dir && dir != st.heldDir) {
        step = true;
        st.repeatAt = now + kRepeatFirst;
    } else if (dir && now >= st.repeatAt) {
        step = true;
        st.repeatAt = now + kRepeatNext;
    }
    st.heldDir = dir;
    const bool backPressed = back && !st.backHeld;
    st.backHeld = back;

    if (st.hideHandFrames > 0) --st.hideHandFrames;
    SetHandVisible(chan, st.hideHandFrames == 0);  // the game's own hand marks the selection
    const Point at = PointerPosition(chan);
    // The selection follows the button being moved to while a snap settles, else whatever the pointer
    // rests on.
    if (st.settleFrames > 0) {
        SelectAt(chan, st.goal);
    } else {
        for (int i = 0; i < s_count; ++i) {
            if (Contains(s_buttons[i], at)) {
                s_selected[chan] = s_buttons[i];
                s_hasSelected[chan] = true;
                break;
            }
        }
    }
    // A new screen (another scene on top, or buttons appearing after none) starts on its default
    // selection, or the one remembered for it, wherever the pointer was left.
    const unsigned long scene = CurrentScene();
    const bool stageSelect = SceneVtable(scene) == kStadiumSelectVtable;
    if (scene != st.scene || !st.hadButtons) {
        // In-match overlays (the pause menu) appear without a scene change.
        st.overlay = !st.hadButtons && scene == st.sceneWhenEmpty;
        st.scene = scene;
        st.hadButtons = true;
        st.memoryKey = SceneVtable(scene) ^ (st.overlay ? 1u : 0u);
        Point start = PickDefault(stageSelect);
        const Remembered* remembered = Recall(chan, st.memoryKey);
        if (remembered) {
            for (int i = 0; i < s_count; ++i) {
                if (Same(s_buttons[i].centre, remembered->at)) {
                    start = remembered->at;
                    break;
                }
            }
        }
        SnapTo(st, start, pointer);
        SelectAt(chan, st.goal);
        return r;
    }
    if (s_hasSelected[chan]) Remember(chan, st.memoryKey, s_selected[chan].centre);
    // L and R page or switch tabs on any screen (the game's - and +), as the shoulder buttons do in SMS.
    if (st.pageFrames > 0) {
        --st.pageFrames;
        r.page = st.pageFrames >= 2 ? st.pageDir : 0;
    }
    if (pageInput != 0 && pageInput != st.heldPage) {
        st.pageDir = pageInput;
        st.pageFrames = 4;
        r.page = pageInput;
    }
    st.heldPage = pageInput;
    // B: to the back button, then a tap of A on it. Overlays without one (the pause menu) close with
    // the game's own pause button instead.
    if (backPressed) {
        const Button* backButton = 0;
        for (int i = 0; i < s_count && backButton == 0; ++i)
            if (s_buttons[i].back) backButton = &s_buttons[i];
        if (backButton) {
            // The hand hides, the pointer goes to BACK and A taps there next frame (the screen changes
            // before anything is seen moving).
            SnapTo(st, backButton->centre, pointer);
            st.backFrames = 2;
            st.hideHandFrames = kHideHandFrames;
        } else if (st.overlay) {
            st.pauseFrames = 4;
        }
    }
    if (st.pauseFrames > 0) {
        --st.pauseFrames;
        r.pressPause = st.pauseFrames >= 2;
    }
    if (st.pressFrames > 0) {
        --st.pressFrames;
        r.pressA = st.pressFrames >= 2;
        return r;
    }
    if (st.backFrames > 0 && --st.backFrames == 0) {
        st.pressFrames = 3;
        r.pressA = true;
        return r;
    }
    // Ring menus (the main menu): a stick or D-pad direction selects the button at that angle around
    // the ring's centre, following the stick as it turns. The back button stays on B.
    const Button* ring[kMaxButtons];
    int ringCount = 0;
    Point ringCentre = MakePoint(0.0f, 0.0f);
    for (int i = 0; i < s_count; ++i)
        if (!s_buttons[i].back) ring[ringCount++] = &s_buttons[i];
    if (ringCount >= 5) {
        for (int i = 0; i < ringCount; ++i) {
            ringCentre.x += ring[i]->centre.x;
            ringCentre.y += ring[i]->centre.y;
        }
        ringCentre.x /= ringCount;
        ringCentre.y /= ringCount;
        float lo = 1e30f, hi = 0.0f, sum = 0.0f;
        for (int i = 0; i < ringCount; ++i) {
            const float d = Length(ring[i]->centre.x - ringCentre.x, ring[i]->centre.y - ringCentre.y);
            if (d < lo) lo = d;
            if (d > hi) hi = d;
            sum += d;
        }
        if (sum <= 0.0f || (hi - lo) / (sum / ringCount) > 0.35f) ringCount = 0;
    } else {
        ringCount = 0;
    }
    if (ringCount > 0 && !stageSelect) {
        Point aim = stick;
        if (Length(aim.x, aim.y) < 0.55f) {
            aim = MakePoint((dir & kNavRight) ? 1.0f : (dir & kNavLeft) ? -1.0f : 0.0f,
                            (dir & kNavUp) ? 1.0f : (dir & kNavDown) ? -1.0f : 0.0f);
            if (!step) aim = MakePoint(0.0f, 0.0f);
        }
        if (aim.x != 0.0f || aim.y != 0.0f) {
            const float want = (float)atan2(aim.y, aim.x);
            const Button* best = 0;
            float bestDiff = 1e30f;
            for (int i = 0; i < ringCount; ++i) {
                float diff = Abs((float)atan2(ring[i]->centre.y - ringCentre.y, ring[i]->centre.x - ringCentre.x) - want);
                if (diff > 3.14159265f) diff = 6.2831853f - diff;
                if (diff < bestDiff) {
                    bestDiff = diff;
                    best = ring[i];
                }
            }
            if (best && !Contains(*best, at) && !(st.settleFrames > 0 && Same(st.goal, best->centre))) {
                SnapTo(st, best->centre, pointer);
                SelectAt(chan, st.goal);
            }
        }
    }
    const Button* current = 0;
    for (int i = 0; i < s_count && current == 0; ++i)
        if (Contains(s_buttons[i], at)) current = &s_buttons[i];
    Point goal;
    if (ringCount == 0 && step && PickInDirection(current, at, dir, goal)) {
        SnapTo(st, goal, pointer);
        SelectAt(chan, st.goal);
        return r;
    }
    if (st.settleFrames > 0) {
        // The game picks the new position up on its next update; wait for it before checking again.
        --st.settleFrames;
        if (Length(st.aim.x - at.x, st.aim.y - at.y) < 2.0f) st.settleFrames = 0;
        return r;
    }
    // Keep something selected. Off every button, the pointer follows the button it was on if that's
    // still there (moved: a screen's buttons slide as it comes or goes), and only after a few frames
    // goes to the default: a screen in transition briefly shows its buttons elsewhere, and snapping
    // there every other frame makes the pointer jitter.
    if (current) {
        st.offButtonFrames = 0;
        if (s_hasSelected[chan]) st.selectedListener = s_selected[chan].listener;
        return r;
    }
    if (st.selectedListener != 0) {
        for (int i = 0; i < s_count; ++i) {
            if (s_buttons[i].listener == st.selectedListener) {
                SnapTo(st, s_buttons[i].centre, pointer);
                SelectAt(chan, st.goal);
                return r;
            }
        }
    }
    if (++st.offButtonFrames < kOffButtonFrames) return r;
    st.offButtonFrames = 0;
    SnapTo(st, PickDefault(stageSelect), pointer);
    SelectAt(chan, st.goal);
    return r;
}
