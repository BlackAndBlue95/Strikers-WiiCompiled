// Native call catalog used by InvokeDirectCpu. Keep project-specific HLE
// address mappings here; abi_bridge.h contains only the generic ABI machinery.
//
// This is a .inc, not a header: it has no includes of its own and references
// CpuContext, KnownTypedNativeCpuCall<> and KnownNativeCpuCall<>, all of which
// are declared above its single mid-file include site in abi_bridge.h. It
// cannot compile standalone and must never be included anywhere else.

extern "C" void nw4r__lyt__detail__DrawQuad_800847c0(CpuContext* ctx);
extern "C" void nw4r__lyt__detail__DrawQuad_80084d20(CpuContext* ctx);

extern "C" void GX__SetVtxDesc_803A26DC(uint32_t a, uint32_t t);
extern "C" void GX__ClearVtxDesc_803A2B1C();
extern "C" void GX__SetVtxAttrFmt_803A2B50(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr);
extern "C" void GX__SetTexCoordGen2_803A2F84(uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm);
extern "C" void GX__SetNumTexGens_803A31AC(uint32_t n);
extern "C" void GX__DrawDone_803A36F4();
extern "C" void GX__Begin_803A3D60(uint32_t t, uint32_t vf, uint32_t nv);
extern "C" void GX__SetLineWidth_803A3F84(uint32_t width, uint32_t texOffsets);
extern "C" void GX__SetPointSize_803A3FB8(uint32_t pointSize, uint32_t texOffsets);
extern "C" void GX__SetCullMode_803A4028(uint32_t m);
extern "C" void GX__SetNumChans_803A5068(uint32_t n);
extern "C" void GX__SetChanCtrl_803A508C(uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af);
extern "C" void GX__InitTexObj_803A5314(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m);
extern "C" void GX__InitTexObjLOD_803A5568(uint32_t oa, uint32_t mif, uint32_t maf, float mil, float mal, float lb, uint32_t bc, uint32_t el, uint32_t ma);
extern "C" void GX__InvalidateTexAll_803A5A34();
extern "C" void GX__LoadTexObj_803A5850(uint32_t oa, uint32_t tid);
extern "C" void GX__SetNumIndStages_803A63F8(uint32_t n);
extern "C" void GX__SetTevDirect_803A6418(uint32_t s);
extern "C" void GX__SetTevColorIn_803A65A0(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" void GX__SetTevAlphaIn_803A65E0(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" void GX__SetTevColorOp_803A6620(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_);
extern "C" void GX__SetTevAlphaOp_803A6678(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_);
extern "C" void GX__SetTevColorS10_803A6730(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevKColor_803A6794(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevKColorSel_803A67F0(uint32_t s, uint32_t sel);
extern "C" void GX__SetTevKAlphaSel_803A6840(uint32_t s, uint32_t sel);
extern "C" void GX__SetTevSwapMode_803A6890(uint32_t s, uint32_t rs, uint32_t ts);
extern "C" void GX__SetTevSwapModeTable_803A68CC(uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
extern "C" void GX__SetTevOrder_803A6A0C(uint32_t s, uint32_t c, uint32_t m, uint32_t col);
extern "C" void GX__SetNumTevStages_803A6B68(uint32_t n);
extern "C" void GX__SetBlendMode_803A6EDC(uint32_t t, uint32_t s, uint32_t d, uint32_t op);
extern "C" void GX__LoadPosMtxImm_803A761C(uint32_t ma, uint32_t id);
extern "C" void GX__SetCurrentMtx_803A76C4(uint32_t id);

#define MKW_KNOWN_TYPED_NATIVE(addr, fn, ...) \
template <> struct KnownTypedNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static void Invoke(CpuContext* cpu) { (void)cpu; fn(__VA_ARGS__); } \
}

MKW_KNOWN_TYPED_NATIVE(0x803A26DCu, GX__SetVtxDesc_803A26DC, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x803A2B1Cu, GX__ClearVtxDesc_803A2B1C);
MKW_KNOWN_TYPED_NATIVE(0x803A2B50u, GX__SetVtxAttrFmt_803A2B50, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x803A2F84u, GX__SetTexCoordGen2_803A2F84, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x803A31ACu, GX__SetNumTexGens_803A31AC, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x803A36F4u, GX__DrawDone_803A36F4);
MKW_KNOWN_TYPED_NATIVE(0x803A3D60u, GX__Begin_803A3D60, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x803A3F84u, GX__SetLineWidth_803A3F84, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x803A3FB8u, GX__SetPointSize_803A3FB8, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x803A4028u, GX__SetCullMode_803A4028, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x803A5068u, GX__SetNumChans_803A5068, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x803A508Cu, GX__SetChanCtrl_803A508C, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9]);
MKW_KNOWN_TYPED_NATIVE(0x803A5314u, GX__InitTexObj_803A5314, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10]);
MKW_KNOWN_TYPED_NATIVE(0x803A5568u, GX__InitTexObjLOD_803A5568, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x803A5A34u, GX__InvalidateTexAll_803A5A34);
MKW_KNOWN_TYPED_NATIVE(0x803A5850u, GX__LoadTexObj_803A5850, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x803A63F8u, GX__SetNumIndStages_803A63F8, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x803A6418u, GX__SetTevDirect_803A6418, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x803A65A0u, GX__SetTevColorIn_803A65A0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x803A65E0u, GX__SetTevAlphaIn_803A65E0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x803A6620u, GX__SetTevColorOp_803A6620, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x803A6678u, GX__SetTevAlphaOp_803A6678, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(0x803A6730u, GX__SetTevColorS10_803A6730, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x803A6794u, GX__SetTevKColor_803A6794, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x803A67F0u, GX__SetTevKColorSel_803A67F0, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x803A6840u, GX__SetTevKAlphaSel_803A6840, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x803A6890u, GX__SetTevSwapMode_803A6890, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(0x803A68CCu, GX__SetTevSwapModeTable_803A68CC, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(0x803A6A0Cu, GX__SetTevOrder_803A6A0C, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x803A6B68u, GX__SetNumTevStages_803A6B68, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(0x803A6EDCu, GX__SetBlendMode_803A6EDC, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(0x803A761Cu, GX__LoadPosMtxImm_803A761C, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(0x803A76C4u, GX__SetCurrentMtx_803A76C4, cpu->gpr[3]);

#undef MKW_KNOWN_TYPED_NATIVE

#define MKW_KNOWN_NATIVE_CPU_CALL(addr, fn) \
template <> struct KnownNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static constexpr uint32_t kNonvolatileFprWriteMask = 0; \
    static constexpr void (*Entry)(CpuContext*) = &fn; \
}

// MSC-UNMAPPED(nw4r::lyt::detail::DrawQuad) MKW_KNOWN_NATIVE_CPU_CALL(0x800847C0u, nw4r__lyt__detail__DrawQuad_800847c0);
// MSC-UNMAPPED(nw4r::lyt::detail::DrawQuad) MKW_KNOWN_NATIVE_CPU_CALL(0x80084D20u, nw4r__lyt__detail__DrawQuad_80084d20);

#undef MKW_KNOWN_NATIVE_CPU_CALL
