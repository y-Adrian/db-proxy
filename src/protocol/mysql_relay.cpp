#include "protocol/mysql_relay.h"
#include "pool/backend_connection.h"
#include "network/tcp_connection.h"
#include "core/logger.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace dbproxy {

namespace {

constexpr size_t kHeader = 4;

bool appendRecv(BackendConnection& backend, std::vector<char>& acc,
                std::chrono::milliseconds timeout) {
    char buf[64 * 1024];
    ssize_t n = backend.recvRaw(buf, sizeof(buf), timeout);
    if (n <= 0) {
        return false;
    }
    acc.insert(acc.end(), buf, buf + n);
    return true;
}

bool readOneMysqlPacket(BackendConnection& backend, std::vector<char>& acc,
                        std::vector<char>& out, std::chrono::milliseconds timeout) {
    while (acc.size() < kHeader) {
        if (!appendRecv(backend, acc, timeout)) {
            return false;
        }
    }
    const size_t payload_len =
        static_cast<size_t>(static_cast<unsigned char>(acc[0])) |
        (static_cast<size_t>(static_cast<unsigned char>(acc[1])) << 8) |
        (static_cast<size_t>(static_cast<unsigned char>(acc[2])) << 16);
    if (payload_len == 0xFFFFFF) {
        LOG_ERROR("MySQL packet length 0xFFFFFF (multi-chunk) not supported");
        return false;
    }
    const size_t need = kHeader + payload_len;
    while (acc.size() < need) {
        if (!appendRecv(backend, acc, timeout)) {
            return false;
        }
    }
    out.assign(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(need));
    acc.erase(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(need));
    return true;
}

bool isEofMarkerPacket(const char* payload, size_t plen) {
    return plen > 0 && static_cast<unsigned char>(payload[0]) == 0xfe && plen < 9;
}

std::optional<uint64_t> readLenEncInt(const char* data, size_t len, size_t& offset) {
    if (offset >= len) {
        return std::nullopt;
    }
    const auto first = static_cast<unsigned char>(data[offset++]);
    if (first < 0xfb) {
        return first;
    }
    if (first == 0xfc) {
        if (offset + 2 > len) {
            return std::nullopt;
        }
        uint64_t v = static_cast<unsigned char>(data[offset]) |
                     (static_cast<uint64_t>(static_cast<unsigned char>(data[offset + 1])) << 8);
        offset += 2;
        return v;
    }
    if (first == 0xfd) {
        if (offset + 3 > len) {
            return std::nullopt;
        }
        uint64_t v = static_cast<unsigned char>(data[offset]) |
                     (static_cast<uint64_t>(static_cast<unsigned char>(data[offset + 1])) << 8) |
                     (static_cast<uint64_t>(static_cast<unsigned char>(data[offset + 2])) << 16);
        offset += 3;
        return v;
    }
    if (first == 0xfe) {
        if (offset + 8 > len) {
            return std::nullopt;
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(static_cast<unsigned char>(data[offset++])) << (i * 8);
        }
        return v;
    }
    return std::nullopt;
}

/** MySQL 8+ CLIENT_DEPRECATE_EOF：行后的结束包为 OK 形态，而非 0xfe EOF。 */
bool looksMysqlOkPacket(const char* payload, size_t plen) {
    if (plen < 5 || static_cast<unsigned char>(payload[0]) != 0x00) {
        return false;
    }
    size_t off = 1;
    if (!readLenEncInt(payload, plen, off).has_value()) {
        return false;
    }
    if (!readLenEncInt(payload, plen, off).has_value()) {
        return false;
    }
    return off + 2 <= plen;
}

bool forwardToClient(TcpConnection& client, const std::vector<char>& pkt) {
    return client.sendInLoop(pkt.data(), pkt.size());
}

/**
 * LOCAL INFILE 等复杂子协议：在已转发若干包后，用短空闲超时把剩余字节刷完。
 */
bool relayRemainderWithIdleTimeout(BackendConnection& backend, TcpConnection& client,
                                   std::vector<char>& acc,
                                   std::chrono::milliseconds idle_timeout) {
    if (!acc.empty()) {
        if (!forwardToClient(client, acc)) {
            return false;
        }
        acc.clear();
    }
    std::vector<char> buf(64 * 1024);
    for (;;) {
        ssize_t n = backend.recvRaw(buf.data(), buf.size(), idle_timeout);
        if (n <= 0) {
            break;
        }
        if (!client.sendInLoop(buf.data(), static_cast<size_t>(n))) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool relayMysqlServerResponse(BackendConnection& backend, TcpConnection& client,
                              std::chrono::milliseconds first_packet_timeout,
                              std::chrono::milliseconds packet_io_timeout) {
    std::vector<char> acc;

    enum class Phase { First, ColumnDefs, AfterColumnDefs, Rows };
    Phase phase = Phase::First;
    uint64_t columns_remaining = 0;

    for (;;) {
        std::vector<char> pkt;
        const auto timeout = (phase == Phase::First) ? first_packet_timeout : packet_io_timeout;
        if (!readOneMysqlPacket(backend, acc, pkt, timeout)) {
            return phase != Phase::First;
        }
        if (!forwardToClient(client, pkt)) {
            return false;
        }

        if (pkt.size() < kHeader + 1) {
            LOG_WARN("MySQL relay: empty payload packet");
            return true;
        }
        const char* payload = pkt.data() + kHeader;
        const size_t plen = pkt.size() - kHeader;

        if (phase == Phase::First) {
            const auto header = static_cast<unsigned char>(payload[0]);
            if (header == 0xff) {
                return true;
            }
            if (header == 0xfb) {
                LOG_WARN("LOCAL INFILE response: relaying remainder with idle timeout");
                return relayRemainderWithIdleTimeout(backend, client, acc,
                                                     std::chrono::milliseconds(300));
            }
            if (header == 0x00) {
                if (plen == 1) {
                    columns_remaining = 0;
                    phase = Phase::AfterColumnDefs;
                    continue;
                }
                return true;
            }

            size_t off = 0;
            auto cc = readLenEncInt(payload, plen, off);
            if (!cc || off != plen) {
                LOG_WARN("MySQL relay: could not parse column count, assuming OK/end");
                return true;
            }
            columns_remaining = *cc;
            phase = (columns_remaining > 0) ? Phase::ColumnDefs : Phase::AfterColumnDefs;
            continue;
        }

        if (phase == Phase::ColumnDefs) {
            if (columns_remaining > 0) {
                --columns_remaining;
            }
            if (columns_remaining == 0) {
                phase = Phase::AfterColumnDefs;
            }
            continue;
        }

        if (phase == Phase::AfterColumnDefs) {
            phase = Phase::Rows;
            continue;
        }

        if (phase == Phase::Rows) {
            if (isEofMarkerPacket(payload, plen) || looksMysqlOkPacket(payload, plen)) {
                return true;
            }
        }
    }
}

}  // namespace dbproxy
