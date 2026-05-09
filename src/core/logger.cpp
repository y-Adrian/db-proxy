#include "core/logger.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace dbproxy {

Logger& Logger::instance() {
    static Logger instance_;
    return instance_;
}

void Logger::init(const std::string& log_file, LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    min_level_ = level;
    log_file_ = log_file;

    // 确保日志目录存在
    if (!log_file.empty()) {
        auto pos = log_file.find_last_of('/');
        if (pos != std::string::npos) {
            std::string dir = log_file.substr(0, pos);
            if (!dir.empty()) {
                std::string cmd = "mkdir -p " + dir;
                system(cmd.c_str());
            }
        }
        // 打开日志文件（追加模式）
        file_stream_.open(log_file, std::ios::app);
        if (!file_stream_.is_open()) {
            std::cerr << "[Logger] Failed to open log file: " << log_file << std::endl;
        }
    }

    initialized_ = true;
}

void Logger::log(LogLevel level, const std::string& message, std::source_location loc) {
    if (!initialized_ || level < min_level_) {
        return;
    }

    std::ostringstream oss;
    oss << "[" << formatTime() << "] "
        << "[" << levelToString(level) << "] "
        << "[" << loc.file_name() << ":" << loc.line() << "] "
        << message;

    std::string output = oss.str();

    // 输出到 stdout
    std::cout << output << std::endl;

    // 输出到日志文件
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_stream_.is_open()) {
            file_stream_ << output << std::endl;
            file_stream_.flush();
        }
    }
}

std::string Logger::formatTime() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
        default: return "UNKNW";
    }
}

void Logger::debug(const std::string& msg, std::source_location loc) {
    log(LogLevel::DEBUG, msg, loc);
}

void Logger::info(const std::string& msg, std::source_location loc) {
    log(LogLevel::INFO, msg, loc);
}

void Logger::warn(const std::string& msg, std::source_location loc) {
    log(LogLevel::WARN, msg, loc);
}

void Logger::error(const std::string& msg, std::source_location loc) {
    log(LogLevel::ERROR, msg, loc);
}

void Logger::fatal(const std::string& msg, std::source_location loc) {
    log(LogLevel::FATAL, msg, loc);
}

}  // namespace dbproxy
