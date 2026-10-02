// SHA-1 known-answer tests (FIPS 180-4 / RFC 3174 vectors), for the /dev/sha emulation.
#include "sha1.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string Hex(const Sha1::Digest& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (uint8_t byte : digest) {
        out += kHex[byte >> 4];
        out += kHex[byte & 0xF];
    }
    return out;
}

int g_failures = 0;

void Expect(const char* name, const Sha1& hash, const char* expected) {
    const std::string actual = Hex(hash.Snapshot());
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s: got %s, want %s\n", name, actual.c_str(), expected);
        ++g_failures;
    }
}

void Hash(Sha1& hash, const std::string& text) {
    hash.Update(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

} // namespace

int main() {
    Sha1 empty;
    Expect("empty", empty, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

    Sha1 abc;
    Hash(abc, "abc");
    Expect("abc", abc, "a9993e364706816aba3e25717850c26c9cd0d89d");

    Sha1 twoBlocks;
    Hash(twoBlocks, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    Expect("448-bit", twoBlocks, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    // A million 'a's, fed in uneven pieces, with snapshots along the way leaving the hash intact.
    Sha1 million;
    const std::vector<uint8_t> chunk(997, 'a');
    size_t fed = 0;
    while (fed < 1000000) {
        const size_t take = std::min<size_t>(chunk.size(), 1000000 - fed);
        million.Update(chunk.data(), take);
        fed += take;
        (void)million.Snapshot();
    }
    Expect("million a", million, "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    // Restart starts over.
    million.Restart();
    Hash(million, "abc");
    Expect("restart", million, "a9993e364706816aba3e25717850c26c9cd0d89d");

    if (g_failures == 0) {
        std::printf("sha1_tests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
