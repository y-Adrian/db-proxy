// SPDX-License-Identifier: MIT
// SHA-256 digest (FIPS 180-4) — 用于 MySQL caching_sha2_password 等。

#ifndef DB_PROXY_CRYPTO_SHA256_H
#define DB_PROXY_CRYPTO_SHA256_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace dbproxy::crypto {

constexpr std::size_t kSha256DigestLength = 32;

// 返回 32 字节原始摘要（非 hex）。
std::string sha256(const void* data, std::size_t len);
inline std::string sha256(const std::string& s) { return sha256(s.data(), s.size()); }

}  // namespace dbproxy::crypto

#endif  // DB_PROXY_CRYPTO_SHA256_H
