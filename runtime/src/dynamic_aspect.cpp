#include "hle_stubs.h"

#include <atomic>
#include "aurora_events.h"
#include "hle/gx/gx_dynamic_aspect.h"

#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>

// MSC dynamic aspect. With widescreen on, the game boots in its native 16:9 mode (SCGetAspectRatio)
// and aurora stretches the content framebuffer to the window's shape. Full-screen perspective
// projections are then corrected for the window (Hor+ wider than 16:9, Vert+ narrower, e.g. a 16:10
// Mac display), so the 3D fills any window without bars or distortion; 2D layouts stay on the game's
// 16:9 canvas. "Force 16:9" keeps a fixed 16:9 image with bars instead, and widescreen off is 4:3.

namespace {

std::atomic<uint32_t> g_surfaceWidth{0};
std::atomic<uint32_t> g_surfaceHeight{0};
bool g_widescreen = false;
std::atomic_bool g_forceAspect169{false};
std::atomic_bool g_policyDirty{false};

void ApplyPolicy() {
    if (!g_widescreen) {
        AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
        VILockAspectRatio(4, 3);
        return;
    }
    VIUnlockAspectRatio();
    AuroraSetViewportPolicy(g_forceAspect169.load() ? AURORA_VIEWPORT_16_9 : AURORA_VIEWPORT_STRETCH);
}

} // namespace

void UpdateDynamicAspectSurface(uint32_t surfaceWidth, uint32_t surfaceHeight) {
    g_surfaceWidth.store(surfaceWidth, std::memory_order_relaxed);
    g_surfaceHeight.store(surfaceHeight, std::memory_order_relaxed);
    // Policy changes drain GX, so they are applied here at the frame boundary.
    if (g_policyDirty.exchange(false, std::memory_order_acq_rel)) {
        ApplyPolicy();
    }
}

void SetDynamicAspectForce169(bool enabled) {
    g_forceAspect169.store(enabled, std::memory_order_release);
    g_dynamicAspectRatioEnabled = g_widescreen && !enabled;
    g_policyDirty.store(true, std::memory_order_release);
}

bool DynamicAspectForce169Requested() {
    return g_forceAspect169.load(std::memory_order_acquire);
}

void ConfigureDynamicAspect(bool widescreen, bool forceAspect169, uint32_t surfaceWidth, uint32_t surfaceHeight) {
    g_widescreen = widescreen || forceAspect169;
    g_forceAspect169.store(forceAspect169, std::memory_order_relaxed);
    g_dynamicAspectRatioEnabled = g_widescreen && !forceAspect169;
    g_surfaceWidth.store(surfaceWidth, std::memory_order_relaxed);
    g_surfaceHeight.store(surfaceHeight, std::memory_order_relaxed);
    // Called before aurora has a window; applied at the first frame boundary.
    g_policyDirty.store(true, std::memory_order_release);
}

bool AdjustPerspectiveForSurface(float m[16]) {
    if (!g_dynamicAspectRatioEnabled) {
        return false;
    }
    const uint32_t width = g_surfaceWidth.load(std::memory_order_relaxed);
    const uint32_t height = g_surfaceHeight.load(std::memory_order_relaxed);
    const float horizontal = DynamicAspect::HorizontalExpansion(width, height);
    const float vertical = DynamicAspect::VerticalExpansion(width, height);
    if (horizontal == 1.0f && vertical == 1.0f) {
        return false;
    }
    // Row 0 maps view x to clip x, row 1 view y to clip y (GX row-major); scaling a whole row keeps
    // off-centre frustums centred on the same point.
    for (int i = 0; i < 4; ++i) {
        m[i] /= horizontal;
        m[4 + i] /= vertical;
    }
    return true;
}
