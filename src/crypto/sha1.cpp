// SPDX-License-Identifier: MIT
//
// 这是一个最小化 SHA-1 实现（用于项目在无 OpenSSL 依赖时可编译）。
// 输出为 20 字节原始摘要（big-endian）。

#include "crypto/sha1.h"

#include <array>
#include <cstring>

namespace dbproxy::crypto {

namespace {

inline uint32_t rol(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

struct Sha1Ctx {
    uint64_t total_bits = 0;
    std::array<uint8_t, 64> buffer{};
    std::size_t buffer_len = 0;
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;
};

inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           (static_cast<uint32_t>(p[3]));
}

inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xff);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[3] = static_cast<uint8_t>(v & 0xff);
}

void process_block(Sha1Ctx& ctx, const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = read_be32(block + i * 4);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx.h0;
    uint32_t b = ctx.h1;
    uint32_t c = ctx.h2;
    uint32_t d = ctx.h3;
    uint32_t e = ctx.h4;

    for (int i = 0; i < 80; ++i) {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }

    ctx.h0 += a;
    ctx.h1 += b;
    ctx.h2 += c;
    ctx.h3 += d;
    ctx.h4 += e;
}

void update(Sha1Ctx& ctx, const uint8_t* data, std::size_t len) {
    ctx.total_bits += static_cast<uint64_t>(len) * 8;

    while (len > 0) {
        const std::size_t space = 64 - ctx.buffer_len;
        const std::size_t take = (len < space) ? len : space;
        std::memcpy(ctx.buffer.data() + ctx.buffer_len, data, take);
        ctx.buffer_len += take;
        data += take;
        len -= take;

        if (ctx.buffer_len == 64) {
            process_block(ctx, ctx.buffer.data());
            ctx.buffer_len = 0;
        }
    }
}

std::array<uint8_t, 20> finalize(Sha1Ctx& ctx) {
    // 追加 0x80
    ctx.buffer[ctx.buffer_len++] = 0x80;

    // 追加 0x00 直到剩余 8 字节用于长度
    if (ctx.buffer_len > 56) {
        while (ctx.buffer_len < 64) ctx.buffer[ctx.buffer_len++] = 0x00;
        process_block(ctx, ctx.buffer.data());
        ctx.buffer_len = 0;
    }
    while (ctx.buffer_len < 56) ctx.buffer[ctx.buffer_len++] = 0x00;

    // 追加 big-endian bit length
    uint64_t bits = ctx.total_bits;
    for (int i = 7; i >= 0; --i) {
        ctx.buffer[ctx.buffer_len++] = static_cast<uint8_t>((bits >> (i * 8)) & 0xff);
    }
    process_block(ctx, ctx.buffer.data());

    std::array<uint8_t, 20> out{};
    write_be32(out.data() + 0, ctx.h0);
    write_be32(out.data() + 4, ctx.h1);
    write_be32(out.data() + 8, ctx.h2);
    write_be32(out.data() + 12, ctx.h3);
    write_be32(out.data() + 16, ctx.h4);
    return out;
}

}  // namespace

std::string sha1(const void* data, std::size_t len) {
    Sha1Ctx ctx;
    update(ctx, static_cast<const uint8_t*>(data), len);
    auto out = finalize(ctx);
    return std::string(reinterpret_cast<const char*>(out.data()), out.size());
}

}  // namespace dbproxy::crypto
