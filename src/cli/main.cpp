/**
 * @file main.cpp
 * @brief dbcli - 数据库 CLI 工具入口
 * 
 * 支持交互式和批处理模式
 */

#include "cli/shell.h"
#include "cli/connection_config.h"
#include "core/logger.h"
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <fstream>
#include <sstream>

// 简化的命令行参数解析
struct CLIArgs {
    bool help = false;
    bool version = false;
    std::optional<std::string> url;
    std::string host;
    int port = 0;
    std::string user;
    std::string password;
    std::string database;
    std::string charset = "utf8mb4";
    std::optional<std::string> execute;       // -c
    std::optional<std::string> file;          // -f
    bool interactive = false;
    OutputFormat format = OutputFormat::Table;
    std::string config_file;
    
    // 连接池参数
    int min_connections = 1;
    int max_connections = 10;
};

void printVersion() {
    std::cout << "dbcli " << "1.0.0" << "\n";
    std::cout << "Database CLI tool based on db-proxy\n";
}

void printHelp(const char* program) {
    std::cout << "Usage: " << program << " [OPTIONS] [database_url]\n";
    std::cout << "\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --host HOST          Database host (default: localhost)\n";
    std::cout << "  -P, --port PORT          Database port\n";
    std::cout << "  -u, --user USER          Username\n";
    std::cout << "  -p, --password PASSWORD  Password\n";
    std::cout << "  -d, --database DB        Database name\n";
    std::cout << "  --charset CHARSET        Character set (default: utf8mb4)\n";
    std::cout << "  -c, --command CMD        Execute command and exit\n";
    std::cout << "  -f, --file FILE          Execute SQL file and exit\n";
    std::cout << "  --config FILE            Load connection config file\n";
    std::cout << "  --json                   Output in JSON format\n";
    std::cout << "  --csv                    Output in CSV format\n";
    std::cout << "  -i, --interactive        Force interactive mode\n";
    std::cout << "  --help                   Show this help\n";
    std::cout << "  --version                Show version\n";
    std::cout << "\n";
    std::cout << "Database URL format:\n";
    std::cout << "  mysql://user:pass@host:port/database\n";
    std::cout << "  postgres://user:pass@host:port/database\n";
    std::cout << "\n";
    std::cout << "Examples:\n";
    std::cout << "  " << program << " mysql://root@localhost/test\n";
    std::cout << "  " << program << " postgres://postgres@localhost:5432/mydb\n";
    std::cout << "  " << program << " -h localhost -P 3306 -u root -d test\n";
    std::cout << "  " << program << " -f query.sql\n";
    std::cout << "  " << program << " -c \"SELECT * FROM users LIMIT 10\"\n";
}

bool parseArgs(int argc, char* argv[], CLIArgs& args) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-help") {
            args.help = true;
        } else if (arg == "--version") {
            args.version = true;
        } else if (arg == "-h" || arg == "--host") {
            if (i + 1 < argc) args.host = argv[++i];
        } else if (arg == "-P" || arg == "--port") {
            if (i + 1 < argc) args.port = std::stoi(argv[++i]);
        } else if (arg == "-u" || arg == "--user" || arg == "-U") {
            if (i + 1 < argc) args.user = argv[++i];
        } else if (arg == "-p" || arg == "--password") {
            if (i + 1 < argc) args.password = argv[++i];
        } else if (arg == "-d" || arg == "--database") {
            if (i + 1 < argc) args.database = argv[++i];
        } else if (arg == "--charset") {
            if (i + 1 < argc) args.charset = argv[++i];
        } else if (arg == "-c" || arg == "--command") {
            if (i + 1 < argc) args.execute = argv[++i];
        } else if (arg == "-f" || arg == "--file") {
            if (i + 1 < argc) args.file = argv[++i];
        } else if (arg == "--config") {
            if (i + 1 < argc) args.config_file = argv[++i];
        } else if (arg == "--json") {
            args.format = OutputFormat::JSON;
        } else if (arg == "--csv") {
            args.format = OutputFormat::CSV;
        } else if (arg == "-i" || arg == "--interactive") {
            args.interactive = true;
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        } else if (arg.rfind("-", 0) != 0) {
            // 可能是 URL
            args.url = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }
    return true;
}

std::optional<dbcli::ConnectionConfig> buildConnectionConfig(const CLIArgs& args) {
    // 优先使用 URL
    if (args.url) {
        auto config = dbcli::ConnectionConfig::fromUrl(*args.url);
        if (config) {
            return config;
        }
        return std::nullopt;
    }
    
    // 使用参数构建
    if (args.host.empty() && args.user.empty() && args.database.empty()) {
        return std::nullopt;
    }
    
    dbcli::ConnectionInfo info;
    info.host = args.host.empty() ? "localhost" : args.host;
    info.port = args.port;
    info.username = args.user;
    info.password = args.password;
    info.database = args.database;
    info.charset = args.charset;
    info.min_connections = args.min_connections;
    info.max_connections = args.max_connections;
    
    // 根据端口推断类型
    dbcli::DBType type = dbcli::DBType::MySQL;
    if (info.port == 5432 || !args.user.empty() && args.user == "postgres") {
        type = dbcli::DBType::PostgreSQL;
        if (info.port == 0) info.port = 5432;
    }
    if (info.port == 0) info.port = 3306;
    
    return dbcli::ConnectionConfig(info, type);
}

bool executeFile(dbcli::Shell& shell, const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Cannot open file: " << filename << "\n";
        return false;
    }
    
    std::string line;
    std::string sql;
    bool success = true;
    
    while (std::getline(file, line)) {
        // 去除注释
        size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        
        // 追加到 SQL
        if (!sql.empty()) sql += "\n";
        sql += line;
        
        // 检查是否完成
        while (!sql.empty() && std::isspace(sql.back())) sql.pop_back();
        if (sql.empty()) continue;
        
        if (sql.back() == ';') {
            auto result = shell.executeSQL(sql);
            shell.printResult(result);
            sql.clear();
            
            if (!result.success) {
                success = false;
            }
        }
    }
    
    // 处理末尾没有分号的 SQL
    if (!sql.empty()) {
        while (!sql.empty() && std::isspace(sql.back())) sql.pop_back();
        if (!sql.empty()) {
            auto result = shell.executeSQL(sql);
            shell.printResult(result);
            if (!result.success) success = false;
        }
    }
    
    return success;
}

int runInteractive(dbcli::Shell& shell) {
    shell.printWelcome();
    
    std::string line;
    std::string sql_buffer;
    
    while (true) {
        // 获取提示符
        std::string prompt = shell.getPrompt();
        
        // 显示提示符并读取一行
        std::cout << prompt << std::flush;
        
        if (!std::getline(std::cin, line)) {
            // EOF
            std::cout << "\n";
            break;
        }
        
        // 处理行
        std::string result = shell.processInputLine(line);
        
        if (result.empty()) {
            continue;  // 继续输入
        }
        
        if (result == "quit" || result == "\\q") {
            break;
        }
        
        // 解析是否为元命令
        auto parsed = dbcli::Shell::parseLine(result);
        
        if (std::holds_alternative<dbcli::MetaCommand>(parsed)) {
            dbcli::MetaCommand cmd = std::get<dbcli::MetaCommand>(parsed);
            
            if (cmd == dbcli::MetaCommand::Exit) {
                break;
            }
            
            // 从缓冲区提取参数
            std::string trimmed = result;
            while (!trimmed.empty() && std::isspace(trimmed.front())) {
                trimmed.erase(trimmed.begin());
            }
            size_t space = trimmed.find(' ');
            std::string args_str = space != std::string::npos 
                ? trimmed.substr(space + 1) : "";
            
            std::vector<std::string> args;
            if (!args_str.empty()) {
                std::istringstream iss(args_str);
                std::string arg;
                while (iss >> arg) {
                    args.push_back(arg);
                }
            }
            
            dbcli::ParsedMetaCommand meta_cmd{cmd, args};
            if (shell.executeMetaCommand(meta_cmd)) {
                break;
            }
        } else {
            // SQL 语句
            std::string sql = std::get<std::string>(parsed);
            auto query_result = shell.executeSQL(sql);
            shell.printResult(query_result);
            shell.addToHistory(sql, query_result.success, query_result.execution_time_ms);
        }
    }
    
    return 0;
}

int main(int argc, char* argv[]) {
    CLIArgs args;
    
    if (!parseArgs(argc, argv, args)) {
        printHelp(argv[0]);
        return 1;
    }
    
    if (args.help) {
        printHelp(argv[0]);
        return 0;
    }
    
    if (args.version) {
        printVersion();
        return 0;
    }
    
    // 初始化日志
    dbproxy::Logger::instance().init("", dbproxy::LogLevel::WARN);
    
    // 构建连接配置
    auto config = buildConnectionConfig(args);
    
    if (!config) {
        // 没有连接参数，检查是否有 -f 或 -c
        if (!args.file && !args.execute) {
            // 进入交互模式，显示帮助
            std::cout << "No connection specified.\n";
            std::cout << "Use: " << argv[0] << " mysql://user:pass@host/db\n";
            std::cout << "     " << argv[0] << " -h host -u user -d db\n\n";
            std::cout << "Or use one of the following modes:\n";
            std::cout << "  -f FILE      Execute SQL file\n";
            std::cout << "  -c CMD       Execute single command\n";
            std::cout << "  -i           Interactive mode\n";
            return 1;
        }
    }
    
    // 创建 Shell
    dbcli::Shell shell;
    shell.setOutputFormat(args.format);
    
    // 连接数据库
    if (config) {
        if (!shell.connect(*config)) {
            std::cerr << "Failed to connect to database\n";
            return 1;
        }
    }
    
    // 执行模式
    int exit_code = 0;
    
    if (args.execute) {
        // 单命令模式
        auto result = shell.executeSQL(*args.execute);
        shell.printResult(result);
        exit_code = result.success ? 0 : 1;
    } else if (args.file) {
        // 文件模式
        if (!executeFile(shell, *args.file)) {
            exit_code = 1;
        }
    } else {
        // 交互模式
        exit_code = runInteractive(shell);
    }
    
    return exit_code;
}
