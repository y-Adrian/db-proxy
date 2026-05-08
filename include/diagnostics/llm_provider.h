#ifndef DB_PROXY_LLM_PROVIDER_H
#define DB_PROXY_LLM_PROVIDER_H

#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace dbproxy {
namespace diagnostics {

/**
 * @brief LLM 诊断结果
 */
struct DiagnosticResult {
    std::string root_cause;
    std::vector<std::string> bottlenecks;
    std::vector<std::string> suggestions;
    std::string severity;  // "low", "medium", "high", "critical"
    std::string raw_response;
};

/**
 * @brief LLM 提供者抽象接口
 * 
 * 所有 LLM 后端（OpenAI、Claude、Ollama、Mock）都实现此接口
 */
class LLMProvider {
public:
    virtual ~LLMProvider() = default;

    /**
     * @brief 发送诊断请求
     * @param prompt 结构化的 Prompt 上下文
     * @return 诊断结果，失败返回 std::nullopt
     */
    virtual std::optional<DiagnosticResult> diagnose(const std::string& prompt) = 0;

    /**
     * @brief 获取提供者名称
     */
    virtual std::string name() const = 0;

    /**
     * @brief 检查提供者是否可用
     */
    virtual bool isAvailable() const = 0;
};

}  // namespace diagnostics
}  // namespace dbproxy

#endif  // DB_PROXY_LLM_PROVIDER_H
