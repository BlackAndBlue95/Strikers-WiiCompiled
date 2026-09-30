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

// Menu navigation (mod): MSC's menus are pointer-only. While the game shows its pointer, the D-pad
// and left-stick flicks move a highlight between buttons instead: the emulated pointer snaps onto
// the centre of the nearest FEPointerRegion in that direction, so the game's own hover/click logic
// (and A to confirm) keeps working. The right stick still drives a free pointer as a fallback.
namespace MenuNav {
constexpr uint32_t kPointerManagerPtr = 0x806E2030u;  // g_pFEPointerManager
constexpr uint32_t kPointerPositions = 0x80578460u;   // gFEPointerPositions[4]: centre origin, +y up
constexpr uint32_t kRegionContainsPoint = 0x8030131Cu; // FEPointerRegion::ContainsPoint (vtable +0x2C)
enum : uint32_t { kNavUp = 1, kNavDown = 2, kNavLeft = 4, kNavRight = 8 };

struct Target { float x, y; };
struct State {
    uint32_t heldDir = 0;
    uint64_t repeatAtMs = 0;
    Target goal{};
    int settleFrames = 0;
};
State s_state[PAD_CHANMAX];
// Screen units per KPAD unit. The game maps KPAD pos to screen as (x * w/2, -y * h/2); these start at
// 4:3 values and are refined from where the pointer actually lands, so widescreen needs no special case.
float s_scaleX = 320.0f, s_scaleY = 240.0f;


Target PointerPosition(uint32_t chan) {
    return {Memory::ReadFloat32(kPointerPositions + chan * 8), Memory::ReadFloat32(kPointerPositions + chan * 8 + 4)};
}

// Centres of every enabled pointer region (buttons), in screen space.
std::vector<Target> CollectTargets() {
    std::vector<Target> out;
    try {
        const uint32_t mgr = Memory::Read32(kPointerManagerPtr);
        // mListeners (nlListContainer) is {?, head, tail}; entries are {next, listener}.
        uint32_t entry = mgr ? Memory::Read32(mgr + 4) : 0;
        for (int guard = 0; entry && guard < 512; ++guard, entry = Memory::Read32(entry)) {
            const uint32_t listener = Memory::Read32(entry + 4);
            if (!listener || Memory::Read32(Memory::Read32(listener) + 0x2C) != kRegionContainsPoint) continue;
            if (Memory::Read8(listener + 0x80)) continue;  // mDisabled
            const float minX = Memory::ReadFloat32(listener + 0x84), maxX = Memory::ReadFloat32(listener + 0x88);
            const float maxY = Memory::ReadFloat32(listener + 0x8C), minY = Memory::ReadFloat32(listener + 0x90);
            if (!(maxX > minX && maxY > minY) || maxX - minX > 600.0f || maxY - minY > 420.0f) continue;
            Target t{(minX + maxX) * 0.5f, (minY + maxY) * 0.5f};
            // ContainsPoint rotates the probe by -rotation about the pivot; rotate the centre forward.
            const float rot = Memory::ReadFloat32(listener + 0x94);
            if (rot != 0.0f) {
                const float px = Memory::ReadFloat32(listener + 0x98), py = Memory::ReadFloat32(listener + 0x9C);
                const float dx = t.x - px, dy = t.y - py, c = std::cos(rot), sn = std::sin(rot);
                t = {px + dx * c - dy * sn, py + dx * sn + dy * c};
            }
            if (std::isfinite(t.x) && std::isfinite(t.y)) out.push_back(t);
        }
    } catch (...) {}
    return out;
}

// Nearest target in the pressed direction, weighting sideways distance so moves stay in line.
bool PickTarget(const std::vector<Target>& targets, Target from, uint32_t dir, Target& out) {
    const float dx = (dir & kNavRight) ? 1.0f : (dir & kNavLeft) ? -1.0f : 0.0f;
    const float dy = (dir & kNavUp) ? 1.0f : (dir & kNavDown) ? -1.0f : 0.0f;
    float best = 1e30f;
    for (const Target& t : targets) {
        const float vx = t.x - from.x, vy = t.y - from.y;
        const float forward = vx * dx + vy * dy;
        if (forward < 8.0f) continue;
        const float score = forward + 2.5f * std::fabs(vx * dy - vy * dx);
        if (score < best) { best = score; out = t; }
    }
    return best < 1e30f;
}

// The free pointer (right stick) takes over from a snap that's still settling.
void CancelSettle(uint32_t chan) { s_state[chan].settleFrames = 0; }

// Returns true while menu navigation owns the pointer this frame.
bool Update(uint32_t chan, uint32_t dir, float cursor[2]) {
    State& st = s_state[chan];
    // A menu is up whenever the game has enabled pointer buttons registered (none during matches).
    const std::vector<Target> targets = CollectTargets();
    if (targets.empty()) { st = {}; return false; }
    // One direction at a time; first press moves at once, holding repeats.
    if (dir & (kNavUp | kNavDown)) dir &= (kNavUp | kNavDown); else dir &= (kNavLeft | kNavRight);
    if (dir & kNavUp && dir & kNavDown) dir = 0;
    if (dir & kNavLeft && dir & kNavRight) dir = 0;
    const uint64_t now = SDL_GetTicks();
    bool step = false;
    if (dir && dir != st.heldDir) { step = true; st.repeatAtMs = now + 380; }
    else if (dir && now >= st.repeatAtMs) { step = true; st.repeatAtMs = now + 140; }
    st.heldDir = dir;
    if (step) {
        Target goal{};
        if (PickTarget(targets, PointerPosition(chan), dir, goal)) {
            st.goal = goal;
            st.settleFrames = 10;
            cursor[0] = std::clamp(goal.x / s_scaleX, -1.0f, 1.0f);
            cursor[1] = std::clamp(-goal.y / s_scaleY, -1.0f, 1.0f);
            return true;
        }
    }
    if (st.settleFrames > 0) {
        // The pointer reflects last frame's KPAD position; nudge toward the goal and learn the scale.
        --st.settleFrames;
        const Target at = PointerPosition(chan);
        if (std::fabs(cursor[0]) > 0.1f && std::fabs(at.x) > 1.0f && at.x * cursor[0] > 0.0f)
            s_scaleX = std::clamp(at.x / cursor[0], 200.0f, 600.0f);
        if (std::fabs(cursor[1]) > 0.1f && std::fabs(at.y) > 1.0f && -at.y * cursor[1] > 0.0f)
            s_scaleY = std::clamp(-at.y / cursor[1], 150.0f, 400.0f);
        cursor[0] = std::clamp(cursor[0] + 0.5f * (st.goal.x - at.x) / s_scaleX, -1.0f, 1.0f);
        cursor[1] = std::clamp(cursor[1] - 0.5f * (st.goal.y - at.y) / s_scaleY, -1.0f, 1.0f);
    }
    return true;
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
    uint32_t nav = navDpad;
    if (sample.stick[1] > 0.6f) nav |= MenuNav::kNavUp;
    if (sample.stick[1] < -0.6f) nav |= MenuNav::kNavDown;
    if (sample.stick[0] < -0.6f) nav |= MenuNav::kNavLeft;
    if (sample.stick[0] > 0.6f) nav |= MenuNav::kNavRight;
    // In menus the left stick/D-pad navigate and the right stick is the free pointer; elsewhere the
    // left stick moves the pointer as before.
    const bool menu = MenuNav::Update(chan, nav, s_cursor[chan]);
    float moveX = menu ? freeX : sample.stick[0], moveY = menu ? freeY : sample.stick[1];
    if (menu) {
        // Only the real D-pad reaches the game as D-pad here, not the right stick used for aiming.
        sample.hold &= ~(kWUp | kWDown | kWLeft | kWRight);
        if (navDpad & MenuNav::kNavUp) sample.hold |= kWUp;
        if (navDpad & MenuNav::kNavDown) sample.hold |= kWDown;
        if (navDpad & MenuNav::kNavLeft) sample.hold |= kWLeft;
        if (navDpad & MenuNav::kNavRight) sample.hold |= kWRight;
        // Radial deadzone, then gentle near the centre for fine aim and quick at full tilt.
        const float mag = std::min(std::hypot(moveX, moveY), 1.0f);
        if (mag < 0.2f) {
            moveX = moveY = 0.0f;
        } else {
            MenuNav::CancelSettle(chan);
            const float t = (mag - 0.2f) / 0.8f, speed = 0.3f + 1.9f * t * t;  // KPAD units per second
            s_cursor[chan][0] = std::clamp(s_cursor[chan][0] + moveX / mag * speed * dt, -1.0f, 1.0f);
            s_cursor[chan][1] = std::clamp(s_cursor[chan][1] - moveY / mag * speed * dt, -1.0f, 1.0f);
            moveX = moveY = 0.0f;
        }
    }
    s_cursor[chan][0] = std::clamp(s_cursor[chan][0] + dz(moveX) * 1.6f * dt, -1.0f, 1.0f);
    s_cursor[chan][1] = std::clamp(s_cursor[chan][1] - dz(moveY) * 1.6f * dt, -1.0f, 1.0f);
    sample.hasPointer = true;
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
