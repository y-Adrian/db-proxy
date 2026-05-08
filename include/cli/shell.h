/**
 * @file shell.h
 * @brief 交互式数据库 Shell
 */

#pragma once

#include "cli/connection_config.h"
#include "pool/connection_pool.h"
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <variant>

namespace dbcli {

// 查询结果
struct QueryResult {
    struct Column {
        std::string name;
        std::string type;
    };
    
    struct Row {
        std::vector<std::string> values;
    };
    
    bool success = false;
    std::string error_message;
    std::vector<Column> columns;
    std::vector<Row> rows;
    int affected_rows = 0;
    long long execution_time_ms = 0;
    
    bool hasResult() const { return !columns.empty(); }
    size_t rowCount() const { return rows.size(); }
};

// 输出格式
enum class OutputFormat {
    Table,
    JSON,
    CSV
};

// 元命令类型
enum class MetaCommand {
    None,
    Help,
    ListDatabases,
    ListTables,
    DescribeTable,
    ListUsers,
    Connect,
    ConnectionInfo,
    SetFormat,
    History,
    Exit,
    Clear,
    ShowIndexes,
    ShowVariables
};

// 元命令解析结果
struct ParsedMetaCommand {
    MetaCommand type;
    std::vector<std::string> args;
};

// SQL 历史记录条目
struct HistoryEntry {
    std::string sql;
    bool success;
    long long execution_time_ms;
};

// Shell 类
class Shell {
public:
    Shell();
    ~Shell();
    
    // 连接数据库
    bool connect(const ConnectionConfig& config);
    
    // 断开连接
    void disconnect();
    
    // 是否已连接
    bool isConnected() const { return pool_ != nullptr; }
    
    // 获取当前连接信息
    const ConnectionConfig* getCurrentConnection() const { return config_.get(); }
    
    // 执行 SQL
    QueryResult executeSQL(const std::string& sql);
    
    // 执行元命令
    bool executeMetaCommand(const ParsedMetaCommand& cmd);
    
    // 解析输入行
    // 返回: 空字符串=继续输入, "quit"=退出, 其他=完整SQL/元命令
    std::string processInputLine(const std::string& line);
    
    // 解析 SQL (检测是否为元命令)
    static std::variant<MetaCommand, std::string> parseLine(const std::string& line);
    
    // 格式化输出
    void setOutputFormat(OutputFormat format) { output_format_ = format; }
    OutputFormat getOutputFormat() const { return output_format_; }
    
    // 输出结果
    void printResult(const QueryResult& result);
    
    // 添加到历史
    void addToHistory(const std::string& sql, bool success, long long time_ms);
    
    // 获取历史
    const std::vector<HistoryEntry>& getHistory() const { return history_; }
    
    // 打印欢迎信息
    void printWelcome();
    
    // 打印提示符
    std::string getPrompt() const;
    
    // SQL 帮助
    static void printSQLHelp();
    
private:
    std::shared_ptr<ConnectionConfig> config_;
    std::shared_ptr<dbproxy::ConnectionPool> pool_;
    
    OutputFormat output_format_ = OutputFormat::Table;
    
    // SQL 缓冲区 (多行输入)
    std::string sql_buffer_;
    std::string last_meta_command_;
    
    // 历史记录
    std::vector<HistoryEntry> history_;
    static constexpr size_t MAX_HISTORY = 1000;
    
    // 辅助方法
    QueryResult executeMySQLQuery(const std::string& sql);
    QueryResult executePostgreSQLQuery(const std::string& sql);
    
    void printTable(const QueryResult& result);
    void printJSON(const QueryResult& result);
    void printCSV(const QueryResult& result);
    
    std::string escapeJSON(const std::string& s);
    std::string escapeCSV(const std::string& s);
    
    bool listMySQLDatabases();
    bool listMySQLTables(const std::string& pattern);
    bool describeMySQLTable(const std::string& table);
    bool listPostgreSQLDatabases();
    bool listPostgreSQLTables(const std::string& pattern);
    bool describePostgreSQLTable(const std::string& table);
};

} // namespace dbcli
