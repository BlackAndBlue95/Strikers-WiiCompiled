// Shared between the pack's files.
#ifndef GAMECUBE_H
#define GAMECUBE_H

struct PADStatus {  // the SDK's
    unsigned short button;
    signed char stickX, stickY, substickX, substickY;
    unsigned char triggerL, triggerR, analogA, analogB;
    signed char err;
};

// A stick axis as -1..1 (+y up), with the game's own dead zone and reach.
float StickAxis(signed char value);

// Menu navigation (navigation.cpp), once a frame for each GameCube controller.
const unsigned long kNavUp = 1, kNavDown = 2, kNavLeft = 4, kNavRight = 8;
struct NavResult {
    bool menu;        // a menu is up: the navigation has the D-pad and sticks
    bool pressA;      // tap A (B going back)
    bool pressPause;  // tap the pause button (B on the pause menu)
    int page;         // tap - (-1) or + (+1)
};
// dpad: kNav* of the D-pad; the sticks -1..1; back: B held; page: R (+1) or L (-1) held. pointer is
// the controller's pointer as KPAD reports it (-1..1, +y down), moved onto the button selected.
NavResult UpdateNavigation(int chan, unsigned long dpad, float stickX, float stickY, float cStickX, float cStickY,
                           bool back, int page, float pointer[2]);

#endif
