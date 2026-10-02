// gx_transform.cpp - Viewport, Projection, and Matrix Functions
#include "isa/big_endian.h"
#include "gx_internal.h"

#include <cstring>

namespace {
    static inline void SwapBeF32ArrayToHost(const uint32_t* srcBe, float* dst, size_t count) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(srcBe);
        for (size_t i = 0; i < count; ++i) {
            dst[i] = BigEndian::ReadFloat32(bytes + i * sizeof(uint32_t));
        }
    }

    static void UpdateProjectionVectorFromMatrix(const float* mtxRowMajor, GXProjectionType projType) {
        g_projectionVector[0] = (projType == GX_PERSPECTIVE) ? 0.0f : 1.0f;
        g_projectionVector[1] = mtxRowMajor[0];
        g_projectionVector[3] = mtxRowMajor[5];
        g_projectionVector[5] = mtxRowMajor[10];
        g_projectionVector[6] = mtxRowMajor[11];
        if (projType == GX_PERSPECTIVE) {
            g_projectionVector[2] = mtxRowMajor[2];
            g_projectionVector[4] = mtxRowMajor[6];
        } else {
            g_projectionVector[2] = mtxRowMajor[3];
            g_projectionVector[4] = mtxRowMajor[7];
        }
    }

    static void UpdateProjectionVectorFromProjV(const float* projV) {
        std::memcpy(g_projectionVector, projV, sizeof(g_projectionVector));
    }

}

// ============================================================================
// Viewport
// ============================================================================

// Viewport and scissor go to aurora in guest EFB coordinates, mapped to the active render target
// exactly once inside gx::map_logical_viewport/map_logical_scissor. A prior VI origin/scale
// transform double-transformed menu viewports and broke 640x480 overscan, so it was removed.
extern "C" void GX__SetViewportJitter_80173378(float l, float t, float w, float h, float nz, float fz, uint32_t f) {
    g_viewportState[0]=l; g_viewportState[1]=t; g_viewportState[2]=w; g_viewportState[3]=h; g_viewportState[4]=nz; g_viewportState[5]=fz;
    GXSetViewportJitter(l, t, w, h, nz, fz, f);
}
// MSC-UNMAPPED(GX::SetViewportJitter) PPC_NATIVE_OVERRIDE_VOID(80173378, GX__SetViewportJitter_80173378, (float l, float t, float w, float h, float nz, float fz, uint32_t f), (l, t, w, h, nz, fz, f));

// Dynamic aspect: full-screen perspective projections are widened to the window's shape (see
// dynamic_aspect.cpp). Smaller viewports are render-to-texture passes (shadows, previews) and keep
// the game's projection, unless it is the scene camera's. The game reads back its own matrix via
// GXGetProjectionv either way.
namespace {
float g_lastPerspective[16];
bool g_lastWasPerspective = false;
bool g_lastPerspectiveAdjusted = false;
float g_sceneProjection[16];
bool g_haveSceneProjection = false;

bool ViewportIsFullScreen() { return g_viewportState[2] >= 600.0f && g_viewportState[3] >= 400.0f; }

// The scene camera's projection is widened, and so is any pass that reuses it in a smaller viewport
// (the heat haze draws its distortion at half size): it must line up with the scene it distorts.
bool WidensProjection(const float raw[16]) {
    if (ViewportIsFullScreen()) return true;
    return g_haveSceneProjection && std::memcmp(raw, g_sceneProjection, sizeof(g_sceneProjection)) == 0;
}

void SubmitProjection(const float raw[16], GXProjectionType type) {
    g_lastWasPerspective = type == GX_PERSPECTIVE;
    g_lastPerspectiveAdjusted = false;
    if (g_lastWasPerspective) {
        std::memcpy(g_lastPerspective, raw, sizeof(g_lastPerspective));
        float adjusted[16];
        std::memcpy(adjusted, raw, sizeof(adjusted));
        if (WidensProjection(raw) && AdjustPerspectiveForSurface(adjusted)) {
            if (ViewportIsFullScreen()) {
                std::memcpy(g_sceneProjection, raw, sizeof(g_sceneProjection));
                g_haveSceneProjection = true;
            }
            g_lastPerspectiveAdjusted = true;
            GXSetProjection(adjusted, type);
            return;
        }
    }
    GXSetProjection(raw, type);
}
} // namespace

extern "C" void GX__SetViewport_803A7828(float l, float t, float w, float h, float nz, float fz) {
    g_viewportState[0]=l; g_viewportState[1]=t; g_viewportState[2]=w; g_viewportState[3]=h; g_viewportState[4]=nz; g_viewportState[5]=fz;
    GXSetViewport(l, t, w, h, nz, fz);
    // A projection set before its viewport: re-issue it once the viewport says which pass it is.
    if (g_lastWasPerspective && g_lastPerspectiveAdjusted != WidensProjection(g_lastPerspective)) {
        SubmitProjection(g_lastPerspective, GX_PERSPECTIVE);
    }
}
PPC_NATIVE_OVERRIDE_VOID(803A7828, GX__SetViewport_803A7828, (float l, float t, float w, float h, float nz, float fz), (l, t, w, h, nz, fz));

extern "C" void GX__GetViewportv_803A7854(uint32_t oa) {
    if (!oa) return; for(int i=0; i<6; ++i) WriteGuestFloat(oa + i*4, g_viewportState[i]);
}
PPC_NATIVE_OVERRIDE_VOID(803A7854, GX__GetViewportv_803A7854, (uint32_t oa), (oa));

extern "C" void GX__SetZScaleOffset_803A7874(float s, float o) {
    GXSetZScaleOffset(s, o);
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            constexpr float kZ24Scale = 16777215.0f;
            Memory::WriteFloat32(gd + 0x55Cu, kZ24Scale * o);
            Memory::WriteFloat32(gd + 0x560u, 1.0f + kZ24Scale * s);
            Memory::Write32(gd + 0x5FCu, Memory::Read32(gd + 0x5FCu) | 0x10000000u);
        }
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(803A7874, GX__SetZScaleOffset_803A7874, (float s, float o), (s, o));

extern "C" void GX__SetScissorBoxOffset_803A7954(int32_t xo, int32_t yo) {
    GXSetScissorBoxOffset(xo, yo);
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) Memory::Write16(gd + 2, 0);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(803A7954, GX__SetScissorBoxOffset_803A7954, (int32_t xo, int32_t yo), (xo, yo));

// ============================================================================
// Scissor
// ============================================================================

extern "C" void GX__SetScissor_803A78A4(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    g_scissorLeft=(int32_t)l; g_scissorTop=(int32_t)t; g_scissorWidth=(int32_t)w; g_scissorHeight=(int32_t)h;
    try { uint32_t gd=Memory::Read32(kGXDataPtrAddr); if(gd){
        uint32_t r148=Memory::Read32(gd+0x148), r14c=Memory::Read32(gd+0x14c);
        uint32_t sx=g_scissorLeft+0x156, sy=g_scissorTop+0x156, ex=sx+g_scissorWidth-1, ey=sy+g_scissorHeight-1;
        Memory::Write32(gd+0x148, ((sx<<12)&0x7ff000u)|(sy&0x7ffu)|(r148&0xff800800u));
        Memory::Write32(gd+0x14c, ((ex<<12)&0x7ff000u)|(ey&0x7ffu)|(r14c&0xff800800u));
        Memory::Write16(gd+2, 0);
    } } catch(...) {}
    // No viewport replay here: aurora recomputes viewport and scissor together
    // on every scissor change (set_logical_scissor -> apply_logical_render_state),
    // so re-issuing the current viewport would be duplicate work.
    GXSetScissor(l, t, w, h);
}
PPC_NATIVE_OVERRIDE_VOID(803A78A4, GX__SetScissor_803A78A4, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

// ============================================================================
// Projection
// ============================================================================

extern "C" void GX__SetProjection_803A752C(uint32_t ma, uint32_t pt) {
    const uint32_t* raw=(const uint32_t*)GuestToHostPtr(ma, 64); float m[16];
    SwapBeF32ArrayToHost(raw, m, 16);
    SubmitProjection(m, (GXProjectionType)pt);
    UpdateProjectionVectorFromMatrix(m, (GXProjectionType)pt);
}
PPC_NATIVE_OVERRIDE_VOID(803A752C, GX__SetProjection_803A752C, (uint32_t ma, uint32_t pt), (ma, pt));

extern "C" void GX__SetProjectionv_803A7590(uint32_t pa) {
    const uint32_t* raw=(const uint32_t*)GuestToHostPtr(pa, 28); float v[7];
    SwapBeF32ArrayToHost(raw, v, 7);
    GXProjectionType pt=(v[0]!=0.f)?GX_ORTHOGRAPHIC:GX_PERSPECTIVE;
    float m[16]={0.f}; m[0]=v[1]; m[5]=v[3]; m[10]=v[5]; m[11]=v[6];
    if(pt==GX_PERSPECTIVE){ m[2]=v[2]; m[6]=v[4]; m[14]=-1.f; } else { m[3]=v[2]; m[7]=v[4]; m[15]=1.f; }
    SubmitProjection(m, pt);
    UpdateProjectionVectorFromProjV(v);
}
PPC_NATIVE_OVERRIDE_VOID(803A7590, GX__SetProjectionv_803A7590, (uint32_t pa), (pa));

extern "C" void GX__GetProjectionv_803A75DC(uint32_t pa) {
    uint8_t* out = static_cast<uint8_t*>(GuestToHostPtr(pa, 28));
    for (int i = 0; i < 7; ++i) {
        BigEndian::WriteFloat32(out + i * sizeof(float), g_projectionVector[i]);
    }
}
PPC_NATIVE_OVERRIDE_VOID(803A75DC, GX__GetProjectionv_803A75DC, (uint32_t pa), (pa));

// ============================================================================
// Matrix Loading
// ============================================================================

extern "C" void GX__LoadPosMtxImm_803A761C(uint32_t ma, uint32_t id) {
    const uint32_t* raw=(const uint32_t*)GuestToHostPtr(ma); float m[12];
    SwapBeF32ArrayToHost(raw, m, 12);
    GXLoadPosMtxImm((float(*)[4])m, id);
}
PPC_NATIVE_OVERRIDE_VOID(803A761C, GX__LoadPosMtxImm_803A761C, (uint32_t ma, uint32_t id), (ma, id));

extern "C" void GX__LoadPosMtxIndx_8017315c(uint32_t mi, uint32_t id) {
    const auto& arr=g_hleGxState.vtxArray[GX_POS_MTX_ARRAY];
    if(arr.base==0||arr.stride==0) return;
    const uint32_t* raw=(const uint32_t*)GuestToHostPtr(arr.base+mi*arr.stride, 48);
    if(raw){ float m[12]; SwapBeF32ArrayToHost(raw,m,12); GXLoadPosMtxImm((float(*)[4])m,id); }
}
// MSC-UNMAPPED(GX::LoadPosMtxIndx) PPC_NATIVE_OVERRIDE_VOID(8017315c, GX__LoadPosMtxIndx_8017315c, (uint32_t mi, uint32_t id), (mi, id));

extern "C" void GX__LoadNrmMtxImm_803A766C(uint32_t ma, uint32_t id) {
    const uint32_t* raw=(const uint32_t*)GuestToHostPtr(ma, 48); float m[12];
    SwapBeF32ArrayToHost(raw, m, 12); GXLoadNrmMtxImm((float(*)[4])m, id);
}
PPC_NATIVE_OVERRIDE_VOID(803A766C, GX__LoadNrmMtxImm_803A766C, (uint32_t ma, uint32_t id), (ma, id));

extern "C" void GX__LoadNrmMtxIndx3x3_801731e0(uint32_t mi, uint32_t id) {
    const auto& arr=g_hleGxState.vtxArray[GX_NRM_MTX_ARRAY];
    if(arr.base==0||arr.stride==0) return;
    const uint32_t* raw=(const uint32_t*)GuestToHostPtr(arr.base+mi*arr.stride, 36);
    if(raw){
        float src[9]{};
        float m[12]{};
        SwapBeF32ArrayToHost(raw, src, 9);
        for (uint32_t row = 0; row < 3; ++row) {
            for (uint32_t col = 0; col < 3; ++col) {
                m[row * 4 + col] = src[row * 3 + col];
            }
        }
        GXLoadNrmMtxImm((float(*)[4])m, id);
    }
}
// MSC-UNMAPPED(GX::LoadNrmMtxIndx3x3) PPC_NATIVE_OVERRIDE_VOID(801731e0, GX__LoadNrmMtxIndx3x3_801731e0, (uint32_t mi, uint32_t id), (mi, id));

extern "C" void GX__LoadTexMtxImm_803A76E4(uint32_t ma, uint32_t id, uint32_t t) {
    size_t c=(t==(uint32_t)GX_MTX3x4)?12:8;
    const uint32_t* raw=(const uint32_t*)GuestToHostPtr(ma,c*4); float l[12]={};
    SwapBeF32ArrayToHost(raw,l,c); GXLoadTexMtxImm(l, id, (GXTexMtxType)t);
}
PPC_NATIVE_OVERRIDE_VOID(803A76E4, GX__LoadTexMtxImm_803A76E4, (uint32_t ma, uint32_t id, uint32_t t), (ma, id, t));

extern "C" void GX__SetCurrentMtx_803A76C4(uint32_t id) { GXSetCurrentMtx(id); }
PPC_NATIVE_OVERRIDE_VOID(803A76C4, GX__SetCurrentMtx_803A76C4, (uint32_t id), (id));
