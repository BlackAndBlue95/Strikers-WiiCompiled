// Mario Strikers Charged (R4QE01) game-specific HLE.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "abi_bridge.h"
#include "hle_stubs.h"
#include <SDL3/SDL_timer.h>
#include "guest_interrupt_context.h"
#include "ppc_runtime.h"

extern "C" void OS__Report_803B5BE4(CpuContext* ctx);

// nlPrintf(const char*, ...) is compiled to a no-op sink in retail; route it to
// OSReport so engine diagnostics (allocator panics, asserts) reach the log.
extern "C" void MSC_nlPrintf_80009B34(CpuContext* ctx)
{
    OS__Report_803B5BE4(ctx);
}

PPC_NATIVE_OVERRIDE_VOID(80009B34, MSC_nlPrintf_80009B34, (CpuContext* ctx), (ctx));

#include "wii_remote_input.h"
#include "memory.h"

// WPADSetConnectCallback(chan, cb) -> previous cb. Strikers Charged's PlatPadManager only polls
// a channel after its connect callback reports WPAD_ERR_OK, and MKW never registers one, so the
// runtime had no implementation. Store it and report a controller that is already present, as
// the SDK does for a remote paired before the game starts; MSC_PollRemoteConnections reports the
// ones that connect or disconnect later.
namespace {
uint32_t s_connectCallbacks[4]{};
bool s_reportedConnected[4]{};
constexpr int32_t kWpadErrOk = 0, kWpadErrNoController = -1;

void ReportConnection(CpuContext* ctx, uint32_t chan, bool connected)
{
    s_reportedConnected[chan] = connected;
    const uint32_t cb = s_connectCallbacks[chan];
    if (cb == 0) return;
    const uint32_t savedLr = ctx->lr;
    ctx->gpr[3] = chan;
    ctx->gpr[4] = static_cast<uint32_t>(connected ? kWpadErrOk : kWpadErrNoController);
    InvokeIndirectCpu(cb, ctx);
    ctx->lr = savedLr;
}
} // namespace

extern "C" void MSC_WPADSetConnectCallback_803CD0DC(CpuContext* ctx)
{
    const uint32_t chan = ctx->gpr[3];
    const uint32_t cb = ctx->gpr[4];
    if (chan >= 4) {
        ctx->gpr[3] = 0;
        return;
    }
    const uint32_t previous = s_connectCallbacks[chan];
    s_connectCallbacks[chan] = cb;
    s_reportedConnected[chan] = false;
    if (cb != 0 && WiiRemoteInput::IsRemoteChannel(chan)) {
        ReportConnection(ctx, chan, true);
    }
    ctx->gpr[3] = previous;
}

PPC_NATIVE_OVERRIDE_VOID(803CD0DC, MSC_WPADSetConnectCallback_803CD0DC, (CpuContext* ctx), (ctx));

// Controllers that appear after the game registered its callbacks (or SDL enumerates late at
// boot) are announced like a remote pairing mid-game, and ones that go away like a disconnect.
// Called from the per-frame KPADRead; callbacks run on a private register file, as alarms do.
void MSC_PollRemoteConnections()
{
    static uint64_t s_lastPollMs = 0;
    const uint64_t now = SDL_GetTicks();
    if (now - s_lastPollMs < 250) return;
    s_lastPollMs = now;
    for (uint32_t chan = 0; chan < 4; ++chan) {
        if (s_connectCallbacks[chan] == 0) continue;
        const bool present = WiiRemoteInput::IsRemoteChannel(chan);
        if (present == s_reportedConnected[chan]) continue;
        GuestInterruptCallbackContext interrupt;
        ReportConnection(interrupt.get(), chan, present);
    }
}

// OSYieldThread: on hardware the AI DMA interrupt keeps firing while a thread yield-spins
// (e.g. DestroyFEState waiting for audio to go idle). Here audio only pumps when the
// scheduler idles, and a yield-spinning main thread never lets it idle, so voices never stop.
// Service the pending audio interrupt first, then do the SDK's disable/SelectThread(TRUE)/restore.
extern "C" int32_t OS__DisableInterrupts_803B8F34();
extern "C" int32_t OS__RestoreInterrupts_803B8F5C(int32_t level);
extern "C" void SelectThread_803BBCB0(CpuContext* ctx);

extern "C" void MSC_OSYieldThread_803BBEF0(CpuContext* ctx)
{
    Audio_HLE_PollDeferred();
    const int32_t level = OS__DisableInterrupts_803B8F34();
    const uint32_t lr = ctx->lr;
    ctx->gpr[3] = 1;
    SelectThread_803BBCB0(ctx);
    ctx->lr = lr;
    OS__RestoreInterrupts_803B8F5C(level);
}

PPC_NATIVE_OVERRIDE_VOID(803BBEF0, MSC_OSYieldThread_803BBEF0, (CpuContext* ctx), (ctx));

// glxSetSwapMode: menus run in swap mode 3, which waits an extra retrace whenever a frame ends more
// than 12 ms after the last one. Frames here can take longer than on a Wii while still fitting in a
// 60 Hz refresh, so mode 3 dropped those menus to 30 fps; wait one retrace per frame (mode 1) instead.
extern "C" void MSC_glxSetSwapMode_8036CEB0(int32_t mode)
{
    constexpr uint32_t kGlxSwapMode = 0x806DFA7Cu;  // glx_SwapMode
    Memory::Write32(kGlxSwapMode, static_cast<uint32_t>(mode == 3 ? 1 : mode));
}
PPC_NATIVE_OVERRIDE_VOID(8036CEB0, MSC_glxSetSwapMode_8036CEB0, (int32_t mode), (mode));

// ---------------------------------------------------------------------------------------------
// Super Mario Strikers controls on GameCube-style controllers, with Charged's extras on top.
//
// The engine is Super Mario Strikers': DetInput still has its GameCube kind (m_nConnected 3) and
// remaps game actions through the GameCube table (g_pPadRemapArray), so pass, shoot/charge,
// switch player, use item and the lob modifier come out on SMS's buttons by themselves. Retail
// Charged stripped the GameCube backend and turned SMS's button actions into Wii gestures read in
// four places; with a GameCube DetInput those read SMS's buttons again (decided as SMS does, from
// who holds the ball):
//   big hit       Y without the ball                 (was: shake the Wii Remote)
//   cycle item    Z                                  (was: shake the Nunchuk)
//   deke          Y or C-stick with the ball         (was: D-pad)
//   slide tackle  B while the other team has the ball (was: D-pad)
// Charged extras: R does the character's special move (SMS's turbo slot); the D-pad keeps
// Charged's own behaviour. Real Wii Remotes keep the original code paths, ported below.
namespace {
constexpr uint32_t kDetConnected = 0x10, kDetButtons = 0x12, kDetLeftTrigger = 0x14, kDetRightTrigger = 0x15;
constexpr uint32_t kDetAnalogRightX = 0x08, kDetAnalogRightY = 0x0C;
constexpr uint32_t kDetRemoteAccel = 0x18, kDetDpdTargets = 0x30, kDetDpdCoord = 0x34;
constexpr uint32_t kDetPolarLeft = 0x44, kDetPolarRight = 0x4C, kDetRemapAngle = 0x88;
constexpr uint8_t kConnectedGameCube = 3, kConnectedFreestyle = 2;
constexpr uint16_t kGameCubeTag = 0x0080;  // set by the emulated remote (pad.cpp), channel in 0x0060
constexpr uint16_t kPadTriggerR = 0x0020;
// Game action indices (shared by SMS and Charged): DetInput::IsPressed(action, remap = true).
enum : int32_t { kActToggleItem = 22, kActHit = 24, kActSlide = 25, kActDeke = 29 };
constexpr uint32_t kBallPtr = 0x806E0BC0u;                                    // g_pBall
constexpr uint32_t kBallOwner = 0xC8, kPlayerController = 0x30C, kPlayerBall = 0x310, kPlayerTeam = 0x314;
constexpr uint32_t kAIPadDetInput = 0x2DC;                                    // cAIPad::m_pGlobalPad (DetInput*)
constexpr uint32_t kPlayerFacing = 0x24 + 0x3E;                               // m_aActualFacingDirection

bool DetCall(CpuContext* ctx, uint32_t fn, uint32_t det, int32_t action)
{
    ctx->gpr[3] = det;
    ctx->gpr[4] = static_cast<uint32_t>(action);
    ctx->gpr[5] = 1;  // remap through the controller kind's table
    InvokeIndirectCpu(fn, ctx);
    return ctx->gpr[3] != 0;
}
bool DetIsPressed(CpuContext* ctx, uint32_t det, int32_t action) { return DetCall(ctx, 0x80331C04u, det, action); }
bool DetJustPressed(CpuContext* ctx, uint32_t det, int32_t action) { return DetCall(ctx, 0x80331C70u, det, action); }
bool IsGameCube(uint32_t det) { return det && Memory::Read8(det + kDetConnected) == kConnectedGameCube; }

uint32_t PlayerDetInput(uint32_t player)
{
    const uint32_t pad = player ? Memory::Read32(player + kPlayerController) : 0;
    return pad ? Memory::Read32(pad + kAIPadDetInput) : 0;
}
uint32_t BallOwner()
{
    const uint32_t ball = Memory::Read32(kBallPtr);
    return ball ? Memory::Read32(ball + kBallOwner) : 0;
}
} // namespace

// DetInput::UpdatePolarAnalog. Its one translated caller is NetworkPeerChannel::
// ApplyNetworkPeerChannelInput, which unpacks each frame's captured input into the DetInput the
// game plays from and sets m_nConnected just before this call. A controller player's input (Wii
// kind, tagged by pad.cpp) becomes the GameCube kind here: buttons, C-stick and analog L/R from
// aurora's GameCube pad, no motion or pointer. Then the stick polars are computed as the original
// does. (Reads the local controller at apply time: right offline; online play would need the
// conversion at capture.)
extern "C" void MSC_DetInputUpdatePolarAnalog_80331D80(CpuContext* ctx)
{
    const uint32_t det = ctx->gpr[3];
    const uint16_t buttons = Memory::Read16(det + kDetButtons);
    if (Memory::Read8(det + kDetConnected) == kConnectedFreestyle && (buttons & kGameCubeTag)) {
        PADStatus pad{};
        if (MscEmulatedRemote::ReadGameCubePad((buttons >> 5) & 3u, pad)) {
            Memory::Write8(det + kDetConnected, kConnectedGameCube);
            Memory::Write16(det + kDetButtons, pad.button & 0x1F7F);
            Memory::Write8(det + kDetLeftTrigger, pad.triggerL);
            Memory::Write8(det + kDetRightTrigger, pad.triggerR);
            Memory::WriteFloat32(det + kDetAnalogRightX, std::clamp(pad.substickX / 72.0f, -1.0f, 1.0f));
            Memory::WriteFloat32(det + kDetAnalogRightY, std::clamp(pad.substickY / 72.0f, -1.0f, 1.0f));
            for (uint32_t i = 0; i < 6; ++i) Memory::WriteFloat32(det + kDetRemoteAccel + i * 4, 0.0f);
            Memory::Write8(det + kDetDpdTargets, 0);
            Memory::WriteFloat32(det + kDetDpdCoord, 0.0f);
            Memory::WriteFloat32(det + kDetDpdCoord + 4, 0.0f);
        }
    }
    // nlCartesianToPolar(m_PolarAnalogLeft, AnalogLeftX(), AnalogLeftY()), then the right stick.
    for (uint32_t side = 0; side < 2; ++side) {
        ctx->gpr[3] = det + (side ? kDetPolarRight : kDetPolarLeft);
        ctx->fpr[1].d = Memory::ReadFloat32(det + side * 8);
        ctx->fpr[2].d = Memory::ReadFloat32(det + side * 8 + 4);
        InvokeIndirectCpu(0x802B5DF4u, ctx);
    }
}
PPC_NATIVE_OVERRIDE_VOID(80331D80, MSC_DetInputUpdatePolarAnalog_80331D80, (CpuContext* ctx), (ctx));

namespace {
// cAIPad::GetMax{Remote,Freestyle}AccelDelta: the largest change against the last `count` samples
// of the pad's 30-entry acceleration history (x >= 999 marks an empty slot).
int MaxAccelDelta(uint32_t aipad, uint32_t history, uint32_t count, float delta[3])
{
    const uint32_t head = Memory::Read32(aipad + 0x2D4);
    const auto sample = [&](uint32_t slot, float v[3]) {
        for (int k = 0; k < 3; ++k) v[k] = Memory::ReadFloat32(aipad + history + slot * 12 + k * 4);
    };
    float current[3], bestPrevious[3] = {}, maximum = 0.0f;
    delta[0] = delta[1] = delta[2] = 0.0f;
    int bestOffset = 0;
    sample((head + 30) % 30, current);
    if (current[0] < 999.0f) {
        const uint32_t samples = count > 30 ? 30 : count;
        for (uint32_t offset = 1; offset < samples; ++offset) {
            float previous[3];
            sample((head + 30 - std::min(offset, 29u)) % 30, previous);
            if (previous[0] > 999.0f) break;
            const float dx = current[0] - previous[0], dy = current[1] - previous[1], dz = current[2] - previous[2];
            const float magnitude = dx * dx + dy * dy + dz * dz;
            if (magnitude > maximum) {
                maximum = magnitude;
                bestOffset = static_cast<int>(offset);
                std::copy(previous, previous + 3, bestPrevious);
            }
        }
        if (bestOffset != 0)
            for (int k = 0; k < 3; ++k) delta[k] = current[k] - bestPrevious[k];
    }
    return bestOffset;
}

// cAIPad::Detect{Left,Right}Shake for a Wii Remote: a big enough jolt, as a facing angle.
bool DetectShake(uint32_t aipad, uint32_t history, uint32_t thresholdTweak, uint32_t dirOut)
{
    const float threshold = Memory::ReadFloat32(thresholdTweak + 0x0C);  // TweakValueFloat::value
    float a[3];
    if (MaxAccelDelta(aipad, history, 5, a) > 0) {
        a[1] = std::fabs(a[1]) < std::fabs(a[2]) ? a[2] : -a[1];
        if (a[0] * a[0] + a[1] * a[1] > threshold * threshold) {
            const uint32_t det = Memory::Read32(aipad + kAIPadDetInput);
            const uint16_t remap = det ? Memory::Read16(det + kDetRemapAngle) : 0;
            Memory::Write16(dirOut, static_cast<uint16_t>(static_cast<int32_t>(std::atan2(a[1], -a[0]) * 10430.378f) + remap));
            return true;
        }
    }
    Memory::Write16(dirOut, 0);
    return false;
}
} // namespace

// bool cAIPad::DetectRightShake(u16* direction): the big hit. GameCube controls: Y, without the ball.
extern "C" void MSC_DetectRightShake_80007688(CpuContext* ctx)
{
    const uint32_t aipad = ctx->gpr[3], dirOut = ctx->gpr[4];
    const uint32_t det = Memory::Read32(aipad + kAIPadDetInput);
    if (IsGameCube(det)) {
        const uint32_t owner = BallOwner();
        const bool hasBall = owner && PlayerDetInput(owner) == det;
        Memory::Write16(dirOut, 0);
        ctx->gpr[3] = (!hasBall && DetJustPressed(ctx, det, kActHit)) ? 1u : 0u;
        return;
    }
    ctx->gpr[3] = DetectShake(aipad, 0x004, 0x80568450u, dirOut) ? 1u : 0u;  // remote history, gfRightShakeThreshold
}
PPC_NATIVE_OVERRIDE_VOID(80007688, MSC_DetectRightShake_80007688, (CpuContext* ctx), (ctx));

// bool cAIPad::DetectLeftShake(u16* direction): cycle the item. GameCube controls: Z.
extern "C" void MSC_DetectLeftShake_80007594(CpuContext* ctx)
{
    const uint32_t aipad = ctx->gpr[3], dirOut = ctx->gpr[4];
    const uint32_t det = Memory::Read32(aipad + kAIPadDetInput);
    if (IsGameCube(det)) {
        Memory::Write16(dirOut, 0);
        ctx->gpr[3] = DetJustPressed(ctx, det, kActToggleItem) ? 1u : 0u;
        return;
    }
    ctx->gpr[3] = DetectShake(aipad, 0x16C, 0x80568430u, dirOut) ? 1u : 0u;  // Nunchuk history, gfLeftShakeThreshold
}
PPC_NATIVE_OVERRIDE_VOID(80007594, MSC_DetectLeftShake_80007594, (CpuContext* ctx), (ctx));

namespace {
// The D-pad-driven actions for a GameCube controller (slide tackle, deke, Charged special), as the
// angle fn_80036D74 reports: right 0, up 0x4000 (screen space, like the D-pad and the sticks).
bool GameCubeActionAngle(CpuContext* ctx, uint32_t fielder, uint32_t det, uint16_t& angle)
{
    const auto polar = [&](uint32_t at, uint16_t& a) {
        a = Memory::Read16(det + at);
        const float r = Memory::ReadFloat32(det + at + 4);
        return r == r && r > 0.3f;  // NaN-safe: the game's fast sqrt gives NaN for a centred stick
    };
    uint16_t leftAngle = 0, cAngle = 0;
    const bool left = polar(kDetPolarLeft, leftAngle);
    // Centred stick: the way the player faces, back in screen space.
    const uint16_t facing = static_cast<uint16_t>(Memory::Read16(fielder + kPlayerFacing) - Memory::Read16(det + kDetRemapAngle));
    const uint16_t moveAngle = left ? leftAngle : facing;
    if (Memory::Read32(fielder + kPlayerBall) != 0) {
        // With the ball: deke on Y (toward the left stick) or the C-stick (toward the C-stick).
        if (polar(kDetPolarRight, cAngle)) { angle = cAngle; return true; }
        if (DetIsPressed(ctx, det, kActDeke)) { angle = moveAngle; return true; }
    } else {
        // Without it: slide tackle on B, only while the other team has the ball.
        const uint32_t owner = BallOwner();
        if (owner && Memory::Read32(owner + kPlayerTeam) != Memory::Read32(fielder + kPlayerTeam) &&
            DetIsPressed(ctx, det, kActSlide)) {
            angle = moveAngle;
            return true;
        }
    }
    // Charged extra: R does what the D-pad does in Charged (special move, slide or deke).
    if (Memory::Read16(det + kDetButtons) & kPadTriggerR) { angle = moveAngle; return true; }
    return false;
}
} // namespace

// bool fn_80036D74(cFielder*, u16* angleOut): the D-pad action (slide tackle / deke / special) and
// its direction. Real remotes: the original D-pad fold into 8 angles. GameCube controllers: SMS's
// buttons first (GameCubeActionAngle), then the same D-pad fold.
extern "C" void MSC_DpadActionDirection_80036D74(CpuContext* ctx)
{
    const uint32_t fielder = ctx->gpr[3], out = ctx->gpr[4];
    const uint32_t det = PlayerDetInput(fielder);
    uint16_t angle = 0;
    if (IsGameCube(det) && GameCubeActionAngle(ctx, fielder, det, angle)) {
        Memory::Write16(out, angle);
        ctx->gpr[3] = 1;
        return;
    }
    enum : int32_t { kLeft = 11, kRight = 12, kUp = 13, kDown = 14 };
    bool found = false;
    uint32_t a = 0;
    if (det && DetIsPressed(ctx, det, kLeft)) {
        a = 0x8000;
        if (DetIsPressed(ctx, det, kUp)) a -= 0x2000;
        else if (DetIsPressed(ctx, det, kDown)) a += 0x2000;
        found = true;
    }
    if (det && DetIsPressed(ctx, det, kRight)) {
        a = 0;
        if (DetIsPressed(ctx, det, kUp)) a = 0x2000;
        else if (DetIsPressed(ctx, det, kDown)) a = 0xE000;
        found = true;
    }
    if (det && DetIsPressed(ctx, det, kUp)) {
        a = 0x4000;
        if (DetIsPressed(ctx, det, kLeft)) a = 0x6000;
        else if (DetIsPressed(ctx, det, kRight)) a = 0x2000;
        found = true;
    }
    if (det && DetIsPressed(ctx, det, kDown)) {
        a = 0xC000;
        if (DetIsPressed(ctx, det, kLeft)) a = 0xA000;
        else if (DetIsPressed(ctx, det, kRight)) a = 0xE000;
        found = true;
    }
    if (found) Memory::Write16(out, static_cast<uint16_t>(a));
    ctx->gpr[3] = found ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE_VOID(80036D74, MSC_DpadActionDirection_80036D74, (CpuContext* ctx), (ctx));

// bool fn_80036F88(cFielder*): is a D-pad action requested (deke / special trigger).
extern "C" void MSC_DpadActionHeld_80036F88(CpuContext* ctx)
{
    const uint32_t fielder = ctx->gpr[3];
    const uint32_t det = PlayerDetInput(fielder);
    uint16_t angle = 0;
    const bool held = det && ((IsGameCube(det) && GameCubeActionAngle(ctx, fielder, det, angle)) ||
                              DetIsPressed(ctx, det, 11) || DetIsPressed(ctx, det, 12) ||
                              DetIsPressed(ctx, det, 14) || DetIsPressed(ctx, det, 13));
    ctx->gpr[3] = held ? 1u : 0u;
}
PPC_NATIVE_OVERRIDE_VOID(80036F88, MSC_DpadActionHeld_80036F88, (CpuContext* ctx), (ctx));
