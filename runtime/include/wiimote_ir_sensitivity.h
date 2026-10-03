// The Wii Remote IR camera's sensitivity registers, for a sensitivity from 1.0 to 5.0 in tenths
// (block 1: 9 bytes at 0xB00000, block 2: 2 bytes at 0xB0001A). Whole numbers are the five levels the
// Wii writes (wiibrew, IR camera). In between, the level below's blocks, with the brightness a dot
// needs to be seen (block 1's last byte; block 2's first is one less) moved part of the way to the
// next level's: geometrically, as the levels' thresholds roughly halve from one to the next.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace WiimoteIr {

constexpr int kMinTenths = 10, kMaxTenths = 50;

inline void SensitivityBlocks(int tenths, uint8_t block1[9], uint8_t block2[2]) {
    static constexpr uint8_t kBlock1[5][9] = {
        {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x64, 0x00, 0xFE},
        {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0x96, 0x00, 0xB4},
        {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xAA, 0x00, 0x64},
        {0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xC8, 0x00, 0x36},
        {0x07, 0x00, 0x00, 0x71, 0x01, 0x00, 0x72, 0x00, 0x20},
    };
    static constexpr uint8_t kBlock2[5][2] = {{0xFD, 0x05}, {0xB3, 0x04}, {0x63, 0x03}, {0x35, 0x03}, {0x1F, 0x03}};
    tenths = std::clamp(tenths, kMinTenths, kMaxTenths);
    const int level = tenths / 10, step = tenths % 10;  // 1-5, 0-9 (5.0 has no step)
    std::copy(kBlock1[level - 1], kBlock1[level - 1] + 9, block1);
    std::copy(kBlock2[level - 1], kBlock2[level - 1] + 2, block2);
    if (step == 0) return;
    const double from = kBlock1[level - 1][8], to = kBlock1[level][8];
    const long threshold = std::lround(from * std::pow(to / from, step / 10.0));
    block1[8] = static_cast<uint8_t>(threshold);
    block2[0] = static_cast<uint8_t>(threshold - 1);
}

}  // namespace WiimoteIr
