#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "input_bindings.h"
#include "wii_remote_input.h"
#include "runtime_config.h"
#include "aurora_events.h"

#include <algorithm>
#include <cmath>
#include <SDL3/SDL_timer.h>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <imgui.h>
#include <cfloat>
#include <dolphin/pad.h>

namespace {

bool NativeButtonHeld(SDL_Gamepad* gamepad, uint32_t nativeButton) {
    if (gamepad == nullptr || nativeButton == PAD_NATIVE_BUTTON_INVALID ||
        nativeButton >= SDL_GAMEPAD_BUTTON_COUNT) {
        return false;
    }
    return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(nativeButton));
}

// A digital button bound to L or R has no analog travel of its own. On real
// hardware the click only engages at full depression, so report a full pull.
void FillTriggersHeldByButtons(PADStatus* statuses) {
    if (InputBindings::InputBlocked()) {
        return;
    }
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) {
            continue;
        }
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) {
            continue;
        }
        SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
        if (gamepad == nullptr) {
            continue;
        }
        const auto scan = [&](PADButtonMapping* mappings, u32 count) {
            if (mappings == nullptr) {
                return;
            }
            for (u32 i = 0; i < count; ++i) {
                const PADButtonMapping& mapping = mappings[i];
                if (mapping.padButton != PAD_TRIGGER_L && mapping.padButton != PAD_TRIGGER_R) {
                    continue;
                }
                if (!NativeButtonHeld(gamepad, mapping.nativeButton)) {
                    continue;
                }
                if (mapping.padButton == PAD_TRIGGER_L) {
                    statuses[port].triggerLeft = 255;
                } else {
                    statuses[port].triggerRight = 255;
                }
            }
        };
        u32 count = 0;
        scan(PADGetButtonMappings(port, &count), count);
        count = 0;
        scan(PADGetAltButtonMappings(port, &count), count);
    }
}

void WritePadStatus(uint32_t base, const PADStatus& status) {
    const auto guestStatus = PadStatusContract::Encode({
        status.button,
        status.stickX,
        status.stickY,
        status.substickX,
        status.substickY,
        status.triggerL,
        status.triggerR,
        status.analogA,
        status.analogB,
        status.err,
    });
    uint8_t* dst = Memory::GetPointer(base, guestStatus.size());
    std::memcpy(dst, guestStatus.data(), guestStatus.size());
}

} // namespace

// MSC: Mario Strikers Charged only reads Wii Remote + Nunchuk (WPAD/KPAD). With no real
// Bluetooth remote on a channel, emulate one from that port's bound GameCube-pad input
// (keyboard or any SDL gamepad, as configured in the F10 overlay).
namespace MscEmulatedRemote {
static float s_cursor[PAD_CHANMAX][2]{};
void RecenterPointer(uint32_t chan) {
    if (chan < PAD_CHANMAX) s_cursor[chan][0] = s_cursor[chan][1] = 0.0f;
}
namespace {
constexpr uint32_t kWLeft = 0x0001, kWRight = 0x0002, kWDown = 0x0004, kWUp = 0x0008, kWPlus = 0x0010,
                   kWTwo = 0x0100, kWOne = 0x0200, kWB = 0x0400, kWA = 0x0800, kWMinus = 0x1000,
                   kWZ = 0x2000, kWC = 0x4000;
// No Wii button sets these bits; they tag the remote's button word with "this player uses a
// controller on channel N" for the GameCube DetInput conversion (msc_game.cpp).
constexpr uint32_t kMscGameCubeTag = 0x0080, kMscGameCubeChannelShift = 5;  // channel in 0x0060

bool ReadBoundPad(uint32_t chan, PADStatus& out) {
    if (chan >= PAD_CHANMAX) return false;
    PADStatus statuses[PAD_CHANMAX]{};
    PADRead(statuses);
    FillTriggersHeldByButtons(statuses);
    InputBindings::Apply(statuses);
    out = statuses[chan];
    return out.err == PAD_ERR_NONE;
}
} // namespace

bool Present(uint32_t chan) {
    PADStatus pad{};
    return ReadBoundPad(chan, pad);
}

// Menu navigation (mod, F10 > Mods > menu_navigation): MSC's menus are pointer-only. To make them feel like a console menu, the
// D-pad and left-stick flicks move a highlight between buttons: the emulated pointer snaps onto the
// nearest FEPointerRegion in that direction and the hand is hidden, so the button's own hover
// highlight is the selection and the game's hover/click logic (A to confirm) runs unchanged. When
// nothing is highlighted (a menu just opened), the nearest button is selected. Moving the right
// stick brings the hand back as a free pointer until the D-pad is used again.
namespace MenuNav {
constexpr uint32_t kPointerManagerPtr = 0x806E2030u;   // g_pFEPointerManager
constexpr uint32_t kPointerPositions = 0x80578460u;    // gFEPointerPositions[4]: centre origin, +y up
constexpr uint32_t kPointerInstances = 0x80578450u;    // gFEPointerInstances[4] (the hand)
constexpr uint32_t kRegionContainsPoint = 0x8030131Cu; // FEPointerRegion::ContainsPoint (vtable +0x2C)
constexpr uint32_t kBackButtonVtable = 0x8051D5E4u;    // __vt__12FEBackButton
constexpr uint32_t kFEInputPtr = 0x806E2038u;          // g_pFEInput (m_InputLockDepth at +0x20)
constexpr uint32_t kSceneManagerPtr = 0x806E1838u;     // nlSingleton<GameSceneManager>::s_pInstance
constexpr uint32_t kStadiumSelectVtable = 0x80519C70u; // __vt__18StadiumSelectScene
// Scenes that keep a character grid's buttons live while the grid is hidden: {vtable, "grid shown"
// flag, first grid FEPointerButton (0xB4 bytes each), count}.
struct HiddenGrid { uint32_t vtable, shownFlag, buttons, count; };
constexpr HiddenGrid kHiddenGrids[] = {
    {0x8051D168u, 0x4C, 0xB0, 12},  // ChooseCaptainsSceneV2: mCaptainsShown, mCaptainButtons
    {0x8051D584u, 0x4C, 0xE8, 8},   // ChooseSidekicksSceneV2: mSidekicksShown, mSidekickButtons
};
enum : uint32_t { kNavUp = 1, kNavDown = 2, kNavLeft = 4, kNavRight = 8 };

struct Point { float x, y; };
struct Button {
    float minX, maxX, minY, maxY, rot, pivotX, pivotY;
    Point centre;
    Point aim;  // where the pointer is placed to select it
    bool back;
    uint32_t listener;
    // Same test as FEPointerRegion::ContainsPoint.
    bool Contains(Point p) const {
        if (rot != 0.0f) {
            const float lx = p.x - pivotX, ly = p.y - pivotY, c = std::cos(rot), sn = std::sin(rot);
            p = {lx * c + ly * sn + pivotX, -lx * sn + ly * c + pivotY};
        }
        return p.x >= minX && p.x <= maxX && p.y >= minY && p.y <= maxY;
    }
};
struct State {
    uint32_t heldDir = 0;
    uint64_t repeatAtMs = 0;
    Point goal{};
    Point aim{};
    int settleFrames = 0;
    bool backHeld = false;
    int backFrames = 0;       // B pressed: heading for the back button
    int pressFrames = 0;      // tapping A on it
    int hideHandFrames = 0;   // hand hidden while B goes back
    int pageFrames = 0;       // tapping +/- (stage select)
    int pageDir = 0;
    int pauseFrames = 0;      // tapping the pause button (B on an in-match overlay)
    uint32_t scene = 0;       // top scene last frame, to spot screen changes
    uint32_t memoryKey = 0;   // s_focusMemory key for this screen
    int heldPage = 0;         // L/R page input last frame
    bool hadButtons = false;  // buttons last frame (false: a menu/overlay just appeared)
    bool overlay = false;     // this menu appeared over an unchanged scene (e.g. the pause menu)
    uint32_t sceneWhenEmpty = 0;
};
State s_state[PAD_CHANMAX];
// Player 1 always navigates; other pads join in once they're used, so an idle second
// controller doesn't snap its pointer around or show a badge in menus player 1 is driving.
bool s_engaged[PAD_CHANMAX] = {true, false, false, false};
// Where each pad last was on each screen (keyed by scene vtable, in-match overlays apart), so
// coming back to a screen or closing a pop-up returns to that button.
std::map<uint32_t, Point> s_focusMemory[PAD_CHANMAX];
// The selected button per pad, for the highlight overlay (DrawHighlight).
Button s_selected[PAD_CHANMAX];
bool s_hasSelected[PAD_CHANMAX]{};
void SelectAt(uint32_t chan, const std::vector<Button>& buttons, Point centre) {
    for (const Button& b : buttons) {
        if (b.centre.x == centre.x && b.centre.y == centre.y) { s_selected[chan] = b; s_hasSelected[chan] = true; return; }
    }
}
// The menu canvas the pointer moves on: GetPointerPosition (feDPD) maps KPAD pos to screen as
// (x * w/2, -y * h/2), w = 854 in widescreen else _ScreenInfo.ScreenWidth, h = ScreenHeight.
constexpr uint32_t kScreenInfo = 0x8057F1F8u;  // _ScreenInfo {int ScreenWidth, ScreenHeight, ...}
constexpr uint32_t kWidescreenFlag = 0x806E1958u;  // sWidescreen
Point HalfExtents() {
    try {
        const float w = Memory::Read8(kWidescreenFlag) ? 854.0f : static_cast<float>(static_cast<int32_t>(Memory::Read32(kScreenInfo)));
        const float h = static_cast<float>(static_cast<int32_t>(Memory::Read32(kScreenInfo + 4)));
        if (w > 100.0f && h > 100.0f) return {w * 0.5f, h * 0.5f};
    } catch (...) {}
    return {320.0f, 240.0f};
}

Point PointerPosition(uint32_t chan) {
    return {Memory::ReadFloat32(kPointerPositions + chan * 8), Memory::ReadFloat32(kPointerPositions + chan * 8 + 4)};
}

// The scene handler on top of the game's scene stack (BaseGameSceneManager: depth +0x04,
// handlers +0x88), or 0.
uint32_t CurrentScene() {
    try {
        const uint32_t mgr = Memory::Read32(kSceneManagerPtr);
        const uint32_t depth = mgr ? Memory::Read32(mgr + 0x04) : 0;
        if (depth == 0 || depth > 32) return 0;
        return Memory::Read32(mgr + 0x88 + (depth - 1) * 4);
    } catch (...) { return 0; }
}
uint32_t CurrentSceneVtable() {
    const uint32_t scene = CurrentScene();
    try { return scene ? Memory::Read32(scene) : 0; } catch (...) { return 0; }
}

// Every live pointer region (button), in screen space. Scenes feed pointer events only to their
// own buttons, so buttons of screens that are registered but not shown never see an event: a
// button counts as live only if its last event for this pad is at the pointer's current position.
std::vector<Button> CollectButtons(uint32_t chan) {
    std::vector<Button> out;
    try {
        const uint32_t mgr = Memory::Read32(kPointerManagerPtr);
        const uint32_t input = Memory::Read32(kFEInputPtr);
        const bool inputLocked = input && Memory::Read32(input + 0x20) != 0;
        const Point at = PointerPosition(chan);
        uint32_t hiddenFrom = 0, hiddenTo = 0;  // listener range of a hidden character grid
        if (const uint32_t scene = CurrentScene()) {
            const uint32_t vtable = Memory::Read32(scene);
            for (const HiddenGrid& g : kHiddenGrids) {
                if (vtable == g.vtable && !Memory::Read8(scene + g.shownFlag)) {
                    hiddenFrom = scene + g.buttons;
                    hiddenTo = hiddenFrom + g.count * 0xB4;
                }
            }
        }
        // mListeners (nlListContainer) is {?, head, tail}; entries are {next, listener}.
        uint32_t entry = mgr ? Memory::Read32(mgr + 4) : 0;
        for (int guard = 0; entry && guard < 512; ++guard, entry = Memory::Read32(entry)) {
            const uint32_t listener = Memory::Read32(entry + 4);
            if (!listener || (listener >= hiddenFrom && listener < hiddenTo)) continue;
            const uint32_t vtable = Memory::Read32(listener);
            if (Memory::Read32(vtable + 0x2C) != kRegionContainsPoint) continue;
            if (Memory::Read8(listener + 0x80)) continue;                  // mDisabled
            if (inputLocked && !Memory::Read8(listener + 0x81)) continue;  // !mIgnoreInputLock
            const uint32_t last = listener + 0x3C + chan * 0x10;           // mPreviousEvents[chan]
            if (static_cast<int32_t>(Memory::Read32(last)) != static_cast<int32_t>(chan) ||
                std::fabs(Memory::ReadFloat32(last + 4) - at.x) > 1.0f ||
                std::fabs(Memory::ReadFloat32(last + 8) - at.y) > 1.0f) continue;
            Button b{};
            b.minX = Memory::ReadFloat32(listener + 0x84);
            b.maxX = Memory::ReadFloat32(listener + 0x88);
            b.maxY = Memory::ReadFloat32(listener + 0x8C);
            b.minY = Memory::ReadFloat32(listener + 0x90);
            if (!(b.maxX > b.minX && b.maxY > b.minY) || b.maxX - b.minX > 600.0f || b.maxY - b.minY > 420.0f) continue;
            b.rot = Memory::ReadFloat32(listener + 0x94);
            b.pivotX = Memory::ReadFloat32(listener + 0x98);
            b.pivotY = Memory::ReadFloat32(listener + 0x9C);
            b.centre = {(b.minX + b.maxX) * 0.5f, (b.minY + b.maxY) * 0.5f};
            if (b.rot != 0.0f) {
                const float dx = b.centre.x - b.pivotX, dy = b.centre.y - b.pivotY, c = std::cos(b.rot), sn = std::sin(b.rot);
                b.centre = {b.pivotX + dx * c - dy * sn, b.pivotY + dx * sn + dy * c};
            }
            // Where the pointer goes: low in the button, so the hand (drawn below its fingertip)
            // leaves the label visible; small or rotated buttons (ring icons) take the centre.
            b.aim = b.centre;
            if (b.rot == 0.0f && b.maxY - b.minY > 24.0f) b.aim.y = b.minY + (b.maxY - b.minY) * 0.3f;
            b.back = vtable == kBackButtonVtable;
            b.listener = listener;
            if (std::isfinite(b.centre.x) && std::isfinite(b.centre.y)) out.push_back(b);
        }
    } catch (...) {}
    return out;
}

// Nearest button in the pressed direction, weighting sideways distance so moves stay in line.
// The next button in the pressed direction. Buttons lined up with the current one (overlapping it
// across the direction of travel: same row for left/right, same column for up/down) come first,
// nearest first; otherwise the nearest, weighting sideways distance. Off the end of a row or
// column, it wraps to the far end of the same row/column. BACK is never a target (B does that).
bool PickInDirection(const std::vector<Button>& buttons, const Button* current, Point from, uint32_t dir, Point& out) {
    const bool horizontal = (dir & (kNavLeft | kNavRight)) != 0;
    const float dx = (dir & kNavRight) ? 1.0f : (dir & kNavLeft) ? -1.0f : 0.0f;
    const float dy = (dir & kNavUp) ? 1.0f : (dir & kNavDown) ? -1.0f : 0.0f;
    // The span we line up against: the current button, or a small box around the pointer.
    const float lo = current ? (horizontal ? current->minY : current->minX) : (horizontal ? from.y : from.x) - 4.0f;
    const float hi = current ? (horizontal ? current->maxY : current->maxX) : (horizontal ? from.y : from.x) + 4.0f;
    const auto sidewaysGap = [&](const Button& b) {
        const float bLo = horizontal ? b.minY : b.minX, bHi = horizontal ? b.maxY : b.maxX;
        return std::max(0.0f, std::max(bLo - hi, lo - bHi));
    };
    float best = 1e30f, wrapBest = -1.0f;
    Point wrap{};
    for (const Button& b : buttons) {
        // BACK is the B button's job: moving around never lands on it.
        if (&b == current || b.back) continue;
        const float vx = b.centre.x - from.x, vy = b.centre.y - from.y;
        const float forward = vx * dx + vy * dy;
        const float gap = sidewaysGap(b);
        if (forward >= 8.0f) {
            const float score = gap == 0.0f ? forward : 100000.0f + forward + 3.0f * gap;
            if (score < best) { best = score; out = b.centre; }
        } else if (forward <= -8.0f && gap == 0.0f && -forward > wrapBest) {
            wrapBest = -forward;  // farthest lined-up button behind us
            wrap = b.centre;
        }
    }
    if (best < 1e30f) return true;
    if (wrapBest > 0.0f) { out = wrap; return true; }
    return false;
}

// Default selection when a menu opens: the first button in reading order (top row first, then
// left to right), skipping BACK and controls at the screen edges (page dots, side arrows). With
// preferWidest (stage select) the widest button wins instead, i.e. PLAY NOW over the arrows.
Point PickDefault(const std::vector<Button>& buttons, bool preferWidest) {
    std::vector<const Button*> pool;
    for (const Button& b : buttons) {
        const Point half = HalfExtents();
        if (b.back || std::fabs(b.centre.x) > half.x - 50.0f || b.centre.y > half.y - 50.0f) continue;
        pool.push_back(&b);
    }
    if (pool.empty()) for (const Button& b : buttons) if (!b.back) pool.push_back(&b);
    if (pool.empty()) return buttons.front().centre;
    const Button* best = pool.front();
    for (const Button* b : pool) {
        if (preferWidest) {
            if (b->maxX - b->minX > best->maxX - best->minX) best = b;
        } else if (b->centre.y > best->centre.y + 20.0f ||
                   (std::fabs(b->centre.y - best->centre.y) <= 20.0f && b->centre.x < best->centre.x)) {
            best = b;  // higher row, or same row further left
        }
    }
    return best->centre;
}

// Hiding the hand: scenes re-show it every frame, but nothing touches its colour (SetPointerColour is
// a stub), so give the hand and its child graphics a colour override with zero alpha, remembering the
// originals to put back. TLInstance: m_next +0x00, pChildren +0x08, overloaded colour +0x6D (RGBA),
// m_overloadFlags +0x84 (0x10 = colour overridden).
struct SavedColour { uint32_t inst, flags; uint8_t rgba[4]; };
struct HandState { uint32_t hand = 0; std::vector<SavedColour> saved; };
HandState s_hand[PAD_CHANMAX];

void CollectInstanceTree(uint32_t inst, std::vector<uint32_t>& out) {
    for (int guard = 0; inst && guard < 64 && out.size() < 64; ++guard) {
        if (std::find(out.begin(), out.end(), inst) != out.end()) return;
        out.push_back(inst);
        CollectInstanceTree(Memory::Read32(inst + 0x08), out);
        inst = Memory::Read32(inst);
    }
}

void SetHandVisible(uint32_t chan, bool visible) {
    HandState& hs = s_hand[chan];
    try {
        const uint32_t hand = Memory::Read32(kPointerInstances + chan * 4);
        if (hand != hs.hand) { hs = {}; hs.hand = hand; }  // new scene/hand: old instances may be gone
        if (!hand) return;
        if (visible) {
            for (const SavedColour& c : hs.saved) {
                Memory::Write32(c.inst + 0x84, c.flags);
                for (int i = 0; i < 4; ++i) Memory::Write8(c.inst + 0x6D + i, c.rgba[i]);
            }
            hs.saved.clear();
            return;
        }
        std::vector<uint32_t> tree;
        tree.push_back(hand);
        CollectInstanceTree(Memory::Read32(hand + 0x08), tree);
        for (uint32_t inst : tree) {
            if (std::none_of(hs.saved.begin(), hs.saved.end(), [&](const SavedColour& c) { return c.inst == inst; })) {
                SavedColour c{inst, Memory::Read32(inst + 0x84), {}};
                for (int i = 0; i < 4; ++i) c.rgba[i] = Memory::Read8(inst + 0x6D + i);
                hs.saved.push_back(c);
            }
            Memory::Write32(inst + 0x84, Memory::Read32(inst + 0x84) | 0x10);
            Memory::Write8(inst + 0x70, 0);  // alpha
        }
    } catch (...) {}
}

void SelectAt(uint32_t chan, const std::vector<Button>& buttons, Point centre);
// Select the button whose centre is `goal`: the pointer goes exactly to its aim point (screen
// space), which the game reads on its next update.
void SnapTo(State& st, const std::vector<Button>& buttons, Point goal, float cursor[2]) {
    const Point half = HalfExtents();
    st.goal = goal;
    st.aim = goal;
    for (const Button& b : buttons)
        if (b.centre.x == goal.x && b.centre.y == goal.y) { st.aim = b.aim; break; }
    st.settleFrames = 4;  // until the game has caught up with the new position
    cursor[0] = std::clamp(st.aim.x / half.x, -1.0f, 1.0f);
    cursor[1] = std::clamp(-st.aim.y / half.y, -1.0f, 1.0f);
}

struct Result {
    bool menu = false;       // a menu is up: navigation owns the D-pad/left stick
    bool hidePointer = false; // report no IR pointer (hand hidden, game keeps the last position)
    bool pressA = false;     // tap A (B-to-back)
    bool pressPause = false; // tap the pause button (B on the pause menu)
    int page = 0;            // tap - (-1) or + (+1): stage select flips stages with left/right
};


Result Update(uint32_t chan, uint32_t dir, Point stick, bool backButton, int pageInput, float cursor[2]) {
    State& st = s_state[chan];
    Result r;
    if (!RuntimeConfigFile::ModMenuNavigation()) {
        // Off (or just switched off): give the game its pointer and hand back.
        if (st.scene != 0 || st.hadButtons) { SetHandVisible(chan, true); st = {}; }
        s_hasSelected[chan] = false;
        return r;
    }
    // A menu is up whenever live pointer buttons exist (none during matches).
    const std::vector<Button> buttons = CollectButtons(chan);
    s_hasSelected[chan] = false;
    if (dir || backButton || std::hypot(stick.x, stick.y) > 0.5f) s_engaged[chan] = true;
    if (!s_engaged[chan]) return r;  // idle extra pad: leave its pointer alone
    if (buttons.empty()) { const uint32_t scene = CurrentScene(); st = {}; st.sceneWhenEmpty = scene; return r; }
    r.menu = true;
    // One direction at a time; first press moves at once, holding repeats.
    if (dir & (kNavUp | kNavDown)) dir &= (kNavUp | kNavDown); else dir &= (kNavLeft | kNavRight);
    if ((dir & kNavUp && dir & kNavDown) || (dir & kNavLeft && dir & kNavRight)) dir = 0;
    const uint64_t now = SDL_GetTicks();
    bool step = false;
    if (dir && dir != st.heldDir) { step = true; st.repeatAtMs = now + 350; }
    else if (dir && now >= st.repeatAtMs) { step = true; st.repeatAtMs = now + 130; }
    st.heldDir = dir;
    const bool backPressed = backButton && !st.backHeld;
    st.backHeld = backButton;

    if (st.hideHandFrames > 0) --st.hideHandFrames;
    SetHandVisible(chan, st.hideHandFrames == 0);  // the game's own hand marks the selection
    const Point at = PointerPosition(chan);
    // The marker follows the button being moved to (st.goal) while a snap settles, else whatever
    // the pointer rests on.
    if (st.settleFrames > 0) {
        SelectAt(chan, buttons, st.goal);
    } else {
        for (const Button& b : buttons) {
            if (b.Contains(at)) { s_selected[chan] = b; s_hasSelected[chan] = true; break; }
        }
    }
    // A new screen (another scene on top, or buttons appearing after none) starts on its default
    // selection (first in reading order), wherever the pointer was left.
    const bool stageSelectScreen = CurrentSceneVtable() == kStadiumSelectVtable;
    const uint32_t scene = CurrentScene();
    if (scene != st.scene || !st.hadButtons) {
        // In-match overlays (the pause menu) appear without a scene change.
        st.overlay = !st.hadButtons && scene == st.sceneWhenEmpty;
        st.scene = scene;
        st.hadButtons = true;
        st.memoryKey = (scene ? Memory::Read32(scene) : 0) ^ (st.overlay ? 1u : 0u);
        Point start = PickDefault(buttons, stageSelectScreen);
        const auto remembered = s_focusMemory[chan].find(st.memoryKey);
        if (remembered != s_focusMemory[chan].end() &&
            std::any_of(buttons.begin(), buttons.end(), [&](const Button& b) {
                return b.centre.x == remembered->second.x && b.centre.y == remembered->second.y; }))
            start = remembered->second;
        SnapTo(st, buttons, start, cursor);
        SelectAt(chan, buttons, st.goal);
        return r;
    }
    if (s_hasSelected[chan]) s_focusMemory[chan][st.memoryKey] = s_selected[chan].centre;
    // Stage select defaults to PLAY NOW (PickDefault) and pages with L/R like any other screen.
    const bool stageSelect = CurrentSceneVtable() == kStadiumSelectVtable;
    if (st.pageFrames > 0) { --st.pageFrames; r.page = st.pageFrames >= 2 ? st.pageDir : 0; }
    // L/R page or switch tabs on any screen (the game's -/+), as the shoulder buttons do on SMS.
    if (pageInput != 0 && pageInput != st.heldPage) { st.pageDir = pageInput; st.pageFrames = 4; r.page = pageInput; }
    st.heldPage = pageInput;
    // B: go to the back button, then tap A on it. Overlays without one (the pause menu) close
    // with the game's own pause button instead.
    if (backPressed) {
        const auto back = std::find_if(buttons.begin(), buttons.end(), [](const Button& b) { return b.back; });
        if (back != buttons.end()) {
            // Seamless: the hand hides, the pointer goes to BACK, and A taps there next frame
            // (the screen changes before anything is seen moving).
            SnapTo(st, buttons, back->centre, cursor);
            st.backFrames = 2;
            st.hideHandFrames = RuntimeConfigFile::FrameRate(60) >= 120 ? 40 : 20;  // about 1/3 s
        }
        else if (st.overlay) { st.pauseFrames = 4; }
    }
    if (st.pauseFrames > 0) { --st.pauseFrames; r.pressPause = st.pauseFrames >= 2; }
    if (st.pressFrames > 0) { --st.pressFrames; r.pressA = st.pressFrames >= 2; return r; }
    if (st.backFrames > 0 && --st.backFrames == 0) { st.pressFrames = 3; r.pressA = true; return r; }
    // Ring menus (the main menu): a stick or D-pad direction selects the button at that angle
    // around the ring's centre, following the stick as it turns. BACK stays on B.
    std::vector<const Button*> ring;
    Point ringCentre{};
    for (const Button& b : buttons) if (!b.back) ring.push_back(&b);
    if (ring.size() >= 5) {
        for (const Button* b : ring) { ringCentre.x += b->centre.x; ringCentre.y += b->centre.y; }
        ringCentre.x /= ring.size(); ringCentre.y /= ring.size();
        float lo = 1e30f, hi = 0.0f, sum = 0.0f;
        for (const Button* b : ring) {
            const float d = std::hypot(b->centre.x - ringCentre.x, b->centre.y - ringCentre.y);
            lo = std::min(lo, d); hi = std::max(hi, d); sum += d;
        }
        if (sum <= 0.0f || (hi - lo) / (sum / ring.size()) > 0.35f) ring.clear();
    } else {
        ring.clear();
    }
    if (!ring.empty() && !stageSelect) {
        Point aim = stick;
        if (std::hypot(aim.x, aim.y) < 0.55f) {
            aim = {(dir & kNavRight) ? 1.0f : (dir & kNavLeft) ? -1.0f : 0.0f,
                   (dir & kNavUp) ? 1.0f : (dir & kNavDown) ? -1.0f : 0.0f};
            if (!step) aim = {};
        }
        if (aim.x != 0.0f || aim.y != 0.0f) {
            const float want = std::atan2(aim.y, aim.x);
            const Button* best = nullptr;
            float bestDiff = 1e30f;
            for (const Button* b : ring) {
                float diff = std::fabs(std::atan2(b->centre.y - ringCentre.y, b->centre.x - ringCentre.x) - want);
                if (diff > 3.14159265f) diff = 6.2831853f - diff;
                if (diff < bestDiff) { bestDiff = diff; best = b; }
            }
            if (best && !best->Contains(at) && !(st.settleFrames > 0 && st.goal.x == best->centre.x && st.goal.y == best->centre.y))
            { SnapTo(st, buttons, best->centre, cursor); SelectAt(chan, buttons, st.goal); }
        }
    }
    Point goal{};
    const Button* current = nullptr;
    for (const Button& b : buttons) if (b.Contains(at)) { current = &b; break; }
    if (ring.empty() && step && PickInDirection(buttons, current, at, dir, goal)) {
        SnapTo(st, buttons, goal, cursor);
        SelectAt(chan, buttons, st.goal);
        return r;
    }
    if (st.settleFrames > 0) {
        // The game picks the new position up on its next update; wait for it before re-checking.
        --st.settleFrames;
        if (std::hypot(st.aim.x - at.x, st.aim.y - at.y) < 2.0f) st.settleFrames = 0;
        return r;
    }
    // Keep something selected: if the pointer rests on no button, select the default.
    const bool onButton = std::any_of(buttons.begin(), buttons.end(), [&](const Button& b) { return b.Contains(at); });
    if (!onButton) { SnapTo(st, buttons, PickDefault(buttons, stageSelect), cursor); SelectAt(chan, buttons, st.goal); return r; }
    return r;
}
// Selection marker: a round badge with the player number, in that player's colour, on the
// selected button's top-left corner (corner placement keeps it tidy even where a button's hit area
// is a bit larger than its graphic).
void DrawHighlight() {
    if (!RuntimeConfigFile::ModMenuNavigation() || !RuntimeConfigFile::ModSelectionBadge()) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) return;
    // The menu canvas (centre origin, +y up, HalfExtents) on screen: the whole window
    // with dynamic aspect, else a centred 4:3 (or forced 16:9) fit.
    ImVec2 origin(0, 0), size = display;
    if (!g_dynamicAspectRatioEnabled) {
        const float aspect = DynamicAspectForce169Requested() ? 16.0f / 9.0f : 4.0f / 3.0f;
        if (display.x / display.y > aspect) { size.x = display.y * aspect; origin.x = (display.x - size.x) * 0.5f; }
        else { size.y = display.x / aspect; origin.y = (display.y - size.y) * 0.5f; }
    }
    const Point half = HalfExtents();
    const auto toScreen = [&](float x, float y) {
        return ImVec2(origin.x + (x / (2.0f * half.x) + 0.5f) * size.x, origin.y + (0.5f - y / (2.0f * half.y)) * size.y);
    };
    static const ImU32 kPlayerColours[PAD_CHANMAX] = {
        IM_COL32(40, 128, 200, 255), IM_COL32(215, 60, 55, 255), IM_COL32(70, 170, 70, 255), IM_COL32(235, 185, 30, 255)};
    const float r = size.y * 0.024f;
    const float bob = std::sin(static_cast<float>(SDL_GetTicks()) * 0.006f) * r * 0.08f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    static bool s_wasShowing[PAD_CHANMAX]{};
    for (uint32_t chan = 0; chan < PAD_CHANMAX; ++chan) {
        if (!s_hasSelected[chan]) { s_wasShowing[chan] = false; continue; }
        const bool appearing = !s_wasShowing[chan];
        s_wasShowing[chan] = true;
        const Button& b = s_selected[chan];
        // Top-left corner, turned with the region on rotated (ring) buttons.
        float cx = b.minX, cy = b.maxY;
        if (b.rot != 0.0f) {
            const float dx = cx - b.pivotX, dy = cy - b.pivotY, cs = std::cos(b.rot), sn = std::sin(b.rot);
            cx = b.pivotX + dx * cs - dy * sn;
            cy = b.pivotY + dx * sn + dy * cs;
        }
        ImVec2 target = toScreen(cx, cy);
        target.x += r * 0.35f;
        target.y += r * 0.35f;
        // Some hit areas (choose-sides cards) reach past the screen; keep the badge visible.
        const float margin = r * 1.3f;
        target.x = std::clamp(target.x, origin.x + margin, origin.x + size.x - margin);
        target.y = std::clamp(target.y, origin.y + margin, origin.y + size.y - margin);
        // Glide at display rate (game frames can be 30 fps); jump when a marker first appears.
        static ImVec2 shown[PAD_CHANMAX];
        static bool showing[PAD_CHANMAX]{};
        if (appearing || !showing[chan]) { shown[chan] = target; showing[chan] = true; }
        const float t = 1.0f - std::exp(-ImGui::GetIO().DeltaTime * 24.0f);
        shown[chan].x += (target.x - shown[chan].x) * t;
        shown[chan].y += (target.y - shown[chan].y) * t;
        const ImVec2 c(shown[chan].x, shown[chan].y + bob);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.08f, c.y + r * 0.14f), r * 1.08f, IM_COL32(0, 0, 0, 90), 32);  // shadow
        dl->AddCircleFilled(c, r, IM_COL32(255, 255, 255, 255), 32);
        dl->AddCircleFilled(c, r * 0.80f, kPlayerColours[chan], 32);
        char label[2] = {static_cast<char>('1' + chan), 0};
        const float textSize = r * 1.25f;
        const ImVec2 ts = font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, label);
        dl->AddText(font, textSize, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), label);
    }
}
} // namespace MenuNav

void DrawOverlay() { MenuNav::DrawHighlight(); }

// No Mega Strikes with controllers (mod, F10 > Mods > no_mega_strikes): defending one means pointing at the incoming balls,
// which a non-Wii-Remote controller can't do. While any emulated remote (a gamepad or keyboard) is
// in use, the current match's per-game settings have Mega Strikes off for both sides, the game's
// own switch (cFielder::CanDoCaptainShootToScore checks it), so a full charge is a normal strong
// shot for players and AI alike. Only the per-match copy changes; saved options are untouched,
// and sessions with only real Wii Remotes never get here.
namespace NoMegaStrikes {
constexpr uint32_t kGameInfoManagerPtr = 0x806E0F54u;  // nlSingleton<GameInfoManager>::s_pInstance
constexpr uint32_t kHomeMegastrikeEnabled = 0x1Au;     // mCurGameGameplayOptions (+0x04) +0x16
constexpr uint32_t kAwayMegastrikeEnabled = 0x1Bu;     // ... +0x17
constexpr uint32_t kUseCurGameSettings = 0x27Cu;

void Apply() {
    if (!RuntimeConfigFile::ModNoMegaStrikes()) return;
    try {
        const uint32_t info = Memory::Read32(kGameInfoManagerPtr);
        if (!info || !Memory::Read8(info + kUseCurGameSettings)) return;
        Memory::Write8(info + kHomeMegastrikeEnabled, 0);
        Memory::Write8(info + kAwayMegastrikeEnabled, 0);
    } catch (...) {}
}
} // namespace NoMegaStrikes

// Shared by every controller: Nunchuk stick, level remote, menu navigation and the pointer.
void ApplyCommon(uint32_t chan, WiiRemoteInput::KpadSample& sample, uint32_t navDpad, float freeX, float freeY,
                 int pageInput) {
    sample.hasNunchuk = true;
    sample.acc[1] = -1.0f;        // level, 1 g down (KPAD frame)
    sample.nunchukAcc[1] = -1.0f;
    // Pointer: the stick moves a cursor that stays where it is left (menus need absolute aim).
    static uint64_t s_lastMs[PAD_CHANMAX]{};
    const uint64_t now = SDL_GetTicks();
    const float dt = s_lastMs[chan] ? std::min((now - s_lastMs[chan]) / 1000.0f, 0.1f) : 0.0f;
    s_lastMs[chan] = now;
    const auto dz = [](float v) { return std::fabs(v) < 0.2f ? 0.0f : v; };
    // Menus: the D-pad and either stick navigate (the stronger stick wins); flicks step between
    // buttons, and on ring menus the stick angle picks directly.
    MenuNav::Point navStick{sample.stick[0], sample.stick[1]};
    if (std::hypot(freeX, freeY) > std::hypot(navStick.x, navStick.y)) navStick = {freeX, freeY};
    uint32_t nav = navDpad;
    if (navStick.y > 0.6f) nav |= MenuNav::kNavUp;
    if (navStick.y < -0.6f) nav |= MenuNav::kNavDown;
    if (navStick.x < -0.6f) nav |= MenuNav::kNavLeft;
    if (navStick.x > 0.6f) nav |= MenuNav::kNavRight;
    const MenuNav::Result navResult = MenuNav::Update(chan, nav, navStick, (sample.hold & kWB) != 0, pageInput, s_cursor[chan]);
    const bool menu = navResult.menu;
    if (navResult.pressA) sample.hold |= kWA;
    if (navResult.pressPause) sample.hold |= kWOne;
    if (navResult.page > 0) sample.hold |= kWPlus;
    if (navResult.page < 0) sample.hold |= kWMinus;
    float moveX = sample.stick[0], moveY = sample.stick[1];
    if (menu) {
        // Only the real D-pad reaches the game as D-pad here, not the sticks used for navigating.
        sample.hold &= ~(kWUp | kWDown | kWLeft | kWRight);
        if (navDpad & MenuNav::kNavUp) sample.hold |= kWUp;
        if (navDpad & MenuNav::kNavDown) sample.hold |= kWDown;
        if (navDpad & MenuNav::kNavLeft) sample.hold |= kWLeft;
        if (navDpad & MenuNav::kNavRight) sample.hold |= kWRight;
        moveX = moveY = 0.0f;  // the pointer belongs to navigation
    }
    NoMegaStrikes::Apply();
    s_cursor[chan][0] = std::clamp(s_cursor[chan][0] + dz(moveX) * 1.6f * dt, -1.0f, 1.0f);
    s_cursor[chan][1] = std::clamp(s_cursor[chan][1] - dz(moveY) * 1.6f * dt, -1.0f, 1.0f);
    sample.hasPointer = !navResult.hidePointer;
    sample.pointer[0] = s_cursor[chan][0];
    sample.pointer[1] = s_cursor[chan][1];
}

// A real Wii Remote with no IR pointer (no sensor bar in view): its D-pad and Nunchuk stick drive
// the same menu navigation gamepads use, A confirms and B goes back. With the pointer visible the
// remote works like on the console and nothing here runs.
void ApplyRemoteMenuNav(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (chan >= PAD_CHANMAX) return;
    uint32_t navDpad = 0;
    if (sample.hold & kWUp) navDpad |= MenuNav::kNavUp;
    if (sample.hold & kWDown) navDpad |= MenuNav::kNavDown;
    if (sample.hold & kWLeft) navDpad |= MenuNav::kNavLeft;
    if (sample.hold & kWRight) navDpad |= MenuNav::kNavRight;
    const MenuNav::Point stick{sample.stick[0], sample.stick[1]};
    uint32_t nav = navDpad;
    if (stick.y > 0.6f) nav |= MenuNav::kNavUp;
    if (stick.y < -0.6f) nav |= MenuNav::kNavDown;
    if (stick.x < -0.6f) nav |= MenuNav::kNavLeft;
    if (stick.x > 0.6f) nav |= MenuNav::kNavRight;
    const MenuNav::Result r = MenuNav::Update(chan, nav, stick, (sample.hold & kWB) != 0, 0, s_cursor[chan]);
    if (!r.menu) return;
    if (r.pressA) sample.hold |= kWA;
    if (r.pressPause) sample.hold |= kWOne;
    sample.hasPointer = !r.hidePointer;
    sample.pointer[0] = s_cursor[chan][0];
    sample.pointer[1] = s_cursor[chan][1];
}

// Every controller (gamepad or keyboard) reaches the game through aurora's GameCube pad for its
// port, so F10 bindings apply and face buttons sit where a GameCube controller's do (A bottom,
// B left, X right, Y top). In matches the game reads that GameCube state directly: the emulated
// remote tags its button word with kMscGameCubeTag | channel, and msc_game.cpp turns the player's
// DetInput into the GameCube kind the engine still supports (Super Mario Strikers' controls). The
// remote itself only carries what the Wii-side code needs: menus, pause, and the Nunchuk stick.
bool ReadGameCubePad(uint32_t chan, PADStatus& out) { return ReadBoundPad(chan, out); }

bool Read(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (InputBindings::InputBlocked()) return Present(chan) && (sample = {}, sample.hasNunchuk = true, true);
    PADStatus pad{};
    if (!ReadBoundPad(chan, pad)) return false;
    sample = {};
    const auto map = [&](uint16_t gc, uint32_t wii) { if (pad.button & gc) sample.hold |= wii; };
    map(PAD_BUTTON_A, kWA);     // menus: select
    map(PAD_BUTTON_B, kWB);     // menus: back
    map(PAD_BUTTON_X, kWC);
    map(PAD_TRIGGER_L, kWZ);
    map(PAD_BUTTON_START, kWOne);
    map(PAD_BUTTON_UP, kWUp);
    map(PAD_BUTTON_DOWN, kWDown);
    map(PAD_BUTTON_LEFT, kWLeft);
    map(PAD_BUTTON_RIGHT, kWRight);
    sample.hold |= kMscGameCubeTag | (chan << kMscGameCubeChannelShift);
    sample.stick[0] = std::clamp(pad.stickX / 72.0f, -1.0f, 1.0f);
    sample.stick[1] = std::clamp(pad.stickY / 72.0f, -1.0f, 1.0f);
    uint32_t navDpad = 0;
    if (pad.button & PAD_BUTTON_UP) navDpad |= MenuNav::kNavUp;
    if (pad.button & PAD_BUTTON_DOWN) navDpad |= MenuNav::kNavDown;
    if (pad.button & PAD_BUTTON_LEFT) navDpad |= MenuNav::kNavLeft;
    if (pad.button & PAD_BUTTON_RIGHT) navDpad |= MenuNav::kNavRight;
    const int pageInput = (pad.button & PAD_TRIGGER_R) ? 1 : (pad.button & PAD_TRIGGER_L) ? -1 : 0;
    ApplyCommon(chan, sample, navDpad, pad.substickX / 72.0f, pad.substickY / 72.0f, pageInput);
    return true;
}
} // namespace MscEmulatedRemote

// PADRead: gathers every GameCube pad source for the frame and writes the statuses to guest memory.
extern "C" uint32_t PAD__Read_HLE(uint32_t statusPtr)
{
    if (statusPtr == 0) {
        return 0;
    }

    PADStatus statuses[PAD_CHANMAX]{};
    // Keep looking for a Bluetooth Wii Remote that dropped out (or was turned on late).
    WiiRemoteInput::Poll();
    uint32_t rumbleMask = PADRead(statuses);
    // Wii Remotes reach the game through KPAD, not as GameCube pads. This also
    // applies while input is blocked (overlay open) so the port does not flip
    // between "connected" and "no controller" every time the overlay toggles.
    WiiRemoteInput::HideRemotesFromPad(statuses, PAD_CHANMAX);

    FillTriggersHeldByButtons(statuses);
    InputBindings::Apply(statuses);

    try {
        for (uint32_t i = 0; i < PAD_CHANMAX; ++i) {
            WritePadStatus(statusPtr + static_cast<uint32_t>(i * PadStatusContract::kGuestStatusSize),
                           statuses[i]);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }

    return rumbleMask;
}
PPC_NATIVE_OVERRIDE(803BF280, PAD__Read_HLE, uint32_t, (uint32_t statusPtr), (statusPtr));

extern "C" uint32_t PAD__Reset_HLE(uint32_t mask)
{
    return PADReset(mask) ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE(803BF178, PAD__Reset_HLE, uint32_t, (uint32_t mask), (mask));
