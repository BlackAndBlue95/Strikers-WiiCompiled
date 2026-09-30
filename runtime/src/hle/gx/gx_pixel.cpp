// gx_pixel.cpp - Pixel Processing, Blending, and Fog
#include "gx_internal.h"

// ============================================================================
// Blend Mode
// ============================================================================

extern "C" void GX__SetBlendMode_803A6EDC(uint32_t t, uint32_t s, uint32_t d, uint32_t op) {
    GXSetBlendMode(static_cast<GXBlendMode>(t), static_cast<GXBlendFactor>(s),
                   static_cast<GXBlendFactor>(d), static_cast<GXLogicOp>(op));
}
PPC_NATIVE_OVERRIDE_VOID(803A6EDC, GX__SetBlendMode_803A6EDC, (uint32_t t, uint32_t s, uint32_t d, uint32_t op), (t, s, d, op));

extern "C" void GX__SetColorUpdate_803A6F2C(uint32_t en) {
    GXSetColorUpdate(static_cast<GXBool>(en));
}
PPC_NATIVE_OVERRIDE_VOID(803A6F2C, GX__SetColorUpdate_803A6F2C, (uint32_t en), (en));

extern "C" void GX__SetAlphaUpdate_803A6F58(uint32_t en) {
    GXSetAlphaUpdate(static_cast<GXBool>(en));
}
PPC_NATIVE_OVERRIDE_VOID(803A6F58, GX__SetAlphaUpdate_803A6F58, (uint32_t en), (en));

// ============================================================================
// Z Buffer
// ============================================================================

extern "C" void GX__SetZMode_803A6F84(uint32_t ce, uint32_t f, uint32_t ue) {
    GXSetZMode(static_cast<GXBool>(ce), static_cast<GXCompare>(f), static_cast<GXBool>(ue));
}
PPC_NATIVE_OVERRIDE_VOID(803A6F84, GX__SetZMode_803A6F84, (uint32_t ce, uint32_t f, uint32_t ue), (ce, f, ue));

extern "C" void GX__SetZCompLoc_803A6FB8(uint32_t bt) {
    GXSetZCompLoc(static_cast<GXBool>(bt));
}
PPC_NATIVE_OVERRIDE_VOID(803A6FB8, GX__SetZCompLoc_803A6FB8, (uint32_t bt), (bt));

// ============================================================================
// Pixel Format and Dither
// ============================================================================

extern "C" void GX__SetPixelFmt_803A6FE8(uint32_t pf, uint32_t zf) {
    GXSetPixelFmt(static_cast<GXPixelFmt>(pf), static_cast<GXZFmt16>(zf));
}
PPC_NATIVE_OVERRIDE_VOID(803A6FE8, GX__SetPixelFmt_803A6FE8, (uint32_t pf, uint32_t zf), (pf, zf));

extern "C" void GX__SetDither_803A7090(uint32_t d) {
    GXSetDither(static_cast<GXBool>(d));
}
PPC_NATIVE_OVERRIDE_VOID(803A7090, GX__SetDither_803A7090, (uint32_t d), (d));

extern "C" void GX__SetDstAlpha_803A70BC(uint32_t en, uint32_t a) {
    GXSetDstAlpha(static_cast<GXBool>(en), static_cast<u8>(a));
}
PPC_NATIVE_OVERRIDE_VOID(803A70BC, GX__SetDstAlpha_803A70BC, (uint32_t en, uint32_t a), (en, a));

// ============================================================================
// Fog and Alpha Compare
// ============================================================================

extern "C" void GX__SetFog_803A6B8C(uint32_t t, float sz, float ez, float nz, float fz, uint32_t cp) {
    GXSetFog((GXFogType)t, sz, ez, nz, fz, DecodeGxColor(Memory::Read32(cp)));
}
PPC_NATIVE_OVERRIDE_VOID(803A6B8C, GX__SetFog_803A6B8C, (uint32_t t, float sz, float ez, float nz, float fz, uint32_t cp), (t, sz, ez, nz, fz, cp));

extern "C" void GX__SetAlphaCompare_803A6948(uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1) {
    g_alphaCompareValid = true;
    GXSetAlphaCompare((GXCompare)c0, (u8)r0, (GXAlphaOp)op, (GXCompare)c1, (u8)r1);
}
PPC_NATIVE_OVERRIDE_VOID(803A6948, GX__SetAlphaCompare_803A6948, (uint32_t c0, uint32_t r0, uint32_t op, uint32_t c1, uint32_t r1), (c0, r0, op, c1, r1));

extern "C" void GX__SetZTexture_803A6980(uint32_t op, uint32_t f, uint32_t b) { GXSetZTexture((GXZTexOp)op, (GXTexFmt)f, b); }
PPC_NATIVE_OVERRIDE_VOID(803A6980, GX__SetZTexture_803A6980, (uint32_t op, uint32_t f, uint32_t b), (op, f, b));

// ============================================================================
// Culling and Clipping
// ============================================================================

extern "C" void GX__SetCullMode_803A4028(uint32_t m) {
    GXSetCullMode(static_cast<GXCullMode>(m));
}
PPC_NATIVE_OVERRIDE_VOID(803A4028, GX__SetCullMode_803A4028, (uint32_t m), (m));

extern "C" void GX__SetCoPlanar_803A4050(uint32_t en) { GXSetCoPlanar((GXBool)en); }
PPC_NATIVE_OVERRIDE_VOID(803A4050, GX__SetCoPlanar_803A4050, (uint32_t en), (en));

extern "C" void GX__SetClipMode_803A7990(uint32_t m) { GXSetClipMode((GXClipMode)m); }
PPC_NATIVE_OVERRIDE_VOID(803A7990, GX__SetClipMode_803A7990, (uint32_t m), (m));
