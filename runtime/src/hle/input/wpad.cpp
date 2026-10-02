#include <cstdio>
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_config.h"
#include "wiimote_hid.h"
#include <dolphin/pad.h>
#include "hle/controller_status_contract.h"
#include "wii_remote_input.h"

#include <cstdint>

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg);

namespace {

constexpr int32_t kStatusOk = 0;

struct WpadStubState {
    bool initSubRan = false;
    WpadContract::State contract{};
};

WpadStubState g_state{};

void InvokeWpadCallback(uint32_t callback, uint32_t chan, int32_t result)
{
    if (callback == 0) {
        return;
    }
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callback)) {
        return;
    }
    auto& cpu = GetPersistentCpuContext();
    cpu.gpr[3] = chan;
    cpu.gpr[4] = static_cast<uint32_t>(result);
    InvokeIndirectCpu(callback, &cpu);
}

int32_t InitializeWpadLibrary()
{
    g_state.contract.Initialize();
    return kStatusOk;
}

// The async WPAD entry points all report their outcome to the guest callback and
// then return the same value.
int32_t CompleteWpadRequest(uint32_t chan, uint32_t callback, int32_t result)
{
    InvokeWpadCallback(callback, chan, result);
    return result;
}

} // namespace

extern "C" int32_t WPADGetStatus_HLE()
{
    // RVL::WPADGetStatus takes no channel arg; it reports global WUD library state.
    // Reading r3 here would leak a caller's stale register value into the result.
    return g_state.contract.GetLibraryStatus();
}
PPC_NATIVE_OVERRIDE(803CBD68, WPADGetStatus_HLE, int32_t, (), ());

extern "C" uint32_t WPADGetDpdSensitivity_HLE()
{
    return static_cast<uint32_t>(RuntimeConfigFile::IrSensitivity());
}
PPC_NATIVE_OVERRIDE(803CF954, WPADGetDpdSensitivity_HLE, uint32_t, (), ());

void SeedWpadSettings();

extern "C" int32_t WPADInitSub_HLE()
{
    if (!g_state.initSubRan) {
        g_state.initSubRan = true;
        SeedWpadSettings();
        return InitializeWpadLibrary();
    }
    return kStatusOk;
}
PPC_NATIVE_OVERRIDE(803CBAF4, WPADInitSub_HLE, int32_t, (), ());

// The SYSCONF values the SDK's WPADInit copies into its globals, which the translated WPAD accessors
// read. Without them the motor reads as disabled (WPADIsMotorEnabled), so the game never asks for
// rumble at all.
void SeedWpadSettings()
{
    constexpr uint32_t kMotorEnabled = 0x806E2C00u;    // _rumble
    constexpr uint32_t kSensorBarPos = 0x806E2C04u;    // _sensorBarPos: 0 below, 1 above the screen
    constexpr uint32_t kDpdSensitivity = 0x806E2C05u;  // _dpdSensitivity: 1-5
    constexpr uint32_t kSpeakerVolume = 0x806E2BFEu;   // _speakerVolume: the Wii's default
    Memory::Write32(kMotorEnabled, 1);  // the F10 switch decides in WPADControlMotor, so it can change live
    Memory::Write8(kSensorBarPos, RuntimeConfigFile::SensorBarAbove() ? 1 : 0);
    Memory::Write8(kDpdSensitivity, static_cast<uint8_t>(RuntimeConfigFile::IrSensitivity()));
    Memory::Write8(kSpeakerVolume, 89);
}

extern "C" int32_t WPADInit_HLE()
{
    SeedWpadSettings();
    return InitializeWpadLibrary();
}
PPC_NATIVE_OVERRIDE(803CBD04, WPADInit_HLE, int32_t, (), ());

extern "C" int32_t WUDGetStatus_HLE()
{
    return WpadContract::kStatusReady;
}
PPC_NATIVE_OVERRIDE(803D8C84, WUDGetStatus_HLE, int32_t, (), ());

// WPADGetDataFormat reads the per-channel format set by WPADSetDataFormat. Can't reuse the
// translated SDK implementation: it dereferences Bluetooth control blocks that HLE'd WPADInit
// never constructs.
// MSC: Strikers Charged sets a per-channel format (core / Nunchuk / Classic + acc + DPD) and
// only reads a channel once WPADSetDataFormat succeeds; the contract stub always refused.
static int32_t g_mscDataFormat[WpadContract::kChannelCount] = {-1, -1, -1, -1};
static bool g_mscDpdEnabled[WpadContract::kChannelCount] = {};

extern "C" int32_t WPADGetDataFormat_HLE(uint32_t chan)
{
    if (chan < WpadContract::kChannelCount && g_mscDataFormat[chan] >= 0) {
        return g_mscDataFormat[chan];
    }
    return g_state.contract.GetDataFormat(chan);
}
PPC_NATIVE_OVERRIDE(803CD1AC, WPADGetDataFormat_HLE, int32_t, (uint32_t chan), (chan));

// WPADSetDataFormat: records the per-channel data format the game asked for.
extern "C" int32_t WPADSetDataFormat_HLE(uint32_t chan, int32_t format)
{
    if (chan < WpadContract::kChannelCount && WiiRemoteInput::IsRemoteChannel(chan)) {
        g_mscDataFormat[chan] = format;
        return 0; // WPAD_ERR_OK
    }
    return g_state.contract.SetDataFormat(chan, format);
}
PPC_NATIVE_OVERRIDE(803CD1F4, WPADSetDataFormat_HLE, int32_t, (uint32_t chan, int32_t format), (chan, format));

// WPADProbe: reports the extension type of a Bluetooth remote on `chan`, or no controller.
extern "C" int32_t WPADProbe_HLE(uint32_t chan, uint32_t typePtr)
{
    if (chan >= WpadContract::kChannelCount) {
        return WpadContract::kErrorBadChannel;
    }

    // Drive the rescan state machine here too: a reconnect probe can arrive
    // before the next PADRead, and only Poll() brings a dropped remote back.
    WiiRemoteInput::Poll();

    // A real Bluetooth remote: WPAD_DEV_CORE (0) for a bare remote,
    // WPAD_DEV_FREESTYLE (1) with a Nunchuk, WPAD_DEV_CLASSIC (2) with a Classic
    // Controller. The game reads the type from here (not from
    // KPADStatus.dev_type) to pick its control scheme, and re-reads it when it
    // changes, which is what makes an extension swap mid-game work like on the
    // console. EffectiveKind keeps the last type through SDL's re-creation of
    // the joystick after a swap.
    const WiiRemoteInput::Kind kind = WiiRemoteInput::EffectiveKind(chan);
    if (WiiRemoteInput::IsRemoteChannel(chan)) {
        if (typePtr != 0) {
            uint32_t type = WpadContract::kExtensionCore;
            if (kind == WiiRemoteInput::Kind::RemoteWithNunchuk) type = 1u;
            if (kind == WiiRemoteInput::Kind::RemoteWithClassic) type = 2u;
            if (kind == WiiRemoteInput::Kind::NotWii) type = 1u; // MSC: emulated remote + Nunchuk
            Memory::Write32(typePtr, type);
        }
        return kStatusOk;
    }
    if (typePtr != 0) {
        Memory::Write32(typePtr, WpadContract::kExtensionCore);
    }
    return WpadContract::kErrorNoController;
}
PPC_NATIVE_OVERRIDE(803CCFE8, WPADProbe_HLE, int32_t, (uint32_t chan, uint32_t typePtr), (chan, typePtr));

// WPADControlMotor(chan, command): 1 starts the remote's motor, 0 stops it. The game times its
// pulses itself (RumbleActions: 111-666 ms, 45 ms on / 150 ms off while the pointer hovers). A real
// remote gets it over Bluetooth; an emulated one (a gamepad or GameCube controller on that port) on
// the controller, as SDL rumble.
extern "C" void WPADControlMotor_HLE(uint32_t chan, uint32_t command)
{
    if (chan >= WpadContract::kChannelCount) return;
    const bool on = command != 0 && RuntimeConfigFile::RumbleEnabled();
    if (WiimoteHid::Present(chan)) {
        WiimoteHid::SetRumble(chan, on);
    } else {
        PADControlMotor(chan, on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
    }
}
PPC_NATIVE_OVERRIDE(803CD57C, WPADControlMotor_HLE, void, (uint32_t chan, uint32_t command), (chan, command));

// MSC: WPADInfo for a connected remote with a Nunchuk: dpd, speaker, attach, !lowBat,
// !nearempty, battery 4/4, LED 1, protocol, firmware.
static void MscFillWpadInfo(uint32_t infoPtr)
{
    try {
        Memory::Write32(infoPtr + 0x00, 1);
        Memory::Write32(infoPtr + 0x04, 1);
        Memory::Write32(infoPtr + 0x08, 1);
        Memory::Write32(infoPtr + 0x0C, 0);
        Memory::Write32(infoPtr + 0x10, 0);
        Memory::Write8(infoPtr + 0x14, 4);
        Memory::Write8(infoPtr + 0x15, 1);
        Memory::Write8(infoPtr + 0x16, 0);
        Memory::Write8(infoPtr + 0x17, 0);
    } catch (const Memory::AccessViolation&) {
    }
}

// MSC: synchronous WPADGetInfo. The translated SDK version waits on Bluetooth control-block
// state the HLE never updates, so Strikers Charged's title screen hung on Start.
extern "C" int32_t MSC_WPADGetInfo_803CD2A4(uint32_t chan, uint32_t infoPtr)
{
    if (chan >= WpadContract::kChannelCount) {
        return WpadContract::kErrorBadChannel;
    }
    if (!WiiRemoteInput::IsRemoteChannel(chan)) {
        return WpadContract::kErrorNoController;
    }
    if (infoPtr != 0) {
        MscFillWpadInfo(infoPtr);
    }
    return 0;
}
PPC_NATIVE_OVERRIDE(803CD2A4, MSC_WPADGetInfo_803CD2A4, int32_t, (uint32_t chan, uint32_t infoPtr), (chan, infoPtr));

// MSC: IR camera control. PlatPadManager only trusts the pointer after WPADControlDpd
// succeeds; the translated SDK version drives Bluetooth state the HLE never builds.
extern "C" int32_t MSC_WPADControlDpd_803CF9D0(uint32_t chan, uint32_t command, uint32_t callback)
{
    if (chan >= WpadContract::kChannelCount) {
        return CompleteWpadRequest(chan, callback, WpadContract::kErrorBadChannel);
    }
    if (!WiiRemoteInput::IsRemoteChannel(chan)) {
        return CompleteWpadRequest(chan, callback, WpadContract::kErrorNoController);
    }
    // The pointer comes back centred when the game turns it on, unless menu navigation is placing it:
    // the game turns it off and on again around screen changes (leaving a match), and recentring
    // there fights navigation's own snap.
    if (command != 0 && !g_mscDpdEnabled[chan] && !RuntimeConfigFile::ModMenuNavigation())
        MscEmulatedRemote::RecenterPointer(chan);
    g_mscDpdEnabled[chan] = command != 0;
    return CompleteWpadRequest(chan, callback, 0);
}
PPC_NATIVE_OVERRIDE(803CF9D0, MSC_WPADControlDpd_803CF9D0, int32_t,
         (uint32_t chan, uint32_t command, uint32_t callback), (chan, command, callback));

extern "C" int32_t MSC_WPADIsDpdEnabled_803CF95C(uint32_t chan)
{
    return chan < WpadContract::kChannelCount && g_mscDpdEnabled[chan] ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(803CF95C, MSC_WPADIsDpdEnabled_803CF95C, int32_t, (uint32_t chan), (chan));

extern "C" int32_t WPADGetInfoAsync_HLE(uint32_t chan, uint32_t infoPtr, uint32_t callback)
{
    if (chan >= WpadContract::kChannelCount) {
        return CompleteWpadRequest(chan, callback, WpadContract::kErrorBadChannel);
    }

    if (WiiRemoteInput::IsRemoteChannel(chan) && infoPtr != 0) {
        MscFillWpadInfo(infoPtr);
        return CompleteWpadRequest(chan, callback, 0);
    }
    return CompleteWpadRequest(chan, callback, WpadContract::kErrorNoController);
}
PPC_NATIVE_OVERRIDE(803CD35C, WPADGetInfoAsync_HLE, int32_t,
         (uint32_t chan, uint32_t infoPtr, uint32_t callback), (chan, infoPtr, callback));

extern "C" int32_t WPADControlLed_HLE(uint32_t chan, uint32_t ledMask, uint32_t callback)
{
    if (chan >= WpadContract::kChannelCount) {
        return CompleteWpadRequest(chan, callback, WpadContract::kErrorBadChannel);
    }
    (void)ledMask;
    return CompleteWpadRequest(chan, callback, WpadContract::kErrorNoController);
}
PPC_NATIVE_OVERRIDE(803CD6B0, WPADControlLed_HLE, int32_t,
         (uint32_t chan, uint32_t ledMask, uint32_t callback), (chan, ledMask, callback));
