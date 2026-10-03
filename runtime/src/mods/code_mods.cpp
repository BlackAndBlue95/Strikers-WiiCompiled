// Code mods: Kamek modules that Riivolution packs put at the disc root as sml_<NN>_<pack>.bin, which
// build.sh translates into the game ahead of time (translator link-code-mods, then translate-mod with
// the project's code-mods profile). The generated support file copies their image in at boot and
// registers a pre-startup initializer that applies their patches and runs their constructors; this
// runs it where the Strikers Mod Loader starts the same modules on a console: nlInit's call to
// glplatPreStartup, with the heap, OS and DVD up and none of the game's own systems yet.
//
// A module's hooks are compiled into the game, so installing, updating or removing a code mod takes a
// rebuild. Startup compares the installed packs with what was built in and says so in console.log.

#include "mods/code_mods.h"

#include "hle_stubs.h"
#include "hle/storage/riivolution.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

extern "C" void func_80369538(CpuContext* ctx); // glplatPreStartup

namespace {

namespace fs = std::filesystem;

// SHA-256 (FIPS 180-4), to tell whether an installed module is the one the build translated.
class Sha256 {
public:
    void Update(const uint8_t* data, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            block_[used_++] = data[i];
            if (used_ == 64) {
                Compress();
                used_ = 0;
            }
        }
        bits_ += static_cast<uint64_t>(length) * 8;
    }

    std::string HexDigest() {
        const uint64_t bits = bits_;
        const uint8_t one = 0x80;
        Update(&one, 1);
        const uint8_t zero = 0;
        while (used_ != 56) {
            Update(&zero, 1);
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            const uint8_t byte = static_cast<uint8_t>(bits >> shift);
            Update(&byte, 1);
        }
        std::string hex;
        char digits[9];
        for (uint32_t word : state_) {
            std::snprintf(digits, sizeof(digits), "%08x", word);
            hex += digits;
        }
        return hex;
    }

private:
    static uint32_t Rotr(uint32_t value, int count) { return (value >> count) | (value << (32 - count)); }

    void Compress() {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t{block_[i * 4]} << 24) | (uint32_t{block_[i * 4 + 1]} << 16) |
                   (uint32_t{block_[i * 4 + 2]} << 8) | uint32_t{block_[i * 4 + 3]};
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<uint8_t, 64> block_{};
    size_t used_ = 0;
    uint64_t bits_ = 0;
};

std::string FileSha256(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        hash.Update(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(stream.gcount()));
    }
    return hash.HexDigest();
}

std::string Lower(std::string_view text) {
    std::string lower(text);
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower;
}

// The code mods the installed packs put on the disc, by lower-case name: within an overlay root the
// later mapping wins, across roots the first (highest-priority) root.
std::map<std::string, fs::path> InstalledCodeModules() {
    std::map<std::string, fs::path> installed;
    for (const auto& overlay : RuntimeRiivolution::Overlays()) {
        if (!overlay.patches) {
            continue;
        }
        std::map<std::string, fs::path> fromRoot;
        for (const auto& mapping : overlay.patches->mappings) {
            std::string name;
            if (mapping.kind == RuntimeRiivolution::Mapping::Kind::File &&
                RuntimeRiivolution::IsCodeModuleDiscPath(mapping.discPath, name)) {
                fromRoot[name] = mapping.hostPath;
            }
        }
        for (auto& [name, path] : fromRoot) {
            installed.emplace(name, std::move(path));
        }
    }
    return installed;
}

std::vector<CodeMods::Module> CompareCodeMods() {
    std::vector<CodeMods::Module> status;
    auto installed = InstalledCodeModules();
    for (const auto& module : RecompMod::CodeModules()) {
        CodeMods::Module entry{Lower(module.name), CodeMods::Module::State::BuiltIn, {}};
        const auto found = installed.find(entry.name);
        if (found == installed.end()) {
            entry.state = CodeMods::Module::State::NotInstalled;
        } else {
            entry.path = RuntimeConfigFile::PathToUtf8(found->second);
            if (FileSha256(found->second) != module.sha256) {
                entry.state = CodeMods::Module::State::Changed;
            }
            installed.erase(found);
        }
        status.push_back(std::move(entry));
    }
    for (const auto& [name, path] : installed) {
        status.push_back({name, CodeMods::Module::State::NotBuiltIn, RuntimeConfigFile::PathToUtf8(path)});
    }
    return status;
}

const std::vector<CodeMods::Module>& StatusOnce() {
    static const std::vector<CodeMods::Module> status = CompareCodeMods();
    return status;
}

void ReportCodeMods() {
    const auto& built = RecompMod::CodeModules();
    const auto& status = StatusOnce();
    if (status.empty()) {
        return;
    }

    RT_LOG(RT_TAG_MODS) << "code mods built into this game: " << built.size() << std::endl;
    for (const auto& module : built) {
        char where[48];
        std::snprintf(where, sizeof(where), "0x%08X, 0x%X bytes", module.base, module.codeSize + module.bssSize);
        RT_LOG(RT_TAG_MODS) << "  " << module.name << " (" << where << ")" << std::endl;
    }
    for (const auto& module : status) {
        switch (module.state) {
        case CodeMods::Module::State::BuiltIn:
            break;
        case CodeMods::Module::State::NotInstalled:
            RT_LOG(RT_TAG_MODS) << "  WARNING: " << module.name
                                << ": no installed pack puts it on the disc any more (removed, or its option turned "
                                   "off); it stays in the game until build.sh runs again"
                                << std::endl;
            break;
        case CodeMods::Module::State::Changed:
            RT_LOG(RT_TAG_MODS) << "  WARNING: " << module.name << ": " << module.path
                                << " changed since the build; the game runs the version it was built with until "
                                   "build.sh runs again"
                                << std::endl;
            break;
        case CodeMods::Module::State::NotBuiltIn:
            RT_LOG(RT_TAG_MODS) << "  WARNING: " << module.name << " (" << module.path
                                << ") is installed but not built in; run build.sh to add it" << std::endl;
            break;
        }
    }
    if (CodeMods::RebuildNeeded()) {
        RT_LOG(RT_TAG_MODS) << "code mods changed since this game was built: run build.sh again" << std::endl;
    }
}

void StartCodeMods(CpuContext* ctx) {
    ReportCodeMods();
    RecompMod::RunPreStartupInitializers(ctx);
    func_80369538(ctx);
}

} // namespace

namespace CodeMods {

const std::vector<Module>& Status() {
    return StatusOnce();
}

bool RebuildNeeded() {
    for (const auto& module : StatusOnce()) {
        if (module.state != Module::State::BuiltIn) {
            return true;
        }
    }
    return false;
}

} // namespace CodeMods

PPC_NATIVE_WRAP(80369538, StartCodeMods);
