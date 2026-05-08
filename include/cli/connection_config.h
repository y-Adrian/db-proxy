/**
 * @file connection_config.h
 * @brief 数据库连接配置
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <variant>

namespace dbcli {

// 数据库类型
enum class DBType {
    MySQL,
    PostgreSQL
};

// 连接信息
struct ConnectionInfo {
    std::string host = "localhost";
    int port = 3306;
    std::string username;
    std::string password;
    std::string database;
    std::string charset = "utf8mb4";
    
    // 连接池配置
    int min_connections = 1;
    int max_connections = 10;
    int connection_timeout_ms = 5000;
    int idle_timeout_ms = 30000;
};

// 命名连接配置 (用于连接文件)
struct NamedConnection {
    std::string name;
    DBType type;
    ConnectionInfo info;
};

// 连接配置管理器
class ConnectionConfig {
public:
    ConnectionConfig() = default;
    explicit ConnectionConfig(const ConnectionInfo& info, DBType type);
    
    // 从 URL 解析
    static std::optional<ConnectionConfig> fromUrl(const std::string& url);
    
    // 从连接别名获取
    static std::optional<ConnectionConfig> fromName(const std::string& name);
    
    // 加载配置文件
    static bool loadConfigFile(const std::string& path);
    
    // 获取所有命名连接
    static const std::vector<NamedConnection>& getNamedConnections();
    
    // 获取当前配置
    const ConnectionInfo& getInfo() const { return info_; }
    DBType getType() const { return type_; }
    
    // 获取连接字符串 (用于日志等)
    std::string toConnectionString() const;
    
    // 获取默认端口
    static int getDefaultPort(DBType type);
    
private:
    ConnectionInfo info_;
    DBType type_ = DBType::MySQL;
    
    static std::vector<NamedConnection> named_connections_;
    
    // 解析 URL
    static std::optional<ConnectionConfig> parseUrl(const std::string& url);
    
    // 解析带密码的 userinfo
    static bool parseUserInfo(const std::string& userinfo, 
                             std::string& username, 
                             std::string& password);
};

using ConnectionConfigPtr = std::shared_ptr<ConnectionConfig>;

} // namespace dbcli
