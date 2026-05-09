#ifndef DB_PROXY_MYSQL_PARSER_H
#define DB_PROXY_MYSQL_PARSER_H

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace dbproxy {

class Connection;

/**
 * @brief MySQL 协议处理器
 * 
 * 面试亮点：
 * - 协议状态机：处理 MySQL 连接握手流程
 * - SQL 解析基础：识别 SQL 类型
 * - 连接复用：代理后端连接复用
 */
class MySQLParser {
public:
    enum class State {
        HANDSHAKE,       // 等待服务端握手
        AUTH,            // 处理客户端认证
        QUERY,           // 处理查询
        RESULT,          // 返回结果
        CLOSE            // 连接关闭
    };
    
    explicit MySQLParser(std::weak_ptr<Connection> backend);
    ~MySQLParser() = default;
    
    // 解析客户端请求
    // 返回：需要转发的数据长度，0表示数据不完整
    size_t parseClientData(const char* data, size_t len);
    
    // 处理后端响应
    size_t parseServerData(size_t len);
    
    State state() const { return state_; }
    void setState(State s) { state_ = s; }
    
    // SQL 分析
    struct SQLInfo {
        enum class Type {
            UNKNOWN,
            SELECT,
            INSERT,
            UPDATE,
            DELETE,
            CREATE,
            DROP,
            ALTER,
            SET,
            SHOW,
            USE,
            BEGIN,     // 事务
            COMMIT,
            ROLLBACK
        };
        Type type;
        std::string table;
        std::string normalized_sql;  // 脱敏后的 SQL
    };
    
    static SQLInfo parseSQL(const std::string& sql);
    static bool isReadQuery(const SQLInfo& info);
    static bool isWriteQuery(const SQLInfo& info);
    
    // 统计
    uint64_t queryCount() const { return query_count_; }
    uint64_t slowQueryCount() const { return slow_query_count_; }
    
private:
    State state_{State::HANDSHAKE};
    std::weak_ptr<Connection> backend_;
    
    // 协议解析相关
    size_t handshake_len_{0};
    std::vector<char> recv_buffer_;
    
    // 统计
    uint64_t query_count_{0};
    uint64_t slow_query_count_{0};
};

}  // namespace dbproxy

#endif  // DB_PROXY_MYSQL_PARSER_H
