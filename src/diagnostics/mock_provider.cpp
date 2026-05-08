#include "diagnostics/mock_provider.h"
#include "diagnostics/prompt_builder.h"
#include <iostream>
#include <algorithm>
#include <sstream>

namespace dbproxy {
namespace diagnostics {

std::optional<DiagnosticResult> MockProvider::diagnose(const std::string& prompt) {
    // 模拟 LLM 调用延迟（50-200ms）
    // 实际面试可以演示：异步调用、超时控制
    DiagnosticResult result;

    // 基于规则的 Mock 诊断（模拟 LLM 理解 Prompt 后的分析）
    result = generateMockDiagnosis(prompt);

    return result;
}

DiagnosticResult MockProvider::generateMockDiagnosis(const std::string& prompt) const {
    DiagnosticResult result;

    // 简单的规则判断（模拟 LLM 的分析能力）
    // 实际接入 LLM 后，这里会是真实的 AI 分析

    bool has_slow_query = prompt.find("慢查询") != std::string::npos ||
                          prompt.find("slow_query") != std::string::npos;
    bool has_error = prompt.find("错误") != std::string::npos ||
                     prompt.find("error") != std::string::npos;
    bool has_pool_pressure = prompt.find("使用率") != std::string::npos ||
                             prompt.find("usage") != std::string::npos;

    // 模拟 LLM 分析结果
    if (has_slow_query && has_pool_pressure) {
        result.root_cause = "连接池接近耗尽，同时存在大量慢查询，形成恶性循环";
        result.bottlenecks = {"连接池", "慢查询", "数据库 CPU"};
        result.suggestions = {
            "立即增大 max_connections 到当前值的 1.5-2 倍",
            "分析慢查询日志，为高频查询添加索引",
            "考虑引入查询超时机制，防止单个慢查询拖垮连接池",
            "建议引入读写分离，将读请求分流到只读副本"
        };
        result.severity = "high";
    } else if (has_slow_query) {
        result.root_cause = "存在慢查询，导致 P99 延迟升高";
        result.bottlenecks = {"慢查询", "缺少索引"};
        result.suggestions = {
            "为 WHERE 条件和 JOIN 字段添加索引",
            "考虑对大表进行分区或分表",
            "避免 SELECT *，只查询需要的字段"
        };
        result.severity = "medium";
    } else if (has_error) {
        result.root_cause = "数据库错误率异常，可能是连接不稳定或 SQL 语法错误";
        result.bottlenecks = {"数据库稳定性"};
        result.suggestions = {
            "检查数据库日志，确认错误码含义",
            "增加重试机制，处理 transient 错误",
            "检查网络连接稳定性"
        };
        result.severity = "medium";
    } else {
        result.root_cause = "系统运行正常，未检测到明显异常";
        result.bottlenecks = {};
        result.suggestions = {
            "当前系统状态良好，建议继续监控",
            "可优化：为高频查询添加缓存层（如 Redis）",
            "建议定期审查慢查询日志"
        };
        result.severity = "low";
    }

    // 构造模拟的 LLM 原始响应
    result.raw_response = "[Mock] 基于以下分析得出诊断结果：\n"
        + result.root_cause + "\n"
        "建议措施：\n";
    for (size_t i = 0; i < result.suggestions.size(); ++i) {
        result.raw_response += "  " + std::to_string(i + 1) + ". " + result.suggestions[i] + "\n";
    }

    return result;
}

}  // namespace diagnostics
}  // namespace dbproxy
