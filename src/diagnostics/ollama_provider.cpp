#include "diagnostics/ollama_provider.h"
#include "core/logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sstream>
#include <cstring>
#include <chrono>

namespace dbproxy {
namespace diagnostics {

// ---------- 简易 JSON 提取（不依赖第三方库） ----------

/**
 * @brief 反转义 JSON 字符串中的转义序列
 * \" → "  \\ → \  \n → 换行  \r → 回车  \t → 制表符
 */
static std::string unescapeJson(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '"':  result += '"';  ++i; break;
                case '\\': result += '\\'; ++i; break;
                case 'n':  result += '\n'; ++i; break;
                case 'r':  result += '\r'; ++i; break;
                case 't':  result += '\t'; ++i; break;
                case '/':  result += '/';  ++i; break;
                case 'b':  result += '\b'; ++i; break;
                case 'f':  result += '\f'; ++i; break;
                default:   result += s[i]; break;  // 未知转义，保留原样
            }
        } else {
            result += s[i];
        }
    }
    return result;
}

/**
 * @brief 从 JSON 字符串中提取指定 key 的字符串值
 * 仅处理简单场景：找 "key":"value" 或 "key": "value"
 * 返回已反转义的字符串
 */
static std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string search_key = "\"" + key + "\"";
    size_t pos = json.find(search_key);
    if (pos == std::string::npos) return {};

    // 跳过 key 和冒号
    pos += search_key.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) {
        ++pos;
    }
    if (pos >= json.size()) return {};

    if (json[pos] == '"') {
        // 字符串值
        ++pos;
        size_t end = pos;
        while (end < json.size() && json[end] != '"') {
            if (json[end] == '\\') ++end;  // 跳过转义
            ++end;
        }
        return unescapeJson(json.substr(pos, end - pos));
    }
    return {};
}

/**
 * @brief 从 JSON 字符串中提取数组内容
 * 找 "key":[...] 返回 [...] 部分的字符串
 */
static std::string extractJsonArray(const std::string& json, const std::string& key) {
    std::string search_key = "\"" + key + "\"";
    size_t pos = json.find(search_key);
    if (pos == std::string::npos) return {};

    pos += search_key.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '[') return {};

    // 匹配 [] 嵌套
    int depth = 0;
    size_t start = pos;
    while (pos < json.size()) {
        if (json[pos] == '[') ++depth;
        else if (json[pos] == ']') {
            --depth;
            if (depth == 0) {
                return json.substr(start, pos - start + 1);
            }
        }
        ++pos;
    }
    return {};
}

/**
 * @brief 解析 JSON 数组中的字符串元素
 * 输入 ["a","b","c"] 返回 {"a","b","c"}
 */
static std::vector<std::string> parseStringArray(const std::string& array_str) {
    std::vector<std::string> result;
    if (array_str.empty()) return result;

    size_t pos = 0;
    while (pos < array_str.size()) {
        size_t quote = array_str.find('"', pos);
        if (quote == std::string::npos) break;
        ++quote;
        size_t end = quote;
        while (end < array_str.size() && array_str[end] != '"') {
            if (array_str[end] == '\\') ++end;
            ++end;
        }
        result.push_back(unescapeJson(array_str.substr(quote, end - quote)));
        pos = end + 1;
    }
    return result;
}

// ---------- OllamaProvider 实现 ----------

OllamaProvider::OllamaProvider(const std::string& base_url,
                               const std::string& model,
                               int timeout_seconds)
    : base_url_(base_url), model_(model), timeout_seconds_(timeout_seconds) {
    // 去掉末尾的 /
    if (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
}

bool OllamaProvider::isAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string response = const_cast<OllamaProvider*>(this)->httpGet("/api/tags");
    return !response.empty();
}

std::optional<DiagnosticResult> OllamaProvider::diagnose(const std::string& prompt) {
    LOG_INFO("OllamaProvider: 开始调用 LLM 诊断, model=" + model_);

    // 构建 Ollama /api/chat 请求体
    std::ostringstream json_body;
    json_body << "{";
    json_body << "\"model\":\"" << model_ << "\",";
    json_body << "\"messages\":[";
    json_body << "{\"role\":\"system\",\"content\":\"You are a database performance diagnostic expert. Analyze the metrics and return results in the exact JSON format requested. Respond ONLY with valid JSON, no markdown fences.\"},";
    json_body << "{\"role\":\"user\",\"content\":";

    // 转义 prompt 中的特殊字符
    std::string escaped_prompt;
    escaped_prompt.reserve(prompt.size());
    for (char c : prompt) {
        switch (c) {
            case '"':  escaped_prompt += "\\\""; break;
            case '\\': escaped_prompt += "\\\\"; break;
            case '\n': escaped_prompt += "\\n"; break;
            case '\r': escaped_prompt += "\\r"; break;
            case '\t': escaped_prompt += "\\t"; break;
            default:   escaped_prompt += c; break;
        }
    }
    json_body << "\"" << escaped_prompt << "\"";
    json_body << "}],";
    json_body << "\"stream\":false";  // 使用非流式模式，简化解析
    json_body << "}";

    auto start_time = std::chrono::steady_clock::now();

    std::string response = httpPost("/api/chat", json_body.str());

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    if (response.empty()) {
        LOG_ERROR("OllamaProvider: LLM 调用失败（无响应）");
        return std::nullopt;
    }

    LOG_INFO("OllamaProvider: LLM 响应耗时 " + std::to_string(elapsed) + "ms");

    // 从响应中提取 assistant 消息内容
    std::string content = extractJsonString(response, "content");
    if (content.empty()) {
        // 尝试提取 message 字段下的 content
        std::string message_part = response;
        // 找 "message":{...} 部分
        size_t msg_pos = response.find("\"message\"");
        if (msg_pos != std::string::npos) {
            content = extractJsonString(response.substr(msg_pos), "content");
        }
    }

    if (content.empty()) {
        LOG_ERROR("OllamaProvider: 无法从响应中提取内容, raw=" + response.substr(0, 200));
        return std::nullopt;
    }

    LOG_INFO("OllamaProvider: LLM 原始回复长度=" + std::to_string(content.size()));

    // 解析为结构化诊断结果
    DiagnosticResult result = parseDiagnosticResult(content);
    result.raw_response = content;

    return result;
}

std::string OllamaProvider::httpPost(const std::string& endpoint,
                                     const std::string& json_body) {
    // 解析 URL
    std::string host;
    int port = 11434;
    std::string path = endpoint;

    // 从 base_url_ 提取 host:port
    std::string url = base_url_;
    if (url.find("http://") == 0) url = url.substr(7);
    else if (url.find("https://") == 0) url = url.substr(8);

    size_t colon = url.find(':');
    if (colon != std::string::npos) {
        host = url.substr(0, colon);
        port = std::stoi(url.substr(colon + 1));
    } else {
        host = url;
    }

    // DNS 解析
    struct addrinfo hints{}, *result;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);

    int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0) {
        LOG_ERROR("OllamaProvider: DNS 解析失败 " + host + " - " + gai_strerror(rc));
        return {};
    }

    // 创建 socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        LOG_ERROR("OllamaProvider: 创建 socket 失败");
        freeaddrinfo(result);
        return {};
    }

    // 设置超时
    struct timeval tv {};
    tv.tv_sec = timeout_seconds_;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // 连接
    if (connect(sockfd, result->ai_addr, result->ai_addrlen) < 0) {
        LOG_ERROR("OllamaProvider: 连接 " + host + ":" + std::to_string(port) + " 失败");
        close(sockfd);
        freeaddrinfo(result);
        return {};
    }
    freeaddrinfo(result);

    // 构造 HTTP POST 请求
    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n";
    request << "Host: " << host << ":" << port << "\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Content-Length: " << json_body.size() << "\r\n";
    request << "Connection: close\r\n";
    request << "\r\n";
    request << json_body;

    std::string req_str = request.str();
    ssize_t sent = send(sockfd, req_str.c_str(), req_str.size(), 0);
    if (sent < 0) {
        LOG_ERROR("OllamaProvider: 发送请求失败");
        close(sockfd);
        return {};
    }

    // 读取响应
    std::string response;
    char buffer[4096];
    ssize_t bytes;
    while ((bytes = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        response += buffer;
    }
    close(sockfd);

    // 分离 HTTP 头和体
    size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        LOG_ERROR("OllamaProvider: HTTP 响应格式异常");
        return {};
    }

    std::string body = response.substr(header_end + 4);

    // 检查 HTTP 状态码
    if (response.find("200 OK") == std::string::npos) {
        LOG_ERROR("OllamaProvider: HTTP 非 200 响应, body=" + body.substr(0, 200));
        return {};
    }

    return body;
}

std::string OllamaProvider::httpGet(const std::string& endpoint) {
    std::string host;
    int port = 11434;

    std::string url = base_url_;
    if (url.find("http://") == 0) url = url.substr(7);
    else if (url.find("https://") == 0) url = url.substr(8);

    size_t colon = url.find(':');
    if (colon != std::string::npos) {
        host = url.substr(0, colon);
        port = std::stoi(url.substr(colon + 1));
    } else {
        host = url;
    }

    struct addrinfo hints{}, *result;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);

    int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0) return {};

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        freeaddrinfo(result);
        return {};
    }

    struct timeval tv {};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sockfd, result->ai_addr, result->ai_addrlen) < 0) {
        close(sockfd);
        freeaddrinfo(result);
        return {};
    }
    freeaddrinfo(result);

    std::ostringstream request;
    request << "GET " << endpoint << " HTTP/1.1\r\n";
    request << "Host: " << host << ":" << port << "\r\n";
    request << "Connection: close\r\n";
    request << "\r\n";

    std::string req_str = request.str();
    send(sockfd, req_str.c_str(), req_str.size(), 0);

    std::string response;
    char buffer[4096];
    ssize_t bytes;
    while ((bytes = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes] = '\0';
        response += buffer;
    }
    close(sockfd);

    size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) return {};

    if (response.find("200 OK") == std::string::npos) return {};

    return response.substr(header_end + 4);
}

std::string OllamaProvider::extractMessageFromStream(const std::string& stream_response) {
    // 流式模式暂未使用（当前使用非流式），留作扩展
    std::string result;
    std::istringstream iss(stream_response);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::string content = extractJsonString(line, "content");
        result += content;
    }
    return result;
}

DiagnosticResult OllamaProvider::parseDiagnosticResult(const std::string& llm_response) {
    DiagnosticResult result;

    // 尝试从 ```json ... ``` 代码块中提取
    std::string json_str = llm_response;

    size_t fence_start = llm_response.find("```json");
    if (fence_start != std::string::npos) {
        fence_start += 7;  // 跳过 ```json
        size_t fence_end = llm_response.find("```", fence_start);
        if (fence_end != std::string::npos) {
            json_str = llm_response.substr(fence_start, fence_end - fence_start);
        }
    } else {
        // 尝试找 { ... } 最外层
        size_t brace_start = llm_response.find('{');
        size_t brace_end = llm_response.rfind('}');
        if (brace_start != std::string::npos && brace_end != std::string::npos && brace_end > brace_start) {
            json_str = llm_response.substr(brace_start, brace_end - brace_start + 1);
        }
    }

    // 解析字段
    result.root_cause = extractJsonString(json_str, "root_cause");
    result.severity = extractJsonString(json_str, "severity");

    std::string bottlenecks_str = extractJsonArray(json_str, "bottlenecks");
    result.bottlenecks = parseStringArray(bottlenecks_str);

    std::string suggestions_str = extractJsonArray(json_str, "suggestions");
    result.suggestions = parseStringArray(suggestions_str);

    // 兜底：如果 JSON 解析失败，把 LLM 原始回复放入 root_cause
    if (result.root_cause.empty() && result.bottlenecks.empty() && result.suggestions.empty()) {
        result.root_cause = "LLM 返回内容无法解析为结构化 JSON，原始回复见 raw_response";
        result.severity = "medium";
        result.suggestions = {"请检查 LLM 返回格式是否正确"};
    }

    return result;
}

std::string OllamaProvider::urlEncode(const std::string& value) {
    std::string encoded;
    for (char c : value) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            encoded += buf;
        }
    }
    return encoded;
}

}  // namespace diagnostics
}  // namespace dbproxy
