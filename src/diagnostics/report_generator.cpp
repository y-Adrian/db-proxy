#include "diagnostics/report_generator.h"
#include "diagnostics/diagnostic_engine.h"
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <ctime>

namespace dbproxy {
namespace diagnostics {

std::string ReportGenerator::toMarkdown(const DiagnosticReport& report) const {
    std::ostringstream oss;

    oss << "# 数据库代理诊断报告\n\n";
    oss << "** 诊断时间**: " << formatTimestamp(report.diagnosis_time) << "\n\n";

    // 严重程度
    std::string severity_color = getSeverityColor(report.severity);
    oss << "** 严重程度**: ";
    if (report.severity == "low") oss << "🟢 低";
    else if (report.severity == "medium") oss << "🟡 中";
    else if (report.severity == "high") oss << "🟠 高";
    else if (report.severity == "critical") oss << "🔴 危急";
    oss << "\n\n";

    // 根因分析
    oss << "## 根因分析\n\n";
    oss << report.root_cause << "\n\n";

    // 瓶颈识别
    if (!report.bottlenecks.empty()) {
        oss << "## 识别的瓶颈\n\n";
        for (size_t i = 0; i < report.bottlenecks.size(); ++i) {
            oss << (i + 1) << ". " << report.bottlenecks[i] << "\n";
        }
        oss << "\n";
    }

    // 优化建议
    if (!report.suggestions.empty()) {
        oss << "## 优化建议\n\n";
        for (size_t i = 0; i < report.suggestions.size(); ++i) {
            oss << (i + 1) << ". " << report.suggestions[i] << "\n";
        }
        oss << "\n";
    }

    // LLM 原始响应（可选）
    if (!report.raw_llm_response.empty()) {
        oss << "---\n\n";
        oss << "## LLM 原始响应\n\n";
        oss << "```\n" << report.raw_llm_response << "\n```\n";
    }

    return oss.str();
}

std::string ReportGenerator::toJSON(const DiagnosticReport& report) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6);

    oss << "{\n";
    oss << "  \"diagnosis_time\": \"" << formatTimestamp(report.diagnosis_time) << "\",\n";
    oss << "  \"severity\": \"" << report.severity << "\",\n";
    oss << "  \"success\": " << (report.success ? "true" : "false") << ",\n";

    if (!report.success) {
        oss << "  \"error_message\": \"" << report.error_message << "\"\n";
    } else {
        oss << "  \"root_cause\": \"" << report.root_cause << "\",\n";

        oss << "  \"bottlenecks\": [";
        for (size_t i = 0; i < report.bottlenecks.size(); ++i) {
            oss << "\"" << report.bottlenecks[i] << "\"";
            if (i < report.bottlenecks.size() - 1) oss << ", ";
        }
        oss << "],\n";

        oss << "  \"suggestions\": [";
        for (size_t i = 0; i < report.suggestions.size(); ++i) {
            oss << "\"" << report.suggestions[i] << "\"";
            if (i < report.suggestions.size() - 1) oss << ", ";
        }
        oss << "]\n";
    }

    oss << "}\n";
    return oss.str();
}

std::string ReportGenerator::toConsole(const DiagnosticReport& report) const {
    std::ostringstream oss;

    // 彩色输出（ANSI 转义码）
    std::string color = getSeverityColor(report.severity);
    std::string reset = "\033[0m";
    std::string bold = "\033[1m";

    oss << "\n" << color << bold << "═══════════════════════════════════════════════════════════════════════════════\n";
    oss << "  数据库代理诊断报告\n";
    oss << "═══════════════════════════════════════════════════════════════════════════════\n" << reset;

    oss << "\n时间: " << formatTimestamp(report.diagnosis_time) << "\n";
    oss << "严重程度: ";
    if (report.severity == "low") oss << color << "● 低" << reset;
    else if (report.severity == "medium") oss << color << "● 中" << reset;
    else if (report.severity == "high") oss << color << "● 高" << reset;
    else if (report.severity == "critical") oss << color << "● 危急" << reset;
    oss << "\n\n";

    oss << bold << "【根因分析】\n" << reset;
    oss << report.root_cause << "\n\n";

    if (!report.bottlenecks.empty()) {
        oss << bold << "【识别的瓶颈】\n" << reset;
        for (size_t i = 0; i < report.bottlenecks.size(); ++i) {
            oss << "  " << (i + 1) << ". " << report.bottlenecks[i] << "\n";
        }
        oss << "\n";
    }

    if (!report.suggestions.empty()) {
        oss << bold << "【优化建议】\n" << reset;
        for (size_t i = 0; i < report.suggestions.size(); ++i) {
            oss << "  " << (i + 1) << ". " << report.suggestions[i] << "\n";
        }
        oss << "\n";
    }

    oss << color << "─────────────────────────────────────────────────────────────────────\n" << reset;

    return oss.str();
}

std::string ReportGenerator::saveToFile(const DiagnosticReport& report,
                                         const std::string& format) const {
    // 确保目录存在
    std::string dir = getReportDir();
    if (!std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
    }

    // 生成文件名
    auto now = std::chrono::system_clock::to_time_t(report.diagnosis_time);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif

    char time_str[64];
    std::strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &tm);

    std::string ext = (format == "json") ? "json" : "md";
    std::string filename = dir + "/diagnosis_" + time_str + "." + ext;

    std::string content;
    if (format == "json") {
        content = toJSON(report);
    } else {
        content = toMarkdown(report);
    }

    std::ofstream file(filename);
    if (file.is_open()) {
        file << content;
        file.close();
    }

    return filename;
}

std::string ReportGenerator::getReportDir() {
    return (std::filesystem::current_path() / "diagnostics" / "reports").string();
}

std::string ReportGenerator::getSeverityColor(const std::string& severity) const {
    if (severity == "low") return "\033[32m";       // 绿色
    if (severity == "medium") return "\033[33m";     // 黄色
    if (severity == "high") return "\033[31m";       // 红色
    if (severity == "critical") return "\033[35m";    // 紫色
    return "\033[0m";
}

std::string ReportGenerator::formatTimestamp(
    const std::chrono::system_clock::time_point& tp) const {
    auto now = std::chrono::system_clock::to_time_t(tp);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif

    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string(buf);
}

}  // namespace diagnostics
}  // namespace dbproxy
