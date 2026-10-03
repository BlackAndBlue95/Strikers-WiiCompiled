// GameCube controllers on a Wii (and in Dolphin): a GameCube controller in port N plays as player N's
// Wii Remote and Nunchuk whenever no Wii Remote is on channel N, as Strikers Recharged's own support
// does (runtime/src/hle/input/pad.cpp and msc_game.cpp there; Recharged has it built in).
//
// The game reads its controllers in one place each frame, UpdatePlatPad: for each channel a Wii Remote
// has connected on, PlatPadManager::UpdateChannel probes it and reads it (WPADProbe, WPADRead,
// KPADRead). A channel with a GameCube controller in its port counts as connected, and those calls,
// made there and only there, answer for the controller: a Wii Remote with a Nunchuk, the stick the
// Nunchuk's, held level, the buttons mapped as below and a pointer. The HOME Button menu and the
// remotes' speakers keep seeing real remotes only.
//
// Menus are pointer-only, so they are navigated as on a console (navigation.cpp): the D-pad and stick
// flicks move between buttons, A confirms, B goes back, L and R page.
//
// In matches it plays with Super Mario Strikers' controls. The engine is Super Mario Strikers': its
// player input (DetInput) still has a GameCube controller kind with SMS's button table, so the remote's
// button word carries a tag (0x0080, which no Wii button uses, with the port in 0x0060) and the
// player's input becomes that kind, read from the controller itself. Charged turned four of SMS's
// button actions into gestures; for a GameCube controller they are SMS's buttons again:
//   big hit       Y without the ball                 (a Wii Remote shake)
//   cycle item    Z                                  (a Nunchuk shake)
//   deke          Y or the C-stick with the ball     (the D-pad)
//   slide tackle  B while the other team has the ball (the D-pad)
//   special move  R                                  (the D-pad)
#include <kamek.h>

#include "gamecube.h"

extern "C" {
unsigned long PADRead(PADStatus* status);
int PADReset(unsigned long mask);
unsigned long VIGetRetraceCount();
long WPADProbe(long chan, unsigned long* type);
long WPADSetDataFormat(long chan, unsigned long format);
void WPADRead(long chan, void* status);
long WPADControlDpd(long chan, unsigned long command, void* callback);
long WPADGetInfo(long chan, void* info);
long KPADRead(long chan, void* samples, unsigned long count);
void* memset(void* to, int value, unsigned long size);
}

namespace {

template <typename T> inline T& At(const void* base, unsigned long offset) {
    return *(T*)((char*)base + offset);
}

// PAD (GameCube controller) buttons, and the Wii Remote's.
const unsigned short kPadLeft = 0x0001, kPadRight = 0x0002, kPadDown = 0x0004, kPadUp = 0x0008, kPadR = 0x0020,
                     kPadL = 0x0040, kPadA = 0x0100, kPadB = 0x0200, kPadX = 0x0400, kPadStart = 0x1000;
const unsigned long kWiiPlus = 0x0010, kWiiOne = 0x0200, kWiiB = 0x0400, kWiiA = 0x0800, kWiiMinus = 0x1000,
                    kWiiZ = 0x2000, kWiiC = 0x4000;
const unsigned long kTag = 0x0080;  // a GameCube controller's remote; its port in 0x0060
const long kWpadOk = 0, kWpadNoController = -1, kDevFreestyle = 1;

PADStatus s_pad[4];
bool s_present[4];   // a controller in the port, read this frame
bool s_emulated[4];  // channel N is GameCube controller N this frame
unsigned long s_hold[4];  // the remote's buttons this frame
float s_pointer[4][2];    // the pointer, as KPAD reports it (-1..1, +y down)
unsigned long s_previousHold[4];
unsigned long s_frame = 0xFFFFFFFF;
bool s_started;

// The controllers, once a frame: present ones read, empty ports probed again (the SDK's way of
// finding a controller plugged in later).
void Refresh() {
    const unsigned long frame = VIGetRetraceCount();
    if (frame == s_frame) return;
    s_frame = frame;
    if (!s_started) {
        s_started = true;
        PADReset(0xF0000000);  // the game never initializes its GameCube controllers
    }
    PADStatus pads[4];
    PADRead(pads);
    unsigned long reset = 0;
    for (int i = 0; i < 4; ++i) {
        if (pads[i].err == 0) {
            s_pad[i] = pads[i];
            s_present[i] = true;
        } else if (pads[i].err == -1) {  // PAD_ERR_NO_CONTROLLER
            s_present[i] = false;
            reset |= 0x80000000 >> i;
        }  // not ready yet, or a transfer error: as last frame
    }
    if (reset) PADReset(reset);
}

// The remote GameCube controller `chan` is this frame: A and B as themselves, X the Nunchuk's C, L its
// Z, Start the 1 button (pause), the D-pad as the D-pad; in menus the navigation's own presses. The
// left stick moves the pointer outside menus.
void BuildRemote(int chan) {
    const PADStatus& pad = s_pad[chan];
    const unsigned short b = pad.button;
    unsigned long hold = b & (kPadLeft | kPadRight | kPadDown | kPadUp);  // the same bits
    if (b & kPadA) hold |= kWiiA;
    if (b & kPadB) hold |= kWiiB;
    if (b & kPadX) hold |= kWiiC;
    if (b & kPadL) hold |= kWiiZ;
    if (b & kPadStart) hold |= kWiiOne;
    hold |= kTag | (unsigned long)chan << 5;
    const float stickX = StickAxis(pad.stickX), stickY = StickAxis(pad.stickY);
    unsigned long dpad = 0;
    if (b & kPadUp) dpad |= kNavUp;
    if (b & kPadDown) dpad |= kNavDown;
    if (b & kPadLeft) dpad |= kNavLeft;
    if (b & kPadRight) dpad |= kNavRight;
    const int page = (b & kPadR) ? 1 : (b & kPadL) ? -1 : 0;
    const NavResult r = UpdateNavigation(chan, dpad, stickX, stickY, StickAxis(pad.substickX), StickAxis(pad.substickY),
                                         (b & kPadB) != 0, page, s_pointer[chan]);
    if (r.pressA) hold |= kWiiA;
    if (r.pressPause) hold |= kWiiOne;
    if (r.page > 0) hold |= kWiiPlus;
    if (r.page < 0) hold |= kWiiMinus;
    float moveX = stickX, moveY = stickY;
    if (r.menu) {
        moveX = moveY = 0.0f;  // the stick belongs to the navigation
    }
    float* p = s_pointer[chan];
    p[0] += moveX * (1.6f / 60.0f);
    p[1] -= moveY * (1.6f / 60.0f);
    for (int k = 0; k < 2; ++k) p[k] = p[k] > 1.0f ? 1.0f : p[k] < -1.0f ? -1.0f : p[k];
    s_hold[chan] = hold;
}

// WPADFSStatus (0x32 bytes; a WPADStatus is its first 0x2A): a Wii Remote held level with a Nunchuk.
// The accelerometers at the SDK's default counts per g (remote 106, Nunchuk 204); no IR dots (KPAD
// reports the pointer).
void FillWpad(int chan, unsigned char* status, bool nunchuk) {
    memset(status, 0, nunchuk ? 0x32 : 0x2A);
    At<unsigned short>(status, 0x00) = (unsigned short)s_hold[chan];
    At<short>(status, 0x06) = 106;  // accZ: 1 g down
    for (int i = 0; i < 4; ++i) {
        At<short>(status, 0x08 + i * 8) = 0x3FF;
        At<short>(status, 0x0A + i * 8) = 0x3FF;
    }
    status[0x28] = kDevFreestyle;
    status[0x29] = 0;
    if (!nunchuk) return;
    At<short>(status, 0x2E) = 204;  // fsAccZ: 1 g down
    status[0x30] = (unsigned char)s_pad[chan].stickX;  // ClampWiiStick takes these as the Nunchuk's
    status[0x31] = (unsigned char)s_pad[chan].stickY;
}

// KPADStatus (0x84 bytes): the same, with the pointer as a two-dot reading.
void FillKpad(int chan, unsigned char* status) {
    memset(status, 0, 0x84);
    const unsigned long hold = s_hold[chan];
    At<unsigned long>(status, 0x00) = hold;
    At<unsigned long>(status, 0x04) = hold & ~s_previousHold[chan];
    At<unsigned long>(status, 0x08) = s_previousHold[chan] & ~hold;
    s_previousHold[chan] = hold;
    At<float>(status, 0x10) = -1.0f;  // acc.y: level
    At<float>(status, 0x18) = 1.0f;   // acc_value
    At<float>(status, 0x20) = s_pointer[chan][0];
    At<float>(status, 0x24) = s_pointer[chan][1];
    At<float>(status, 0x34) = 1.0f;  // horizon.x: level
    At<float>(status, 0x48) = 1.0f;  // dist
    status[0x5C] = kDevFreestyle;    // dev_type
    status[0x5E] = 2;                // dpd_valid_fg: two dots seen
    status[0x5F] = 5;                // data_format: WPAD_FMT_FS_BTN_ACC_DPD
    At<float>(status, 0x60) = StickAxis(s_pad[chan].stickX);
    At<float>(status, 0x64) = StickAxis(s_pad[chan].stickY);
    At<float>(status, 0x6C) = -1.0f;  // Nunchuk acc.y: level
    At<float>(status, 0x74) = 1.0f;
}

}  // namespace

// A stick axis as -1..1: the game's own dead zone (15) and reach (56), as ClampWiiStick has them.
float StickAxis(signed char value) {
    int v = value;
    if (v > -15 && v < 15) return 0.0f;
    float f = (float)(v > 0 ? v - 15 : v + 15) / 56.0f;
    return f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
}

// ---- The controllers, as the pad manager reads them

asm static void UpdatePlatPad_Original() {
    nofralloc
    stwu r1, -16(r1)
    lis r12, 0x8037
    ori r12, r12, 0x5380
    mtctr r12
    bctr
}

// void UpdatePlatPad(PlatPadManager*): updates each channel a Wii Remote is connected on. A GameCube
// controller in a channel's port connects it too.
static void UpdatePlatPad(unsigned char* manager) {
    Refresh();
    for (int chan = 0; chan < 4; ++chan)
        if (s_present[chan]) manager[0x2F0 + chan] = true;  // PlatPadManager::connected
    ((void (*)(unsigned char*))UpdatePlatPad_Original)(manager);
}
kmBranch(0x8037537C, UpdatePlatPad);

// In PlatPadManager::UpdateChannel: a channel with no Wii Remote and a GameCube controller in its
// port has a Wii Remote with a Nunchuk, built once a frame here, where the channel's update starts.
static long Probe(long chan, unsigned long* type) {
    const long result = WPADProbe(chan, type);
    const int c = chan & 3;
    s_emulated[c] = result == kWpadNoController && s_present[c];
    if (!s_emulated[c]) return result;
    BuildRemote(c);
    *type = kDevFreestyle;
    return kWpadOk;
}
kmCall(0x80375408, Probe);

static long SetDataFormat(long chan, unsigned long format) {
    return s_emulated[chan & 3] ? kWpadOk : WPADSetDataFormat(chan, format);
}
kmCall(0x803754C8, SetDataFormat);
kmCall(0x803754E8, SetDataFormat);
kmCall(0x80375508, SetDataFormat);

// The remote's read (a WPADStatus), when the game takes the controller for a remote alone.
static void ReadRemote(long chan, void* status) {
    if (s_emulated[chan & 3])
        FillWpad(chan & 3, (unsigned char*)status, false);
    else
        WPADRead(chan, status);
}
kmCall(0x80375564, ReadRemote);

// The remote and Nunchuk's (a WPADFSStatus), and the Classic Controller's (longer).
static void Read(long chan, void* status) {
    if (s_emulated[chan & 3])
        FillWpad(chan & 3, (unsigned char*)status, true);
    else
        WPADRead(chan, status);
}
kmCall(0x80375778, Read);
kmCall(0x803759B4, Read);

static long ReadKpad(long chan, void* samples, unsigned long count) {
    if (!s_emulated[chan & 3]) return KPADRead(chan, samples, count);
    FillKpad(chan & 3, (unsigned char*)samples);
    return 1;
}
kmCall(0x8037563C, ReadKpad);
kmCall(0x80375878, ReadKpad);
kmCall(0x80375AC4, ReadKpad);

// In PlatPadManager::UpdateDPD: the pointer needs no switching on.
static long ControlDpd(long chan, unsigned long command, void* callback) {
    return s_emulated[chan & 3] ? kWpadOk : WPADControlDpd(chan, command, callback);
}
kmCall(0x80375E68, ControlDpd);
kmCall(0x80375E7C, ControlDpd);
kmCall(0x80375EA0, ControlDpd);

// TitleScene::OnControllerPointerPress: the title screen asks the remote that pressed A for its
// battery level (a low one gets a warning) with WPADGetInfo, which waits for the answer. On a channel
// with no remote the SDK calls back before it waits and then waits forever: the game hangs. A
// GameCube controller has no battery to report.
static long GetInfo(long chan, void* info) {
    unsigned long type;
    const long probe = WPADProbe(chan, &type);
    return probe == kWpadOk ? WPADGetInfo(chan, info) : probe;
}
kmCall(0x801D2370, GetInfo);

// ---- Super Mario Strikers' controls in matches

namespace {

// DetInput: m_AnalogRightX/Y, m_nConnected, m_ButtonBitfield, m_LeftTrigger/m_RightTrigger, the remote's
// and Nunchuk's accelerometers, the pointer, the sticks' polars, m_aRemapAngle.
const unsigned long kDetRightX = 0x08, kDetRightY = 0x0C, kDetConnected = 0x10, kDetButtons = 0x12,
                    kDetLeftTrigger = 0x14, kDetRightTrigger = 0x15, kDetAccel = 0x18, kDetDpdTargets = 0x30,
                    kDetDpdCoord = 0x34, kDetPolarLeft = 0x44, kDetPolarRight = 0x4C, kDetRemapAngle = 0x88;
const unsigned char kKindFreestyle = 2, kKindGameCube = 3;
// Game actions (DetInput::IsPressed(action, remap = true)).
const int kActToggleItem = 22, kActHit = 24, kActSlide = 25, kActDeke = 29;

typedef bool (*DetQuery)(const void* det, int action, bool remap);
bool IsPressed(const void* det, int action) {
    return ((DetQuery)0x80331C04)(det, action, true);  // DetInput::IsPressed
}
bool JustPressed(const void* det, int action) {
    return ((DetQuery)0x80331C70)(det, action, true);  // DetInput::JustPressed
}

bool IsGameCube(const unsigned char* det) {
    return det && det[kDetConnected] == kKindGameCube;
}

// A player's input: cPlayer::m_pController (cAIPad) -> m_pGlobalPad.
const unsigned char* PlayerDetInput(const unsigned char* player) {
    const unsigned char* pad = player ? At<const unsigned char*>(player, 0x30C) : 0;
    return pad ? At<const unsigned char*>(pad, 0x2DC) : 0;
}

const unsigned char* BallOwner() {
    const unsigned char* ball = *(const unsigned char* const*)0x806E0BC0;  // g_pBall
    return ball ? At<const unsigned char*>(ball, 0xC8) : 0;
}

// The D-pad actions for a GameCube controller (deke, slide tackle, Charged's special), as the angle
// Charged's D-pad gives them: right 0, up 0x4000, in screen space like the sticks.
bool ActionAngle(const unsigned char* fielder, const unsigned char* det, unsigned short& angle) {
    unsigned short leftAngle = At<unsigned short>(det, kDetPolarLeft);
    const float leftRadius = At<float>(det, kDetPolarLeft + 4);
    const bool left = leftRadius == leftRadius && leftRadius > 0.3f;  // a centred stick's can be NaN
    // A centred stick: the way the player faces, in screen space.
    const unsigned short facing =
        (unsigned short)(At<unsigned short>(fielder, 0x24 + 0x3E) - At<unsigned short>(det, kDetRemapAngle));
    const unsigned short move = left ? leftAngle : facing;
    if (At<const void*>(fielder, 0x310) != 0) {  // with the ball: deke toward the C-stick, or Y
        const float cRadius = At<float>(det, kDetPolarRight + 4);
        if (cRadius == cRadius && cRadius > 0.3f) {
            angle = At<unsigned short>(det, kDetPolarRight);
            return true;
        }
        if (IsPressed(det, kActDeke)) {
            angle = move;
            return true;
        }
    } else {  // without it: slide tackle on B, while the other team has the ball
        const unsigned char* owner = BallOwner();
        if (owner && At<const void*>(owner, 0x314) != At<const void*>(fielder, 0x314) && IsPressed(det, kActSlide)) {
            angle = move;
            return true;
        }
    }
    if (At<unsigned short>(det, kDetButtons) & kPadR) {  // Charged's: what its D-pad does
        angle = move;
        return true;
    }
    return false;
}

}  // namespace

asm static void UpdatePolarAnalog_Original() {
    nofralloc
    stwu r1, -16(r1)
    lis r12, 0x8033
    ori r12, r12, 0x1D84
    mtctr r12
    bctr
}

// void DetInput::UpdatePolarAnalog(): the last step of a player's input each frame. A GameCube
// controller's (the Nunchuk kind, tagged) becomes the GameCube kind, from the controller: its buttons,
// C-stick and analog L and R; no motion, no pointer. Then the sticks' polars, as always.
static void UpdatePolarAnalog(unsigned char* det) {
    const unsigned short buttons = At<unsigned short>(det, kDetButtons);
    const int port = (buttons >> 5) & 3;
    if (det[kDetConnected] == kKindFreestyle && (buttons & kTag) && s_present[port]) {
        const PADStatus& pad = s_pad[port];
        det[kDetConnected] = kKindGameCube;
        At<unsigned short>(det, kDetButtons) = pad.button & 0x1F7F;
        det[kDetLeftTrigger] = pad.triggerL;
        det[kDetRightTrigger] = pad.triggerR;
        At<float>(det, kDetRightX) = StickAxis(pad.substickX);
        At<float>(det, kDetRightY) = StickAxis(pad.substickY);
        for (int i = 0; i < 6; ++i) At<float>(det, kDetAccel + i * 4) = 0.0f;
        det[kDetDpdTargets] = 0;
        At<float>(det, kDetDpdCoord) = 0.0f;
        At<float>(det, kDetDpdCoord + 4) = 0.0f;
    }
    ((void (*)(unsigned char*))UpdatePolarAnalog_Original)(det);
}
kmBranch(0x80331D80, UpdatePolarAnalog);

asm static void DetectRightShake_Original() {
    nofralloc
    stwu r1, -64(r1)
    lis r12, 0x8000
    ori r12, r12, 0x768C
    mtctr r12
    bctr
}

// bool cAIPad::DetectRightShake(u16* direction): the big hit. A GameCube controller: Y, without the ball.
static bool DetectRightShake(unsigned char* aipad, unsigned short* direction) {
    const unsigned char* det = At<const unsigned char*>(aipad, 0x2DC);
    if (!IsGameCube(det)) return ((bool (*)(unsigned char*, unsigned short*))DetectRightShake_Original)(aipad, direction);
    const unsigned char* owner = BallOwner();
    *direction = 0;
    return !(owner && PlayerDetInput(owner) == det) && JustPressed(det, kActHit);
}
kmBranch(0x80007688, DetectRightShake);

asm static void DetectLeftShake_Original() {
    nofralloc
    stwu r1, -64(r1)
    lis r12, 0x8000
    ori r12, r12, 0x7598
    mtctr r12
    bctr
}

// bool cAIPad::DetectLeftShake(u16* direction): cycle the item. A GameCube controller: Z.
static bool DetectLeftShake(unsigned char* aipad, unsigned short* direction) {
    const unsigned char* det = At<const unsigned char*>(aipad, 0x2DC);
    if (!IsGameCube(det)) return ((bool (*)(unsigned char*, unsigned short*))DetectLeftShake_Original)(aipad, direction);
    *direction = 0;
    return JustPressed(det, kActToggleItem);
}
kmBranch(0x80007594, DetectLeftShake);

asm static void DpadActionDirection_Original() {
    nofralloc
    stwu r1, -32(r1)
    lis r12, 0x8003
    ori r12, r12, 0x6D78
    mtctr r12
    bctr
}

// bool fn_80036D74(cFielder*, u16* angle): the D-pad action (deke, slide tackle, special) and its
// direction. A GameCube controller: SMS's buttons first, then the D-pad as a remote's.
static bool DpadActionDirection(unsigned char* fielder, unsigned short* angle) {
    const unsigned char* det = PlayerDetInput(fielder);
    unsigned short a;
    if (IsGameCube(det) && ActionAngle(fielder, det, a)) {
        *angle = a;
        return true;
    }
    return ((bool (*)(unsigned char*, unsigned short*))DpadActionDirection_Original)(fielder, angle);
}
kmBranch(0x80036D74, DpadActionDirection);

asm static void DpadActionHeld_Original() {
    nofralloc
    stwu r1, -16(r1)
    lis r12, 0x8003
    ori r12, r12, 0x6F8C
    mtctr r12
    bctr
}

// bool fn_80036F88(cFielder*): whether a D-pad action is asked for.
static bool DpadActionHeld(unsigned char* fielder) {
    const unsigned char* det = PlayerDetInput(fielder);
    unsigned short a;
    if (IsGameCube(det) && ActionAngle(fielder, det, a)) return true;
    return ((bool (*)(unsigned char*))DpadActionHeld_Original)(fielder);
}
kmBranch(0x80036F88, DpadActionHeld);
