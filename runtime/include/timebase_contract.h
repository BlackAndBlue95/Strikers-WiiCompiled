#pragma once

#include <chrono>
#include <cstdint>

namespace TimeBaseContract {

// Broadway's time base runs at one quarter of the 243 MHz Wii bus clock.
// Reduced against one billion nanoseconds per second, that is exactly
// 243 time-base ticks per 4000 nanoseconds (60.75 MHz).
inline constexpr uint64_t kBusClockHz = 243'000'000u;
inline constexpr uint64_t kTimeBaseDivider = 4u;
inline constexpr uint64_t kTicksPerSecond = kBusClockHz / kTimeBaseDivider;
inline constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000u;
inline constexpr uint64_t kTickRatioNumerator = 243u;
inline constexpr uint64_t kTickRatioDenominator = 4'000u;

static_assert(kTicksPerSecond == 60'750'000u);
static_assert(kTicksPerSecond * kTickRatioDenominator ==
              kNanosecondsPerSecond * kTickRatioNumerator);

// Split the rational conversion around the division so the intermediate
// product cannot overflow. The result is floor(nanoseconds * 243 / 4000).
constexpr uint64_t NanosecondsToTicks(uint64_t nanoseconds) noexcept
{
    return (nanoseconds / kTickRatioDenominator) * kTickRatioNumerator +
           ((nanoseconds % kTickRatioDenominator) * kTickRatioNumerator) /
               kTickRatioDenominator;
}

// The Wii's clock counts from 2000-01-01 in local time (it keeps no time zone), and boot sets the time
// base from it. Given the local wall-clock time as seconds since 1970 (read as if it were UTC), the
// time base value it implies; 0 for anything before 2000.
inline constexpr int64_t kWiiEpochUnixSeconds = 946'684'800; // 2000-01-01T00:00:00

constexpr uint64_t WiiTicksForLocalUnixSeconds(int64_t localUnixSeconds) noexcept
{
    return localUnixSeconds <= kWiiEpochUnixSeconds
        ? 0
        : static_cast<uint64_t>(localUnixSeconds - kWiiEpochUnixSeconds) * kTicksPerSecond;
}

static_assert(WiiTicksForLocalUnixSeconds(kWiiEpochUnixSeconds - 1) == 0);
static_assert(WiiTicksForLocalUnixSeconds(kWiiEpochUnixSeconds + 86'400) == 86'400 * kTicksPerSecond);
// 2026-10-02T00:00:00: 9771 days after the Wii epoch.
static_assert(WiiTicksForLocalUnixSeconds(1'790'899'200) == 9'771ull * 86'400 * kTicksPerSecond);

// Convert guest ticks to a host duration without applying scheduling policy.
// Oversized durations retain the existing zero-duration failure behavior.
constexpr std::chrono::nanoseconds TicksToDuration(uint64_t ticks) noexcept
{
    constexpr uint64_t kMaxNanoseconds =
        static_cast<uint64_t>(std::chrono::nanoseconds::max().count());
    constexpr uint64_t kMaxTicks = NanosecondsToTicks(kMaxNanoseconds);

    if (ticks == 0 || ticks > kMaxTicks) {
        return std::chrono::nanoseconds::zero();
    }

    const uint64_t nanoseconds =
        (ticks / kTickRatioNumerator) * kTickRatioDenominator +
        ((ticks % kTickRatioNumerator) * kTickRatioDenominator) /
            kTickRatioNumerator;
    return std::chrono::nanoseconds(static_cast<int64_t>(nanoseconds));
}

} // namespace TimeBaseContract
