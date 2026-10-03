// The IR camera sensitivity registers (wiimote_ir_sensitivity.h): the Wii's five levels exactly, and
// the steps between them always asking for a dimmer dot than the step before.
#include "wiimote_ir_sensitivity.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what, int tenths) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s at %d.%d\n", what, tenths / 10, tenths % 10);
        ++g_failures;
    }
}

}  // namespace

int main() {
    // wiibrew's blocks for the Wii's levels 1-5.
    static const uint8_t kLevel1[5][9] = {
        {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x64, 0x00, 0xFE}, {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x96, 0x00, 0xB4},
        {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xAA, 0x00, 0x64}, {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xC8, 0x00, 0x36},
        {0x07, 0x00, 0x00, 0x71, 0x01, 0x00, 0x72, 0x00, 0x20},
    };
    static const uint8_t kLevel2[5][2] = {{0xFD, 0x05}, {0xB3, 0x04}, {0x63, 0x03}, {0x35, 0x03}, {0x1F, 0x03}};
    for (int level = 1; level <= 5; ++level) {
        uint8_t b1[9], b2[2];
        WiimoteIr::SensitivityBlocks(level * 10, b1, b2);
        Check(std::memcmp(b1, kLevel1[level - 1], 9) == 0 && std::memcmp(b2, kLevel2[level - 1], 2) == 0,
              "a whole level is the Wii's", level * 10);
    }
    int previous = 256;
    for (int tenths = WiimoteIr::kMinTenths; tenths <= WiimoteIr::kMaxTenths; ++tenths) {
        uint8_t b1[9], b2[2];
        WiimoteIr::SensitivityBlocks(tenths, b1, b2);
        Check(b1[8] < previous, "each step needs a dimmer dot", tenths);
        Check(b2[0] == b1[8] - 1, "block 2 follows block 1's threshold", tenths);
        Check(std::memcmp(b1, kLevel1[tenths / 10 - 1], 8) == 0, "a step keeps its level's other values", tenths);
        previous = b1[8];
    }
    uint8_t low[9], lowest[9], high[9], highest[9], b2[2];
    WiimoteIr::SensitivityBlocks(0, low, b2);
    WiimoteIr::SensitivityBlocks(10, lowest, b2);
    WiimoteIr::SensitivityBlocks(99, high, b2);
    WiimoteIr::SensitivityBlocks(50, highest, b2);
    Check(std::memcmp(low, lowest, 9) == 0 && std::memcmp(high, highest, 9) == 0, "out of range is clamped", 0);
    if (g_failures == 0) std::printf("wiimote IR sensitivity: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
