#include "protocol/mysql_packet.h"
#include "core/logger.h"
#include <cstring>

namespace dbproxy {

bool MySQLPacket::parse(const char* data, size_t len) {
    if (len < HEADER_SIZE) {
        return false;
    }
    
    // 解析包头
    // MySQL 包头：3 字节长度 + 1 字节序列号
    payload_len_ = static_cast<size_t>(data[0]) | 
                   (static_cast<size_t>(data[1]) << 8) | 
                   (static_cast<size_t>(data[2]) << 16);
    seq_id_ = static_cast<uint8_t>(data[3]);
    
    // 检查数据完整性
    if (len < HEADER_SIZE + payload_len_) {
        return false;
    }
    
    // 跳过包头
    const char* payload = data + HEADER_SIZE;

    // 解析包类型
    if (payload_len_ == 0) {
        // 空包
        packet_ = std::monostate{};
        return true;
    }
    
    uint8_t packet_type = static_cast<uint8_t>(payload[0]);
    
    switch (packet_type) {
        case 0x00:  // OK Packet
        case 0xff:  // Error Packet
            // 简化处理
            packet_ = std::monostate{};
            break;
            
        case 0xfe:  // EOF Packet
            packet_ = std::monostate{};
            break;
            
        default:
            // 其他类型（握手包、认证包等）
            packet_ = std::monostate{};
            break;
    }
    
    return true;
}

std::vector<char> MySQLPacket::serialize() const {
    std::vector<char> result;
    
    // 包头：3 字节长度 + 1 字节序列号
    result.resize(HEADER_SIZE);
    result[0] = static_cast<char>(payload_len_ & 0xff);
    result[1] = static_cast<char>((payload_len_ >> 8) & 0xff);
    result[2] = static_cast<char>((payload_len_ >> 16) & 0xff);
    result[3] = static_cast<char>(seq_id_);
    
    return result;
}

std::optional<uint64_t> MySQLPacket::readLenEncInt(const char* data, size_t len, size_t& offset) {
    if (offset >= len) {
        return std::nullopt;
    }
    
    uint8_t first = static_cast<uint8_t>(data[offset++]);
    
    if (first < 0xfb) {
        // 1 字节
        return first;
    } else if (first == 0xfc) {
        // 2 字节
        if (offset + 2 > len) return std::nullopt;
        uint64_t val = static_cast<uint8_t>(data[offset]) |
                      (static_cast<uint8_t>(data[offset + 1]) << 8);
        offset += 2;
        return val;
    } else if (first == 0xfd) {
        // 3 字节
        if (offset + 3 > len) return std::nullopt;
        uint64_t val = static_cast<uint8_t>(data[offset]) |
                      (static_cast<uint8_t>(data[offset + 1]) << 8) |
                      (static_cast<uint8_t>(data[offset + 2]) << 16);
        offset += 3;
        return val;
    } else if (first == 0xfe) {
        // 8 字节
        if (offset + 8 > len) return std::nullopt;
        uint64_t val = 0;
        for (int i = 0; i < 8; ++i) {
            val |= (static_cast<uint64_t>(static_cast<uint8_t>(data[offset++]))) << (i * 8);
        }
        return val;
    }
    
    return std::nullopt;
}

std::optional<size_t> MySQLPacket::readLenEncString(const char* data, size_t len, 
                                                     size_t& offset, std::string& out) {
    auto length = readLenEncInt(data, len, offset);
    if (!length || offset + *length > len) {
        return std::nullopt;
    }
    
    out.assign(data + offset, data + offset + *length);
    offset += *length;
    return *length;
}

}  // namespace dbproxy
