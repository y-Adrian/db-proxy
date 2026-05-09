#include "protocol/mysql_parser.h"
#include "protocol/mysql_packet.h"
#include "pool/connection.h"
#include "core/logger.h"
#include <algorithm>
#include <cstring>

namespace dbproxy {

MySQLParser::MySQLParser(std::weak_ptr<Connection> backend)
    : backend_(backend) {
}

size_t MySQLParser::parseClientData(const char* data, size_t len) {
    // 简单实现：完整转发所有数据
    // 实际应该解析 MySQL 协议包
    
    if (len == 0) {
        return 0;
    }
    
    // 检查是否是完整的 MySQL 包
    // MySQL 包头：4 字节（3 字节长度 + 1 字节序列号）
    if (len < 4) {
        return 0;
    }
    
    // 提取包长度
    size_t packet_len = static_cast<size_t>(data[0]) |
                        (static_cast<size_t>(data[1]) << 8) |
                        (static_cast<size_t>(data[2]) << 16);
    
    // 检查是否收到完整包
    if (len >= 4 + packet_len) {
        // 完整包，解析并处理
        
        // 检查命令类型
        if (packet_len > 0) {
            uint8_t cmd = static_cast<uint8_t>(data[4]);
            
            if (cmd == 0x03) {  // COM_QUERY
                std::string sql(data + 5, packet_len - 1);
                auto info = parseSQL(sql);
                
                // 更新状态
                if (state_ == State::AUTH || state_ == State::QUERY) {
                    state_ = State::QUERY;
                }
                
                query_count_++;
                
                // 记录慢查询
                // 简化：假设每个查询 0ms 延迟
                LOG_DEBUG("SQL: " + sql.substr(0, 100));
            } else if (cmd == 0x01) {  // COM_QUIT
                state_ = State::CLOSE;
                LOG_DEBUG("Client QUIT");
            } else if (cmd == 0x0e) {  // COM_PING
                LOG_DEBUG("Client PING");
            }
        }
        
        // 返回包长度（包括头）
        return 4 + packet_len;
    }
    
    return 0;  // 数据不完整
}

size_t MySQLParser::parseServerData(size_t len) {
    // 简化实现：直接转发
    return len;
}

MySQLParser::SQLInfo MySQLParser::parseSQL(const std::string& sql) {
    SQLInfo info;
    info.type = SQLInfo::Type::UNKNOWN;
    
    std::string trimmed = sql;
    // 去首尾空格
    while (!trimmed.empty() && std::isspace(trimmed.front())) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(trimmed.back())) {
        trimmed.pop_back();
    }
    
    // 转换为大写检测关键字
    std::string upper = trimmed;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    
    if (upper.compare(0, 6, "SELECT") == 0) {
        info.type = SQLInfo::Type::SELECT;
    } else if (upper.compare(0, 6, "INSERT") == 0) {
        info.type = SQLInfo::Type::INSERT;
    } else if (upper.compare(0, 6, "UPDATE") == 0) {
        info.type = SQLInfo::Type::UPDATE;
    } else if (upper.compare(0, 6, "DELETE") == 0) {
        info.type = SQLInfo::Type::DELETE;
    } else if (upper.compare(0, 6, "CREATE") == 0) {
        info.type = SQLInfo::Type::CREATE;
    } else if (upper.compare(0, 4, "DROP") == 0) {
        info.type = SQLInfo::Type::DROP;
    } else if (upper.compare(0, 5, "ALTER") == 0) {
        info.type = SQLInfo::Type::ALTER;
    } else if (upper.compare(0, 3, "SET") == 0) {
        info.type = SQLInfo::Type::SET;
    } else if (upper.compare(0, 4, "SHOW") == 0) {
        info.type = SQLInfo::Type::SHOW;
    } else if (upper.compare(0, 3, "USE") == 0) {
        info.type = SQLInfo::Type::USE;
    } else if (upper.compare(0, 5, "BEGIN") == 0) {
        info.type = SQLInfo::Type::BEGIN;
    } else if (upper.compare(0, 6, "COMMIT") == 0) {
        info.type = SQLInfo::Type::COMMIT;
    } else if (upper.compare(0, 8, "ROLLBACK") == 0) {
        info.type = SQLInfo::Type::ROLLBACK;
    }
    
    // 提取表名（简化实现）
    info.normalized_sql = trimmed;
    
    return info;
}

bool MySQLParser::isReadQuery(const SQLInfo& info) {
    return info.type == SQLInfo::Type::SELECT ||
           info.type == SQLInfo::Type::SHOW ||
           info.type == SQLInfo::Type::USE;
}

bool MySQLParser::isWriteQuery(const SQLInfo& info) {
    return info.type != SQLInfo::Type::SELECT &&
           info.type != SQLInfo::Type::SHOW &&
           info.type != SQLInfo::Type::UNKNOWN;
}

}  // namespace dbproxy
