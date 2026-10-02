// gx_vertex.cpp - Vertex Descriptor and Attribute Functions
#include "gx_internal.h"
#include "gx_stream_common.h"
#include "fiber_manager.h"
#include "guest_interrupt_context.h"
#include "runtime_log.h"

#include <cstdio>
#include <cstdlib>

namespace {
uint32_t CanonicalVtxAttr(uint32_t attr) {
    return attr == GX_VA_NBT ? GX_VA_NRM : attr;
}

// There is deliberately no indexed-aurora path here. The immediate begin path
// streams vertex data incrementally, and aurora's indexed-array upload must
// precede the draw carrying the max-index bounds, which cannot be known without
// buffering the whole primitive. Indexed attributes are therefore expanded into
// the packed direct stream below (GX_INDEX8/16 -> GX_DIRECT).
void ApplyAuroraVtxStateForBegin(GXVtxFmt fmt) {
    GxStream::PublishAuroraVtxState(fmt,
                                    GxStream::AuroraVtxPublishOptions{
                                        /*includeNbt=*/false,
                                        /*fmtLoopFirst=*/static_cast<int>(GX_VA_POS),
                                        /*fmtLoopLast=*/static_cast<int>(GX_VA_TEX7)});
}
}

// ============================================================================
// Vertex Descriptor
// ============================================================================

extern "C" void GX__ClearVtxDesc_803A2B1C() {
    bool changed = false;
    for(int i=0; i<26; ++i){
        changed |= g_hleGxState.vtxDesc[i] != GX_NONE;
        g_hleGxState.vtxDesc[i]=GX_NONE;
    }
    if (changed) g_hleGxState.InvalidateVtxLayoutHash();
    // GXClearVtxDesc resets descriptors only; array base/stride state persists.
    GXClearVtxDesc();
}
PPC_NATIVE_OVERRIDE_VOID(803A2B1C, GX__ClearVtxDesc_803A2B1C, (), ());

extern "C" void GX__SetVtxDesc_803A26DC(uint32_t a, uint32_t t) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(attr>=26||attr==GX_VA_NULL) return;
    const GXAttrType oldType = g_hleGxState.vtxDesc[attr];
    g_hleGxState.vtxDesc[attr]=(GXAttrType)t;
    if (a == GX_VA_NBT) {
        g_hleGxState.vtxDesc[GX_VA_NBT] = GX_NONE;
    }
    if (g_hleGxState.vtxDesc[attr] != oldType) g_hleGxState.InvalidateVtxLayoutHash();
    if(IsMatrixIndexAttr((GXAttr)attr)) return;
    GXSetVtxDesc((GXAttr)a, (t==GX_INDEX8||t==GX_INDEX16)?GX_DIRECT:(GXAttrType)t);
}
PPC_NATIVE_OVERRIDE_VOID(803A26DC, GX__SetVtxDesc_803A26DC, (uint32_t a, uint32_t t), (a, t));

// ============================================================================
// Vertex Attribute Format
// ============================================================================

extern "C" void GX__SetVtxAttrFmt_803A2B50(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(vf<8&&attr<26){
        const VtxAttrFmt oldFmt = g_hleGxState.vtxAttrFmt[vf][attr];
        g_hleGxState.vtxAttrFmt[vf][attr].cnt=(GXCompCnt)c;
        g_hleGxState.vtxAttrFmt[vf][attr].type=(GXCompType)t;
        g_hleGxState.vtxAttrFmt[vf][attr].frac=(u8)fr;
        if (a == GX_VA_NBT) {
            g_hleGxState.vtxAttrFmt[vf][GX_VA_NBT] = {};
        }
        const auto& newFmt = g_hleGxState.vtxAttrFmt[vf][attr];
        if (oldFmt.cnt != newFmt.cnt || oldFmt.type != newFmt.type || oldFmt.frac != newFmt.frac) {
            g_hleGxState.InvalidateVtxLayoutHash();
        }
    }
    if (vf >= GX_MAX_VTXFMT || a < GX_VA_POS || a >= GX_VA_MAX_ATTR) {
        return;
    }
    GXSetVtxAttrFmt((GXVtxFmt)vf, (GXAttr)a, (GXCompCnt)c, (GXCompType)t, (u8)fr);
}
PPC_NATIVE_OVERRIDE_VOID(803A2B50, GX__SetVtxAttrFmt_803A2B50, (uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr), (vf, a, c, t, fr));

extern "C" void GX__SetVtxAttrFmtv_803A2CF0(uint32_t vf, uint32_t la) {
    if(!la) return; uint32_t p=la;
    while(true){
        uint32_t a=Memory::Read32(p);
        if(a==0xFFu) break;
        GX__SetVtxAttrFmt_803A2B50(vf, a, Memory::Read32(p+4), Memory::Read32(p+8), Memory::Read32(p+12)&0xFFu);
        p+=16;
    }
}
PPC_NATIVE_OVERRIDE_VOID(803A2CF0, GX__SetVtxAttrFmtv_803A2CF0, (uint32_t vf, uint32_t la), (vf, la));

// ============================================================================
// Vertex Arrays
// ============================================================================

extern "C" void GX__SetArray_803A2F34(uint32_t a, uint32_t ba, uint32_t str) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(attr<26){ g_hleGxState.vtxArray[attr].base=ba; g_hleGxState.vtxArray[attr].stride=str; }
}
PPC_NATIVE_OVERRIDE_VOID(803A2F34, GX__SetArray_803A2F34, (uint32_t a, uint32_t ba, uint32_t str), (a, ba, str));

// ============================================================================
// Texture Coordinate Generation
// ============================================================================

extern "C" void GX__SetNumTexGens_803A31AC(uint32_t n) {
    GXSetNumTexGens((u8)n);
}
PPC_NATIVE_OVERRIDE_VOID(803A31AC, GX__SetNumTexGens_803A31AC, (uint32_t n), (n));

extern "C" void GX__SetTexCoordGen2_803A2F84(uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm) {
    if (sp >= static_cast<uint32_t>(GX_MAX_TEXGENSRC)) {
        uint32_t pc = 0;
        uint32_t lr = 0;
        if (auto* cpu = TryGetCpuContext()) {
            pc = cpu->pc;
            lr = cpu->lr;
        }
        RT_LOGF(RT_TAG_GX,
                "GXSetTexCoordGen2 invalid src=%u dst=%u type=%u mtx=%u norm=%u post=%u PC=0x%08X LR=0x%08X\n",
                sp, dc, f, m, n, pm, pc, lr);
    }
    GXSetTexCoordGen2((GXTexCoordID)dc, (GXTexGenType)f, (GXTexGenSrc)sp, m, (GXBool)n, pm);
}
PPC_NATIVE_OVERRIDE_VOID(803A2F84, GX__SetTexCoordGen2_803A2F84, (uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm), (dc, f, sp, m, n, pm));

extern "C" void GX__EnableTexOffsets_803A3FEC(uint32_t coord, uint32_t lineEnable, uint32_t pointEnable) {
    GXEnableTexOffsets(static_cast<GXTexCoordID>(coord),
                       lineEnable ? GX_TRUE : GX_FALSE,
                       pointEnable ? GX_TRUE : GX_FALSE);
}
PPC_NATIVE_OVERRIDE_VOID(803A3FEC, GX__EnableTexOffsets_803A3FEC, (uint32_t coord, uint32_t lineEnable, uint32_t pointEnable), (coord, lineEnable, pointEnable));

extern "C" void GX__SetLineWidth_803A3F84(uint32_t width, uint32_t texOffsets) {
    GXSetLineWidth(static_cast<u8>(width), static_cast<GXTexOffset>(texOffsets));
}
PPC_NATIVE_OVERRIDE_VOID(803A3F84, GX__SetLineWidth_803A3F84, (uint32_t width, uint32_t texOffsets), (width, texOffsets));

extern "C" void GX__SetPointSize_803A3FB8(uint32_t pointSize, uint32_t texOffsets) {
    GXSetPointSize(static_cast<u8>(pointSize), static_cast<GXTexOffset>(texOffsets));
}
PPC_NATIVE_OVERRIDE_VOID(803A3FB8, GX__SetPointSize_803A3FB8, (uint32_t pointSize, uint32_t texOffsets), (pointSize, texOffsets));

// ============================================================================
// Begin/End Drawing
// ============================================================================

namespace {
thread_local bool g_inGxBeginOrFifo = false;
thread_local std::chrono::steady_clock::time_point g_nextDeferredTimingPoll{};

void ServiceDeferredTimingDuringGxWork() {
    if (g_inGxBeginOrFifo) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextDeferredTimingPoll) {
        return;
    }
    g_nextDeferredTimingPoll = now + std::chrono::milliseconds(1);

    g_inGxBeginOrFifo = true;
    try {
        if (Fiber::GuestFiberManager::IsInitialized()) {
            // VI callbacks are what advance EGG::AsyncDisplay on NTSC/PAL60;
            // PAL50 uses an OS alarm. Keep both clocks moving while a slow
            // host frame is still being submitted, but defer fiber switches
            // and Aurora work until the native GX call has unwound.
            VI_HLE_ProcessRetracesDeferred(8);
            OS_HLE_ProcessAlarmsDeferred(8);
        } else if (TryGetCpuContext() != nullptr) {
            // Same interrupt isolation as the deferred paths above: this GX call
            // is mid-execution of a translated function, so the retrace
            // callbacks must not publish their registers into its context.
            GuestInterruptCallbackContext interrupt;
            VI_HLE_PollRetrace(interrupt.get());
        }
    } catch (...) {
        g_inGxBeginOrFifo = false;
        throw;
    }
    g_inGxBeginOrFifo = false;
}
}

extern "C" void GX__Begin_803A3D60(uint32_t t, uint32_t vf, uint32_t nv) {
    if(IsDisplayListActive()){
        WriteDisplayListData((u8)(t|vf), 1);
        WriteDisplayListData((u16)nv, 2);
        return;
    }
    ServiceDeferredTimingDuringGxWork();
    ApplyAuroraVtxStateForBegin(static_cast<GXVtxFmt>(vf));
    g_hleGxState.currentVtxFmt=(GXVtxFmt)vf; g_hleGxState.currentPrim=(GXPrimitive)t;
    g_hleGxState.vertsRemaining=nv; g_hleGxState.inBegin=true; g_hleGxState.auroraBeginCalled=false;
    g_hleGxState.fifoReadOffset = 0;
    g_hleGxState.fifoByteCount=0; g_hleGxState.ResetVertex();
}
PPC_NATIVE_OVERRIDE_VOID(803A3D60, GX__Begin_803A3D60, (uint32_t t, uint32_t vf, uint32_t nv), (t, vf, nv));
