// Mario Strikers Charged (R4QE01) guest helpers shared by the game HLE (msc_game.cpp) and the mod
// framework (runtime/src/mods): calling guest functions, guest strings and allocations, the engine's
// string hashes, the character table, and FEFinder-style lookups in front-end scenes.
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include "abi_bridge.h"
#include "memory.h"

namespace MscGuest {

constexpr uint32_t kCharacterInfo = 0x80505944u;  // sCharacterInfo[33], 0x5C per row (BasicGameInfo)
constexpr uint32_t kCharacterInfoSize = 0x5C;
constexpr uint32_t kCharacterInfoRows = 33;
constexpr uint32_t kGameInfoManager = 0x806E0F54u;  // GameInfoManager singleton pointer
constexpr uint32_t kNlMalloc = 0x802AA79Cu;         // nlMalloc(size, alignment, fromEnd)
constexpr uint32_t kSetActiveSlide = 0x80301E6Cu;   // TLComponent::SetActiveSlide(ulong hash, bool, bool)

// nlStringLowerHash: h = h * 33 + lower(c), from 0xFFFFFFFF.
inline uint32_t LowerHash(const char* s, uint32_t h = 0xFFFFFFFFu) {
    for (; *s; ++s) h = h * 33 + static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(*s)));
    return h;
}
inline uint32_t LowerHash(const std::string& s) { return LowerHash(s.c_str()); }

// nlStringHash (case-sensitive; glGetTexture, sound cues, bank names).
inline uint32_t StringHash(const char* s, uint32_t h = 0xFFFFFFFFu) {
    for (; *s; ++s) h = h * 33 + static_cast<uint8_t>(*s);
    return h;
}
inline uint32_t StringHash(const std::string& s) { return StringHash(s.c_str()); }

// Calls a guest function with integer arguments in r3.., keeping the caller's LR; returns r3.
inline uint32_t Call(CpuContext* ctx, uint32_t fn, std::initializer_list<uint32_t> args) {
    const uint32_t savedLr = ctx->lr;
    uint32_t reg = 3;
    for (uint32_t arg : args) ctx->gpr[reg++] = arg;
    InvokeIndirectCpu(fn, ctx);
    ctx->lr = savedLr;
    return ctx->gpr[3];
}

// A guest heap block from nlMalloc (never freed by the game; use for data kept for the session).
inline uint32_t Alloc(CpuContext* ctx, uint32_t size, uint32_t alignment = 8) {
    return Call(ctx, kNlMalloc, {size, alignment, 0});
}

// A NUL-terminated guest string, at most `limit` characters.
inline std::string CString(uint32_t addr, uint32_t limit = 64) {
    std::string s;
    for (uint32_t i = 0; addr != 0 && i < limit; ++i) {
        const char c = static_cast<char>(Memory::Read8(addr + i));
        if (c == 0) break;
        s += c;
    }
    return s;
}

// A copy of `s` on the guest heap (see Alloc), or 0.
inline uint32_t AllocString(CpuContext* ctx, const std::string& s) {
    const uint32_t addr = Alloc(ctx, static_cast<uint32_t>(s.size() + 1), 4);
    if (addr == 0) return 0;
    for (size_t i = 0; i < s.size(); ++i) Memory::Write8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
    Memory::Write8(addr + static_cast<uint32_t>(s.size()), 0);
    return addr;
}

// GetCharacterInfo(index): captains 0-11, partners 12-19, goalies 20-31 (row 32 = none).
inline uint32_t CharacterInfo(uint32_t index) {
    return kCharacterInfo + (index < kCharacterInfoRows - 1 ? index : kCharacterInfoRows - 1) * kCharacterInfoSize;
}

// Front-end scene graph (TLInstance / TLSlide / TLComponent) and the FEFinder lookups (feFinder.cpp).
namespace FE {
constexpr uint32_t kInstanceChildren = 0x08, kInstanceComponent = 0x0C, kInstanceHash = 0x38,
                   kInstanceRotation = 0x3C + 0x0C, kInstanceOverloadFlags = 0x84, kInstanceType = 0x88,
                   kInstanceVisible = 0x8E, kImageTexture = 0x90;            // TLInstance, TLImageInstance
constexpr uint32_t kSlideChildren = 0x08, kSlideHash = 0x40;                 // TLSlide
constexpr uint32_t kComponentSlides = 0x78, kComponentActiveSlide = 0x7C;    // TLComponent

inline uint32_t ActiveSlide(uint32_t instance) {
    const uint32_t component = instance ? Memory::Read32(instance + kInstanceComponent) : 0;
    return component ? Memory::Read32(component + kComponentActiveSlide) : 0;
}

inline void SetActiveSlide(CpuContext* ctx, uint32_t instance, const char* slide, bool restart = true) {
    if (instance == 0 || Memory::Read32(instance + kInstanceComponent) == 0) return;
    Call(ctx, kSetActiveSlide, {Memory::Read32(instance + kInstanceComponent), LowerHash(slide), restart ? 1u : 0u, 0});
}

// FindItemByHashID: a ring whose list pointer is its last entry.
inline uint32_t FindItem(uint32_t list, uint32_t hash, uint32_t hashOffset) {
    if (list == 0) return 0;
    uint32_t item = Memory::Read32(list);
    for (int guard = 0; item != 0 && guard < 256; ++guard) {
        if (Memory::Read32(item + hashOffset) == hash) return item;
        if (item == list) break;
        item = Memory::Read32(item);
    }
    return 0;
}

inline uint32_t FindIn(uint32_t instance, const char* const* path, size_t count);
inline uint32_t FindInSlide(uint32_t slide, const char* const* path, size_t count) {  // FEFinder::_Find(TLSlide*, ...)
    if (slide == 0 || count == 0) return 0;
    const uint32_t child = FindItem(Memory::Read32(slide + kSlideChildren), LowerHash(path[0]), kInstanceHash);
    return child == 0 || count == 1 ? child : FindIn(child, path + 1, count - 1);
}
inline uint32_t FindIn(uint32_t instance, const char* const* path, size_t count) {  // FEFindInstanceRecursive
    if (instance == 0 || count == 0) return 0;
    if (Memory::Read32(instance + kInstanceType) == 4) {
        const uint32_t component = Memory::Read32(instance + kInstanceComponent);
        const uint32_t slide = FindItem(Memory::Read32(component + kComponentSlides), LowerHash(path[0]), kSlideHash);
        if (slide == 0) return FindInSlide(ActiveSlide(instance), path, count);
        return count == 1 ? slide : FindInSlide(slide, path + 1, count - 1);
    }
    const uint32_t child = FindItem(Memory::Read32(instance + kInstanceChildren), LowerHash(path[0]), kInstanceHash);
    return child == 0 || count == 1 ? child : FindIn(child, path + 1, count - 1);
}
inline uint32_t FindInPresentation(uint32_t presentation, const char* const* path, size_t count) {  // FEFindInstance
    if (presentation == 0 || count == 0) return 0;
    const uint32_t slide = FindItem(Memory::Read32(presentation), LowerHash(path[0]), kSlideHash);
    if (slide == 0) return FindInSlide(Memory::Read32(presentation + 0x04), path, count);
    return count == 1 ? slide : FindInSlide(slide, path + 1, count - 1);
}
template <size_t N> uint32_t FindInSlide(uint32_t slide, const char* const (&path)[N]) { return FindInSlide(slide, path, N); }
template <size_t N> uint32_t FindIn(uint32_t instance, const char* const (&path)[N]) { return FindIn(instance, path, N); }
}  // namespace FE

}  // namespace MscGuest
