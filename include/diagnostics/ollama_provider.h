#ifndef DB_PROXY_OLLAMA_PROVIDER_H
#define DB_PROXY_OLLAMA_PROVIDER_H

#include "llm_provider.h"
#include <string>
#include <mutex>

namespace dbproxy {
namespace diagnostics {

/**
 * @brief Ollama LLM 提供者
 *
 * 通过 Ollama REST API (/api/chat) 调用本地大模型进行诊断分析。
 * 零外部依赖：使用 POSIX socket 直接发送 HTTP 请求，解析 JSON 响应。
 *
 * 使用方式：
 *   auto provider = std::make_unique<OllamaProvider>("http://localhost:11434", "qwen3:14b");
 *   DiagnosticEngine engine(std::move(provider));
 *
 * 面试亮点：
 * - 策略模式：与 MockProvider 实现同一接口，无缝切换
 * - 零外部依赖：不用 libcurl，POSIX socket + 手写 HTTP 请求
 * - 超时控制：防止 LLM 响应过慢阻塞主流程
 * - 健康检查：isAvailable() 通过 /api/tags 检测 Ollama 是否在线
 */
class OllamaProvider : public LLMProvider {
public:
    /**
     * @brief 构造 Ollama 提供者
     * @param base_url Ollama 服务地址，默认 http://localhost:11434
     * @param model 模型名称，默认 qwen3:14b
     * @param timeout_seconds HTTP 请求超时（秒），默认 120
     */
    explicit OllamaProvider(const std::string& base_url = "http://localhost:11434",
                            const std::string& model = "qwen3:14b",
                            int timeout_seconds = 120);

    std::optional<DiagnosticResult> diagnose(const std::string& prompt) override;

    std::string name() const override {
        return "OllamaProvider(" + model_ + ")";
    }

    bool isAvailable() const override;

    /** @brief 获取当前使用的模型名称 */
    std::string model() const { return model_; }

private:
    /**
     * @brief 发送 HTTP POST 请求到 Ollama API
     * @param endpoint API 路径，如 "/api/chat"
     * @param json_body JSON 格式的请求体
     * @return 响应体字符串，失败返回空
     */
    std::string httpPost(const std::string& endpoint,
                         const std::string& json_body);

    /**
     * @brief 发送 HTTP GET 请求
     */
    std::string httpGet(const std::string& endpoint);

    /**
     * @brief 从 Ollama 响应中提取最终文本
     *
     * Ollama /api/chat 返回流式 NDJSON，每行格式：
     *   {"message":{"role":"assistant","content":"..."},...}
     * 最后一行包含 "done":true
     */
    std::string extractMessageFromStream(const std::string& stream_response);

    /**
     * @brief 从 LLM 原始文本中解析结构化诊断结果
     *
     * LLM 返回的 JSON 可能包含在 ```json ``` 代码块中，
     * 也可能直接输出 JSON，此方法处理两种情况。
     */
    DiagnosticResult parseDiagnosticResult(const std::string& llm_response);

    /**
     * @brief URL 编码辅助
     */
    static std::string urlEncode(const std::string& value);

    std::string base_url_;
    std::string model_;
    int timeout_seconds_;
    mutable std::mutex mutex_;
};

}  // namespace diagnostics
}  // namespace dbproxy

#endif  // DB_PROXY_OLLAMA_PROVIDER_H
