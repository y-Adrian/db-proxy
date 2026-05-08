#ifndef DB_PROXY_DIAGNOSTIC_ENGINE_H
#define DB_PROXY_DIAGNOSTIC_ENGINE_H

#include "llm_provider.h"
#include "prompt_builder.h"
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>

namespace dbproxy {
namespace diagnostics {

/**
 * @brief 诊断报告
 * 
 * 包含 LLM 分析结果和元数据
 */
struct DiagnosticReport {
    std::chrono::system_clock::time_point diagnosis_time;
    std::string severity;  // "low", "medium", "high", "critical"
    std::string root_cause;
    std::vector<std::string> bottlenecks;
    std::vector<std::string> suggestions;
    std::string raw_llm_response;
    bool success{false};
    std::string error_message;
};

/**
 * @brief 诊断引擎
 * 
 * 协调整个诊断流程：
 * 1. 收集运行时指标（通过 PromptBuilder）
 * 2. 构建结构化 Prompt
 * 3. 调用 LLM Provider
 * 4. 解析结果并生成报告
 * 
 * 面试亮点：
 * - 外观模式：封装复杂的诊断流程
 * - 策略模式：可切换不同的 LLM Provider
 * - 异步支持：不阻塞主流程（TODO）
 * - 可观测：诊断过程本身也有日志
 */
class DiagnosticEngine {
public:
    /**
     * @brief 构造函数
     * @param provider LLM 提供者（可传入 MockProvider 或真实 Provider）
     */
    explicit DiagnosticEngine(std::unique_ptr<LLMProvider> provider);

    ~DiagnosticEngine() = default;

    /**
     * @brief 执行完整诊断
     * 
     * @param language 输出语言 ("zh" / "en")
     * @return 诊断报告
     */
    DiagnosticReport runDiagnosis(const std::string& language = "zh");

    /**
     * @brief 执行快速诊断（使用简短 Prompt）
     */
    DiagnosticReport runQuickDiagnosis(const std::string& language = "zh");

    /**
     * @brief 设置 Provider
     */
    void setProvider(std::unique_ptr<LLMProvider> provider);

    /**
     * @brief 获取最后一次诊断报告
     */
    std::optional<DiagnosticReport> getLastReport() const;

    /**
     * @brief 获取诊断历史
     */
    std::vector<DiagnosticReport> getDiagnosisHistory(size_t limit = 10) const;

private:
    std::unique_ptr<LLMProvider> provider_;
    PromptBuilder prompt_builder_;
    mutable std::mutex mutex_;
    std::vector<DiagnosticReport> history_;
    static constexpr size_t MAX_HISTORY = 100;
};

}  // namespace diagnostics
}  // namespace dbproxy

#endif  // DB_PROXY_DIAGNOSTIC_ENGINE_H
