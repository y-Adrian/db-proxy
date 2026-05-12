// SPDX-License-Identifier: MIT
// 最小 SHA-256 实现（FIPS 180-4），输出 32 字节 big-endian 摘要。

#include "crypto/sha256.h"

#include <array>
#include <cstring>

namespace dbproxy::crypto {

namespace {

constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint32_t bigSigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t bigSigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t smallSigma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t smallSigma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

struct Sha256Ctx {
    std::array<uint8_t, 64> buffer{};
    std::size_t buffer_len = 0;
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
};

void transform(Sha256Ctx& ctx, const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = read_be32(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        w[i] = smallSigma1(w[i - 2]) + w[i - 7] + smallSigma0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = ctx.h[0];
    uint32_t b = ctx.h[1];
    uint32_t c = ctx.h[2];
    uint32_t d = ctx.h[3];
    uint32_t e = ctx.h[4];
    uint32_t f = ctx.h[5];
    uint32_t g = ctx.h[6];
    uint32_t h = ctx.h[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + bigSigma1(e) + ch(e, f, g) + kK[i] + w[i];
        uint32_t t2 = bigSigma0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx.h[0] += a;
    ctx.h[1] += b;
    ctx.h[2] += c;
    ctx.h[3] += d;
    ctx.h[4] += e;
    ctx.h[5] += f;
    ctx.h[6] += g;
    ctx.h[7] += h;
}

void write_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xff);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[3] = static_cast<uint8_t>(v & 0xff);
}

}  // namespace

std::string sha256(const void* data, std::size_t len) {
    Sha256Ctx ctx;
    const auto* bytes = static_cast<const uint8_t*>(data);

    for (std::size_t i = 0; i < len; ++i) {
        ctx.buffer[ctx.buffer_len++] = bytes[i];
        if (ctx.buffer_len == 64) {
            transform(ctx, ctx.buffer.data());
            ctx.buffer_len = 0;
        }
    }

    const uint64_t bit_length = static_cast<uint64_t>(len) * 8ULL;
    ctx.buffer[ctx.buffer_len++] = 0x80;
    if (ctx.buffer_len > 56) {
        while (ctx.buffer_len < 64) {
            ctx.buffer[ctx.buffer_len++] = 0;
        }
        transform(ctx, ctx.buffer.data());
        ctx.buffer_len = 0;
    }
    while (ctx.buffer_len < 56) {
        ctx.buffer[ctx.buffer_len++] = 0;
    }

    for (int i = 7; i >= 0; --i) {
        ctx.buffer[56 + static_cast<std::size_t>(7 - i)] =
            static_cast<uint8_t>((bit_length >> (i * 8)) & 0xff);
    }
    transform(ctx, ctx.buffer.data());

    std::string out(kSha256DigestLength, '\0');
    for (int i = 0; i < 8; ++i) {
        write_be32(reinterpret_cast<uint8_t*>(&out[i * 4]), ctx.h[i]);
    }
    return out;
}

}  // namespace dbproxy::crypto
