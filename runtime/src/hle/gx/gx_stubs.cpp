#include "gx_internal.h"
#include "runtime_log.h"

extern "C" void __GXSetSUTexRegs();

// ============================================================================
// FIFO Write Helpers
// ============================================================================

extern "C" void GX_HLE_FIFO_WriteFloat(float val) {
    u32 raw; std::memcpy(&raw, &val, 4);
    try { HleFifoWrite(raw, 4); } catch (...) { RT_LOGF(RT_TAG_GX, "FIFO write float failed\n"); }
}

extern "C" void GX_HLE_FIFO_Write32(uint32_t val) { HleFifoWrite(val, 4); }
extern "C" void GX_HLE_FIFO_Write16(uint16_t val) { HleFifoWrite(static_cast<u32>(val), 2); }
extern "C" void GX_HLE_FIFO_Write8(uint8_t val) { HleFifoWrite(static_cast<u32>(val), 1); }

extern "C" void GX__SetDrawSync_8016ed08(uint32_t token) {
    (void)token;
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) {
        if (Memory::Read32(gd + 0x5FCu)) GX__SetDirtyState_803A3AE8();
        Memory::Write16(gd + 2, 0);
    } } catch (...) {}
}

extern "C" void GX__SetDrawSync_8016e9fc(uint32_t token) { GX__SetDrawSync_8016ed08(token); }
// MSC-UNMAPPED(GX::SetDrawSync) PPC_NATIVE_OVERRIDE_VOID(8016e9fc, GX__SetDrawSync_8016e9fc, (uint32_t token), (token));

extern "C" void GX__FinishInterruptHandler_803A3A04() {
    try {
        uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) Memory::Write16(gd + 0x0Au, static_cast<uint16_t>(Memory::Read16(gd + 0x0Au) | 0x0008u));
        Memory::Write8(kGxDrawDoneFlagAddr, 1);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(803A3A04, GX__FinishInterruptHandler_803A3A04, (), ());

// MSC: Strikers Charged's glx swap waits on GXSetDrawDoneCallback's callback (PE finish
// interrupt) rather than the synchronous GXDrawDone MKW uses. Complete the draw-done
// immediately and deliver the callback the way __GXFinishInterruptHandler would.
constexpr uint32_t kMscDrawDoneCbAddr = 0x806E27FC; // DrawDoneCB
extern "C" void MSC_GXSetDrawDone_803A3610(CpuContext* ctx) {
    GXDrawDone();
    GX__FinishInterruptHandler_803A3A04();
    uint32_t cb = 0;
    try { cb = Memory::Read32(kMscDrawDoneCbAddr); } catch (...) {}
    if (cb != 0 && ctx) {
        const uint32_t savedLr = ctx->lr;
        InvokeIndirectCpu(cb, ctx);
        ctx->lr = savedLr;
    }
}
PPC_NATIVE_OVERRIDE_VOID(803A3610, MSC_GXSetDrawDone_803A3610, (CpuContext* ctx), (ctx));

extern "C" void GX__DrawDone_803A36F4() {
    try { Memory::Write8(kGxDrawDoneFlagAddr, 0); } catch (...) {}
    GXDrawDone(); GX__FinishInterruptHandler_803A3A04();
}
PPC_NATIVE_OVERRIDE_VOID(803A36F4, GX__DrawDone_803A36F4, (), ());

extern "C" void GX__PixModeSync_803A37B4() {
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
    GXPixModeSync();
}
PPC_NATIVE_OVERRIDE_VOID(803A37B4, GX__PixModeSync_803A37B4, (), ());

// ============================================================================
// Hardware Revision / Thread Query - No-ops
// ============================================================================

extern "C" void __GX__InitRevisionBits_803A0BE0() {}
PPC_NATIVE_OVERRIDE_VOID(803A0BE0, __GX__InitRevisionBits_803A0BE0, (), ());

// ============================================================================
// Texture State Management - Aurora handles internally
// ============================================================================

extern "C" void __GX__SetSUTexRegs_803A5BB0() {
    __GXSetSUTexRegs();
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(803A5BB0, __GX__SetSUTexRegs_803A5BB0, (), ());

extern "C" void __GX__SetTmemConfig_803A5D18(uint32_t mode) {
    // TMEM layout configuration - Aurora manages internally
    (void)mode;
}
PPC_NATIVE_OVERRIDE_VOID(803A5D18, __GX__SetTmemConfig_803A5D18, (uint32_t mode), (mode));

extern "C" void __GX__FlushTextureState_803A64E8() {
    // BP texture state flush - Aurora handles via API
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(803A64E8, __GX__FlushTextureState_803A64E8, (), ());

// ============================================================================
// Copy Configuration - No-ops for features Aurora doesn't use
// ============================================================================

extern "C" void GX__SetDispCopyFrame2Field_803A437C(uint32_t f) {
    GXSetDispCopyFrame2Field(f);
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            Memory::Write32(gd + 0x23Cu, (Memory::Read32(gd + 0x23Cu) & 0xFFFFCFFFu) | ((f & 3u) << 12));
            Memory::Write32(gd + 0x24Cu, Memory::Read32(gd + 0x24Cu) & 0xFFFFCFFFu);
        }
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(803A437C, GX__SetDispCopyFrame2Field_803A437C, (uint32_t f), (f));

extern "C" void GX__SetCopyClamp_803A439C(uint32_t c) {
    GXSetCopyClamp(static_cast<GXFBClamp>(c));
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            const uint32_t clamp = c & 3u;
            Memory::Write32(gd + 0x23Cu, (Memory::Read32(gd + 0x23Cu) & 0xFFFFFFFCu) | clamp);
            Memory::Write32(gd + 0x24Cu, (Memory::Read32(gd + 0x24Cu) & 0xFFFFFFFCu) | clamp);
        }
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(803A439C, GX__SetCopyClamp_803A439C, (uint32_t c), (c));

extern "C" void GX__ClearBoundingBox_803A4BC4() {
    GXClearBoundingBox();
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) Memory::Write16(gd + 2, 0);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(803A4BC4, GX__ClearBoundingBox_803A4BC4, (), ());

// ============================================================================
// FIFO/State Management - No-ops
// ============================================================================

extern "C" void GX__SetDirtyState_803A3AE8() {
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write32(gd + 0x5FCu, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(803A3AE8, GX__SetDirtyState_803A3AE8, (), ());

extern "C" void GX__ResetWriteGatherPipe_803A32B8() {
    // WPAR reset - not needed on host
}
PPC_NATIVE_OVERRIDE_VOID(803A32B8, GX__ResetWriteGatherPipe_803A32B8, (), ());
