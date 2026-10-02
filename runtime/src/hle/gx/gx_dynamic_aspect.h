#pragma once

#include <algorithm>
#include <aurora/render_size_limits.hpp>
#include <cmath>
#include <cstdint>

// The window-shape correction for full-screen perspective projections (dynamic_aspect.cpp). With
// k = surfaceAspect / (16:9), the horizontal FOV widens by max(k, 1) and the vertical by
// max(1/k, 1): Hor+ above 16:9 and Vert+ below it, without cropping either axis.
namespace DynamicAspect {

inline constexpr float kPresentationAspect169 = 16.0f / 9.0f;
// Guard against a degenerate window turning the world inside out: a 6x vertical expansion of a
// 45 degree FOV is already ~145 degrees, past which near-plane clipping and the game's own
// culling stop behaving.
inline constexpr float kMaxVerticalExpansion =
    aurora::render_size_limits::kMaxDynamicPortraitExpansion;

inline float SurfaceAspect(uint32_t width, uint32_t height) noexcept {
    if (width == 0 || height == 0) {
        return kPresentationAspect169;
    }
    return aurora::render_size_limits::clamp_dynamic_aspect(
        static_cast<float>(width) / static_cast<float>(height));
}

// surfaceAspect / (16:9). Above 1 the surface is wider than 16:9, below 1 it
// is tighter (4:3, portrait).
inline float AspectRatioFactor(uint32_t width, uint32_t height) noexcept {
    return SurfaceAspect(width, height) / kPresentationAspect169;
}

// Hor+ factor, clamped at 1 so a tighter-than-16:9 surface never narrows the
// canvas (that would crop away scene the 16:9 view showed).
inline float HorizontalExpansion(uint32_t width, uint32_t height) noexcept {
    return std::max(1.0f, AspectRatioFactor(width, height));
}

// Vert+ factor, clamped at 1 so a wider-than-16:9 surface never shortens the
// frustum. Exactly one of the two expansions is ever greater than 1.
inline float VerticalExpansion(uint32_t width, uint32_t height) noexcept {
    const float factor = AspectRatioFactor(width, height);
    if (!(factor > 0.0f)) {
        return 1.0f;
    }
    return std::clamp(1.0f / factor, 1.0f, kMaxVerticalExpansion);
}

} // namespace DynamicAspect
