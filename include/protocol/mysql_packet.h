#ifndef DB_PROXY_MYSQL_PACKET_H
#define DB_PROXY_MYSQL_PACKET_H

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <array>
#include <variant>

namespace dbproxy {

/**
 * @brief MySQL 协议包解析
 * 
 * 面试亮点：
 * - 了解 MySQL 协议结构：Packet Header + Payload
 * - 二进制协议解析：长度编码整数(LENENC)
 * - 协议版本兼容性处理
 */
class MySQLPacket {
public:
    // MySQL 包头：3字节长度 + 1字节序列号
    static constexpr size_t HEADER_SIZE = 4;
    
    // 握手初始化包
    struct HandshakePacket {
        uint8_t protocol_version;
        std::string server_version;
        uint32_t connection_id;
        std::string auth_plugin_data;
        uint16_t server_capabilities;
        uint8_t server_charset;
        uint16_t server_status;
    };
    
    // 客户端认证包
    struct AuthPacket {
        uint32_t client_capabilities;
        uint32_t max_packet_size;
        uint8_t charset;
        std::array<char, 23> reserved;
        std::string username;
        std::string auth_response;
        std::string database;
    };
    
    // COM_QUIT 命令
    struct QuitPacket {};
    
    // COM_QUERY 命令 - 最常用
    struct QueryPacket {
        std::string query;
    };
    
    // COM_PING 命令
    struct PingPacket {};
    
    // 使用联合体存储不同类型的包
    using PacketType = std::variant<
        std::monostate,
        HandshakePacket,
        AuthPacket,
        QuitPacket,
        QueryPacket,
        PingPacket
    >;
    
    MySQLPacket() = default;
    
    // 解析包
    bool parse(const char* data, size_t len);
    
    // 序列化包
    std::vector<char> serialize() const;
    
    uint8_t sequenceId() const { return seq_id_; }
    void setSequenceId(uint8_t id) { seq_id_ = id; }
    size_t packetLength() const { return payload_len_; }
    const PacketType& packet() const { return packet_; }
    
private:
    bool parseHandshake(const char* data, size_t len, size_t& offset);
    bool parseAuth(const char* data, size_t len, size_t& offset);
    bool parseCommand(const char* data, size_t len, size_t& offset);
    
    // 读取 LENENC 整数（MySQL 特色）
    static std::optional<uint64_t> readLenEncInt(const char* data, size_t len, size_t& offset);
    static std::optional<size_t> readLenEncString(const char* data, size_t len, size_t& offset, std::string& out);
    
    uint8_t seq_id_{0};
    size_t payload_len_{0};
    PacketType packet_;
};

}  // namespace dbproxy

#endif  // DB_PROXY_MYSQL_PACKET_H
