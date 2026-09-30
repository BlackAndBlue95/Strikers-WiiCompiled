#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "input_bindings.h"
#include "wii_remote_input.h"

#include <algorithm>
#include <cmath>
#include <SDL3/SDL_timer.h>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>

namespace {

std::atomic<bool> g_rumbleEnabled{true};

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

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled)
{
    g_rumbleEnabled.store(enabled, std::memory_order_relaxed);
}

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

// Menu navigation (mod): MSC's menus are pointer-only. To make them feel like a console menu, the
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
    int settleFrames = 0;
    bool backHeld = false;
    int backFrames = 0;       // B pressed: heading for the back button
    int pressFrames = 0;      // tapping A on it
    int pageFrames = 0;       // tapping +/- (stage select)
    int pageDir = 0;
    uint32_t scene = 0;       // top scene last frame, to spot screen changes
};
State s_state[PAD_CHANMAX];
// Screen units per KPAD unit. The game maps KPAD pos to screen as (x * w/2, -y * h/2); these start at
// 4:3 values and are refined from where the pointer actually lands, so widescreen needs no special case.
float s_scaleX = 320.0f, s_scaleY = 240.0f;

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
            b.back = vtable == kBackButtonVtable;
            b.listener = listener;
            if (std::isfinite(b.centre.x) && std::isfinite(b.centre.y)) out.push_back(b);
        }
    } catch (...) {}
    return out;
}

// Nearest button in the pressed direction, weighting sideways distance so moves stay in line.
bool PickInDirection(const std::vector<Button>& buttons, Point from, uint32_t dir, Point& out) {
    const float dx = (dir & kNavRight) ? 1.0f : (dir & kNavLeft) ? -1.0f : 0.0f;
    const float dy = (dir & kNavUp) ? 1.0f : (dir & kNavDown) ? -1.0f : 0.0f;
    float best = 1e30f;
    for (const Button& b : buttons) {
        const float vx = b.centre.x - from.x, vy = b.centre.y - from.y;
        const float forward = vx * dx + vy * dy;
        if (forward < 8.0f) continue;
        const float score = forward + 2.5f * std::fabs(vx * dy - vy * dx);
        if (score < best) { best = score; out = b.centre; }
    }
    return best < 1e30f;
}

// Default selection when a menu opens: the first button in reading order (top row first, then
// left to right), skipping BACK and controls at the screen edges (page dots, side arrows). With
// preferWidest (stage select) the widest button wins instead, i.e. PLAY NOW over the arrows.
Point PickDefault(const std::vector<Button>& buttons, bool preferWidest) {
    std::vector<const Button*> pool;
    for (const Button& b : buttons) {
        if (b.back || std::fabs(b.centre.x) > s_scaleX - 50.0f || b.centre.y > s_scaleY - 50.0f) continue;
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

void SnapTo(State& st, Point goal, float cursor[2]) {
    st.goal = goal;
    st.settleFrames = 10;
    cursor[0] = std::clamp(goal.x / s_scaleX, -1.0f, 1.0f);
    cursor[1] = std::clamp(-goal.y / s_scaleY, -1.0f, 1.0f);
}

struct Result {
    bool menu = false;       // a menu is up: navigation owns the D-pad/left stick
    bool hidePointer = false; // report no IR pointer (hand hidden, game keeps the last position)
    bool pressA = false;     // tap A (B-to-back)
    int page = 0;            // tap - (-1) or + (+1): stage select flips stages with left/right
};


Result Update(uint32_t chan, uint32_t dir, Point stick, bool backButton, float cursor[2]) {
    State& st = s_state[chan];
    Result r;
    // A menu is up whenever live pointer buttons exist (none during matches).
    const std::vector<Button> buttons = CollectButtons(chan);
    if (buttons.empty()) { st = {}; return r; }
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

    SetHandVisible(chan, false);
    const Point at = PointerPosition(chan);
    // A new screen (another scene on top, or buttons appearing after none) starts on its default
    // selection (first in reading order), wherever the pointer was left.
    const bool stageSelectScreen = CurrentSceneVtable() == kStadiumSelectVtable;
    const uint32_t scene = CurrentScene();
    if (scene != st.scene) {
        st.scene = scene;
        SnapTo(st, PickDefault(buttons, stageSelectScreen), cursor);
        return r;
    }
    // Stage select pages with +/-; let left/right do that instead of moving between buttons.
    const bool stageSelect = CurrentSceneVtable() == kStadiumSelectVtable;
    if (st.pageFrames > 0) { --st.pageFrames; r.page = st.pageFrames >= 2 ? st.pageDir : 0; }
    if (step && (dir & (kNavLeft | kNavRight)) && stageSelect) {
        st.pageDir = (dir & kNavRight) ? 1 : -1;
        st.pageFrames = 4;
        r.page = st.pageDir;
        step = false;
    }
    // B: go to the back button, then tap A on it.
    if (backPressed) {
        for (const Button& b : buttons) {
            if (b.back) { SnapTo(st, b.centre, cursor); st.backFrames = 30; break; }
        }
    }
    if (st.pressFrames > 0) { --st.pressFrames; r.pressA = st.pressFrames >= 2; return r; }
    if (st.backFrames > 0) {
        --st.backFrames;
        const auto back = std::find_if(buttons.begin(), buttons.end(), [&](const Button& b) { return b.back; });
        if (back != buttons.end() && back->Contains(at)) { st.backFrames = 0; st.pressFrames = 5; r.pressA = true; return r; }
    }
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
                SnapTo(st, best->centre, cursor);
        }
    }
    Point goal{};
    if (ring.empty() && step && PickInDirection(buttons, at, dir, goal)) {
        SnapTo(st, goal, cursor);
        return r;
    }
    if (st.settleFrames > 0) {
        // The pointer reflects last frame's KPAD position; nudge toward the goal and learn the scale.
        --st.settleFrames;
        if (std::fabs(cursor[0]) > 0.1f && std::fabs(at.x) > 1.0f && at.x * cursor[0] > 0.0f)
            s_scaleX = std::clamp(at.x / cursor[0], 200.0f, 600.0f);
        if (std::fabs(cursor[1]) > 0.1f && std::fabs(at.y) > 1.0f && -at.y * cursor[1] > 0.0f)
            s_scaleY = std::clamp(-at.y / cursor[1], 150.0f, 400.0f);
        cursor[0] = std::clamp(cursor[0] + 0.5f * (st.goal.x - at.x) / s_scaleX, -1.0f, 1.0f);
        cursor[1] = std::clamp(cursor[1] - 0.5f * (st.goal.y - at.y) / s_scaleY, -1.0f, 1.0f);
        // Settled once the pointer is on the goal: stop steering.
        if (std::hypot(st.goal.x - at.x, st.goal.y - at.y) < 2.0f) st.settleFrames = 0;
        return r;
    }
    // Keep something selected: if the pointer rests on no button, select the default.
    const bool onButton = std::any_of(buttons.begin(), buttons.end(), [&](const Button& b) { return b.Contains(at); });
    if (!onButton) { SnapTo(st, PickDefault(buttons, stageSelect), cursor); return r; }
    return r;
}
} // namespace MenuNav

// Layout follows Vague Rant's Classic Controller hack for this game (GBAtemp), by button
// position: east=A pass, south/ZR=B shoot, north=C item, west=remote shake (big hit),
// ZL=Z chip, L=Nunchuk shake (switch item), R/right stick/D-pad=D-pad (deke, tackle),
// +/-=1 pause, left stick=Nunchuk stick and pointer.
void ApplyCommon(uint32_t chan, WiiRemoteInput::KpadSample& sample, bool remoteShake, bool nunchukShake,
                 uint32_t navDpad, float freeX, float freeY) {
    static uint32_t s_shakePhase[PAD_CHANMAX]{};
    sample.hasNunchuk = true;
    sample.acc[1] = -1.0f;        // level, 1 g down (KPAD frame)
    sample.nunchukAcc[1] = -1.0f;
    const float swing = (s_shakePhase[chan]++ & 2) ? 3.0f : -3.0f;
    if (remoteShake) { sample.acc[0] = swing; sample.acc[1] = -1.0f - swing; }
    if (nunchukShake) { sample.nunchukAcc[0] = swing; sample.nunchukAcc[1] = -1.0f - swing; }
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
    const MenuNav::Result navResult = MenuNav::Update(chan, nav, navStick, (sample.hold & kWB) != 0, s_cursor[chan]);
    const bool menu = navResult.menu;
    if (navResult.pressA) sample.hold |= kWA;
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
    s_cursor[chan][0] = std::clamp(s_cursor[chan][0] + dz(moveX) * 1.6f * dt, -1.0f, 1.0f);
    s_cursor[chan][1] = std::clamp(s_cursor[chan][1] - dz(moveY) * 1.6f * dt, -1.0f, 1.0f);
    sample.hasPointer = !navResult.hidePointer;
    sample.pointer[0] = s_cursor[chan][0];
    sample.pointer[1] = s_cursor[chan][1];
}

bool ReadFromGamepad(uint32_t chan, SDL_Gamepad* gp, WiiRemoteInput::KpadSample& sample) {
    sample = {};
    const auto btn = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(gp, b); };
    const auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(gp, a) / 32767.0f; };
    const float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    const bool zl = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.5f;
    const bool zr = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.5f;
    if (btn(SDL_GAMEPAD_BUTTON_EAST)) sample.hold |= kWA;
    if (btn(SDL_GAMEPAD_BUTTON_SOUTH) || zr) sample.hold |= kWB;
    if (btn(SDL_GAMEPAD_BUTTON_NORTH)) sample.hold |= kWC;
    if (zl) sample.hold |= kWZ;
    if (btn(SDL_GAMEPAD_BUTTON_START) || btn(SDL_GAMEPAD_BUTTON_BACK)) sample.hold |= kWOne;
    const bool r = btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || ry < -0.5f) sample.hold |= kWUp;
    if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ry > 0.5f || r) sample.hold |= kWDown;
    if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || rx < -0.5f) sample.hold |= kWLeft;
    if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || rx > 0.5f) sample.hold |= kWRight;
    sample.stick[0] = std::clamp(axis(SDL_GAMEPAD_AXIS_LEFTX), -1.0f, 1.0f);
    sample.stick[1] = std::clamp(-axis(SDL_GAMEPAD_AXIS_LEFTY), -1.0f, 1.0f);
    uint32_t navDpad = 0;
    if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) navDpad |= MenuNav::kNavUp;
    if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) navDpad |= MenuNav::kNavDown;
    if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) navDpad |= MenuNav::kNavLeft;
    if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) navDpad |= MenuNav::kNavRight;
    ApplyCommon(chan, sample, btn(SDL_GAMEPAD_BUTTON_WEST), btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), navDpad, rx, -ry);
    return true;
}

bool Read(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (InputBindings::InputBlocked()) return Present(chan) && (sample = {}, sample.hasNunchuk = true, true);
    if (SDL_Gamepad* gp = SDL_GetGamepadFromPlayerIndex(static_cast<int>(chan))) {
        return ReadFromGamepad(chan, gp, sample);
    }
    // Keyboard (or anything else bound through the GameCube-pad mapping), same semantics:
    // A pass, B shoot, X item, Y big hit, L chip, R switch item, Z/C-stick/D-pad deke, Start pause.
    PADStatus pad{};
    if (!ReadBoundPad(chan, pad)) return false;
    sample = {};
    const auto map = [&](uint16_t gc, uint32_t wii) { if (pad.button & gc) sample.hold |= wii; };
    map(PAD_BUTTON_A, kWA);
    map(PAD_BUTTON_B, kWB);
    map(PAD_BUTTON_X, kWC);
    map(PAD_TRIGGER_L, kWZ);
    map(PAD_BUTTON_START, kWOne);
    map(PAD_BUTTON_UP, kWUp);
    map(PAD_BUTTON_DOWN, kWDown);
    map(PAD_BUTTON_LEFT, kWLeft);
    map(PAD_BUTTON_RIGHT, kWRight);
    map(PAD_TRIGGER_Z, kWDown);
    if (pad.substickY > 64) sample.hold |= kWUp;
    if (pad.substickY < -64) sample.hold |= kWDown;
    if (pad.substickX < -64) sample.hold |= kWLeft;
    if (pad.substickX > 64) sample.hold |= kWRight;
    sample.stick[0] = std::clamp(pad.stickX / 72.0f, -1.0f, 1.0f);
    sample.stick[1] = std::clamp(pad.stickY / 72.0f, -1.0f, 1.0f);
    uint32_t navDpad = 0;
    if (pad.button & PAD_BUTTON_UP) navDpad |= MenuNav::kNavUp;
    if (pad.button & PAD_BUTTON_DOWN) navDpad |= MenuNav::kNavDown;
    if (pad.button & PAD_BUTTON_LEFT) navDpad |= MenuNav::kNavLeft;
    if (pad.button & PAD_BUTTON_RIGHT) navDpad |= MenuNav::kNavRight;
    ApplyCommon(chan, sample, (pad.button & PAD_BUTTON_Y) != 0, (pad.button & PAD_TRIGGER_R) != 0, navDpad,
                pad.substickX / 72.0f, pad.substickY / 72.0f);
    return true;
}
} // namespace MscEmulatedRemote

extern "C" uint32_t PAD__Init_HLE()
{
    return PADInit() ? 1u : 0u;
}
// MSC-UNMAPPED(PAD::Init) PPC_NATIVE_OVERRIDE(801AF2F0, PAD__Init_HLE, uint32_t, (), ());

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

extern "C" uint32_t PAD__Recalibrate_HLE(uint32_t mask)
{
    return PADRecalibrate(mask) ? 1u : 0u;
}
// MSC-UNMAPPED(PAD::Recalibrate) PPC_NATIVE_OVERRIDE(801AF1E4, PAD__Recalibrate_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" void PAD__ControlMotor_HLE(int32_t chan, uint32_t command)
{
    if (command == PAD_MOTOR_RUMBLE && !g_rumbleEnabled.load(std::memory_order_relaxed)) {
        command = PAD_MOTOR_STOP;
    }
    PADControlMotor(chan, command);
}
// MSC-UNMAPPED(PAD::ControlMotor) PPC_NATIVE_OVERRIDE_VOID(801AF908, PAD__ControlMotor_HLE, (int32_t chan, uint32_t command), (chan, command));
