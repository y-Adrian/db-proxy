#include "diagnostics/diagnostic_engine.h"
#include "diagnostics/prompt_builder.h"
#include <iostream>

namespace dbproxy {
namespace diagnostics {

DiagnosticEngine::DiagnosticEngine(std::unique_ptr<LLMProvider> provider)
    : provider_(std::move(provider)) {}

DiagnosticReport DiagnosticEngine::runDiagnosis(const std::string& language) {
    DiagnosticReport report;
    report.diagnosis_time = std::chrono::system_clock::now();

    if (!provider_ || !provider_->isAvailable()) {
        report.success = false;
        report.error_message = "LLM Provider 不可用";
        return report;
    }

    // 1. 收集上下文
    DiagnosticContext ctx = prompt_builder_.collectContext();

    // 2. 构建 Prompt
    std::string prompt = prompt_builder_.buildPrompt(ctx, language);

    // 3. 调用 LLM
    auto result = provider_->diagnose(prompt);

    if (!result.has_value()) {
        report.success = false;
        report.error_message = "LLM 调用失败";
        return report;
    }

    // 4. 解析结果
    report.root_cause = result->root_cause;
    report.bottlenecks = result->bottlenecks;
    report.suggestions = result->suggestions;
    report.severity = result->severity;
    report.raw_llm_response = result->raw_response;
    report.success = true;

    // 5. 保存到历史
    {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back(report);
        if (history_.size() > MAX_HISTORY) {
            history_.erase(history_.begin());
        }
    }

    std::cout << "[DiagnosticEngine] 诊断完成，严重程度: " << report.severity << std::endl;

    return report;
}

DiagnosticReport DiagnosticEngine::runQuickDiagnosis(const std::string& language) {
    DiagnosticReport report;
    report.diagnosis_time = std::chrono::system_clock::now();

    if (!provider_ || !provider_->isAvailable()) {
        report.success = false;
        report.error_message = "LLM Provider 不可用";
        return report;
    }

    DiagnosticContext ctx = prompt_builder_.collectContext();
    std::string prompt = prompt_builder_.buildBriefPrompt(ctx, language);

    auto result = provider_->diagnose(prompt);

    if (!result.has_value()) {
        report.success = false;
        report.error_message = "LLM 调用失败";
        return report;
    }

    report.root_cause = result->root_cause;
    report.bottlenecks = result->bottlenecks;
    report.suggestions = result->suggestions;
    report.severity = result->severity;
    report.raw_llm_response = result->raw_response;
    report.success = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        history_.push_back(report);
        if (history_.size() > MAX_HISTORY) {
            history_.erase(history_.begin());
        }
    }

    return report;
}

void DiagnosticEngine::setProvider(std::unique_ptr<LLMProvider> provider) {
    std::lock_guard<std::mutex> lock(mutex_);
    provider_ = std::move(provider);
}

std::optional<DiagnosticReport> DiagnosticEngine::getLastReport() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_.empty()) {
        return std::nullopt;
    }
    return history_.back();
}

std::vector<DiagnosticReport> DiagnosticEngine::getDiagnosisHistory(size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_.size() <= limit) {
        return history_;
    }
    return std::vector<DiagnosticReport>(
        history_.end() - static_cast<int>(limit), history_.end());
}

}  // namespace diagnostics
}  // namespace dbproxy
