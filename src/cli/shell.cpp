/**
 * @file shell.cpp
 * @brief 交互式数据库 Shell 实现
 */

#include "cli/shell.h"
#include "core/logger.h"
#include "pool/pool_manager.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>

namespace dbcli {

Shell::Shell() = default;

Shell::~Shell() {
    disconnect();
}

bool Shell::connect(const ConnectionConfig& config) {
    // 断开现有连接
    disconnect();
    
    config_ = std::make_shared<ConnectionConfig>(config);
    const auto& info = config.getInfo();
    DBType type = config.getType();
    
    LOG_INFO("Connecting to {}:{}:{} as {}",
             info.host, info.port, info.database, info.username);
    
    try {
        auto protocol = type == DBType::MySQL
            ? dbproxy::BackendProtocol::MySQL
            : dbproxy::BackendProtocol::PostgreSQL;
        pool_ = std::make_shared<dbproxy::ConnectionPool>(
            info.host, info.port, info.username, info.password, info.database,
            info.min_connections, info.max_connections,
            std::chrono::milliseconds(info.idle_timeout_ms),
            std::chrono::milliseconds(info.connection_timeout_ms),
            protocol
        );
        
        // 预热连接池
        if (!pool_->warmup()) {
            LOG_ERROR("Failed to warm up connection pool");
            pool_.reset();
            config_.reset();
            return false;
        }
        
        LOG_INFO("Connected successfully");
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Connection failed: {}", e.what());
        pool_.reset();
        config_.reset();
        return false;
    }
}

void Shell::disconnect() {
    if (pool_) {
        pool_.reset();
        config_.reset();
        LOG_INFO("Disconnected");
    }
}

QueryResult Shell::executeSQL(const std::string& sql) {
    if (!pool_) {
        return {.success = false, .error_message = "Not connected"};
    }
    
    if (sql.empty() || sql.find_first_not_of(" \t\n\r") == std::string::npos) {
        return {.success = false, .error_message = "Empty query"};
    }
    
    auto start = std::chrono::steady_clock::now();
    
    try {
        auto conn = pool_->getConnection(std::chrono::seconds(5));
        if (!conn) {
            return {.success = false, .error_message = "Failed to get connection from pool"};
        }
        
        QueryResult result;
        result.success = conn->execute(sql);
        result.error_message = conn->lastError();
        result.affected_rows = conn->affectedRows();
        
        // 获取结果列
        const auto& columns = conn->resultColumns();
        for (const auto& col : columns) {
            result.columns.push_back({
                .name = col.name,
                .type = col.type
            });
        }
        
        // 获取结果行
        const auto& rows = conn->resultRows();
        for (const auto& row : rows) {
            QueryResult::Row r;
            for (const auto& cell : row) {
                r.values.push_back(cell);
            }
            result.rows.push_back(std::move(r));
        }
        
        pool_->returnConnection(conn);
        
        auto end = std::chrono::steady_clock::now();
        result.execution_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start).count();
        
        return result;
        
    } catch (const std::exception& e) {
        auto end = std::chrono::steady_clock::now();
        return {
            .success = false,
            .error_message = e.what(),
            .execution_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start).count()
        };
    }
}

std::variant<MetaCommand, std::string> Shell::parseLine(const std::string& line) {
    std::string trimmed = line;
    // 去除首尾空白
    while (!trimmed.empty() && std::isspace(trimmed.front())) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(trimmed.back())) {
        trimmed.pop_back();
    }
    
    if (trimmed.empty() || trimmed.front() != '\\') {
        return trimmed;
    }
    
    // 解析元命令
    std::istringstream iss(trimmed);
    std::string cmd;
    iss >> cmd;
    
    std::vector<std::string> args;
    std::string arg;
    while (iss >> arg) {
        args.push_back(arg);
    }
    
    // 单命令
    if (cmd == "\\q" || cmd == "\\quit" || cmd == "\\exit") {
        return MetaCommand::Exit;
    }
    if (cmd == "\\h" || cmd == "\\help") {
        return MetaCommand::Help;
    }
    if (cmd == "\\?" || cmd == "\\commands") {
        return MetaCommand::Help;
    }
    
    // 连接相关
    if (cmd == "\\c" || cmd == "\\connect") {
        return MetaCommand::Connect;
    }
    if (cmd == "\\conninfo") {
        return MetaCommand::ConnectionInfo;
    }
    
    // 数据库/表列表
    if (cmd == "\\l" || cmd == "\\list" || cmd == "\\ld" || cmd == "\\listdb") {
        return MetaCommand::ListDatabases;
    }
    if (cmd == "\\dt" || cmd == "\\tables") {
        return MetaCommand::ListTables;
    }
    if (cmd == "\\d" || cmd == "\\di" || cmd == "\\describe") {
        return MetaCommand::DescribeTable;
    }
    if (cmd == "\\du" || cmd == "\\users" || cmd == "\\roles") {
        return MetaCommand::ListUsers;
    }
    if (cmd == "\\di") {
        return MetaCommand::ShowIndexes;
    }
    
    // 格式设置
    if (cmd == "\\G") {
        return MetaCommand::SetFormat;  // \G 切换到 JSON
    }
    if (cmd == "\\json") {
        return MetaCommand::SetFormat;
    }
    if (cmd == "\\csv") {
        return MetaCommand::SetFormat;
    }
    if (cmd == "\\table" || cmd == "\\tabular") {
        return MetaCommand::SetFormat;
    }
    
    // 其他
    if (cmd == "\\history" || cmd == "\\hist") {
        return MetaCommand::History;
    }
    if (cmd == "\\clear" || cmd == "\\cls") {
        return MetaCommand::Clear;
    }
    
    return trimmed;  // 未知命令当作普通 SQL
}

std::string Shell::processInputLine(const std::string& line) {
    // 检查是否以分号结束或包含 \G
    std::string trimmed = line;
    while (!trimmed.empty() && std::isspace(trimmed.back())) {
        trimmed.pop_back();
    }
    
    if (trimmed.empty()) {
        return "";
    }
    
    // 检查 \G (JSON 输出模式)
    if (trimmed == "\\G") {
        output_format_ = (output_format_ == OutputFormat::JSON) 
            ? OutputFormat::Table : OutputFormat::JSON;
        std::cout << "Format set to " 
                  << (output_format_ == OutputFormat::JSON ? "JSON" : "TABLE")
                  << std::endl;
        return "";
    }
    
    // 检查 \csv
    if (trimmed == "\\csv") {
        output_format_ = OutputFormat::CSV;
        std::cout << "Format set to CSV" << std::endl;
        return "";
    }
    
    // 检查 \table
    if (trimmed == "\\table") {
        output_format_ = OutputFormat::Table;
        std::cout << "Format set to TABLE" << std::endl;
        return "";
    }
    
    // 检查 \clear
    if (trimmed == "\\clear" || trimmed == "\\cls") {
        std::cout << "\033[2J\033[H";  // ANSI 清屏
        return "";
    }
    
    // 添加到缓冲区
    if (!sql_buffer_.empty()) {
        sql_buffer_ += "\n";
    }
    sql_buffer_ += line;
    
    // 去除末尾空白后检查是否以分号结束
    std::string check = sql_buffer_;
    while (!check.empty() && std::isspace(check.back())) {
        check.pop_back();
    }
    
    if (check.back() == ';') {
        std::string result = sql_buffer_;
        sql_buffer_.clear();
        return result;
    }
    
    // 继续输入
    return "";
}

void Shell::printWelcome() {
    std::cout << "================================================\n";
    std::cout << "       dbcli - Database CLI Tool\n";
    std::cout << "       Type \\h for help, \\q to quit\n";
    std::cout << "================================================\n\n";
}

std::string Shell::getPrompt() const {
    if (!config_) {
        return "(not connected) > ";
    }
    
    std::ostringstream oss;
    oss << config_->getInfo().database;
    oss << " > ";
    return oss.str();
}

void Shell::printSQLHelp() {
    std::cout << R"(
SQL Help:
---------
Basic SQL commands are supported:
  SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, ALTER

Meta Commands:
--------------
  \h, \help       Show this help
  \q, \quit       Exit
  \c, \connect    Connect to a database
  \conninfo       Show current connection info
  \l, \list       List databases
  \dt, \tables    List tables
  \d, \describe   Describe a table
  \du, \users     List users/roles
  \G              Toggle JSON output format
  \json           Set output format to JSON
  \csv            Set output format to CSV
  \table          Set output format to table
  \history        Show command history
  \clear, \cls    Clear screen

Examples:
---------
  SELECT * FROM users WHERE id = 1;
  INSERT INTO users (name, email) VALUES ('test', 'test@example.com');
  UPDATE users SET name = 'new' WHERE id = 1;
  DELETE FROM users WHERE id = 1;

)";
}

void Shell::printResult(const QueryResult& result) {
    if (!result.success) {
        std::cerr << "Error: " << result.error_message << std::endl;
        return;
    }
    
    if (result.hasResult()) {
        switch (output_format_) {
            case OutputFormat::Table:
                printTable(result);
                break;
            case OutputFormat::JSON:
                printJSON(result);
                break;
            case OutputFormat::CSV:
                printCSV(result);
                break;
        }
    } else {
        if (result.affected_rows > 0) {
            std::cout << "Query OK, " << result.affected_rows 
                      << " row" << (result.affected_rows != 1 ? "s" : "") 
                      << " affected";
        } else {
            std::cout << "Query OK";
        }
        std::cout << " (" << result.execution_time_ms << " ms)" << std::endl;
    }
}

void Shell::printTable(const QueryResult& result) {
    if (result.columns.empty()) return;
    
    // 计算每列宽度
    std::vector<size_t> widths;
    widths.reserve(result.columns.size());
    
    for (const auto& col : result.columns) {
        widths.push_back(col.name.length());
    }
    
    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.values.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], row.values[i].length());
        }
    }
    
    // 限制最大宽度
    for (auto& w : widths) {
        w = std::min(w, static_cast<size_t>(50));
    }
    
    // 打印表头
    std::cout << "+";
    for (size_t w : widths) {
        std::cout << std::string(w + 2, '-') << "+";
    }
    std::cout << "\n|";
    
    for (size_t i = 0; i < result.columns.size(); ++i) {
        std::cout << " " << std::left << std::setw(widths[i]) 
                  << result.columns[i].name << "|";
    }
    std::cout << "\n+";
    
    for (size_t w : widths) {
        std::cout << std::string(w + 2, '-') << "+";
    }
    std::cout << "\n";
    
    // 打印数据行
    for (const auto& row : result.rows) {
        std::cout << "|";
        for (size_t i = 0; i < row.values.size() && i < widths.size(); ++i) {
            std::string val = row.values[i];
            if (val.length() > widths[i]) {
                val = val.substr(0, widths[i] - 3) + "...";
            }
            std::cout << " " << std::left << std::setw(widths[i]) << val << "|";
        }
        std::cout << "\n";
    }
    
    // 打印表尾
    std::cout << "+";
    for (size_t w : widths) {
        std::cout << std::string(w + 2, '-') << "+";
    }
    std::cout << "\n";
    
    std::cout << result.rowCount() << " row" << (result.rowCount() != 1 ? "s" : "") 
              << " in set (" << result.execution_time_ms << " ms)" << std::endl;
}

void Shell::printJSON(const QueryResult& result) {
    std::cout << "[\n";
    
    for (size_t row_idx = 0; row_idx < result.rows.size(); ++row_idx) {
        const auto& row = result.rows[row_idx];
        std::cout << "  {\n";
        
        for (size_t col_idx = 0; col_idx < result.columns.size(); ++col_idx) {
            std::string key = "\"" + escapeJSON(result.columns[col_idx].name) + "\"";
            std::string value;
            
            if (col_idx < row.values.size()) {
                value = "\"" + escapeJSON(row.values[col_idx]) + "\"";
            } else {
                value = "null";
            }
            
            std::cout << "    " << key << ": " << value;
            if (col_idx < result.columns.size() - 1) {
                std::cout << ",";
            }
            std::cout << "\n";
        }
        
        std::cout << "  }";
        if (row_idx < result.rows.size() - 1) {
            std::cout << ",";
        }
        std::cout << "\n";
    }
    
    std::cout << "]\n";
    std::cout << "(" << result.rowCount() << " rows, " 
              << result.execution_time_ms << " ms)" << std::endl;
}

void Shell::printCSV(const QueryResult& result) {
    // 打印表头
    for (size_t i = 0; i < result.columns.size(); ++i) {
        std::cout << escapeCSV(result.columns[i].name);
        if (i < result.columns.size() - 1) {
            std::cout << ",";
        }
    }
    std::cout << "\n";
    
    // 打印数据行
    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.values.size(); ++i) {
            std::cout << escapeCSV(row.values[i]);
            if (i < row.values.size() - 1) {
                std::cout << ",";
            }
        }
        std::cout << "\n";
    }
}

std::string Shell::escapeJSON(const std::string& s) {
    std::ostringstream oss;
    for (char c : s) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (c >= 0 && c < 32) {
                    oss << "\\u" << std::hex << std::setw(4) 
                        << std::setfill('0') << (int)c;
                } else {
                    oss << c;
                }
        }
    }
    return oss.str();
}

std::string Shell::escapeCSV(const std::string& s) {
    if (s.find(',') != std::string::npos ||
        s.find('"') != std::string::npos ||
        s.find('\n') != std::string::npos ||
        s.find('\r') != std::string::npos) {
        
        std::ostringstream oss;
        oss << '"';
        for (char c : s) {
            if (c == '"') {
                oss << "\"\"";
            } else {
                oss << c;
            }
        }
        oss << '"';
        return oss.str();
    }
    return s;
}

bool Shell::executeMetaCommand(const ParsedMetaCommand& cmd) {
    switch (cmd.type) {
        case MetaCommand::Help:
            printSQLHelp();
            break;
            
        case MetaCommand::Exit:
            return true;  // 返回 true 表示退出
            
        case MetaCommand::ConnectionInfo:
            if (config_) {
                const auto& info = config_->getInfo();
                std::cout << "Current connection:\n";
                std::cout << "  Type: " 
                          << (config_->getType() == DBType::MySQL ? "MySQL" : "PostgreSQL")
                          << "\n";
                std::cout << "  Host: " << info.host << ":" << info.port << "\n";
                std::cout << "  User: " << info.username << "\n";
                std::cout << "  Database: " << info.database << "\n";
            } else {
                std::cout << "Not connected\n";
            }
            break;
            
        case MetaCommand::ListDatabases:
            if (!isConnected()) {
                std::cerr << "Not connected\n";
                return false;
            }
            if (config_->getType() == DBType::MySQL) {
                return listMySQLDatabases();
            } else {
                return listPostgreSQLDatabases();
            }
            
        case MetaCommand::ListTables: {
            std::string pattern = cmd.args.empty() ? "%" : cmd.args[0];
            if (!isConnected()) {
                std::cerr << "Not connected\n";
                return false;
            }
            if (config_->getType() == DBType::MySQL) {
                return listMySQLTables(pattern);
            } else {
                return listPostgreSQLTables(pattern);
            }
        }
            
        case MetaCommand::DescribeTable: {
            if (!isConnected()) {
                std::cerr << "Not connected\n";
                return false;
            }
            std::string table = cmd.args.empty() ? "" : cmd.args[0];
            if (table.empty()) {
                std::cerr << "Usage: \\d table_name\n";
                return false;
            }
            if (config_->getType() == DBType::MySQL) {
                return describeMySQLTable(table);
            } else {
                return describePostgreSQLTable(table);
            }
        }
            
        case MetaCommand::ListUsers:
            if (!isConnected()) {
                std::cerr << "Not connected\n";
                return false;
            }
            // TODO: 实现用户列表
            std::cout << "Listing users is not implemented yet\n";
            break;
            
        case MetaCommand::SetFormat:
            if (!cmd.args.empty()) {
                std::string fmt = cmd.args[0];
                if (fmt == "json") {
                    output_format_ = OutputFormat::JSON;
                    std::cout << "Format set to JSON\n";
                } else if (fmt == "csv") {
                    output_format_ = OutputFormat::CSV;
                    std::cout << "Format set to CSV\n";
                } else if (fmt == "table") {
                    output_format_ = OutputFormat::Table;
                    std::cout << "Format set to TABLE\n";
                } else {
                    std::cerr << "Unknown format: " << fmt << "\n";
                }
            } else {
                // 切换格式
                if (output_format_ == OutputFormat::Table) {
                    output_format_ = OutputFormat::JSON;
                    std::cout << "Format set to JSON\n";
                } else {
                    output_format_ = OutputFormat::Table;
                    std::cout << "Format set to TABLE\n";
                }
            }
            break;
            
        case MetaCommand::History:
            std::cout << "Command history:\n";
            for (size_t i = 0; i < history_.size(); ++i) {
                const auto& entry = history_[i];
                std::cout << "  " << (i + 1) << ": " 
                          << (entry.success ? "[OK]" : "[FAIL]")
                          << " " << entry.execution_time_ms << "ms: "
                          << entry.sql.substr(0, 60)
                          << (entry.sql.length() > 60 ? "..." : "")
                          << "\n";
            }
            break;
            
        case MetaCommand::Clear:
            std::cout << "\033[2J\033[H";
            break;
            
        default:
            std::cerr << "Unknown command\n";
            break;
    }
    
    return false;  // 默认不退出
}

void Shell::addToHistory(const std::string& sql, bool success, long long time_ms) {
    // 去除分号和首尾空白
    std::string clean_sql = sql;
    while (!clean_sql.empty() && std::isspace(clean_sql.back())) {
        clean_sql.pop_back();
    }
    if (!clean_sql.empty() && clean_sql.back() == ';') {
        clean_sql.pop_back();
    }
    while (!clean_sql.empty() && std::isspace(clean_sql.front())) {
        clean_sql.erase(clean_sql.begin());
    }
    
    if (clean_sql.empty()) return;
    
    // 检查是否与最后一个历史记录相同
    if (!history_.empty() && history_.back().sql == clean_sql) {
        return;
    }
    
    history_.push_back({clean_sql, success, time_ms});
    
    // 限制历史记录数量
    while (history_.size() > MAX_HISTORY) {
        history_.erase(history_.begin());
    }
}

// MySQL 实现
bool Shell::listMySQLDatabases() {
    auto result = executeSQL("SHOW DATABASES");
    printResult(result);
    return true;
}

bool Shell::listMySQLTables(const std::string& pattern) {
    std::string sql = "SHOW TABLES";
    if (!pattern.empty() && pattern != "%") {
        sql += " LIKE '" + pattern + "'";
    }
    auto result = executeSQL(sql);
    printResult(result);
    return true;
}

bool Shell::describeMySQLTable(const std::string& table) {
    auto result = executeSQL("DESCRIBE " + table);
    if (!result.success) {
        // 尝试 SHOW CREATE TABLE
        result = executeSQL("SHOW CREATE TABLE " + table);
    }
    printResult(result);
    return true;
}

// PostgreSQL 实现
bool Shell::listPostgreSQLDatabases() {
    auto result = executeSQL(
        "SELECT datname AS database_name, "
        "pg_size_pretty(pg_database_size(datname)) AS size "
        "FROM pg_database "
        "WHERE datistemplate = false "
        "ORDER BY datname");
    printResult(result);
    return true;
}

bool Shell::listPostgreSQLTables(const std::string& pattern) {
    std::string sql = 
        "SELECT table_name, table_type "
        "FROM information_schema.tables "
        "WHERE table_schema = 'public'";
    if (!pattern.empty() && pattern != "%") {
        sql += " AND table_name LIKE '" + pattern + "'";
    }
    sql += " ORDER BY table_name";
    
    auto result = executeSQL(sql);
    printResult(result);
    return true;
}

bool Shell::describePostgreSQLTable(const std::string& table) {
    auto result = executeSQL(
        "SELECT column_name, data_type, is_nullable, column_default "
        "FROM information_schema.columns "
        "WHERE table_name = '" + table + "' "
        "ORDER BY ordinal_position");
    printResult(result);
    return true;
}

} // namespace dbcli
