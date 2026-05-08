#ifndef DB_PROXY_LOGGER_H
#define DB_PROXY_LOGGER_H

#include <string>
#include <memory>
#include <source_location>

namespace dbproxy {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL
};

class Logger {
public:
    static Logger& instance();
    
    void init(const std::string& log_file, LogLevel level);
    void log(LogLevel level, const std::string& message, 
             std::source_location loc = std::source_location::current());
    
    void debug(const std::string& msg, std::source_location loc = std::source_location::current());
    void info(const std::string& msg, std::source_location loc = std::source_location::current());
    void warn(const std::string& msg, std::source_location loc = std::source_location::current());
    void error(const std::string& msg, std::source_location loc = std::source_location::current());
    void fatal(const std::string& msg, std::source_location loc = std::source_location::current());

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    std::string formatTime();
    std::string levelToString(LogLevel level);
    
    LogLevel min_level_ = LogLevel::INFO;
    bool initialized_ = false;
};

#define LOG_DEBUG(msg) dbproxy::Logger::instance().debug(msg)
#define LOG_INFO(msg)  dbproxy::Logger::instance().info(msg)
#define LOG_WARN(msg)  dbproxy::Logger::instance().warn(msg)
#define LOG_ERROR(msg) dbproxy::Logger::instance().error(msg)
#define LOG_FATAL(msg) dbproxy::Logger::instance().fatal(msg)

}  // namespace dbproxy

#endif  // DB_PROXY_LOGGER_H
