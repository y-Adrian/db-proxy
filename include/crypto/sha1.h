// SPDX-License-Identifier: MIT
// Minimal SHA-1 implementation (public-domain/MIT-style).
// 用于在缺少 OpenSSL 依赖时，支持 MySQL mysql_native_password 的认证计算。

#ifndef DB_PROXY_CRYPTO_SHA1_H
#define DB_PROXY_CRYPTO_SHA1_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace dbproxy::crypto {

constexpr std::size_t kSha1DigestLength = 20;

// 返回 20 字节的二进制摘要（非 hex）。
std::string sha1(const void* data, std::size_t len);
inline std::string sha1(const std::string& s) { return sha1(s.data(), s.size()); }

}  // namespace dbproxy::crypto

#endif  // DB_PROXY_CRYPTO_SHA1_H
