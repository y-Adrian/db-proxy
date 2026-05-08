/**
 * @file connection_config.cpp
 * @brief 数据库连接配置实现
 */

#include "cli/connection_config.h"
#include "core/logger.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <fstream>

namespace dbcli {

std::vector<NamedConnection> ConnectionConfig::named_connections_;

ConnectionConfig::ConnectionConfig(const ConnectionInfo& info, DBType type)
    : info_(info), type_(type) {}

int ConnectionConfig::getDefaultPort(DBType type) {
    switch (type) {
        case DBType::MySQL: return 3306;
        case DBType::PostgreSQL: return 5432;
    }
    return 3306;
}

std::string ConnectionConfig::toConnectionString() const {
    std::ostringstream oss;
    oss << (type_ == DBType::MySQL ? "mysql" : "postgres")
        << "://" << info_.username;
    if (!info_.password.empty()) {
        oss << ":****";
    }
    oss << "@" << info_.host << ":" << info_.port
        << "/" << info_.database;
    return oss.str();
}

std::optional<ConnectionConfig> ConnectionConfig::fromUrl(const std::string& url) {
    return parseUrl(url);
}

std::optional<ConnectionConfig> ConnectionConfig::fromName(const std::string& name) {
    auto it = std::find_if(named_connections_.begin(), named_connections_.end(),
        [&name](const NamedConnection& nc) {
            return nc.name == name;
        });
    
    if (it == named_connections_.end()) {
        LOG_ERROR("Unknown connection name: {}", name);
        return std::nullopt;
    }
    
    return ConnectionConfig(it->info, it->type);
}

const std::vector<NamedConnection>& ConnectionConfig::getNamedConnections() {
    return named_connections_;
}

bool ConnectionConfig::loadConfigFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_WARN("Cannot open config file: {}", path);
        return false;
    }
    
    named_connections_.clear();
    
    std::string line;
    NamedConnection* current = nullptr;
    
    while (std::getline(file, line)) {
        // 去除首尾空白
        while (!line.empty() && std::isspace(line.front())) line.erase(line.begin());
        while (!line.empty() && std::isspace(line.back())) line.pop_back();
        
        // 跳过空行和注释
        if (line.empty() || line.front() == '#') continue;
        
        // 解析节标题 [connection_name]
        if (line.front() == '[' && line.back() == ']') {
            std::string name = line.substr(1, line.size() - 2);
            named_connections_.push_back({
                .name = name,
                .type = DBType::MySQL,  // 默认 MySQL
                .info = ConnectionInfo{}
            });
            current = &named_connections_.back();
            continue;
        }
        
        if (!current) {
            // 检查全局类型设置
            if (line.find("default_type") == 0) {
                size_t eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string type = line.substr(eq + 1);
                    type.erase(0, type.find_first_not_of(" \t"));
                    if (type == "postgres" || type == "postgresql") {
                        // 设置所有后续连接的默认类型
                    }
                }
            }
            continue;
        }
        
        // 解析 key = value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        
        // 去除空白
        while (!key.empty() && std::isspace(key.back())) key.pop_back();
        while (!value.empty() && std::isspace(value.front())) value.erase(value.begin());
        while (!value.empty() && std::isspace(value.back())) value.pop_back();
        
        // 转换键为小写
        std::transform(key.begin(), key.end(), key.begin(), 
            [](unsigned char c) { return std::tolower(c); });
        
        // 设置类型
        if (key == "type") {
            if (value == "postgres" || value == "postgresql") {
                current->type = DBType::PostgreSQL;
                current->info.port = 5432;
            }
            continue;
        }
        
        // 设置连接参数
        if (key == "host") current->info.host = value;
        else if (key == "port") current->info.port = std::stoi(value);
        else if (key == "user" || key == "username") current->info.username = value;
        else if (key == "password") current->info.password = value;
        else if (key == "database" || key == "dbname") current->info.database = value;
        else if (key == "charset") current->info.charset = value;
        else if (key == "min_connections") current->info.min_connections = std::stoi(value);
        else if (key == "max_connections") current->info.max_connections = std::stoi(value);
    }
    
    LOG_INFO("Loaded {} named connections from {}", 
             named_connections_.size(), path);
    return true;
}

bool ConnectionConfig::parseUserInfo(const std::string& userinfo,
                                    std::string& username,
                                    std::string& password) {
    size_t colon = userinfo.find(':');
    if (colon == std::string::npos) {
        username = userinfo;
        password = "";
    } else {
        username = userinfo.substr(0, colon);
        password = userinfo.substr(colon + 1);
    }
    return !username.empty();
}

std::optional<ConnectionConfig> ConnectionConfig::parseUrl(const std::string& url) {
    ConnectionInfo info;
    DBType type = DBType::MySQL;
    
    // 解析协议 mysql:// 或 postgres://
    std::string::size_type colon_slash = url.find("://");
    if (colon_slash == std::string::npos) {
        LOG_ERROR("Invalid URL format: missing '://'");
        return std::nullopt;
    }
    
    std::string scheme = url.substr(0, colon_slash);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
        [](unsigned char c) { return std::tolower(c); });
    
    if (scheme == "mysql") {
        type = DBType::MySQL;
        info.port = 3306;
    } else if (scheme == "postgres" || scheme == "postgresql") {
        type = DBType::PostgreSQL;
        info.port = 5432;
    } else {
        LOG_ERROR("Unknown database type: {}", scheme);
        return std::nullopt;
    }
    
    std::string remaining = url.substr(colon_slash + 3);
    
    // 解析 userinfo (user:password@)
    std::string::size_type at_sign = remaining.rfind('@');  // 用 rfind 因为密码里可能有 @
    std::string host_port_db;
    
    if (at_sign != std::string::npos) {
        std::string userinfo = remaining.substr(0, at_sign);
        if (!parseUserInfo(userinfo, info.username, info.password)) {
            return std::nullopt;
        }
        host_port_db = remaining.substr(at_sign + 1);
    } else {
        host_port_db = remaining;
    }
    
    // 解析 host:port/database
    std::string::size_type slash = host_port_db.find('/');
    std::string host_port;
    std::string db_part;
    
    if (slash != std::string::npos) {
        host_port = host_port_db.substr(0, slash);
        db_part = host_port_db.substr(slash + 1);
        
        // 解析数据库名 (可能包含查询参数)
        std::string::size_type question = db_part.find('?');
        if (question != std::string::npos) {
            info.database = db_part.substr(0, question);
            // 可以扩展解析查询参数
        } else {
            info.database = db_part;
        }
    } else {
        host_port = host_port_db;
    }
    
    // 解析 host:port
    std::string::size_type colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        info.host = host_port.substr(0, colon);
        std::string port_str = host_port.substr(colon + 1);
        if (!port_str.empty()) {
            try {
                info.port = std::stoi(port_str);
            } catch (...) {
                LOG_WARN("Invalid port: {}, using default", port_str);
                info.port = getDefaultPort(type);
            }
        }
    } else {
        info.host = host_port;
        info.port = getDefaultPort(type);
    }
    
    return ConnectionConfig(info, type);
}

} // namespace dbcli
