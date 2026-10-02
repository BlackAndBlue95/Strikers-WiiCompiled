// SHA-1 (FIPS 180-4), for the emulated IOS SHA engine (/dev/sha).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

class Sha1 {
public:
    static constexpr size_t kDigestSize = 20;
    using Digest = std::array<uint8_t, kDigestSize>;

    Sha1() { Restart(); }

    void Restart() {
        state_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
        length_ = 0;
        buffered_ = 0;
    }

    void Update(const uint8_t* data, size_t size) {
        length_ += size;
        Absorb(data, size);
    }

    // The digest of everything hashed so far; the hash itself carries on unchanged.
    Digest Snapshot() const {
        Sha1 copy = *this;
        static constexpr uint8_t kPadding[64] = {0x80};
        copy.Absorb(kPadding, copy.buffered_ < 56 ? 56 - copy.buffered_ : 120 - copy.buffered_);
        const uint64_t bits = length_ * 8u;
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) {
            length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        }
        copy.Absorb(length, sizeof(length));
        Digest digest{};
        for (size_t i = 0; i < kDigestSize; ++i) {
            digest[i] = static_cast<uint8_t>(copy.state_[i / 4] >> (24 - 8 * (i % 4)));
        }
        return digest;
    }

private:
    static uint32_t Rotl(uint32_t value, int count) { return (value << count) | (value >> (32 - count)); }

    void Absorb(const uint8_t* data, size_t size) {
        while (size != 0) {
            const size_t take = std::min(size, block_.size() - buffered_);
            std::memcpy(block_.data() + buffered_, data, take);
            buffered_ += take;
            data += take;
            size -= take;
            if (buffered_ == block_.size()) {
                Compress();
                buffered_ = 0;
            }
        }
    }

    void Compress() {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = static_cast<uint32_t>(block_[i * 4]) << 24 | static_cast<uint32_t>(block_[i * 4 + 1]) << 16 |
                   static_cast<uint32_t>(block_[i * 4 + 2]) << 8 | static_cast<uint32_t>(block_[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t t = Rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rotl(b, 30);
            b = a;
            a = t;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    std::array<uint32_t, 5> state_{};
    std::array<uint8_t, 64> block_{};
    uint64_t length_ = 0;
    size_t buffered_ = 0;
};
