#ifndef DB_PROXY_LOGGER_H
#define DB_PROXY_LOGGER_H

#include <string>
#include <memory>
#include <source_location>
#include <sstream>
#include <utility>

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

namespace detail {

template <typename T>
std::string logValueToString(T&& value) {
    std::ostringstream oss;
    oss << std::forward<T>(value);
    return oss.str();
}

inline void formatInto(std::ostringstream& oss, const std::string& fmt, size_t pos) {
    oss << fmt.substr(pos);
}

template <typename T, typename... Args>
void formatInto(std::ostringstream& oss, const std::string& fmt, size_t pos,
                T&& value, Args&&... args) {
    size_t placeholder = fmt.find("{}", pos);
    if (placeholder == std::string::npos) {
        oss << fmt.substr(pos);
        oss << " " << logValueToString(std::forward<T>(value));
        ((oss << " " << logValueToString(std::forward<Args>(args))), ...);
        return;
    }

    oss << fmt.substr(pos, placeholder - pos);
    oss << logValueToString(std::forward<T>(value));
    formatInto(oss, fmt, placeholder + 2, std::forward<Args>(args)...);
}

}  // namespace detail

inline std::string formatLogMessage(const std::string& message) {
    return message;
}

inline std::string formatLogMessage(const char* message) {
    return message ? std::string(message) : std::string{};
}

template <typename... Args>
std::string formatLogMessage(const std::string& fmt, Args&&... args) {
    std::ostringstream oss;
    detail::formatInto(oss, fmt, 0, std::forward<Args>(args)...);
    return oss.str();
}

template <typename... Args>
std::string formatLogMessage(const char* fmt, Args&&... args) {
    return formatLogMessage(fmt ? std::string(fmt) : std::string{},
                            std::forward<Args>(args)...);
}

#define LOG_DEBUG(...) dbproxy::Logger::instance().debug(dbproxy::formatLogMessage(__VA_ARGS__))
#define LOG_INFO(...)  dbproxy::Logger::instance().info(dbproxy::formatLogMessage(__VA_ARGS__))
#define LOG_WARN(...)  dbproxy::Logger::instance().warn(dbproxy::formatLogMessage(__VA_ARGS__))
#define LOG_ERROR(...) dbproxy::Logger::instance().error(dbproxy::formatLogMessage(__VA_ARGS__))
#define LOG_FATAL(...) dbproxy::Logger::instance().fatal(dbproxy::formatLogMessage(__VA_ARGS__))

}  // namespace dbproxy

#endif  // DB_PROXY_LOGGER_H
