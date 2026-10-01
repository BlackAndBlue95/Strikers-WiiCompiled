// Mario Strikers Charged (R4QE01) game-specific HLE.
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
