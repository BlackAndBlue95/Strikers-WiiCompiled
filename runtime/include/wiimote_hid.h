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

// Starts the background thread that finds, initialises and reads remotes. Idempotent.
void Start();
// Stops it and closes every remote.
void Stop();
bool Running();
// Latest state of the remote on a game channel; false when none is there.
bool Read(uint32_t chan, Sample& out);
// True when a remote currently owns `chan`.
bool Present(uint32_t chan);
// Number of remotes connected right now.
uint32_t ConnectedCount();

// Where the sensor bar sits: above the screen, or below it (the default).
void SetSensorBarAbove(bool above);
bool SensorBarAbove();
// KPAD's pointer position (KPADStatus.pos) for a sensor-bar midpoint in raw camera pixels with
// the remote's roll already undone, and the midpoint that gives a pointer position.
void MidpointToPointer(float mx, float my, float pos[2]);
void PointerToMidpoint(const float pos[2], float& mx, float& my);

} // namespace WiimoteHid
