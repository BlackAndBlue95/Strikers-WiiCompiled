// Auto-generated GX fatal stubs
#include "hle_stubs.h"
#include "runtime_log.h"

namespace {
[[noreturn]] void HaltGX(uint32_t addr, const char* name) {
    const char* symbol = name ? name : "<unknown GX symbol>";
    RT_LOGF(RT_TAG_GX,
            "unimplemented GX entry point: %s at guest address 0x%08X.\n"
            "[gx] This graphics call has no Aurora implementation bound to it yet, so the\n"
            "[gx] runtime cannot continue without silently dropping GPU state. Bind it in\n"
            "[gx] runtime/src/hle/gx/ and remove the stub from gx_fatal_stubs.cpp.\n",
            symbol, addr);
    std::fflush(stderr);
    char message[512]{};
    std::snprintf(message, sizeof(message),
                  "%s at guest address 0x%08X has no Aurora implementation bound to it, so the "
                  "runtime stopped rather than keep rendering with missing GPU state.",
                  symbol, addr);
    ShowRuntimeFatalPopup("the game called an unimplemented graphics function", message);
    std::abort();
}
} // namespace

// Every fatal stub is the same two statements with the address and the symbol
// name substituted, so the body comes from this macro. The registration is
// deliberately still spelled out per entry so the translator's runtime-native
// index sees the literal PPC_NATIVE_OVERRIDE_VOID invocation. Hiding it inside
// this macro would leave the index unable to associate an address with the stub.
// MSC: GX entry points MKW never reached but Strikers Charged does, where dropping
// the call is harmless for now (EFB poke state only affects CPU-side EFB writes).
#define GX_NOOP_STUB(addr, sym) \
    extern "C" void gx_stub_##addr(CpuContext* ctx) { (void)ctx; }

#define GX_FATAL_STUB(addr, sym) \
    extern "C" void gx_stub_##addr(CpuContext* ctx) { (void)ctx; HaltGX(0x##addr, sym); }

GX_FATAL_STUB(803A095C, "__GX__DefaultTexRegionCallback_803A095C") PPC_NATIVE_OVERRIDE_VOID(803A095C, gx_stub_803A095C, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A0A50, "__GX__DefaultTlutRegionCallback_803A0A50") PPC_NATIVE_OVERRIDE_VOID(803A0A50, gx_stub_803A0A50, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A0A74, "__GX__Shutdown_803A0A74") PPC_NATIVE_OVERRIDE_VOID(803A0A74, gx_stub_803A0A74, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A1B24, "GX__CPInterruptHandler_803A1B24") PPC_NATIVE_OVERRIDE_VOID(803A1B24, gx_stub_803A1B24, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A247C, "GX__SetBreakPtCallback_803A247C") PPC_NATIVE_OVERRIDE_VOID(803A247C, gx_stub_803A247C, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A253C, "__GX__CleanGPFifo_803A253C") PPC_NATIVE_OVERRIDE_VOID(803A253C, gx_stub_803A253C, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A2940, "__GX__SetVCD_803A2940") PPC_NATIVE_OVERRIDE_VOID(803A2940, gx_stub_803A2940, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A29F0, "__GX__CalculateVLim_803A29F0") PPC_NATIVE_OVERRIDE_VOID(803A29F0, gx_stub_803A29F0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A2EB4, "__GX__SetVAT_803A2EB4") PPC_NATIVE_OVERRIDE_VOID(803A2EB4, gx_stub_803A2EB4, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A32EC, "__GX__Abort_803A32EC") PPC_NATIVE_OVERRIDE_VOID(803A32EC, gx_stub_803A32EC, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A3450, "GX__AbortFrame_803A3450") PPC_NATIVE_OVERRIDE_VOID(803A3450, gx_stub_803A3450, (CpuContext* ctx), (ctx));
GX_NOOP_STUB(803A37D8, "GX__PokeAlphaMode_803A37D8") PPC_NATIVE_OVERRIDE_VOID(803A37D8, gx_stub_803A37D8, (CpuContext* ctx), (ctx));
GX_NOOP_STUB(803A3800, "GX__PokeAlphaUpdate_803A3800") PPC_NATIVE_OVERRIDE_VOID(803A3800, gx_stub_803A3800, (CpuContext* ctx), (ctx));
GX_NOOP_STUB(803A3814, "GX__PokeBlendMode_803A3814") PPC_NATIVE_OVERRIDE_VOID(803A3814, gx_stub_803A3814, (CpuContext* ctx), (ctx));
GX_NOOP_STUB(803A3870, "GX__PokeColorUpdate_803A3870") PPC_NATIVE_OVERRIDE_VOID(803A3870, gx_stub_803A3870, (CpuContext* ctx), (ctx));
GX_NOOP_STUB(803A3884, "GX__PokeDstAlpha_803A3884") PPC_NATIVE_OVERRIDE_VOID(803A3884, gx_stub_803A3884, (CpuContext* ctx), (ctx));
GX_NOOP_STUB(803A389C, "GX__PokeDither_803A389C") PPC_NATIVE_OVERRIDE_VOID(803A389C, gx_stub_803A389C, (CpuContext* ctx), (ctx));
GX_NOOP_STUB(803A38B0, "GX__PokeZMode_803A38B0") PPC_NATIVE_OVERRIDE_VOID(803A38B0, gx_stub_803A38B0, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A3EAC, "__GX__SendFlushPrim_803A3EAC") PPC_NATIVE_OVERRIDE_VOID(803A3EAC, gx_stub_803A3EAC, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A74E8, "__GX__SetProjection_803A74E8") PPC_NATIVE_OVERRIDE_VOID(803A74E8, gx_stub_803A74E8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A7798, "__GX__SetViewport_803A7798") PPC_NATIVE_OVERRIDE_VOID(803A7798, gx_stub_803A7798, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A79B8, "__GX__SetMatrixIndex_803A79B8") PPC_NATIVE_OVERRIDE_VOID(803A79B8, gx_stub_803A79B8, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A7A40, "GX__SetGPMetric_803A7A40") PPC_NATIVE_OVERRIDE_VOID(803A7A40, gx_stub_803A7A40, (CpuContext* ctx), (ctx));
GX_FATAL_STUB(803A826C, "GX__ClearGPMetric_803A826C") PPC_NATIVE_OVERRIDE_VOID(803A826C, gx_stub_803A826C, (CpuContext* ctx), (ctx));
