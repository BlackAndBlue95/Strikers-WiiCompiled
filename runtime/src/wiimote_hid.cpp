#include "wiimote_hid.h"

#include "runtime_log.h"

#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Protocol details checked against Dolphin (dolphin-emu/dolphin 771fb15): WiimoteReal (connection
// probing, I/O), WiimoteCommon (report and calibration layouts) and WiimoteEmu (IR camera registers
// and image orientation, extension encryption/identification, Nunchuk data and calibration).
// Dolphin passes real remotes through to the emulated game, whose WPAD code programs them; this
// driver does that programming itself, following the console's IR setup order Dolphin documents.
namespace WiimoteHid {
namespace {

// Camera pixels per unit of KPAD pointer position. Dolphin's emulated remote (Dynamics.cpp
// EmulatePoint + Camera.cpp GetCameraPoints, default pointing range) moves the sensor bar's dots
// about 605 px horizontally and 480 px vertically from the image centre for a pointer at +/-1, and
// in the opposite direction: aiming right moves the dots left, aiming up moves them up.
constexpr float kPointerPixelsX = 605.f;
constexpr float kPointerPixelsY = 480.f;

constexpr uint16_t kNintendoVid = 0x057E;
constexpr uint16_t kPidRvlCnt01 = 0x0306;    // original Wii Remote
constexpr uint16_t kPidRvlCnt01Tr = 0x0330;  // Wii Remote Plus (MotionPlus inside)
constexpr uint32_t kMaxRemotes = 4;
constexpr size_t kReportSize = 22;            // largest input/output report incl. the id

// Output reports.
constexpr uint8_t kOutRumble = 0x10;
constexpr uint8_t kOutLeds = 0x11;
constexpr uint8_t kOutReportMode = 0x12;
constexpr uint8_t kOutIrPixelClock = 0x13;
constexpr uint8_t kOutStatusRequest = 0x15;
constexpr uint8_t kOutWriteMemory = 0x16;
constexpr uint8_t kOutReadMemory = 0x17;
constexpr uint8_t kOutIrLogic = 0x1A;
// Input reports.
constexpr uint8_t kInStatus = 0x20;
constexpr uint8_t kInReadData = 0x21;
constexpr uint8_t kInAck = 0x22;
constexpr uint8_t kInCoreAccIr10Ext6 = 0x37;

constexpr uint8_t kSpaceEeprom = 0x00;
constexpr uint8_t kSpaceRegister = 0x04;

// WPAD_BUTTON_* bits; the core buttons are the report's two button bytes, low byte first.
constexpr uint32_t kCoreButtonMask = 0x9F1F;
constexpr uint32_t kWpadZ = 0x2000, kWpadC = 0x4000;

struct AccelCalibration {
    float zero[3] = {512.f, 512.f, 512.f};
    float oneG[3] = {616.f, 616.f, 616.f};
};

struct StickCalibration {
    float min[2] = {28.f, 28.f};
    float center[2] = {128.f, 128.f};
    float max[2] = {228.f, 228.f};
};

struct Remote {
    SDL_hid_device* device = nullptr;
    std::string path;
    uint32_t chan = 0;
    AccelCalibration accel;
    AccelCalibration nunchukAccel;
    StickCalibration stick;
    bool hasNunchuk = false;
    bool extensionAttached = false; // status report flag (InputReportStatus::extension)
    bool needsSetup = true;       // extension/IR/report mode (re)initialisation pending
    uint64_t lastInputMs = 0;
    uint64_t lastWriteTestMs = 0;
    uint64_t lastLogMs = 0;
    uint32_t dataReports = 0;     // 0x37 reports since the last log line
    float lastPair[2] = {200.f, 0.f}; // last seen dot1->dot2 vector, for single-dot frames
    float smoothed[2] = {0.f, 0.f};
    bool smoothedValid = false;
};

std::mutex g_mutex;                         // guards g_samples and g_owned
std::array<Sample, kMaxRemotes> g_samples{};
std::array<bool, kMaxRemotes> g_owned{};
std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
std::thread g_thread;
std::atomic<uint32_t> g_connected{0};

// --- low-level I/O ---------------------------------------------------------------------------------

bool Send(Remote& r, std::initializer_list<uint8_t> bytes) {
    uint8_t buf[kReportSize] = {};
    size_t n = 0;
    for (uint8_t b : bytes) {
        if (n < sizeof(buf)) buf[n++] = b;
    }
    // Byte 1 bit 0 is the rumble motor in every output report; it stays off.
    return SDL_hid_write(r.device, buf, n) >= 0;
}

int ReadReport(Remote& r, uint8_t* buf, int timeoutMs) {
    return SDL_hid_read_timeout(r.device, buf, kReportSize, timeoutMs);
}

void HandleInput(Remote& r, const uint8_t* buf, int len);

// Waits for a report of type `want`, handling everything else that arrives meanwhile.
bool WaitFor(Remote& r, uint8_t want, uint8_t* out, int timeoutMs) {
    const uint64_t deadline = SDL_GetTicks() + static_cast<uint64_t>(timeoutMs);
    uint8_t buf[kReportSize];
    while (SDL_GetTicks() < deadline) {
        const int n = ReadReport(r, buf, 20);
        if (n < 0) return false;
        if (n == 0) continue;
        if (buf[0] == want) {
            std::memcpy(out, buf, kReportSize);
            return true;
        }
        HandleInput(r, buf, n);
    }
    return false;
}

// Writes up to 16 bytes to the remote's register space and waits for the acknowledgement.
bool WriteRegister(Remote& r, uint32_t addr, const uint8_t* data, uint8_t size) {
    uint8_t buf[kReportSize] = {kOutWriteMemory, kSpaceRegister, static_cast<uint8_t>(addr >> 16),
                                static_cast<uint8_t>(addr >> 8), static_cast<uint8_t>(addr), size};
    std::memcpy(buf + 6, data, std::min<size_t>(size, 16));
    if (SDL_hid_write(r.device, buf, sizeof(buf)) < 0) return false;
    uint8_t ack[kReportSize];
    for (int tries = 0; tries < 4; ++tries) {
        if (!WaitFor(r, kInAck, ack, 250)) return false;
        if (ack[3] == kOutWriteMemory) return ack[4] == 0;
    }
    return false;
}

bool WriteRegister8(Remote& r, uint32_t addr, uint8_t value) { return WriteRegister(r, addr, &value, 1); }

// Reads up to 16 bytes from EEPROM or register space.
bool ReadMemory(Remote& r, uint8_t space, uint32_t addr, uint8_t* out, uint16_t size) {
    if (!Send(r, {kOutReadMemory, space, static_cast<uint8_t>(addr >> 16), static_cast<uint8_t>(addr >> 8),
                  static_cast<uint8_t>(addr), static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)})) {
        return false;
    }
    uint16_t got = 0;
    uint8_t rep[kReportSize];
    while (got < size) {
        if (!WaitFor(r, kInReadData, rep, 400)) return false;
        if ((rep[3] & 0x0F) != 0) return false; // error flags
        const uint16_t chunk = static_cast<uint16_t>((rep[3] >> 4) + 1);
        const uint16_t offset = static_cast<uint16_t>(((rep[4] << 8) | rep[5]) - (addr & 0xFFFF));
        for (uint16_t i = 0; i < chunk && offset + i < size; ++i) out[offset + i] = rep[6 + i];
        got = static_cast<uint16_t>(std::max<int>(got, offset + chunk));
    }
    return true;
}

// --- setup -----------------------------------------------------------------------------------------

void ReadAccelCalibration(Remote& r) {
    uint8_t cal[10] = {};
    if (!ReadMemory(r, kSpaceEeprom, 0x0016, cal, sizeof(cal))) {
        RT_LOGF(RT_TAG_CONFIG, "Wii Remote %u: accelerometer calibration read failed, using defaults\n", r.chan + 1);
        return;
    }
    const float zero[3] = {static_cast<float>((cal[0] << 2) | ((cal[3] >> 4) & 3)),
                           static_cast<float>((cal[1] << 2) | ((cal[3] >> 2) & 3)),
                           static_cast<float>((cal[2] << 2) | (cal[3] & 3))};
    const float one[3] = {static_cast<float>((cal[4] << 2) | ((cal[7] >> 4) & 3)),
                          static_cast<float>((cal[5] << 2) | ((cal[7] >> 2) & 3)),
                          static_cast<float>((cal[6] << 2) | (cal[7] & 3))};
    for (int i = 0; i < 3; ++i) {
        if (one[i] - zero[i] < 20.f) return; // implausible: keep defaults
    }
    std::memcpy(r.accel.zero, zero, sizeof(zero));
    std::memcpy(r.accel.oneG, one, sizeof(one));
}

// Initialises the extension unencrypted (0x55 to A400F0, 0x00 to A400FB) and identifies it.
void SetupExtension(Remote& r) {
    r.hasNunchuk = false;
    if (!r.extensionAttached) return;
    if (!WriteRegister8(r, 0xA400F0, 0x55) || !WriteRegister8(r, 0xA400FB, 0x00)) {
        RT_LOGF(RT_TAG_CONFIG, "Wii Remote %u: extension init failed\n", r.chan + 1);
        return;
    }
    uint8_t id[6] = {};
    if (!ReadMemory(r, kSpaceRegister, 0xA400FA, id, sizeof(id))) {
        RT_LOGF(RT_TAG_CONFIG, "Wii Remote %u: extension id read failed\n", r.chan + 1);
        return;
    }
    // Nunchuk: xx 00 A4 20 00 00 (the first byte varies between models).
    if (id[2] == 0xA4 && id[3] == 0x20 && id[4] == 0x00 && id[5] == 0x00) {
        r.hasNunchuk = true;
        uint8_t cal[14] = {};
        if (ReadMemory(r, kSpaceRegister, 0xA40020, cal, sizeof(cal)) && cal[0] != 0 && cal[0] != 0xFF) {
            // Same packing as the remote's own calibration points: low bits z 0-1, y 2-3, x 4-5.
            for (int i = 0; i < 3; ++i) {
                r.nunchukAccel.zero[i] = static_cast<float>((cal[i] << 2) | ((cal[3] >> (4 - 2 * i)) & 3));
                r.nunchukAccel.oneG[i] = static_cast<float>((cal[4 + i] << 2) | ((cal[7] >> (4 - 2 * i)) & 3));
            }
            for (int axis = 0; axis < 2; ++axis) {
                const float mx = cal[8 + axis * 3], mn = cal[9 + axis * 3], ce = cal[10 + axis * 3];
                if (mx > ce + 20.f && mn + 20.f < ce) {
                    r.stick.max[axis] = mx;
                    r.stick.min[axis] = mn;
                    r.stick.center[axis] = ce;
                }
            }
        }
        RT_LOGF(RT_TAG_CONFIG, "Wii Remote %u: Nunchuk connected\n", r.chan + 1);
    } else {
        RT_LOGF(RT_TAG_CONFIG, "Wii Remote %u: extension %02X %02X %02X %02X %02X %02X (not a Nunchuk)\n", r.chan + 1,
                id[0], id[1], id[2], id[3], id[4], id[5]);
    }
}

// Sends an enable-feature report (0x13 camera, 0x1A camera logic) with the ack bit set and waits
// for the acknowledgement: bit 2 enable, bit 1 ack, bit 0 rumble (WiimoteCommon).
bool EnableFeature(Remote& r, uint8_t report) {
    if (!Send(r, {report, 0x06})) return false;
    uint8_t ack[kReportSize];
    for (int tries = 0; tries < 4; ++tries) {
        if (!WaitFor(r, kInAck, ack, 250)) return false;
        if (ack[3] == report) return ack[4] == 0;
    }
    return false;
}

// IR camera in basic mode (10 bytes for 4 dots), Wii sensitivity level 3. The camera only answers
// on its bus once report 0x13 enabled it, and only produces dots while 0x30 holds 0x08; the
// console writes 0x01 there before changing sensitivity and mode (WiimoteEmu/Camera.cpp).
bool SetupIr(Remote& r) {
    static constexpr uint8_t kSensitivity1[9] = {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xAA, 0x00, 0x64};
    static constexpr uint8_t kSensitivity2[2] = {0x63, 0x03};
    bool ok = EnableFeature(r, kOutIrPixelClock);
    ok = ok && EnableFeature(r, kOutIrLogic);
    ok = ok && WriteRegister8(r, 0xB00030, 0x01);
    ok = ok && WriteRegister(r, 0xB00000, kSensitivity1, sizeof(kSensitivity1));
    ok = ok && WriteRegister(r, 0xB0001A, kSensitivity2, sizeof(kSensitivity2));
    ok = ok && WriteRegister8(r, 0xB00033, 0x01);
    ok = ok && WriteRegister8(r, 0xB00030, 0x08);
    if (!ok) RT_LOGF(RT_TAG_CONFIG, "Wii Remote %u: IR camera setup failed\n", r.chan + 1);
    return ok;
}

void Setup(Remote& r) {
    r.needsSetup = false;
    SetupExtension(r);
    SetupIr(r);
    // Continuous reporting: buttons, accelerometer, 10 IR bytes, 6 extension bytes.
    Send(r, {kOutReportMode, 0x04, kInCoreAccIr10Ext6});
}

// --- input -----------------------------------------------------------------------------------------

float AccelG(const AccelCalibration& cal, int axis, float raw) {
    return (raw - cal.zero[axis]) / std::max(cal.oneG[axis] - cal.zero[axis], 1.f);
}

// The KPAD pointer from the sensor bar's two dots: their midpoint, with the remote's roll taken
// out using the dots themselves (the bar is level).
void UpdatePointer(Remote& r, Sample& s) {
    float x[4], y[4];
    int n = 0;
    for (int i = 0; i < 4; ++i) {
        if (s.dotX[i] < 1023 && s.dotY[i] < 767) {
            x[n] = s.dotX[i];
            y[n] = s.dotY[i];
            ++n;
        }
    }
    float mx, my, dx, dy;
    if (n >= 2) {
        // Order the pair left to right in the camera as long as the remote is held right side up
        // (the accelerometer's z says which way up it is).
        dx = x[1] - x[0];
        dy = y[1] - y[0];
        const bool upsideDown = s.acc[2] < -0.3f;
        if ((dx < 0.f) != upsideDown) {
            dx = -dx;
            dy = -dy;
            std::swap(x[0], x[1]);
            std::swap(y[0], y[1]);
        }
        r.lastPair[0] = dx;
        r.lastPair[1] = dy;
        mx = (x[0] + x[1]) * 0.5f;
        my = (y[0] + y[1]) * 0.5f;
    } else if (n == 1) {
        // One dot: assume it is whichever end keeps the midpoint closest to the last one.
        dx = r.lastPair[0];
        dy = r.lastPair[1];
        const float ax = x[0] + dx * 0.5f, ay = y[0] + dy * 0.5f;
        const float bx = x[0] - dx * 0.5f, by = y[0] - dy * 0.5f;
        const float px = 512.f - r.smoothed[0] * kPointerPixelsX, py = 384.f + r.smoothed[1] * kPointerPixelsY;
        const bool useA = (ax - px) * (ax - px) + (ay - py) * (ay - py) <= (bx - px) * (bx - px) + (by - py) * (by - py);
        mx = useA ? ax : bx;
        my = useA ? ay : by;
    } else {
        s.hasPointer = false;
        r.smoothedValid = false;
        return;
    }
    // Undo the roll: rotate the midpoint's offset from the image centre by the bar's angle.
    const float angle = std::atan2(dy, dx);
    const float c = std::cos(-angle), sn = std::sin(-angle);
    const float ox = mx - 512.f, oy = my - 384.f;
    const float rx = ox * c - oy * sn, ry = ox * sn + oy * c;
    // Aiming right moves the dots left and aiming down moves them down (see kPointerPixelsX).
    float px = std::clamp(-rx / kPointerPixelsX, -1.25f, 1.25f);
    float py = std::clamp(ry / kPointerPixelsY, -1.25f, 1.25f);
    // Light smoothing against camera jitter (KPAD smooths its pointer too).
    if (r.smoothedValid) {
        px = r.smoothed[0] + (px - r.smoothed[0]) * 0.6f;
        py = r.smoothed[1] + (py - r.smoothed[1]) * 0.6f;
    }
    r.smoothed[0] = px;
    r.smoothed[1] = py;
    r.smoothedValid = true;
    s.hasPointer = true;
    s.pointer[0] = px;
    s.pointer[1] = py;
}

void LogState(Remote& r, const Sample& s) {
    int dots = 0;
    for (int i = 0; i < 4; ++i) dots += (s.dotX[i] < 1023 && s.dotY[i] < 767) ? 1 : 0;
    RT_LOGF(RT_TAG_CONFIG,
            "Wii Remote %u: %u reports/2s hold=%04X dots=%d ptr=%s(%+.2f,%+.2f) acc=(%+.2f,%+.2f,%+.2f) nunchuk=%d "
            "stick=(%+.2f,%+.2f)\n",
            r.chan + 1, r.dataReports, s.hold, dots, s.hasPointer ? "" : "off", s.pointer[0], s.pointer[1], s.acc[0],
            s.acc[1], s.acc[2], s.hasNunchuk ? 1 : 0, s.stick[0], s.stick[1]);
}

void HandleDataReport(Remote& r, const uint8_t* buf) {
    Sample s;
    s.connected = true;
    s.hasNunchuk = r.hasNunchuk;
    const uint32_t buttons = static_cast<uint32_t>(buf[1]) | (static_cast<uint32_t>(buf[2]) << 8);
    s.hold = buttons & kCoreButtonMask;
    // Accelerometer: 8 high bits per axis, low bits packed into the button bytes.
    const float ax = static_cast<float>((buf[3] << 2) | ((buf[1] >> 5) & 3));
    const float ay = static_cast<float>((buf[4] << 2) | ((buf[2] >> 4) & 2));
    const float az = static_cast<float>((buf[5] << 2) | ((buf[2] >> 5) & 2));
    s.acc[0] = AccelG(r.accel, 0, ax);
    s.acc[1] = AccelG(r.accel, 1, ay);
    s.acc[2] = AccelG(r.accel, 2, az);
    // Basic IR: two 5-byte groups of two dots each.
    for (int g = 0; g < 2; ++g) {
        const uint8_t* p = buf + 6 + g * 5;
        s.dotX[g * 2] = static_cast<uint16_t>(p[0] | ((p[2] >> 4) & 3) << 8);
        s.dotY[g * 2] = static_cast<uint16_t>(p[1] | ((p[2] >> 6) & 3) << 8);
        s.dotX[g * 2 + 1] = static_cast<uint16_t>(p[3] | (p[2] & 3) << 8);
        s.dotY[g * 2 + 1] = static_cast<uint16_t>(p[4] | ((p[2] >> 2) & 3) << 8);
    }
    UpdatePointer(r, s);
    if (r.hasNunchuk) {
        const uint8_t* e = buf + 16;
        for (int axis = 0; axis < 2; ++axis) {
            const float v = e[axis];
            const float ce = r.stick.center[axis];
            const float range = v >= ce ? r.stick.max[axis] - ce : ce - r.stick.min[axis];
            s.stick[axis] = std::clamp((v - ce) / std::max(range, 1.f), -1.f, 1.f);
        }
        const float nx = static_cast<float>((e[2] << 2) | ((e[5] >> 2) & 3));
        const float ny = static_cast<float>((e[3] << 2) | ((e[5] >> 4) & 3));
        const float nz = static_cast<float>((e[4] << 2) | ((e[5] >> 6) & 3));
        s.nunchukAcc[0] = AccelG(r.nunchukAccel, 0, nx);
        s.nunchukAcc[1] = AccelG(r.nunchukAccel, 1, ny);
        s.nunchukAcc[2] = AccelG(r.nunchukAccel, 2, nz);
        if (!(e[5] & 0x01)) s.hold |= kWpadZ;
        if (!(e[5] & 0x02)) s.hold |= kWpadC;
    }
    ++r.dataReports;
    // A line every 2 s while connected, so a tester's console.log shows what the remote sends.
    if (SDL_GetTicks() - r.lastLogMs >= 2000) {
        LogState(r, s);
        r.lastLogMs = SDL_GetTicks();
        r.dataReports = 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_samples[r.chan] = s;
}

void HandleInput(Remote& r, const uint8_t* buf, int len) {
    if (len <= 0) return;
    r.lastInputMs = SDL_GetTicks();
    switch (buf[0]) {
    case kInCoreAccIr10Ext6:
        if (len >= 22) HandleDataReport(r, buf);
        break;
    case kInStatus: {
        // Sent whenever an extension is plugged in or out; the remote then stops data reporting
        // until the mode is set again, so the whole setup is redone.
        if (len >= 4) r.extensionAttached = (buf[3] & 0x02) != 0;
        r.needsSetup = true;
        break;
    }
    default:
        break;
    }
}

// --- device management -----------------------------------------------------------------------------

int ClaimChannel() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < kMaxRemotes; ++i) {
        if (!g_owned[i]) {
            g_owned[i] = true;
            g_samples[i] = {};
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ReleaseChannel(uint32_t chan) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_owned[chan] = false;
    g_samples[chan] = {};
}

// Paths that did not answer recently. A paired remote that is switched off can still accept the
// write on some stacks, and waiting on it every scan would stall the connected remotes' input.
struct Cooldown {
    std::string path;
    uint64_t untilMs;
};
std::vector<Cooldown> g_cooldowns;

bool CoolingDown(const char* path) {
    const uint64_t now = SDL_GetTicks();
    std::erase_if(g_cooldowns, [&](const Cooldown& c) { return c.untilMs <= now; });
    return std::ranges::any_of(g_cooldowns, [&](const Cooldown& c) { return c.path == path; });
}

bool IsOpen(const std::vector<std::unique_ptr<Remote>>& remotes, const char* path) {
    for (const auto& r : remotes) {
        if (r->path == path) return true;
    }
    return false;
}

void Connect(std::vector<std::unique_ptr<Remote>>& remotes, const SDL_hid_device_info* info) {
    SDL_hid_device* device = SDL_hid_open_path(info->path);
    if (device == nullptr) return;
    const int chan = ClaimChannel();
    if (chan < 0) {
        SDL_hid_close(device);
        return;
    }
    auto r = std::make_unique<Remote>();
    r->device = device;
    r->path = info->path;
    r->chan = static_cast<uint32_t>(chan);
    r->lastInputMs = SDL_GetTicks();
    // Player LED, then a status request: the reply says whether an extension is attached.
    Send(*r, {kOutLeds, static_cast<uint8_t>(0x10 << chan)});
    // Windows keeps interfaces for paired remotes that are switched off, and a DolphinBar exposes
    // all four slots whether or not a remote is in them; a write to an empty one fails at once
    // (ERROR_GEN_FAILURE / EPIPE, per Dolphin), so only a remote that answers is really there.
    uint8_t status[kReportSize];
    bool writeAccepted = false;
    const auto probe = [&] {
        if (!Send(*r, {kOutStatusRequest, 0x00})) return false;
        writeAccepted = true;
        return WaitFor(*r, kInStatus, status, 300);
    };
    bool answered = probe();
    if (!answered) {
        SDL_Delay(100); // Dolphin retries once: a fresh connection may need a moment to settle
        answered = probe();
    }
    if (answered) r->extensionAttached = (status[3] & 0x02) != 0;
    if (!answered) {
        // A failed write costs nothing to retry next scan; an accepted write that nobody answers
        // cost a timeout, so that path rests for a while.
        if (writeAccepted) g_cooldowns.push_back({info->path, SDL_GetTicks() + 15000});
        ReleaseChannel(r->chan);
        SDL_hid_close(device);
        return;
    }
    ReadAccelCalibration(*r);
    Setup(*r);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_samples[r->chan].connected = true;
    }
    RT_LOGF(RT_TAG_CONFIG, "Wii Remote connected on port %u (HID, product %04X)\n", r->chan + 1, info->product_id);
    remotes.push_back(std::move(r));
    g_connected.store(static_cast<uint32_t>(remotes.size()));
}

void Disconnect(std::vector<std::unique_ptr<Remote>>& remotes, size_t index) {
    Remote& r = *remotes[index];
    RT_LOGF(RT_TAG_CONFIG, "Wii Remote on port %u disconnected\n", r.chan + 1);
    SDL_hid_close(r.device);
    ReleaseChannel(r.chan);
    remotes.erase(remotes.begin() + static_cast<std::ptrdiff_t>(index));
    g_connected.store(static_cast<uint32_t>(remotes.size()));
}

// Dolphin's test: a known id, or a product name starting "Nintendo RVL-CNT" (the Wii checks just
// that prefix too), which also covers third-party remotes. The Wii U Pro Controller shares 0330 but
// is "Nintendo RVL-CNT-01-UC" and is not a Wii Remote.
bool IsWiiRemote(const SDL_hid_device_info* info) {
    std::wstring name = info->product_string != nullptr ? info->product_string : L"";
    if (name.find(L"-UC") != std::wstring::npos) return false;
    if (name.rfind(L"Nintendo RVL-CNT", 0) == 0) return true;
    return info->vendor_id == kNintendoVid && (info->product_id == kPidRvlCnt01 || info->product_id == kPidRvlCnt01Tr);
}

void Scan(std::vector<std::unique_ptr<Remote>>& remotes) {
    if (remotes.size() >= kMaxRemotes) return;
    SDL_hid_device_info* list = SDL_hid_enumerate(0, 0);
    for (SDL_hid_device_info* info = list; info != nullptr; info = info->next) {
        if (!IsWiiRemote(info)) continue;
        if (info->path == nullptr || IsOpen(remotes, info->path) || CoolingDown(info->path)) continue;
        Connect(remotes, info);
        if (remotes.size() >= kMaxRemotes) break;
    }
    SDL_hid_free_enumeration(list);
}

void ThreadMain() {
    std::vector<std::unique_ptr<Remote>> remotes;
    uint64_t lastScan = 0;
    uint8_t buf[kReportSize];
    while (!g_stop.load()) {
        const uint64_t now = SDL_GetTicks();
        if (now - lastScan >= 2000) {
            lastScan = now;
            Scan(remotes);
        }
        if (remotes.empty()) {
            SDL_Delay(50);
            continue;
        }
        for (size_t i = 0; i < remotes.size();) {
            Remote& r = *remotes[i];
            bool dead = false;
            // Drain everything queued; the remote reports roughly every 10 ms.
            for (int k = 0; k < 16; ++k) {
                const int n = ReadReport(r, buf, k == 0 ? 4 : 0);
                if (n <= 0) break; // errors count as no data, like Dolphin; silence times it out

                HandleInput(r, buf, n);
            }
            if (!dead && r.needsSetup) Setup(r);
            // Connected but no data reports: the reporting mode never took (logged every 3 s).
            if (!dead && r.dataReports == 0 && SDL_GetTicks() - r.lastLogMs >= 3000) {
                RT_LOGF(RT_TAG_CONFIG, "Wii Remote %u: connected, but no data reports (0x37) arriving\n", r.chan + 1);
                r.lastLogMs = SDL_GetTicks();
            }
            // Windows and the DolphinBar only report a remote that went away when it is written to
            // (Dolphin's WRITE_TEST_INTERVAL): send a rumble-off report every second.
            if (!dead && SDL_GetTicks() - r.lastWriteTestMs >= 1000) {
                r.lastWriteTestMs = SDL_GetTicks();
                if (!Send(r, {kOutRumble, 0x00})) dead = true;
            }
            // Continuous reporting means silence is a remote that went away (or was turned off).
            if (!dead && SDL_GetTicks() - r.lastInputMs > 3000) dead = true;
            if (dead) {
                Disconnect(remotes, i);
            } else {
                ++i;
            }
        }
    }
    while (!remotes.empty()) Disconnect(remotes, remotes.size() - 1);
}

} // namespace

void Start() {
    if (g_running.exchange(true)) return;
    if (SDL_hid_init() != 0) {
        RT_LOGF(RT_TAG_CONFIG, "Wii Remotes: SDL_hid_init failed: %s\n", SDL_GetError());
        g_running.store(false);
        return;
    }
    g_stop.store(false);
    g_thread = std::thread(ThreadMain);
    RT_LOGF(RT_TAG_CONFIG, "Wii Remotes: HID backend started (IR pointer, Nunchuk)\n");
}

void Stop() {
    if (!g_running.exchange(false)) return;
    g_stop.store(true);
    if (g_thread.joinable()) g_thread.join();
}

bool Running() { return g_running.load(); }

bool Read(uint32_t chan, Sample& out) {
    if (chan >= kMaxRemotes) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_owned[chan] || !g_samples[chan].connected) return false;
    out = g_samples[chan];
    return true;
}

bool Present(uint32_t chan) {
    if (chan >= kMaxRemotes) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_owned[chan];
}

uint32_t ConnectedCount() { return g_connected.load(); }

} // namespace WiimoteHid
