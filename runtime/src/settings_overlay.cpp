#include "settings_overlay.h"
#include "audio_backend.h"
#include "aurora_events.h"
#include "controller_button_names.h"
#include "controller_mapping_wizard.h"
#include "hle/storage/riivolution.h"
#include "input_bindings.h"
#include "mods/code_mods.h"
#include "nand_path.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "wii_remote_input.h"
#include "wiimote_hid.h"

#include <imgui.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#endif

#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <aurora/aurora.h>
#include <aurora/gfx.h>

extern "C" int g_gxFrameCount;

// Defined in runtime/src/hle/audio/ax_mix.cpp. That header is private to the HLE
// directory and is not on this target's include path.

void VI_HLE_SetFrameRate(uint32_t setting);  // hle/vi.cpp
double VI_HLE_FrameRate();

namespace AxDspHle {
void SetMixWorkerEnabled(bool enabled);
}

namespace settings_overlay {
namespace {

const char* GraphicsApiDisplayName() {
    switch (aurora_get_backend()) {
    case BACKEND_D3D11: return "Direct3D 11";
    case BACKEND_D3D12: return "Direct3D 12";
    case BACKEND_METAL: return "Metal";
    case BACKEND_VULKAN: return "Vulkan";
    case BACKEND_OPENGL: return "OpenGL";
    case BACKEND_OPENGLES: return "OpenGL ES";
    case BACKEND_WEBGPU: return "WebGPU";
    case BACKEND_NULL: return "Null";
    case BACKEND_AUTO: return "Automatic";
    }
    return "Unknown";
}

bool g_topBarVisible = false;
bool g_settingsOpen = true;  // the settings window, opened again with each F10
bool g_exitPromptOpen = false;
int g_controllerPort = 0;
float g_resolutionScale = RuntimeConfigFile::ResolutionMultiplier(1.0f);
int g_audioVolumePercent = static_cast<int>(std::lround(RuntimeConfigFile::AudioVolume(1.0f) * 100.0f));
bool g_audioMuted = RuntimeConfigFile::AudioMuted(false);
int32_t g_muteHotkey = RuntimeConfigFile::MuteHotkey(SDL_SCANCODE_BACKSLASH);
bool g_audioMixWorker = RuntimeConfigFile::AudioMixWorkerEnabled(true);
int g_displayMode = [] {
    const std::string mode = RuntimeConfigFile::DisplayMode("windowed");
    if (mode == "borderless") {
        return static_cast<int>(AURORA_DISPLAY_MODE_BORDERLESS);
    }
    if (mode == "exclusive") {
        return static_cast<int>(AURORA_DISPLAY_MODE_EXCLUSIVE);
    }
    return static_cast<int>(AURORA_DISPLAY_MODE_WINDOWED);
}();
bool g_skipUnreadyPipelines = RuntimeConfigFile::SkipUnreadyPipelines(true);
bool g_disableCopyFilter = RuntimeConfigFile::DisableCopyFilter(true);
bool g_scaledEfbCopy = RuntimeConfigFile::ScaledEfbCopy(true);
bool g_showFps = RuntimeConfigFile::ShowFps(true);
bool g_forceAspect169 = RuntimeConfigFile::ForceAspect169Enabled();
std::array<int32_t, PAD_MAX_CONTROLLERS> g_configuredControllerIndices = [] {
    std::array<int32_t, PAD_MAX_CONTROLLERS> indices{};
    indices.fill(std::numeric_limits<int32_t>::min());
    return indices;
}();

using ControllerNames::kNativeButtons;
using ControllerNames::NativeButtonItem;
constexpr const auto& kControllerButtons = ControllerNames::kGameCubeButtons;

// Classic Controller Pro layout, indexed like kControllerButtons: the SNES-style
// diamond (A right, B bottom, X top, Y left) with digital bumpers driving the GC
// triggers and Z on Back/Select (the same home the NSO GC default gives it).
constexpr std::array<const char*, PAD_BUTTON_COUNT> kClassicProPreset = {
    "east",           // A
    "south",          // B
    "north",          // X
    "west",           // Y
    "start",          // Start
    "back",           // Z
    "left_shoulder",  // L
    "right_shoulder", // R
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

// PlayStation layout: bumpers drive the GC triggers, Z moves to Create/Share.
constexpr std::array<const char*, PAD_BUTTON_COUNT> kPlayStationPreset = {
    "south", "east", "west", "north", "start", "back",
    "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
};

struct ResolutionItem {
    const char* label;
    float scale;
};

using Clock = std::chrono::steady_clock;

constexpr auto kCursorAutoHideDelay = std::chrono::seconds(5);
Clock::time_point g_lastMouseActivity{Clock::now()};
bool g_cursorHidden = false;

constexpr std::array<std::string_view, 3> kDisplayModeConfigNames = {
    "windowed", "borderless", "exclusive",
};

uint64_t g_presentedFrame = 0;
std::atomic_bool g_strapInputAccepted = false;
std::atomic_uint64_t g_startupDismissFrame = UINT64_MAX;
constexpr uint64_t kStrapTransitionCoverFrames = 60;
std::atomic_bool g_bootShadersReady = false;
bool g_bootShaderNotice = false;
Clock::time_point g_bootShaderWaitStart{};
constexpr uint32_t kBootShaderNoticeThreshold = 100;
constexpr auto kBootShaderWaitLimit = std::chrono::minutes(3);

constexpr std::array<ResolutionItem, 8> kResolutions = {{
    {"Auto (window size)", 0.0f}, {"Native (1x)", 1.0f}, {"1.5x", 1.5f}, {"2x", 2.0f},
    {"3x", 3.0f}, {"4x", 4.0f}, {"6x", 6.0f}, {"8x", 8.0f},
}};

void SetResolutionScale(float scale) {
    g_resolutionScale = scale;
    VISetFrameBufferScale(scale);
    RuntimeConfigFile::SetResolutionMultiplier(scale);
}

using ControllerNames::FindNativeButton;

uint32_t ConfiguredNativeButton(const NativeButtonItem& item, const std::string& token) {
    if (!PADIsAxisButton(item.nativeButton)) return item.nativeButton;
    const size_t separator = token.find('@');
    if (separator == std::string::npos) return item.nativeButton;
    uint32_t threshold = 0;
    const char* end = token.data() + token.size();
    const auto parsed = std::from_chars(token.data() + separator + 1, end, threshold);
    if (parsed.ec != std::errc{} || parsed.ptr != end || threshold < 1 || threshold > 100)
        return item.nativeButton;
    return PADAxisButtonIdentity(item.nativeButton) | (threshold << 8);
}

struct ControllerBindingPair {
    std::string primary;
    std::string secondary;
};


// Config values hold up to two comma-separated button names ("dpad_up" or
// "dpad_up,left_shoulder"); pressing either one counts as the GC button.
ControllerBindingPair SplitControllerBinding(const std::string& value) {
    const size_t comma = value.find(',');
    if (comma == std::string::npos) {
        return {ControllerNames::TrimToken(value), {}};
    }
    return {ControllerNames::TrimToken(value.substr(0, comma)), ControllerNames::TrimToken(value.substr(comma + 1))};
}

using ControllerNames::NativeButtonForValue;

std::string NativeBindingConfig(uint32_t binding) {
    std::string value = NativeButtonForValue(binding).configName;
    if (PADIsAxisButton(binding)) value += '@' + std::to_string(PADAxisButtonThreshold(binding));
    return value;
}


void SetTopBarVisible(bool visible) {
    if (g_topBarVisible == visible) {
        return;
    }
    g_topBarVisible = visible;
    if (visible) g_settingsOpen = true;
}

void ApplyConfiguredMappings() {
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const int32_t controllerIndex = PADGetIndexForPort(port);
        if (controllerIndex == g_configuredControllerIndices[port]) {
            continue;
        }
        g_configuredControllerIndices[port] = controllerIndex;
        if (controllerIndex < 0) {
            continue;
        }
        // The [controller] bindings are positional and shared by every port, so
        // they describe whatever pad the user set them up with (usually an Xbox
        // layout: a = south). A Wii U Pro Controller has a fixed, known layout
        // (A on the east position) that aurora already maps by name; applying
        // the shared bindings on top swaps A/B and X/Y. A GameCube pad on the
        // adapter is the same: its buttons are the game's own and aurora's mapping
        // for it is fixed. (Wii Remotes with any extension never reach the PAD
        // layer: the game reads them through KPAD.)
        if (WiiRemoteInput::KindForPort(port) == WiiRemoteInput::Kind::WiiUPro || PADIsGCAdapter(port)) {
            continue;
        }

        uint32_t count = 0;
        if (PADGetButtonMappings(port, &count) == nullptr || count != PAD_BUTTON_COUNT) {
            continue;
        }
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            const auto& configured = RuntimeConfigFile::ControllerButton(i);
            if (!configured) {
                continue;
            }
            const ControllerBindingPair binding = SplitControllerBinding(*configured);
            if (const NativeButtonItem* native = FindNativeButton(binding.primary)) {
                PADSetButtonMapping(port, PADButtonMapping{ConfiguredNativeButton(*native, binding.primary), kControllerButtons[i].padButton});
            } else {
                RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                          << " button '" << binding.primary << "'" << std::endl;
            }
            uint32_t altNative = PAD_NATIVE_BUTTON_INVALID;
            if (!binding.secondary.empty()) {
                if (const NativeButtonItem* native = FindNativeButton(binding.secondary)) {
                    altNative = ConfiguredNativeButton(*native, binding.secondary);
                } else {
                    RT_LOG(RT_TAG_CONFIG) << "Unknown controller." << kControllerButtons[i].configKey
                              << " secondary button '" << binding.secondary << "'" << std::endl;
                }
            }
            PADSetAltButtonMapping(port, PADButtonMapping{altNative, kControllerButtons[i].padButton});
        }
    }
}

bool g_wiiRemotesEnabled = RuntimeConfigFile::WiiRemotesEnabled(true);
bool g_wiiContinuousScan = RuntimeConfigFile::WiiContinuousScanEnabled(false);
bool g_sensorBarAbove = RuntimeConfigFile::SensorBarAbove();
void DrawRumbleAndPointerSettings();

// Accelerometer readout and zero-point calibration for a bare remote / remote + Nunchuk.
void DrawWiiRemoteAccelerometer(uint32_t port) {
    ImGui::SeparatorText("Accelerometer");
    float sdlG[3] = {};
    float kpad[3] = {};
    if (WiiRemoteInput::ReadAccelDebug(port, sdlG, kpad)) {
        ImGui::Text("KPAD acc: x %+.2f  y %+.2f  z %+.2f g", kpad[0], kpad[1], kpad[2]);
        ImGui::TextDisabled("Flat, buttons up: (0, -1, 0).");
    } else {
        ImGui::TextDisabled("No accelerometer data yet.");
    }
    // SDL's read of the remote's calibration block often times out over Bluetooth
    // and it falls back to a nominal zero point, leaving a small per-axis bias;
    // measured here with the remote at rest.
    if (WiiRemoteInput::IsAccelCalibrating()) {
        ImGui::ProgressBar(WiiRemoteInput::AccelCalibrationProgress(), ImVec2(220.0f, 0.0f), "Hold still...");
    } else if (ImGui::Button("Calibrate (remote lying flat, buttons up)")) {
        WiiRemoteInput::StartAccelCalibration(port);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Put the remote down on a flat surface with the buttons facing up and do not touch it\n"
                          "for about two seconds. Corrects the tilt bias SDL leaves when it can't read the remote's\n"
                          "own calibration.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!RuntimeConfigFile::HasWiiAccelOffset() || WiiRemoteInput::IsAccelCalibrating());
    if (ImGui::Button("Clear calibration")) {
        WiiRemoteInput::ClearAccelCalibration();
    }
    ImGui::EndDisabled();
    if (const char* message = WiiRemoteInput::AccelCalibrationMessage()) {
        ImGui::TextWrapped("%s", message);
    } else if (RuntimeConfigFile::HasWiiAccelOffset()) {
        const std::array<double, 3> offset = RuntimeConfigFile::WiiAccelOffset();
        ImGui::TextDisabled("Stored offset: x %+.3f  y %+.3f  z %+.3f g", offset[0], offset[1], offset[2]);
    } else {
        ImGui::TextDisabled("Not calibrated (using SDL's zero point; see console.log for \"fallback accelerometer calibration\").");
    }
}

// Wii Remotes (Bluetooth) menu: driver switch, pairing help, continuous scanning and the port's controller kind.
void DrawPortSelector();

// Settings > Wii Remotes: real remotes over Bluetooth (the HID backend), then the selected port's live
// state.
void DrawWiiRemoteSettings() {
    if (ImGui::Checkbox("Use Wii Remotes / Wii U Pro Controllers", &g_wiiRemotesEnabled)) {
        RuntimeConfigFile::SetWiiRemotesEnabled(g_wiiRemotesEnabled);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Takes effect on the next launch. A Mayflash DolphinBar works too, in mode 4.");
    }
#if defined(_WIN32)
    ImGui::TextDisabled("Connecting: press 1+2 (or the red SYNC button) on the remote while the game");
    ImGui::TextDisabled("is looking: the first minute after launch, after Find Wii Remotes, or all the");
    ImGui::TextDisabled("time with Keep scanning on. Adding it in Windows' Bluetooth settings works too.");
#else
    ImGui::TextDisabled("Pairing: add the remote in the system's Bluetooth settings, pressing 1+2");
    ImGui::TextDisabled("(or the red SYNC button) on it. Leave the PIN empty.");
    ImGui::TextDisabled("A remote that was paired before also needs to be turned on with 1+2/SYNC.");
#endif
    {
        bool rumble = RuntimeConfigFile::WiiRemoteRumbleEnabled();
        if (ImGui::Checkbox("Rumble", &rumble)) {
            RuntimeConfigFile::SetWiiRemoteRumbleEnabled(rumble);
            // Stop whatever is running now: the game won't send another stop until its pulse ends.
            if (!rumble) {
                for (uint32_t chan = 0; chan < PAD_MAX_CONTROLLERS; ++chan) WiimoteHid::SetRumble(chan, false);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The game's vibration on Wii Remotes. Controllers have their own switch, under\n"
                              "Controllers.");
        }
    }
    if (ImGui::Checkbox("Sensor bar is above the screen", &g_sensorBarAbove)) {
        RuntimeConfigFile::SetSensorBarAbove(g_sensorBarAbove);
        WiimoteHid::SetSensorBarAbove(g_sensorBarAbove);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Like the Wii's Sensor Bar Position setting: where the sensor bar or DolphinBar\n"
                          "sits, so the pointer lines up with where the remote points. Off = below the screen.");
    }
    {
        // The Wii's five levels, and tenths in between (wiimote_ir_sensitivity.h).
        float irSensitivity = static_cast<float>(RuntimeConfigFile::IrSensitivity());
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::SliderFloat("IR sensitivity", &irSensitivity, 1.0f, 5.0f, "%.1f")) {
            const double level = std::round(irSensitivity * 10.0) / 10.0;
            if (level != RuntimeConfigFile::IrSensitivity()) {
                RuntimeConfigFile::SetIrSensitivity(level);
                WiimoteHid::SetIrSensitivity(level);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Like the Wii's IR Sensitivity setting (its five levels are the whole numbers):\n"
                              "raise it if the pointer drops out far from the sensor bar, lower it if lamps,\n"
                              "sunlight or reflections throw it off.");
        }
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextDisabled("How dim a light a real Wii Remote's camera still takes for a sensor bar dot (%u "
                            "connected; a controller's pointer doesn't use it). It doesn't change the pointer's "
                            "speed: with a sensor bar or DolphinBar at a normal distance every setting sees it the "
                            "same, so it only matters far away or with other lights about.",
                            WiimoteHid::ConnectedCount());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::Checkbox("Keep scanning for Wii Remotes (like Dolphin's Continuous Scanning)",
                        &g_wiiContinuousScan)) {
        RuntimeConfigFile::SetWiiContinuousScanEnabled(g_wiiContinuousScan);
        WiimoteHid::SetContinuousSearch(g_wiiContinuousScan);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While no Wii Remote is connected, keep looking for one, so a remote that dropped\n"
                          "out or was turned on later comes back by itself. Each search takes the PC's\n"
                          "Bluetooth for a moment, which Bluetooth headphones may notice.");
    }
    // The driver hint is only read at launch, so a rescan after the user turned
    // the setting off would still re-enumerate Wii devices in this session.
    ImGui::BeginDisabled(!g_wiiRemotesEnabled);
    if (WiimoteHid::Running()) {
        // The HID backend (the usual way): it scans by itself; this searches Bluetooth now.
        if (ImGui::Button("Find Wii Remotes")) {
            WiimoteHid::FindRemotes();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (WiimoteHid::Searching()) {
            ImGui::TextDisabled("Searching... press 1+2 on the remote");
        } else {
            ImGui::TextDisabled("%u connected", WiimoteHid::ConnectedCount());
        }
    } else {
        if (ImGui::Button("Rescan now")) {
            WiiRemoteInput::RescanNow();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (WiiRemoteInput::IsScanning()) {
            ImGui::TextDisabled("Scanning... (%u so far) - press 1+2 on the remote", WiiRemoteInput::ScanCount());
        } else {
            ImGui::TextDisabled("Not scanning");
        }
    }
    ImGui::SeparatorText("Port");
    DrawPortSelector();
    const uint32_t selectedGamePort = static_cast<uint32_t>(g_controllerPort);
    const WiiRemoteInput::Kind kind = WiiRemoteInput::KindForPort(selectedGamePort);
    ImGui::Text("Port %u: %s", static_cast<unsigned>(selectedGamePort + 1), WiiRemoteInput::KindLabel(kind));
    if (kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        WiiRemoteInput::KpadSample sample;
        if (WiiRemoteInput::ReadKpadSample(selectedGamePort, sample)) {
            // WPAD_CL_BUTTON_* bits, in the game's own layout (no mapping involved).
            const auto held = [&](uint32_t bit, const char* on, const char* off) { return (sample.clHold & bit) ? on : off; };
            ImGui::Text("Classic: %s %s %s %s  %s %s  %s %s  %s %s  %s %s %s %s", held(0x0010, "A", "a"),
                        held(0x0040, "B", "b"), held(0x0008, "X", "x"), held(0x0020, "Y", "y"), held(0x2000, "L", "l"),
                        held(0x0200, "R", "r"), held(0x0080, "ZL", "zl"), held(0x0004, "ZR", "zr"),
                        held(0x0400, "PLUS", "plus"), held(0x1000, "MINUS", "minus"), held(0x0001, "UP", "up"),
                        held(0x4000, "DOWN", "down"), held(0x0002, "LEFT", "left"), held(0x8000, "RIGHT", "right"));
            ImGui::Text("Sticks: L %+.2f %+.2f (WPAD %+d %+d)  R %+.2f %+.2f (WPAD %+d %+d)", sample.clLStick[0],
                        sample.clLStick[1], static_cast<int>(sample.clLStickRaw[0]),
                        static_cast<int>(sample.clLStickRaw[1]), sample.clRStick[0], sample.clRStick[1],
                        static_cast<int>(sample.clRStickRaw[0]), static_cast<int>(sample.clRStickRaw[1]));
            ImGui::TextDisabled("Capitals = held. The game reads this Classic Controller through KPAD, as on the");
            ImGui::TextDisabled("console: its buttons mean what the game says they mean, no mapping applies.");
        }
    }
    if (kind == WiiRemoteInput::Kind::WiiUPro) {
        if (SDL_Gamepad* gamepad = SDL_GetGamepadFromPlayerIndex(static_cast<int>(selectedGamePort))) {
            // SDL's Wii driver posts the D-pad as joystick buttons 11-14 (the
            // SDL_GAMEPAD_BUTTON_DPAD_* values) while its default HIDAPI mapping
            // expects a hat, so SDL_GetGamepadButton never sees them; read the
            // joystick directly, like the fallback in aurora's PADRead does.
            SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
            const auto rawButton = [&](int index) {
                return joystick != nullptr && SDL_GetJoystickButton(joystick, index);
            };
            ImGui::Text("Raw D-pad: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_DPAD_UP) ? "UP" : "up",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN) ? "DOWN" : "down",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_LEFT) ? "LEFT" : "left",
                        rawButton(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) ? "RIGHT" : "right");
            ImGui::Text("Raw face buttons: %s %s %s %s", rawButton(SDL_GAMEPAD_BUTTON_EAST) ? "A" : "a",
                        rawButton(SDL_GAMEPAD_BUTTON_SOUTH) ? "B" : "b", rawButton(SDL_GAMEPAD_BUTTON_NORTH) ? "X" : "x",
                        rawButton(SDL_GAMEPAD_BUTTON_WEST) ? "Y" : "y");
            ImGui::Text("Raw ZL/ZR: %d / %d (pressed above 0)",
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                        SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
            ImGui::TextDisabled("Capitals = held. If a button never turns to capitals while physically held,");
            ImGui::TextDisabled("that press is not reaching SDL at all (a driver-level issue, not a mapping one).");
            ImGui::TextDisabled("This pad uses Nintendo's own layout (a/b/x/y as labelled); the shared");
            ImGui::TextDisabled("button mapping above does not apply to it.");
        }
    }
    if (WiimoteHid::Sample hid; WiimoteHid::Read(selectedGamePort, hid)) {
        // Live state straight from the remote (HID backend), for checking a setup.
        ImGui::SeparatorText("Live input");
        const auto held = [&](uint32_t bit, const char* on, const char* off) { return (hid.hold & bit) ? on : off; };
        ImGui::Text("Buttons: %s %s %s %s %s %s %s  %s %s %s %s  %s %s", held(0x0800, "A", "a"), held(0x0400, "B", "b"),
                    held(0x0200, "1", "1"), held(0x0100, "2", "2"), held(0x0010, "PLUS", "plus"),
                    held(0x1000, "MINUS", "minus"), held(0x8000, "HOME", "home"), held(0x0008, "UP", "up"),
                    held(0x0004, "DOWN", "down"), held(0x0001, "LEFT", "left"), held(0x0002, "RIGHT", "right"),
                    held(0x2000, "Z", "z"), held(0x4000, "C", "c"));
        int dots = 0;
        for (int i = 0; i < 4; ++i) dots += (hid.dotX[i] < 1023 && hid.dotY[i] < 767) ? 1 : 0;
        ImGui::Text("IR dots: %d   (%u,%u) (%u,%u) (%u,%u) (%u,%u)", dots, hid.dotX[0], hid.dotY[0], hid.dotX[1],
                    hid.dotY[1], hid.dotX[2], hid.dotY[2], hid.dotX[3], hid.dotY[3]);
        if (hid.hasPointer) {
            ImGui::Text("Pointer: x %+.2f  y %+.2f", hid.pointer[0], hid.pointer[1]);
        } else {
            ImGui::Text("Pointer: off screen (no sensor bar dots seen)");
        }
        ImGui::Text("Accel: x %+.2f  y %+.2f  z %+.2f g", hid.acc[0], hid.acc[1], hid.acc[2]);
        if (hid.hasNunchuk) {
            ImGui::Text("Nunchuk: stick %+.2f %+.2f   accel %+.2f %+.2f %+.2f g", hid.stick[0], hid.stick[1],
                        hid.nunchukAcc[0], hid.nunchukAcc[1], hid.nunchukAcc[2]);
        } else {
            ImGui::Text("Nunchuk: not detected");
        }
        ImGui::TextDisabled("Capitals = held. Lying flat, buttons up, accel reads about (0, 0, +1).");
        ImGui::TextDisabled("Two IR dots should appear when pointing at the sensor bar.");
    } else if (kind == WiiRemoteInput::Kind::Remote || kind == WiiRemoteInput::Kind::RemoteWithNunchuk ||
               kind == WiiRemoteInput::Kind::RemoteWithClassic) {
        DrawWiiRemoteAccelerometer(selectedGamePort);
    }
}

const char* KeyBindingName(int scancode) {
    switch (scancode) {
    case PAD_KEY_MOUSE_LEFT: return "Mouse left";
    case PAD_KEY_MOUSE_RIGHT: return "Mouse right";
    case PAD_KEY_MOUSE_MIDDLE: return "Mouse middle";
    case PAD_KEY_MOUSE_X1: return "Mouse side 1";
    case PAD_KEY_MOUSE_X2: return "Mouse side 2";
    case PAD_KEY_INVALID: return "Unmapped";
    default:
        return scancode >= 0 && scancode < SDL_SCANCODE_COUNT
            ? SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode)) : "Unknown";
    }
}

enum class RebindKind { KeyboardButton, KeyboardAxis, Controller, MuteHotkey };
struct RebindState {
    bool active = false;
    bool openPopup = false;
    RebindKind kind{};
    uint32_t port = 0;
    uint16_t target = 0;
    bool secondary = false;
    SDL_JoystickID instance = 0;
    Clock::time_point deadline{};
    std::string label;
    std::array<bool, SDL_SCANCODE_COUNT> keys{};
    uint32_t mouse = 0;
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<bool, SDL_GAMEPAD_AXIS_COUNT> axesReady{};
} g_rebind;

void BeginRebind(RebindKind kind, uint16_t target, const char* label, bool secondary = false) {
    g_rebind = {};
    g_rebind.active = true;
    g_rebind.openPopup = true;
    g_rebind.kind = kind;
    g_rebind.port = static_cast<uint32_t>(g_controllerPort);
    g_rebind.target = target;
    g_rebind.secondary = secondary;
    g_rebind.label = label;
    g_rebind.deadline = Clock::now() + std::chrono::seconds(10);
    int count = 0;
    const bool* keys = SDL_GetKeyboardState(&count);
    std::copy_n(keys, std::min(count, static_cast<int>(g_rebind.keys.size())), g_rebind.keys.begin());
    g_rebind.mouse = SDL_GetMouseState(nullptr, nullptr);
    const int index = PADGetIndexForPort(g_rebind.port);
    if (kind == RebindKind::Controller && index >= 0) {
        if (auto* pad = PADGetSDLGamepadForIndex(index)) {
            g_rebind.instance = SDL_GetGamepadID(pad);
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
                g_rebind.buttons[i] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
            for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i)
                g_rebind.axesReady[i] = std::abs(static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i)))) < 8000;
        }
    }
}

void CompleteRebind(uint32_t value) {
    const auto& capture = g_rebind;
    if (capture.kind == RebindKind::Controller) {
        const int index = PADGetIndexForPort(capture.port);
        auto* pad = index >= 0 ? PADGetSDLGamepadForIndex(index) : nullptr;
        if (pad == nullptr || SDL_GetGamepadID(pad) != capture.instance) {
            g_rebind.active = false;
            return;
        }
        if (capture.secondary) PADSetAltButtonMapping(capture.port, {value, capture.target});
        else PADSetButtonMapping(capture.port, {value, capture.target});
        uint32_t count = 0, altCount = 0;
        auto* primary = PADGetButtonMappings(capture.port, &count);
        auto* alternate = PADGetAltButtonMappings(capture.port, &altCount);
        uint32_t primaryValue = PAD_NATIVE_BUTTON_INVALID, alternateValue = PAD_NATIVE_BUTTON_INVALID;
        for (uint32_t i = 0; i < count; ++i)
            if (primary[i].padButton == capture.target) primaryValue = primary[i].nativeButton;
        for (uint32_t i = 0; i < altCount; ++i)
            if (alternate[i].padButton == capture.target) alternateValue = alternate[i].nativeButton;
        std::string config = NativeBindingConfig(primaryValue);
        if (alternateValue != PAD_NATIVE_BUTTON_INVALID) config += ',' + NativeBindingConfig(alternateValue);
        for (size_t i = 0; i < kControllerButtons.size(); ++i)
            if (kControllerButtons[i].padButton == capture.target) RuntimeConfigFile::SetControllerButton(i, config);
    } else if (capture.kind == RebindKind::MuteHotkey) {
        g_muteHotkey = static_cast<int32_t>(value);
        RuntimeConfigFile::SetMuteHotkey(g_muteHotkey);
        g_rebind.active = false;
        return;
    } else if (capture.kind == RebindKind::KeyboardButton) {
        PADSetKeyButtonBinding(capture.port, {static_cast<int32_t>(value), capture.target});
    } else {
        PADSetKeyAxisBinding(capture.port, {static_cast<int32_t>(value), capture.target, 1});
    }
    PADSerializeMappings();
    g_rebind.active = false;
}

void DrawRebindPrompt() {
    if (g_rebind.openPopup) {
        ImGui::OpenPopup("Rebind input");
        g_rebind.openPopup = false;
    }
    if (!ImGui::BeginPopupModal("Rebind input", &g_rebind.active, ImGuiWindowFlags_AlwaysAutoResize)) {
        g_rebind.active = false;
        return;
    }
    if (g_rebind.active) {
        ImGui::Text("Rebind: %s", g_rebind.label.c_str());
        ImGui::TextUnformatted(g_rebind.kind == RebindKind::Controller
            ? "Press a controller button, pull a trigger, or move a stick."
            : g_rebind.kind == RebindKind::MuteHotkey
                ? "Press a keyboard key."
                : "Press a keyboard key or click a mouse button.");
        ImGui::TextUnformatted("Release any held input first. Backspace or Delete clears the mapping.");
        ImGui::TextUnformatted("Escape can be bound. F10 is reserved for settings.");
        const float remaining = std::chrono::duration<float>(g_rebind.deadline - Clock::now()).count();
        ImGui::Text("Unmapped in %d seconds", std::max(0, static_cast<int>(std::ceil(remaining))));
        const bool clear = ImGui::Button("Clear mapping");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) g_rebind.active = false;
        // UI clicks must not become mouse bindings (buttons activate on release).
        const bool overControl = ImGui::IsAnyItemHovered();
        if (g_rebind.active && (clear || remaining <= 0.0f)) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind != RebindKind::Controller) {
            int count = 0;
            const bool* keys = SDL_GetKeyboardState(&count);
            for (int i = 1; i < std::min(count, static_cast<int>(SDL_SCANCODE_COUNT)) && g_rebind.active; ++i) {
                if (keys[i] && !g_rebind.keys[i] && i != SDL_SCANCODE_F10) CompleteRebind(i);
                g_rebind.keys[i] = keys[i];
            }
            const uint32_t mouse = SDL_GetMouseState(nullptr, nullptr);
            for (int i = 1; i <= 5 && g_rebind.active; ++i)
                if (!overControl && g_rebind.kind != RebindKind::MuteHotkey &&
                    (mouse & ~g_rebind.mouse & (1u << (i - 1))) != 0) CompleteRebind(static_cast<uint32_t>(-i - 1));
            g_rebind.mouse = mouse;
        } else if (g_rebind.active && SDL_GetKeyboardFocus() != nullptr && g_rebind.kind == RebindKind::Controller) {
            auto* pad = SDL_GetGamepadFromID(g_rebind.instance);
            if (pad != nullptr) {
                for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && g_rebind.active; ++i) {
                    const bool pressed = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(i));
                    if (pressed && !g_rebind.buttons[i]) CompleteRebind(i);
                    g_rebind.buttons[i] = pressed;
                }
                for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT && g_rebind.active; ++i) {
                    const int value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(i));
                    if (std::abs(value) < 8000) g_rebind.axesReady[i] = true;
                    if (g_rebind.axesReady[i] && std::abs(value) >= 16384)
                        CompleteRebind(PADEncodeAxisButton(i, value < 0));
                }
            }
        }
    }
    if (!g_rebind.active) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawKeyBinding(const char* label, int scancode, RebindKind kind, uint16_t target,
                    float width = 220.0f) {
    const std::string caption = std::string(KeyBindingName(scancode)) + "##binding";
    if (ImGui::Button(caption.c_str(), ImVec2(width, 0.0f))) BeginRebind(kind, target, label);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);

}

bool DrawKeyboardSettings(uint32_t port) {
    uint32_t count = 0;
    auto* buttons = PADGetKeyButtonBindings(port, &count);
    bool enabled = buttons != nullptr;
    bool usePreset = false;
    if (ImGui::Checkbox("Keyboard and mouse", &enabled)) {
        PADSetKeyboardActive(port, enabled);
        PADSerializeMappings();
        buttons = PADGetKeyButtonBindings(port, &count);
        usePreset = enabled && std::all_of(buttons, buttons + count, [](const auto& binding) {
            return binding.scancode == PAD_KEY_INVALID;
        });
    }
    if (!enabled) return false;
    ImGui::TextDisabled("Replaces the gamepad on this port. F10 opens settings.");
    if (ImGui::Button("Use WASD + mouse preset") || usePreset) {
        const std::array<int, PAD_BUTTON_COUNT> keys = {
            PAD_KEY_MOUSE_LEFT, SDL_SCANCODE_SPACE, SDL_SCANCODE_E, SDL_SCANCODE_Q,
            SDL_SCANCODE_RETURN, PAD_KEY_MOUSE_MIDDLE, SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        };
        for (size_t i = 0; i < keys.size(); ++i)
            PADSetKeyButtonBinding(port, {keys[i], kControllerButtons[i].padButton});
        const std::array<int, PAD_AXIS_COUNT> axes = {
            SDL_SCANCODE_D, SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S,
            SDL_SCANCODE_L, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_K,
            SDL_SCANCODE_LSHIFT, PAD_KEY_MOUSE_RIGHT,
        };
        uint32_t axisCount = 0;
        auto* mappings = PADGetKeyAxisBindings(port, &axisCount);
        for (uint32_t i = 0; i < axisCount; ++i)
            PADSetKeyAxisBinding(port, {axes[i], mappings[i].padAxis, 1});
        PADSerializeMappings();
    }
    ImGui::SeparatorText("Button mapping");
    for (uint32_t i = 0; i < count; ++i) {
        int key = buttons[i].scancode;
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(220.0f);
        DrawKeyBinding(PADGetButtonName(buttons[i].padButton), key, RebindKind::KeyboardButton, buttons[i].padButton);
        ImGui::PopID();
    }
    ImGui::SeparatorText("Stick and trigger mapping");
    uint32_t axisCount = 0;
    auto* axes = PADGetKeyAxisBindings(port, &axisCount);
    for (uint32_t i = 0; i < axisCount; ++i) {
        int key = axes[i].scancode;
        ImGui::PushID(static_cast<int>(count + i));
        const char* direction = PADGetAxisDirectionLabel(axes[i].padAxis);
        const std::string label = std::string(PADGetAxisName(axes[i].padAxis)) + " " +
                                  (direction != nullptr ? direction : "");
        ImGui::SetNextItemWidth(220.0f);
        DrawKeyBinding(label.c_str(), key, RebindKind::KeyboardAxis, axes[i].padAxis);
        ImGui::PopID();
    }
    return true;
}

// Controller settings menu: port selection, controller assignment and button mapping.
int ExpressionResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

void DrawExpressionSettings() {
    ImGui::SeparatorText("Expressions (Dolphin syntax)");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440.0f);
    ImGui::TextDisabled(
        "Optional. An expression overrides nothing: its result is combined with the "
        "button mapping above. Operators ! & | ^ and functions if, min, max, clamp, "
        "timer, toggle, hold, tap, pulse, smooth, deadzone behave as they do in Dolphin.");
    ImGui::PopTextWrapPos();

    static std::array<std::string, InputBindings::kControls.size()> errors;
    static std::array<std::string, InputBindings::kControls.size()> buffers;
    static std::string importStatus;
    static int loadedPort = -1;
    static bool reloadBuffers = true;
    const auto port = static_cast<uint32_t>(g_controllerPort);

    if (loadedPort != g_controllerPort || reloadBuffers) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            buffers[i] = InputBindings::GetExpression(port, i);
        }
        errors.fill(std::string());
        loadedPort = g_controllerPort;
        reloadBuffers = false;
    }

    if (ImGui::Button("Import from Dolphin")) {
        const std::string path = InputBindings::DefaultDolphinConfigPath();
        std::string summary;
        std::string error;
        if (InputBindings::ImportDolphinConfig(path, g_controllerPort + 1, port, summary, error) < 0) {
            importStatus = error;
        } else {
            importStatus = summary;
            errors.fill(std::string());
            reloadBuffers = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reads [GCPad%d] from %%APPDATA%%\\Dolphin Emulator\\Config\\GCPadNew.ini,\n"
                          "or GCPadNew.ini next to the executable.", g_controllerPort + 1);
    }
    if (!importStatus.empty()) {
        ImGui::TextDisabled("%s", importStatus.c_str());
    }

    for (size_t i = 0; i < InputBindings::kControls.size(); ++i) {
        ImGui::PushID(static_cast<int>(i) + 2000);
        std::string& text = buffers[i];
        ImGui::SetNextItemWidth(300.0f);
        if (ImGui::InputText(InputBindings::kControls[i].label, text.data(), text.capacity() + 1,
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
                             ExpressionResizeCallback, &text)) {
            std::string error;
            errors[i] = InputBindings::SetExpression(port, i, text, error) ? std::string() : error;
        }
        if (InputBindings::IsActive(port, i)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "active");
        }
        if (!errors[i].empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%s", errors[i].c_str());
        }
        ImGui::PopID();
    }
}

// The player port the Controllers and Wii Remotes tabs show.
void DrawPortSelector() {
    for (int port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const std::string label = "Player " + std::to_string(port + 1);
        ImGui::RadioButton(label.c_str(), &g_controllerPort, port);
        if (port + 1 < PAD_MAX_CONTROLLERS) {
            ImGui::SameLine();
        }
    }
}

void DrawControllerSettings() {
    DrawPortSelector();
    ImGui::Separator();
    const uint32_t selectedGamePort = static_cast<uint32_t>(g_controllerPort);
    if (DrawKeyboardSettings(selectedGamePort)) {
        return;
    }
    ImGui::Separator();
    const char* currentName = PADGetName(selectedGamePort);
    if (WiimoteHid::Sample hid; currentName == nullptr && WiimoteHid::Read(selectedGamePort, hid)) {
        // Real remotes are driven over HID, outside SDL's controller list.
        ImGui::Text("Assigned: %s (Bluetooth HID)", hid.hasNunchuk ? "Wii Remote + Nunchuk" : "Wii Remote");
    } else {
        const int32_t index = PADGetIndexForPort(selectedGamePort);
        const int32_t slot = index >= 0 ? PADGetAdapterSlotForIndex(static_cast<uint32_t>(index)) : -1;
        if (slot >= 0) {
            ImGui::Text("Assigned: %s (adapter slot %d)", currentName != nullptr ? currentName : "None", slot + 1);
        } else {
            ImGui::Text("Assigned: %s", currentName != nullptr ? currentName : "None");
        }
    }
    if (ImGui::Button("Unassign controller")) {
        PADClearPort(selectedGamePort);
        g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The controller stays off every port, through restarts and reconnects, until you\n"
                          "assign it again.");
    }
    ImGui::Separator();
    controller_mapping_wizard::DrawSetupList();
    const uint32_t controllerCount = PADCount();
    if (controllerCount == 0) {
        ImGui::TextDisabled("No controller connected");
        return;
    }

    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::BeginCombo("##assign", "Assign a connected controller to this player")) {
        // A GameCube adapter's pads share one name: each is told apart by its slot, and the one being
        // pressed is marked.
        for (uint32_t index = 0; index < controllerCount; ++index) {
            const char* name = PADGetNameForControllerIndex(index);
            std::string label = name != nullptr ? name : "Unknown controller";
            if (const int32_t slot = PADGetAdapterSlotForIndex(index); slot >= 0) label += " (adapter slot " + std::to_string(slot + 1) + ")";
            const int32_t port = PADGetPortForIndex(index);
            label += port >= 0 ? "  - player " + std::to_string(port + 1) : "  - unassigned";
            if (PADIsControllerIndexActive(index)) label += "  <- pressing";
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(label.c_str())) {
                PADSetPortForIndex(index, selectedGamePort);
                g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
                ApplyConfiguredMappings();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // A GameCube pad on the adapter (WUP-028 or Wii U mode) has the game's own buttons, so its
    // mapping is fixed (see __PADLoadMapping); presets and per-button rebinding would only break it.
    if (PADIsGCAdapter(selectedGamePort)) {
        ImGui::SeparatorText("Button mapping");
        ImGui::TextDisabled("GameCube controller: its buttons are the game's own, so no mapping is needed.");
        DrawExpressionSettings();
        DrawRumbleAndPointerSettings();
        return;
    }

    uint32_t mappingCount = 0;
    PADButtonMapping* mappings = PADGetButtonMappings(static_cast<uint32_t>(g_controllerPort), &mappingCount);
    if (mappings == nullptr || mappingCount != PAD_BUTTON_COUNT) {
        ImGui::TextDisabled("Assign a controller to edit its buttons");
        return;
    }

    uint32_t altMappingCount = 0;
    PADButtonMapping* altMappings =
        PADGetAltButtonMappings(static_cast<uint32_t>(g_controllerPort), &altMappingCount);

    const auto writeBinding = [](size_t index, uint32_t primaryNative, uint32_t altNative) {
        std::string value = NativeBindingConfig(primaryNative);
        if (altNative != PAD_NATIVE_BUTTON_INVALID) {
            value += ',';
            value += NativeBindingConfig(altNative);
        }
        RuntimeConfigFile::SetControllerButton(index, value);
    };

    // Which rows show the second-binding combo without one being bound yet;
    // reset when the user switches ports so a stale "+" click doesn't linger.
    static std::array<bool, PAD_BUTTON_COUNT> altRowExpanded{};
    static int altRowExpandedPort = -1;
    if (altRowExpandedPort != g_controllerPort) {
        altRowExpandedPort = g_controllerPort;
        altRowExpanded.fill(false);
    }

    ImGui::SeparatorText("Presets");
    if (ImGui::Button("GameCube")) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        PADRestoreDefaultMapping(port);
        uint32_t restoredCount = 0;
        if (PADButtonMapping* restored = PADGetButtonMappings(port, &restoredCount)) {
            for (size_t i = 0; i < kControllerButtons.size(); ++i) {
                const auto it = std::find_if(restored, restored + restoredCount, [&](const PADButtonMapping& mapping) {
                    return mapping.padButton == kControllerButtons[i].padButton;
                });
                if (it != restored + restoredCount) {
                    RuntimeConfigFile::SetControllerButton(i, NativeButtonForValue(it->nativeButton).configName);
                }
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    }
    const auto applyPreset = [&](const std::array<const char*, PAD_BUTTON_COUNT>& preset) {
        const uint32_t port = static_cast<uint32_t>(g_controllerPort);
        for (size_t i = 0; i < kControllerButtons.size(); ++i) {
            if (const NativeButtonItem* native = FindNativeButton(preset[i])) {
                PADSetButtonMapping(port, PADButtonMapping{native->nativeButton, kControllerButtons[i].padButton});
                PADSetAltButtonMapping(port,
                                       PADButtonMapping{PAD_NATIVE_BUTTON_INVALID, kControllerButtons[i].padButton});
                RuntimeConfigFile::SetControllerButton(i, preset[i]);
            }
        }
        altRowExpanded.fill(false);
        PADSerializeMappings();
        mappings = PADGetButtonMappings(port, &mappingCount);
    };

    ImGui::SameLine();
    if (ImGui::Button("Classic Controller Pro")) {
        applyPreset(kClassicProPreset);
    }
    ImGui::SameLine();
    if (ImGui::Button("PlayStation")) {
        applyPreset(kPlayStationPreset);
    }

    ImGui::SeparatorText("Button mapping");
    ImGui::TextDisabled("LT / L2 = left trigger. RT / R2 = right trigger.");
    ImGui::TextDisabled("LB / L1 = left shoulder. RB / R1 = right shoulder.");
    ImGui::TextDisabled("Click a binding, then press an input. No input for 10 seconds clears it.");
    const float bindingWidth = ImGui::CalcTextSize("Right shoulder (RB / R1)").x +
                               ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
    for (size_t i = 0; i < kControllerButtons.size(); ++i) {
        auto mappingIt = std::find_if(mappings, mappings + mappingCount, [&](const PADButtonMapping& mapping) {
            return mapping.padButton == kControllerButtons[i].padButton;
        });
        if (mappingIt == mappings + mappingCount) {
            continue;
        }
        PADButtonMapping* altIt = nullptr;
        if (altMappings != nullptr && altMappingCount == PAD_BUTTON_COUNT) {
            const auto it = std::find_if(altMappings, altMappings + altMappingCount, [&](const PADButtonMapping& mapping) {
                return mapping.padButton == kControllerButtons[i].padButton;
            });
            if (it != altMappings + altMappingCount) {
                altIt = it;
            }
        }

        const NativeButtonItem& current = NativeButtonForValue(mappingIt->nativeButton);
        ImGui::PushID(static_cast<int>(i));
        const auto drawThreshold = [&](PADButtonMapping* mapping, bool secondary) {
            if (!PADIsAxisButton(mapping->nativeButton)) return;
            int threshold = static_cast<int>(PADAxisButtonThreshold(mapping->nativeButton));
            ImGui::SetNextItemWidth(bindingWidth);
            if (ImGui::SliderInt(secondary ? "##altThreshold" : "##primaryThreshold", &threshold,
                                 1, 100, "Threshold: %d%%", ImGuiSliderFlags_AlwaysClamp)) {
                const PADButtonMapping updated = {
                    PADAxisButtonIdentity(mapping->nativeButton) | (static_cast<uint32_t>(threshold) << 8),
                    mapping->padButton,
                };
                if (secondary) PADSetAltButtonMapping(selectedGamePort, updated);
                else PADSetButtonMapping(selectedGamePort, updated);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                writeBinding(i, mappingIt->nativeButton,
                             altIt != nullptr ? altIt->nativeButton : PAD_NATIVE_BUTTON_INVALID);
                PADSerializeMappings();
            }
        };
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(bindingWidth);
        const std::string primaryCaption = std::string(current.label) + "##primary";
        if (ImGui::Button(primaryCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
            BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label);
        }
        drawThreshold(mappingIt, false);
        ImGui::EndGroup();
        if (altIt != nullptr) {
            const bool altBound = altIt->nativeButton != PAD_NATIVE_BUTTON_INVALID;
            if (!altBound && !altRowExpanded[i]) {
                ImGui::SameLine();
                if (ImGui::SmallButton("+")) {
                    altRowExpanded[i] = true;
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Add a second binding; pressing either one works");
                }
            } else {
                ImGui::SameLine();
                ImGui::TextUnformatted("or");
                ImGui::SameLine();
                ImGui::BeginGroup();
                const char* altLabel = altBound ? NativeButtonForValue(altIt->nativeButton).label : "None";
                ImGui::SetNextItemWidth(bindingWidth);
                const std::string altCaption = std::string(altLabel) + "##alt";
                if (ImGui::Button(altCaption.c_str(), ImVec2(bindingWidth, 0.0f))) {
                    BeginRebind(RebindKind::Controller, kControllerButtons[i].padButton, kControllerButtons[i].label, true);
                }
                drawThreshold(altIt, true);
                ImGui::EndGroup();
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(kControllerButtons[i].label);
        ImGui::PopID();
    }
    DrawExpressionSettings();
    DrawRumbleAndPointerSettings();
}

// The master volume, mute and its shortcut (Settings > Audio, and the top bar's Audio menu).
void DrawMasterVolume() {
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Master", &g_audioVolumePercent, 0, 100, "%d%%")) {
        const float volume = static_cast<float>(g_audioVolumePercent) / 100.0f;
        AudioBackend::Instance().SetMasterVolume(volume);
        RuntimeConfigFile::SetAudioVolume(volume);
    }
    const float labelColumn = ImGui::GetCursorPosX() + ImGui::CalcItemWidth();
    if (ImGui::Checkbox("Mute", &g_audioMuted)) {
        AudioBackend::Instance().SetMuted(g_audioMuted);
        RuntimeConfigFile::SetAudioMuted(g_audioMuted);
    }
    ImGui::SameLine();
    DrawKeyBinding("Mute shortcut", g_muteHotkey, RebindKind::MuteHotkey, 0,
                   std::max(60.0f, labelColumn - ImGui::GetCursorPosX()));
}

void DrawAudioSettings() {
    DrawMasterVolume();
    ImGui::SeparatorText("Volume by kind");
    static constexpr std::array<std::pair<SoundCategory, const char*>, 5> kCategories{{
        {SoundCategory::Music, "Music"},
        {SoundCategory::Effects, "Sound effects"},
        {SoundCategory::Voices, "Voices"},
        {SoundCategory::Menus, "Menus"},
        {SoundCategory::Cutscenes, "Cutscenes"},
    }};
    for (const auto& [category, label] : kCategories) {
        int percent = static_cast<int>(std::lround(RuntimeConfigFile::SoundCategoryVolume(category) * 100.0f));
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderInt(label, &percent, 0, 100, "%d%%"))
            RuntimeConfigFile::SetSoundCategoryVolume(category, static_cast<float>(percent) / 100.0f);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled("On top of the game's own Music, SFX and Voice options. Menus: the front end's clicks "
                        "and whooshes. Cutscenes: goal and match intro scenes, and movies (from the next one).");
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    if (ImGui::Checkbox("Mix audio on a worker thread", &g_audioMixWorker)) {
        // Applies immediately: SetMixWorkerEnabled joins any in-flight mix
        // before switching, so the change never lands mid-frame.
        AxDspHle::SetMixWorkerEnabled(g_audioMixWorker);
        RuntimeConfigFile::SetAudioMixWorker(g_audioMixWorker);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Runs the AX/DSP voice mix off the game thread. Turn this off if you "
            "suspect an audio problem; the mix then runs inline as it used to.");
    }
}

// Rumble and the stick-driven pointer, for every port.
void DrawRumbleAndPointerSettings() {
    ImGui::SeparatorText("Rumble and pointer");
    bool rumble = RuntimeConfigFile::RumbleEnabled();
    if (ImGui::Checkbox("Rumble", &rumble)) {
        RuntimeConfigFile::SetRumbleEnabled(rumble);
        if (!rumble) {
            // Stop whatever is running now: the game won't send another stop until its pulse ends.
            constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
                PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD,
            };
            PADControlAllMotors(stopAll.data());
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The game's vibration on controllers that can rumble (GameCube controllers on the\n"
                          "adapter too). Wii Remotes have their own switch, under Wii Remotes.");
    }
    float pointerSpeed = static_cast<float>(RuntimeConfigFile::PointerSpeed());
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Pointer speed", &pointerSpeed, 0.25f, 4.0f, "%.2fx")) {
        RuntimeConfigFile::SetPointerSpeed(pointerSpeed);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How fast a controller's stick moves the pointer. Wii Remotes point by themselves.");
    }
}

// Shows a folder in the system's file browser (as a file:// URL: forward slashes, a leading one
// before a drive letter, the rest percent-escaped).
void OpenFolder(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path[0] != '/') path.insert(0, "/");
    std::string url = "file://";
    for (const unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~') {
            url += static_cast<char>(c);
        } else {
            char escaped[4];
            std::snprintf(escaped, sizeof(escaped), "%%%02X", c);
            url += escaped;
        }
    }
    SDL_OpenURL(url.c_str());
}

// The Wii's Mii database (NAND /shared2/menu/FaceLib/RFL_DB.dat), for the game's Mii features (online
// play, once it's supported): copied from Dolphin's NAND or imported from a file, such as a Wii
// backup's. The game reads it at startup.
namespace Miis {
std::filesystem::path DatabasePath() {
    return RuntimeNandPath::DiscoverNandRootPath() / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat";
}

// Dolphin's database, in its default user folder on this system, if it's there.
std::filesystem::path DolphinDatabase() {
    std::vector<std::filesystem::path> roots;
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA")) roots.push_back(std::filesystem::path(appData) / "Dolphin Emulator");
    if (const char* profile = std::getenv("USERPROFILE"))
        roots.push_back(std::filesystem::path(profile) / "Documents" / "Dolphin Emulator");
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        roots.push_back(std::filesystem::path(home) / "Library" / "Application Support" / "Dolphin");
#else
    if (const char* data = std::getenv("XDG_DATA_HOME")) roots.push_back(std::filesystem::path(data) / "dolphin-emu");
    if (const char* home = std::getenv("HOME")) {
        roots.push_back(std::filesystem::path(home) / ".local" / "share" / "dolphin-emu");
        roots.push_back(std::filesystem::path(home) / ".dolphin-emu");
    }
#endif
    std::error_code error;
    for (const auto& root : roots) {
        const auto database = root / "Wii" / "shared2" / "menu" / "FaceLib" / "RFL_DB.dat";
        if (std::filesystem::is_regular_file(database, error)) return database;
    }
    return {};
}

// The Miis in a database: its first 100 entries (74 bytes each, after the "RNOD" magic) that aren't
// empty. -1 when the file isn't a Mii database.
int CountMiis(const std::filesystem::path& file) {
    constexpr size_t kEntries = 100, kEntrySize = 74;
    std::ifstream in(file, std::ios::binary);
    std::array<char, 4 + kEntries * kEntrySize> data{};
    if (!in.read(data.data(), static_cast<std::streamsize>(data.size())) || std::string_view(data.data(), 4) != "RNOD")
        return -1;
    int count = 0;
    for (size_t i = 0; i < kEntries; ++i) {
        const char* entry = data.data() + 4 + i * kEntrySize;
        count += std::any_of(entry, entry + kEntrySize, [](char c) { return c != 0; }) ? 1 : 0;
    }
    return count;
}

std::mutex g_statusMutex;
std::string g_status;  // the last import's outcome

void Import(const std::filesystem::path& source) {
    std::string status;
    const int count = CountMiis(source);
    if (count < 0) {
        status = "That file isn't a Mii database (RFL_DB.dat).";
    } else {
        const auto target = DatabasePath();
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        if (std::filesystem::exists(target, error))
            std::filesystem::copy_file(target, target.string() + ".bak", std::filesystem::copy_options::overwrite_existing, error);
        std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
        status = error ? "Couldn't copy it: " + error.message()
                       : std::to_string(count) + (count == 1 ? " Mii" : " Miis") + " imported. Restart the game to use them.";
        RT_LOG(RT_TAG_NAND) << "Mii database from " << RuntimeConfigFile::PathToUtf8(source) << ": " << status << std::endl;
    }
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status = status;
}

void SDLCALL OnFileChosen(void*, const char* const* files, int) {
    if (files != nullptr && files[0] != nullptr) Import(std::filesystem::u8path(files[0]));
}

void Draw() {
    ImGui::SeparatorText("Miis");
    const int count = CountMiis(DatabasePath());
    if (count >= 0) ImGui::Text("%d %s in the game's Mii database", count, count == 1 ? "Mii" : "Miis");
    else ImGui::TextUnformatted("No Mii database yet");
    if (const auto dolphin = DolphinDatabase(); !dolphin.empty()) {
        if (ImGui::Button("Copy Miis from Dolphin")) Import(dolphin);
    }
    if (ImGui::Button("Import RFL_DB.dat...")) {
        static constexpr SDL_DialogFileFilter kFilter{"Mii database (RFL_DB.dat)", "dat"};
        SDL_ShowOpenFileDialog(OnFileChosen, nullptr, nullptr, &kFilter, 1, nullptr, false);
    }
    if (ImGui::Button("Open the Mii folder")) {
        std::error_code error;
        std::filesystem::create_directories(DatabasePath().parent_path(), error);
        OpenFolder(RuntimeConfigFile::PathToUtf8(DatabasePath().parent_path()));
    }
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        if (!g_status.empty()) ImGui::TextUnformatted(g_status.c_str());
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled("For the game's Mii features, such as online play once it's supported. Dolphin keeps its "
                        "Miis in its Wii folder (shared2/menu/FaceLib); a Wii backup's RFL_DB.dat works too.");
    ImGui::PopTextWrapPos();
}
} // namespace Miis

// F10 > Mods, Riivolution packs: every pack XML in the overlay roots with its options. A choice is
// saved to the root's riivolution/config/R4QE.xml, as Riivolution and Dolphin save it. An option whose
// choices only add files the disc doesn't have (each of the Strikers Tweaks, a file its code looks
// for) applies at once; one that replaces the game's files at the next launch, the disc being put
// together at boot; one that adds code after another build.sh too, code mods being built in.
void DrawRiivolutionPacks() {
    static std::map<std::string, uint32_t> chosen;  // pack xml + option -> choice saved this session
    static std::string saveError;
    const ImVec4 errorColour(1.0f, 0.45f, 0.35f, 1.0f), warningColour(1.0f, 0.75f, 0.25f, 1.0f);
    const auto& packs = RuntimeRiivolution::Packs();
    const std::string folder =
        RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ResolveConfigPath().parent_path() / "Riivolution");

    ImGui::SeparatorText("Riivolution packs");
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
    if (packs.empty()) {
        ImGui::TextUnformatted("No Riivolution packs installed.");
        ImGui::TextDisabled("Copy a pack as it goes on an SD card (its riivolution folder and its own folder) into %s",
                            folder.c_str());
    }
    if (!packs.empty()) {
        ImGui::TextDisabled("Options that only add files, like each of the Strikers Tweaks, apply at once; the others "
                            "at the next launch.");
    }
    bool restart = false, rebuild = false;
    for (const RuntimeRiivolution::Pack& pack : packs) {
        const std::string xml = RuntimeConfigFile::PathToUtf8(pack.xml);
        ImGui::PushID(xml.c_str());
        ImGui::TextUnformatted(RuntimeConfigFile::PathToUtf8(pack.xml.stem()).c_str());
        ImGui::TextDisabled("%s", xml.c_str());
        if (!pack.codeModules.empty()) {
            std::string names;
            for (const std::string& name : pack.codeModules) names += (names.empty() ? "" : ", ") + name;
            ImGui::TextDisabled("Code: %s", names.c_str());
        }
        // One width for the pack's choice boxes, so their labels line up.
        float width = ImGui::CalcTextSize("Disabled").x;
        for (const RuntimeRiivolution::PackOption& option : pack.options)
            for (const std::string& choice : option.choices) width = std::max(width, ImGui::CalcTextSize(choice.c_str()).x);
        width += ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
        std::string_view section;
        for (const RuntimeRiivolution::PackOption& option : pack.options) {
            if (option.section != section) {
                section = option.section;
                if (!section.empty()) ImGui::TextUnformatted(option.section.c_str());
            }
            const std::string key = xml + "\n" + option.configId;
            const auto saved = chosen.find(key);
            const uint32_t current = saved != chosen.end() ? saved->second : option.selected;
            const char* preview = current == 0 || current > option.choices.size()
                                      ? "Disabled"
                                      : option.choices[current - 1].c_str();
            ImGui::PushID(option.configId.c_str());
            ImGui::SetNextItemWidth(width);
            if (ImGui::BeginCombo(option.name.c_str(), preview)) {
                for (uint32_t choice = 0; choice <= option.choices.size(); ++choice) {
                    const char* name = choice == 0 ? "Disabled" : option.choices[choice - 1].c_str();
                    ImGui::PushID(static_cast<int>(choice));
                    if (ImGui::Selectable(name, choice == current) && choice != current) {
                        saveError = RuntimeRiivolution::SaveOptionChoice(pack, option, choice);
                        if (saveError.empty()) chosen[key] = choice;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (!option.description.empty()) ImGui::TextDisabled("%s", option.description.c_str());
            ImGui::PopID();
            if (current != option.selected && !option.live) {  // a live option's choice applies at once
                restart = true;
                rebuild |= option.addsCode;  // the others only change the disc's files
            }
        }
        ImGui::PopID();
        ImGui::Separator();
    }
    for (const CodeMods::Module& module : CodeMods::Status()) {
        switch (module.state) {
        case CodeMods::Module::State::BuiltIn:
            ImGui::TextDisabled("Code mod %s: built in", module.name.c_str());
            break;
        case CodeMods::Module::State::NotInstalled:
            ImGui::TextColored(warningColour, "Code mod %s is built in but no longer installed or turned on; it "
                               "stays in the game until build.sh runs again.", module.name.c_str());
            break;
        case CodeMods::Module::State::Changed:
            ImGui::TextColored(warningColour, "Code mod %s changed since this game was built; run build.sh again.",
                               module.name.c_str());
            break;
        case CodeMods::Module::State::NotBuiltIn:
            ImGui::TextColored(warningColour, "Code mod %s is installed but not built in; run build.sh again.",
                               module.name.c_str());
            break;
        }
    }
    if (!saveError.empty()) ImGui::TextColored(errorColour, "Couldn't save: %s", saveError.c_str());
    if (rebuild) {
        ImGui::TextColored(warningColour, "Run build.sh again, then restart the game, to apply these changes "
                                          "(the pack's code is built into the game).");
    } else if (restart) {
        ImGui::TextColored(warningColour, "Restart the game to apply these changes.");
    }
    if (ImGui::Button("Open the Riivolution folder")) {
        std::error_code ec;
        std::filesystem::create_directories(RuntimeConfigFile::PathFromUtf8(folder), ec);
        OpenFolder(folder);
    }
    ImGui::PopTextWrapPos();
}

// F10 > Mods: the Riivolution packs (DrawRiivolutionPacks).
void DrawModPackages() {
    DrawRiivolutionPacks();
}

// F10 > Tweaks: changes to the game built into the runtime ([mods] in Config.toml). The others are the
// Strikers Tweaks pack (packs/tweaks), in F10 > Mods.
void DrawModSettings() {
    bool navigation = RuntimeConfigFile::ModMenuNavigation();
    if (ImGui::Checkbox("Controller menu navigation", &navigation)) {
        RuntimeConfigFile::SetModMenuNavigation(navigation);
    }
    ImGui::TextDisabled("The D-pad and sticks move between buttons, B goes back, and L/R flip\n"
                        "pages such as stages on stage select.");
    ImGui::BeginDisabled(!navigation);
    bool badge = RuntimeConfigFile::ModSelectionBadge();
    if (ImGui::Checkbox("Selection badge", &badge)) {
        RuntimeConfigFile::SetModSelectionBadge(badge);
    }
    ImGui::TextDisabled("Mark the selected button with a badge in the player's colour.");
    ImGui::EndDisabled();
    ImGui::Separator();
    bool kitChoice = RuntimeConfigFile::ModKitChoice();
    if (ImGui::Checkbox("Choose home/away kits on captain select", &kitChoice)) {
        RuntimeConfigFile::SetModKitChoice(kitChoice);
    }
    ImGui::TextDisabled("With both captains picked, X switches the home team between its home and\n"
                        "away kit and Y the away team (Wii Remote: - and 2). Only one team can wear\n"
                        "its away kit at a time. Mario, Luigi, Waluigi and Wario get generated away\n"
                        "kits (white, sky blue, orange, blue) recoloured from their own.");
    bool allCaptains = RuntimeConfigFile::ModAllCaptains();
    if (ImGui::Checkbox("Captain-only teams", &allCaptains)) {
        RuntimeConfigFile::SetModAllCaptains(allCaptains);
    }
    ImGui::TextDisabled("Captains can be teammates (a switch the developers left in the game). On the\n"
                        "sidekick screen, pick a slot then a captain from the grid; - and + switch to\n"
                        "the sidekicks, so teams can mix both. On captain select, - and + let a sidekick\n"
                        "lead a team in a captain's colours (X/Y cycle them).");
    ImGui::SeparatorText("More tweaks");
    ImGui::TextDisabled("Skip intro, fast menus, unlock everything, win by 2, the NK bug fix and the\n"
                        "rest are the Strikers Tweaks pack, in Mods: the same pack and options as in\n"
                        "Dolphin and on a Wii.");
}

// Dolphin-style custom textures (aurora's texture_replacement.cpp): the renderer indexes the folder
// once, at startup, so changes apply on the next launch.
const bool g_customTexturesAtLaunch = RuntimeConfigFile::TextureReplacements(false);
const bool g_textureDumpsAtLaunch = g_customTexturesAtLaunch && RuntimeConfigFile::TextureDumps(false);

void DrawCustomTextureSettings() {
    ImGui::SeparatorText("Custom textures");
    bool customTextures = RuntimeConfigFile::TextureReplacements(false);
    if (ImGui::Checkbox("Custom textures", &customTextures)) RuntimeConfigFile::SetTextureReplacements(customTextures);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled("Dolphin texture packs for this game (R4QE01): put the pack's files, folders and all, in "
                        "the custom textures folder.");
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(!customTextures);
    bool dumpTextures = RuntimeConfigFile::TextureDumps(false);
    if (ImGui::Checkbox("Dump textures", &dumpTextures)) RuntimeConfigFile::SetTextureDumps(dumpTextures);
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled("Saves each texture the game shows that has no replacement yet, named as its replacement "
                        "must be, to the dumps folder: edit one and put it in the custom textures folder.");
    ImGui::PopTextWrapPos();
    if (g_customTexturesAtLaunch) ImGui::Text("%u custom textures found", aurora_get_texture_replacement_count());
    if (g_textureDumpsAtLaunch) ImGui::Text("%u textures dumped", aurora_get_texture_dump_count());
    if (customTextures != g_customTexturesAtLaunch || (customTextures && dumpTextures) != g_textureDumpsAtLaunch)
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Restart the game to apply.");
    const auto openFolder = [](const std::filesystem::path& folder) {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        OpenFolder(RuntimeConfigFile::PathToUtf8(folder));
    };
    const std::filesystem::path data = RuntimeConfigFile::ApplicationDataDirectory();
    if (ImGui::Button("Open the custom textures folder")) openFolder(data / "texture_replacements");
    ImGui::SameLine();
    if (ImGui::Button("Open the dumps folder")) openFolder(data / "Cache" / "texture_dumps");
}

void DrawGraphicsSettings() {
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    if (ImGui::Checkbox("Force 16:9", &g_forceAspect169)) {
        SetDynamicAspectForce169(g_forceAspect169);
        RuntimeConfigFile::SetForceAspect169(g_forceAspect169);
    }
    ImGui::TextDisabled("Keep a 16:9 image with black bars when the window has another shape.");
    ImGui::Separator();
    static constexpr const char* kDisplayModes[] = {
        "Windowed",
        "Borderless fullscreen",
        "Exclusive fullscreen",
    };
    if (ImGui::Combo("Display mode", &g_displayMode, kDisplayModes, static_cast<int>(std::size(kDisplayModes)))) {
        const auto mode = static_cast<AuroraDisplayMode>(g_displayMode);
        aurora_set_display_mode(mode);
        const AuroraDisplayMode activeMode = aurora_get_display_mode();
        if (activeMode == mode) {
            RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(g_displayMode)]));
        } else {
            g_displayMode = static_cast<int>(activeMode);
        }
    }
    if (g_displayMode == AURORA_DISPLAY_MODE_EXCLUSIVE) {
        ImGui::TextDisabled(
            "Requests the closest native-resolution display mode to the frame rate.");
    }
    // Native frame rate: the game renders every frame itself (its simulation is time-based), so this is
    // not interpolation.
    {
        const auto label = [](uint32_t rate) {
            if (rate != 0) return std::to_string(rate) + " FPS";
            char text[48];
            std::snprintf(text, sizeof(text), "Match the display (%.0f FPS)", VI_HLE_FrameRate());
            return std::string(text);
        };
        const uint32_t current = RuntimeConfigFile::FrameRate(60);
        if (ImGui::BeginCombo("Frame rate", label(current).c_str())) {
            for (const uint32_t rate : kFrameRates) {
                const bool selected = rate == current;
                if (ImGui::Selectable(rate == 0 ? "Match the display" : label(rate).c_str(), selected) && !selected) {
                    RuntimeConfigFile::SetFrameRate(rate);
                    VI_HLE_SetFrameRate(rate);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
        ImGui::TextDisabled("Real frames, not interpolation. Above 60 needs a display that refreshes that fast "
                            "and uses more power; 30 halves the work for slow machines.");
        ImGui::PopTextWrapPos();
    }
    if (ImGui::Checkbox("Disable copy filter", &g_disableCopyFilter)) {
        aurora_set_disable_copy_filter(g_disableCopyFilter);
        RuntimeConfigFile::SetDisableCopyFilter(g_disableCopyFilter);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled("Skips the Wii's vertical smoothing on the final image and on the screen copies the "
                        "game's effects are built from, for a sharper picture.");
    ImGui::PopTextWrapPos();
    if (ImGui::Checkbox("Scaled EFB copies", &g_scaledEfbCopy)) {
        aurora_set_scaled_efb_copies(g_scaledEfbCopy);
        RuntimeConfigFile::SetScaledEfbCopy(g_scaledEfbCopy);
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled("As Dolphin's Scaled EFB Copy. Off: the screen copies effects are built from stay at "
                        "native size, so blur, bloom and depth of field look as on a Wii at any resolution, "
                        "and while heat haze or an impact ripple plays the scene shows at native size too.");
    ImGui::PopTextWrapPos();
    if (ImGui::Checkbox("Skip draws while shaders compile", &g_skipUnreadyPipelines)) {
        aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
        RuntimeConfigFile::SetSkipUnreadyPipelines(g_skipUnreadyPipelines);
    }
    if (ImGui::Checkbox("Show FPS", &g_showFps)) {
        RuntimeConfigFile::SetShowFps(g_showFps);
    }
    DrawCustomTextureSettings();
    ImGui::Separator();
    ImGui::Text("Graphics API: %s", GraphicsApiDisplayName());
}

void DrawFpsOverlay() {
    AuroraPresentTiming presentTiming{};
    aurora_get_present_timing(&presentTiming);
    if (!g_showFps) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - kMargin, top), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoDecoration |
                                         ImGuiWindowFlags_NoFocusOnAppearing |
                                         ImGuiWindowFlags_NoInputs |
                                         ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoNav |
                                         ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("FPS Overlay", nullptr, kFlags)) {
        if (presentTiming.sampleCount == 0) {
            ImGui::TextUnformatted("FPS: --");
        } else {
            ImGui::Text("FPS: %.1f", presentTiming.framesPerSecond);
        }
    }
    ImGui::End();
}

void DrawShaderCompilationStatus() {
    const uint32_t queuedPipelines = aurora_get_queued_pipeline_count();
    if (queuedPipelines == 0) {
        return;
    }

    constexpr float kMargin = 10.0f;
    const float top = g_topBarVisible ? ImGui::GetFrameHeight() + kMargin : kMargin;
    ImGui::SetNextWindowPos(ImVec2(kMargin, top), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(7.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Shader Compilation Status", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        ImGui::Text("%u shader%s compiling", queuedPipelines, queuedPipelines == 1 ? "" : "s");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void DrawStartupScreen() {
    if (!StartupScreenVisible()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Wiicompiled Startup", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(1.25f);
        constexpr const char* kTitle = "Strikers Recharged";
        const ImVec2 titleSize = ImGui::CalcTextSize(kTitle);
        const float titleX = std::max(0.0f, (viewport->Size.x - titleSize.x) * 0.5f);
        const float startY = std::max(0.0f, (viewport->Size.y - titleSize.y) * 0.5f);
        ImGui::SetCursorPos(ImVec2(titleX, startY));
        ImGui::TextUnformatted(kTitle);
        if (g_bootShaderNotice && !g_bootShadersReady.load(std::memory_order_relaxed)) {
            ImGui::SetWindowFontScale(0.9f);
            char line[96];
            std::snprintf(line, sizeof(line), "Compiling shaders, please hold on: %u remaining",
                          aurora_get_queued_pipeline_count());
            const float lineX = std::max(0.0f, (viewport->Size.x - ImGui::CalcTextSize(line).x) * 0.5f);
            ImGui::SetCursorPos(ImVec2(lineX, startY + titleSize.y * 1.8f));
            ImGui::TextUnformatted(line);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void UpdateBootShaderState() {
    if (g_bootShadersReady.load(std::memory_order_relaxed)) {
        return;
    }
    const auto now = Clock::now();
    if (g_bootShaderWaitStart == Clock::time_point{}) {
        g_bootShaderWaitStart = now;
    }
    const uint32_t queued = aurora_get_queued_pipeline_count();
    g_bootShaderNotice |= queued > kBootShaderNoticeThreshold;
    if (queued == 0 || now - g_bootShaderWaitStart > kBootShaderWaitLimit) {
        g_bootShadersReady.store(true, std::memory_order_release);
    }
}

void DrawExitPrompt() {
    constexpr const char* kTitle = "Exit";
    if (g_exitPromptOpen && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    if (!ImGui::BeginPopupModal(kTitle, &g_exitPromptOpen, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Quit the game?");
    if (ImGui::Button("Exit", ImVec2(120.0f, 0.0f))) ExitForAuroraWindowClose();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) g_exitPromptOpen = false;
    if (!g_exitPromptOpen) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Settings > Graphics: the internal resolution, then the other video options.
void DrawResolutionChoice() {
    const auto current = std::find_if(kResolutions.begin(), kResolutions.end(), [](const ResolutionItem& item) {
        return std::fabs(item.scale - g_resolutionScale) < 0.001f;
    });
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::BeginCombo("Resolution", current != kResolutions.end() ? current->label : "Custom")) {
        for (const auto& resolution : kResolutions) {
            const bool selected = std::fabs(resolution.scale - g_resolutionScale) < 0.001f;
            if (ImGui::Selectable(resolution.label, selected)) SetResolutionScale(resolution.scale);
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("The game's internal resolution. Higher is sharper and heavier.");
    ImGui::Separator();
}

// The settings window (F10): one tab per area. Its tab bar stays put while a tab's content scrolls.
void DrawSettingsWindow() {
    if (!g_settingsOpen) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float top = ImGui::GetFrameHeight() + 12.0f;
    const ImVec2 available(viewport->Size.x - 24.0f, viewport->Size.y - top - 12.0f);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + top), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(std::min(780.0f, available.x), std::min(680.0f, available.y)), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(380.0f, available.x), std::min(260.0f, available.y)), available);
    if (ImGui::Begin("Settings", &g_settingsOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextDisabled("Game controls are off while settings are open. F10 returns to the game.");
        if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
            const auto tab = [](const char* name, void (*draw)()) {
                if (!ImGui::BeginTabItem(name)) return;
                if (ImGui::BeginChild("TabContent", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                                      ImGuiWindowFlags_HorizontalScrollbar)) {
                    draw();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            };
            tab("Graphics", [] {
                DrawResolutionChoice();
                DrawGraphicsSettings();
            });
            tab("Audio", DrawAudioSettings);
            tab("Controllers", DrawControllerSettings);
            tab("Wii Remotes", DrawWiiRemoteSettings);
            tab("Tweaks", DrawModSettings);
            tab("Mods", DrawModPackages);
            ImGui::EndTabBar();
        }
        DrawRebindPrompt();
    }
    ImGui::End();
}

void DrawTopBar() {
    if (!g_topBarVisible) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, 70));
    if (!g_settingsOpen) {  // the settings window says it itself
        constexpr float kHintMargin = 10.0f;
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                     viewport->Pos.y + ImGui::GetFrameHeight() + kHintMargin),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.55f);
        if (ImGui::Begin("Settings input hint", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing)) {
            for (const char* line : {"Settings open - game controls disabled.",
                                     "Press F10 to return to the game."}) {
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(line).x) * 0.5f);
                ImGui::TextUnformatted(line);
            }
        }
        ImGui::End();
    }
    DrawSettingsWindow();
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("Strikers Recharged")) {
        Miis::Draw();
        ImGui::Separator();
        if (ImGui::Button("Open the game's data folder"))
            OpenFolder(RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ApplicationDataDirectory()));
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Settings", nullptr, g_settingsOpen)) {
        g_settingsOpen = !g_settingsOpen;
    }

    // Master volume and mute, at hand; the rest is in Settings > Audio.
    const std::string audioLabel = g_audioMuted
        ? "Audio: Muted"
        : "Audio: " + std::to_string(g_audioVolumePercent) + "%";
    // Keep the popup ID stable while the Master slider changes the visible
    // label. Without the ### suffix, ImGui treats every new percentage as a
    // different menu and closes the popup on the first drag update.
    const std::string audioMenuLabel = audioLabel + "###AudioSettingsMenu";
    if (ImGui::BeginMenu(audioMenuLabel.c_str())) {
        DrawMasterVolume();
        ImGui::TextDisabled("More in Settings > Audio.");
        DrawRebindPrompt();
        ImGui::EndMenu();
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float hideWidth = ImGui::CalcTextSize("Hide (F10)").x + style.FramePadding.x * 2.0f;
    const float exitWidth = ImGui::CalcTextSize("X").x + style.FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),
                                  ImGui::GetWindowWidth() - hideWidth - exitWidth - style.ItemSpacing.x - 8.0f));
    if (ImGui::MenuItem("Hide (F10)")) {
        SetTopBarVisible(false);
    }
    if (ImGui::MenuItem("X")) {
        g_exitPromptOpen = true;
    }
    ImGui::EndMainMenuBar();
}

bool IsToggleKey(const SDL_Event& event, SDL_Scancode code) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == code;
}

bool IsMouseActivity(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

// Runs on the thread that pumps SDL events (the same one that calls Draw), so
// the SDL cursor calls are safe here.
void UpdateCursorAutoHide() {
    const bool shouldHide =
        !g_topBarVisible && Clock::now() - g_lastMouseActivity >= kCursorAutoHideDelay;
    if (shouldHide == g_cursorHidden) {
        return;
    }
    g_cursorHidden = shouldHide;
    // ImGui_ImplSDL3_NewFrame calls SDL_ShowCursor every frame unless this flag is set.
    if (shouldHide) {
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        SDL_HideCursor();
    } else {
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    }
}

// Alt+Enter toggles the display mode inside aurora without going through the
// F10 combo, so the active mode is compared against the last persisted one
// every frame and written back on change.
void PersistDisplayModeIfChanged() {
    const int active = static_cast<int>(aurora_get_display_mode());
    if (active == g_displayMode) {
        return;
    }
    g_displayMode = active;
    RuntimeConfigFile::SetDisplayMode(std::string(kDisplayModeConfigNames[static_cast<size_t>(active)]));
}

void ApplyInputBlockState() {
    const bool blocked = controller_mapping_wizard::IsActive() || g_rebind.active ||
                         g_exitPromptOpen || g_topBarVisible || StartupScreenVisible();
    PADBlockInput(blocked);
    InputBindings::SetInputBlocked(blocked);
}
} // namespace

void InitializeRuntimeSettings() noexcept {
    InputBindings::Reload();
    controller_mapping_wizard::LoadPersistedMappings();
    ApplyConfiguredMappings();
    AudioBackend::Instance().SetMasterVolume(static_cast<float>(g_audioVolumePercent) / 100.0f);
    AudioBackend::Instance().SetMuted(g_audioMuted);
    aurora_set_display_mode(static_cast<AuroraDisplayMode>(g_displayMode));
    g_displayMode = static_cast<int>(aurora_get_display_mode());
    aurora_set_disable_copy_filter(g_disableCopyFilter);
    aurora_set_scaled_efb_copies(g_scaledEfbCopy);
    aurora_set_skip_unready_pipelines(g_skipUnreadyPipelines);
    g_strapInputAccepted.store(false, std::memory_order_relaxed);
    g_startupDismissFrame.store(UINT64_MAX, std::memory_order_relaxed);
    g_bootShadersReady.store(false, std::memory_order_relaxed);
    g_bootShaderNotice = false;
    g_bootShaderWaitStart = {};
    PADBlockInput(false);
    InputBindings::SetInputBlocked(false);
}

void HandleEvents(const AuroraEvent* events) noexcept {
    if (!events) {
        return;
    }
    for (const AuroraEvent* ev = events; ev->type != AURORA_NONE; ++ev) {
        if (ev->type == AURORA_CONTROLLER_ADDED || ev->type == AURORA_CONTROLLER_REMOVED) {
            g_configuredControllerIndices.fill(std::numeric_limits<int32_t>::min());
        }
        if (ev->type != AURORA_SDL_EVENT) {
            continue;
        }
        controller_mapping_wizard::HandleSdlEvent(ev->sdl);
        if (g_rebind.active && (IsToggleKey(ev->sdl, SDL_SCANCODE_BACKSPACE) ||
                                IsToggleKey(ev->sdl, SDL_SCANCODE_DELETE))) {
            CompleteRebind(g_rebind.kind == RebindKind::Controller ? PAD_NATIVE_BUTTON_DISABLED
                                                                  : static_cast<uint32_t>(PAD_KEY_INVALID));
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_F10)) {
            SetTopBarVisible(!g_topBarVisible);
            ApplyInputBlockState();
        }
        if (!g_rebind.active && g_muteHotkey != PAD_KEY_INVALID &&
            IsToggleKey(ev->sdl, static_cast<SDL_Scancode>(g_muteHotkey))) {
            g_audioMuted = !g_audioMuted;
            AudioBackend::Instance().SetMuted(g_audioMuted);
            RuntimeConfigFile::SetAudioMuted(g_audioMuted);
        }
        if (!g_rebind.active && IsToggleKey(ev->sdl, SDL_SCANCODE_ESCAPE)) {
            if (g_exitPromptOpen) {
                g_exitPromptOpen = false;
            } else if (g_topBarVisible) {
                SetTopBarVisible(false);
            } else {
                g_exitPromptOpen = true;
            }
            ApplyInputBlockState();
        }
        if (IsMouseActivity(ev->sdl)) {
            g_lastMouseActivity = Clock::now();
        }
    }
}

void ReleaseControllers() noexcept {
    // Aurora drives the LED white on first PADRead and never clears it, and the
    // exit paths terminate the process outright, so do it here.
    bool queued = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) continue;
        if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(static_cast<u32>(index))) {
            SDL_SetGamepadLED(pad, 0, 0, 0);
            queued = true;
        }
    }
    constexpr std::array<uint32_t, PAD_MAX_CONTROLLERS> stopAll{
        PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD, PAD_MOTOR_STOP_HARD};
    PADControlAllMotors(stopAll.data());
    // SDL hands LED and rumble reports to its own HIDAPI sender thread rather
    // than writing them here, so without this the process dies before the
    // controller ever receives them.
    if (queued) SDL_Delay(120);
}

void Draw() noexcept {
    // Wait for the frame worker's DONE phase: it has replayed the previous frame's ImGui draw lists
    // and started the next ImGui frame, so all overlay callers can now safely issue ImGui commands.
    aurora_wait_for_frame_worker();
    // Also drive the Wii Remote rescan from here: PADRead runs it too, but this
    // runs once per presented frame whatever the game is doing (e.g. sitting in
    // its "communications interrupted" prompt without polling pads). Same guest
    // thread as PADRead, so no concurrent access to the scanner's state.
    WiiRemoteInput::Poll();
    ApplyConfiguredMappings();
    PersistDisplayModeIfChanged();
    UpdateCursorAutoHide();
    UpdateBootShaderState();
    if (!StartupScreenVisible()) {
        DrawShaderCompilationStatus();
    }
    MscEmulatedRemote::DrawOverlay();
    DrawFpsOverlay();
    DrawTopBar();
    DrawExitPrompt();
    controller_mapping_wizard::Draw();
    ApplyInputBlockState();
    DrawStartupScreen();
}

void NotifyStrapInputAccepted() noexcept;
bool StartupScreenVisible() noexcept {
    // MSC: there is no MKW strap scene to accept input on; drop the splash once shaders are ready.
    if (g_bootShadersReady.load(std::memory_order_acquire) &&
        !g_strapInputAccepted.load(std::memory_order_acquire)) {
        NotifyStrapInputAccepted();
    }
    return !g_strapInputAccepted.load(std::memory_order_acquire) ||
           !g_bootShadersReady.load(std::memory_order_acquire) ||
           g_presentedFrame < g_startupDismissFrame.load(std::memory_order_relaxed);
}

void NotifyStrapInputAccepted() noexcept {
    bool expected = false;
    if (g_strapInputAccepted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        g_startupDismissFrame.store(g_presentedFrame + kStrapTransitionCoverFrames,
                                    std::memory_order_release);
    }
}

void AdvancePresentedFrame() noexcept { ++g_presentedFrame; }
} // namespace settings_overlay
