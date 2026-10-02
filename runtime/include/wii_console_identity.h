// The console's device ID, which /dev/es reports (ES_GetDeviceID): read from the NAND's keys.bin when
// there is one (a real console's dump), else the default console identity Dolphin uses
// (Source/Core/Core/IOS/IOSC.cpp).
#pragma once

#include "isa/big_endian.h"
#include "nand_path.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>

namespace WiiConsoleIdentity {

constexpr uint32_t kDefaultDeviceId = 0x0403AC68;

inline std::optional<uint32_t> DeviceIdFromKeysBin(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::array<uint8_t, 0x400> dump{};
    if (!file.read(reinterpret_cast<char*>(dump.data()), static_cast<std::streamsize>(dump.size()))) {
        return std::nullopt;
    }
    const uint32_t deviceId = BigEndian::Read32(dump.data() + 0x124);
    if (deviceId == 0) {
        return std::nullopt;
    }
    return deviceId;
}

inline uint32_t DeviceId() {
    static const uint32_t deviceId = [] {
        const std::filesystem::path nandRoot = RuntimeNandPath::DiscoverNandRootPath();
        if (!nandRoot.empty()) {
            if (const auto loaded = DeviceIdFromKeysBin(nandRoot / "keys.bin")) {
                return *loaded;
            }
        }
        return kDefaultDeviceId;
    }();
    return deviceId;
}

} // namespace WiiConsoleIdentity
