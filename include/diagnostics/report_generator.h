#ifndef DB_PROXY_REPORT_GENERAT_OR_H
#define DB_PROXY_REPORT_GENERAT_OR_H

#include "diagnostic_engine.h"
#include <string>
#include <fstream>

namespace dbproxy {
namespace diagnostics {

/**
 * @brief 报告生成器
 * 
 * 将诊断结果格式化为多种输出格式：
 * - Markdown（人类可读）
 * - JSON（机器可读）
 * - Console（实时输出）
 * 
 * 面试亮点：
 * - 模板方法模式：不同输出格式的通用流程
 * - 工厂方法：根据格式选择生成器
 * - 观察者模式：诊断完成后自动触发报告生成
 */
class ReportGenerator {
public:
    ReportGenerator() = default;

    /**
     * @brief 生成 Markdown 格式报告
     */
    std::string toMarkdown(const DiagnosticReport& report) const;

    /**
     * @brief 生成 JSON 格式报告
     */
    std::string toJSON(const DiagnosticReport& report) const;

    /**
     * @brief 生成控制台输出（彩色）
     */
    std::string toConsole(const DiagnosticReport& report) const;

    /**
     * @brief 保存报告到文件
     * @return 保存的文件路径
     */
    std::string saveToFile(const DiagnosticReport& report,
                           const std::string& format = "markdown") const;

    /**
     * @brief 获取报告保存目录
     */
    static std::string getReportDir();

private:
    /**
     * @brief 获取严重程度对应的颜色（控制台输出）
     */
    std::string getSeverityColor(const std::string& severity) const;

    /**
     * @brief 格式化时间戳
     */
    std::string formatTimestamp(
        const std::chrono::system_clock::time_point& tp) const;
};

}  // namespace diagnostics
}  // namespace dbproxy

#endif  // DB_PROXY_REPORT_GENERAT_OR_H
