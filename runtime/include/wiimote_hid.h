#pragma once

#include <cstdint>

// Real Wii Remotes driven directly over HID, the way Dolphin's real-Wiimote support does it.
//
// SDL's own Wii driver never enables the remote's IR camera, so the pointer Strikers Charged's
// menus and Mega Strikes are built around does not exist through it. This backend opens the
// remote through SDL's raw HID API instead, enables the IR camera and the Nunchuk, and reports
// buttons, accelerometers, IR dots and the extension together (data report 0x37). Each connected
// remote takes the lowest free game channel. While it is running, SDL's Wii driver is kept off so
// the two never talk to the same device.
namespace WiimoteHid {

struct Sample {
    bool connected = false;
    bool hasNunchuk = false;
    uint32_t hold = 0;          // WPAD_BUTTON_* bits, Nunchuk Z/C included
    float acc[3] = {};          // remote accelerometer in g, remote axes (x left, y forward, z up)
    float nunchukAcc[3] = {};   // Nunchuk accelerometer in g, same axes
    float stick[2] = {};        // Nunchuk stick, -1..1, +y up
    bool hasPointer = false;
    float pointer[2] = {};      // KPAD pos, -1..1, +y down
    // Raw IR dots as the camera reports them (1024x768, 0x3FF = none), for WPADRead.
    uint16_t dotX[4] = {0x3FF, 0x3FF, 0x3FF, 0x3FF};
    uint16_t dotY[4] = {0x3FF, 0x3FF, 0x3FF, 0x3FF};
};

// Starts the background threads that find, initialise and read remotes. Idempotent.
void Start();
// Stops them and closes every remote.
void Stop();
bool Running();
// Looks for remotes now. On Windows this also connects remotes in discoverable mode (1+2 or SYNC
// pressed) over Bluetooth, as adding one in Windows' Bluetooth settings does, and removes Wii Remote
// pairings Windows remembers that aren't connected (they keep a remote from being found again).
// While no remote is connected, searches also run one after another for the first minute after
// Start, and always with continuous searching on.
void FindRemotes();
// True while a Bluetooth search (FindRemotes) is running.
bool Searching();
// Whether to search continuously while no remote is connected (Windows; [input] Keep scanning).
void SetContinuousSearch(bool on);
// Latest state of the remote on a game channel; false when none is there.
bool Read(uint32_t chan, Sample& out);
// True when a connected remote owns `chan` (not while a device is only being probed there).
bool Present(uint32_t chan);
// Number of remotes connected right now.
uint32_t ConnectedCount();

// Where the sensor bar sits: above the screen, or below it (the default).
void SetSensorBarAbove(bool above);
bool SensorBarAbove();
// The remote's motor on a game channel. The game times its own pulses; a motor left on for more
// than a second is stopped anyway.
void SetRumble(uint32_t chan, bool on);
// The IR camera sensitivity (the Wii's setting, 1-5); connected remotes are reprogrammed.
void SetIrSensitivity(int level);
// KPAD's pointer position (KPADStatus.pos) for a sensor-bar midpoint in raw camera pixels with
// the remote's roll already undone, and the midpoint that gives a pointer position.
void MidpointToPointer(float mx, float my, float pos[2]);
void PointerToMidpoint(const float pos[2], float& mx, float& my);

} // namespace WiimoteHid
