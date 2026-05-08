#ifndef DB_PROXY_MOCK_PROVIDER_H
#define DB_PROXY_MOCK_PROVIDER_H

#include "llm_provider.h"
#include <string>
#include <optional>
#include <random>

namespace dbproxy {
namespace diagnostics {

/**
 * @brief Mock LLM 提供者
 * 
 * 用于开发和测试，返回基于规则的诊断结果。
 * 不依赖任何外部 API，零配置即可运行。
 * 
 * 面试亮点：
 * - 策略模式：LLMProvider 接口的不同实现
 * - 可扩展：轻松切换到真实 LLM 后端
 * - 零依赖：不引入 curl/libcurl 等网络库
 */
class MockProvider : public LLMProvider {
public:
    MockProvider() = default;

    std::optional<DiagnosticResult> diagnose(const std::string& prompt) override;

    std::string name() const override {
        return "MockProvider";
    }

    bool isAvailable() const override {
        return true;  // Mock 永远可用
    }

private:
    /**
     * @brief 基于规则的简单诊断（模拟 LLM 行为）
     * 
     * 实际面试中可以说：
     * "这里用规则模拟了 LLM 的诊断逻辑，
     *  接入真实 LLM 时只需替换 Provider 实现，
     *  上层 DiagnosticEngine 完全无感知"
     */
    DiagnosticResult generateMockDiagnosis(const std::string& prompt) const;
};

}  // namespace diagnostics
}  // namespace dbproxy

#endif  // DB_PROXY_MOCK_PROVIDER_H
