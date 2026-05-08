#include "diagnostics/prompt_builder.h"
#include "monitor/statistics.h"
#include "monitor/metrics.h"
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace dbproxy {
namespace diagnostics {

DiagnosticContext PromptBuilder::collectContext() {
    DiagnosticContext ctx;
    ctx.snapshot_time = std::chrono::system_clock::now();

    // 从 Statistics 单例收集数据
    const auto& stats = Statistics::instance();
    Statistics::GlobalStats global = stats.getGlobalStats();

    ctx.total_queries = global.total_queries;
    ctx.failed_queries = global.failed_queries;
    ctx.slow_queries = global.slow_queries;
    ctx.current_qps = global.current_qps;
    ctx.read_qps = global.current_rps;
    ctx.write_qps = global.current_wps;
    ctx.active_connections = global.active_connections;

    // 获取慢查询 Top 10
    ctx.slow_queries = stats.getSlowQueries(10);

    // 获取 Top QPS 查询（用于分析查询模式）
    auto top_queries = stats.getTopQueries(5);
    if (!top_queries.empty()) {
        ctx.busiest_table = top_queries[0].database;  // 简化处理
    }

    // 连接池状态（从 Metrics 获取）
    const auto& metrics = Metrics::instance();
    ctx.pool_status.active_connections = global.active_connections;
    // idle_connections 需要从 ConnectionPool 获取，这里用估算
    ctx.pool_status.idle_connections = static_cast<int>(metrics.getGauge("idle_connections"));
    ctx.pool_status.max_connections = static_cast<int>(metrics.getGauge("max_connections"));
    ctx.pool_status.pool_usage_percent =
        ctx.pool_status.max_connections > 0
            ? (ctx.pool_status.active_connections * 100.0 / ctx.pool_status.max_connections)
            : 0.0;

    // TODO: 错误分布需要从 Statistics 增强收集
    // 当前用默认值
    ctx.error_distribution = {};

    return ctx;
}

std::string PromptBuilder::buildPrompt(const DiagnosticContext& ctx,
                                       const std::string& language) const {
    std::ostringstream oss;

    if (language == "zh") {
        oss << "# 数据库代理诊断请求\n\n";
        oss << "请分析以下数据库代理的运行指标，识别性能瓶颈、异常模式和潜在风险，";
        oss << "并给出具体的调优建议。\n\n";

        oss << formatGlobalStats(ctx, language);
        oss << "\n\n";
        oss << formatSlowQueries(ctx.slow_queries, language);
        oss << "\n\n";
        oss << formatErrorDistribution(ctx.error_distribution, language);
        oss << "\n\n";
        oss << formatPoolStatus(ctx.pool_status, language);

        oss << "\n\n---\n\n";
        oss << "请以以下 JSON 格式返回诊断结果（不要包含```json```标记）：\n";
        oss << "{\n";
        oss << "  \"root_cause\": \"根因描述\",\n";
        oss << "  \"bottlenecks\": [\"瓶颈1\", \"瓶颈2\"],\n";
        oss << "  \"suggestions\": [\"建议1\", \"建议2\", \"建议3\"],\n";
        oss << "  \"severity\": \"low|medium|high|critical\"\n";
        oss << "}\n";
    } else {
        oss << "# Database Proxy Diagnostics Request\n\n";
        oss << "Please analyze the following database proxy metrics, ";
        oss << "identify performance bottlenecks, anomalies, and potential risks.\n\n";

        // Same structure, English
        oss << formatGlobalStats(ctx, language);
        oss << "\n\n";
        oss << formatSlowQueries(ctx.slow_queries, "en");
        oss << "\n\n";
        oss << formatErrorDistribution(ctx.error_distribution, "en");
        oss << "\n\n";
        oss << formatPoolStatus(ctx.pool_status, "en");

        oss << "\n\n---\n\n";
        oss << "Return diagnosis result in JSON format:\n";
        oss << "{\n";
        oss << "  \"root_cause\": \"description\",\n";
        oss << "  \"bottlenecks\": [\"bottleneck1\"],\n";
        oss << "  \"suggestions\": [\"suggestion1\"],\n";
        oss << "  \"severity\": \"low|medium|high|critical\"\n";
        oss << "}\n";
    }

    return oss.str();
}

std::string PromptBuilder::buildBriefPrompt(const DiagnosticContext& ctx,
                                            const std::string& language) const {
    std::ostringstream oss;

    if (language == "zh") {
        oss << "数据库代理状态：";
        oss << "QPS=" << std::fixed << std::setprecision(1) << ctx.current_qps << "，";
        oss << "慢查询=" << ctx.slow_queries << "，";
        oss << "连接池使用率=" << std::fixed << std::setprecision(1)
            << ctx.pool_status.pool_usage_percent << "%";
        oss << "。请快速分析是否存在异常。";
    } else {
        oss << "DB Proxy status: ";
        oss << "QPS=" << ctx.current_qps << ", ";
        oss << "slow_queries=" << ctx.slow_queries << ", ";
        oss << "pool_usage=" << ctx.pool_status.pool_usage_percent << "%";
        oss << ". Quick analysis: any anomaly?";
    }

    return oss.str();
}

std::string PromptBuilder::formatSlowQueries(
    const std::vector<SlowQueryRecord>& queries,
    const std::string& language) const {
    std::ostringstream oss;

    if (language == "zh") {
        oss << "## 慢查询 Top " << queries.size() << "\n\n";
        if (queries.empty()) {
            oss << "（无慢查询记录）\n";
        } else {
            for (size_t i = 0; i < queries.size(); ++i) {
                const auto& q = queries[i];
                oss << (i + 1) << ". `" << q.sql_fingerprint << "`\n";
                oss << "   - 类型: " << q.sql_type << "\n";
                oss << "   - 执行次数: " << q.exec_count << "\n";
                oss << "   - 平均延迟: " << q.avg_latency_ms << "ms\n";
                oss << "   - 最大延迟: " << q.max_latency_ms << "ms\n\n";
            }
        }
    } else {
        oss << "## Slow Queries Top " << queries.size() << "\n\n";
        if (queries.empty()) {
            oss << "(No slow queries)\n";
        } else {
            for (size_t i = 0; i < queries.size(); ++i) {
                const auto& q = queries[i];
                oss << (i + 1) << ". `" << q.sql_fingerprint << "`\n";
                oss << "   - Type: " << q.sql_type << "\n";
                oss << "   - Count: " << q.exec_count << "\n";
                oss << "   - Avg Latency: " << q.avg_latency_ms << "ms\n\n";
            }
        }
    }

    return oss.str();
}

std::string PromptBuilder::formatErrorDistribution(
    const std::vector<ErrorDistributionRecord>& errors,
    const std::string& language) const {
    std::ostringstream oss;

    if (language == "zh") {
        oss << "## 错误分布\n\n";
        if (errors.empty()) {
            oss << "（无错误记录）\n";
        } else {
            for (const auto& e : errors) {
                oss << "- " << e.error_code << ": " << e.error_message
                    << "（" << e.count << " 次，" << std::fixed << std::setprecision(1)
                    << e.percentage << "%）\n";
            }
        }
    } else {
        oss << "## Error Distribution\n\n";
        if (errors.empty()) {
            oss << "(No errors)\n";
        } else {
            for (const auto& e : errors) {
                oss << "- " << e.error_code << ": " << e.error_message
                    << " (" << e.count << " times)\n";
            }
        }
    }

    return oss.str();
}

std::string PromptBuilder::formatPoolStatus(
    const PoolStatusSnapshot& pool,
    const std::string& language) const {
    std::ostringstream oss;

    if (language == "zh") {
        oss << "## 连接池状态\n\n";
        oss << "- 活跃连接: " << pool.active_connections << "\n";
        oss << "- 空闲连接: " << pool.idle_connections << "\n";
        oss << "- 最大连接: " << pool.max_connections << "\n";
        oss << "- 使用率: " << std::fixed << std::setprecision(1)
            << pool.pool_usage_percentage << "%\n";
        oss << "- 等待线程: " << pool.waiting_threads << "\n";
    } else {
        oss << "## Connection Pool Status\n\n";
        oss << "- Active: " << pool.active_connections << "\n";
        oss << "- Idle: " << pool.idle_connections << "\n";
        oss << "- Max: " << pool.max_connections << "\n";
        oss << "- Usage: " << pool.pool_usage_percentage << "%\n";
        oss << "- Waiting: " << pool.waiting_threads << "\n";
    }

    return oss.str();
}

std::string PromptBuilder::formatGlobalStats(
    const DiagnosticContext& ctx,
    const std::string& language) const {
    std::ostringstream oss;

    if (language == "zh") {
        oss << "## 全局统计\n\n";
        oss << "- 总查询数: " << ctx.total_queries << "\n";
        oss << "- 失败查询: " << ctx.failed_queries << "\n";
        oss << "- 慢查询数: " << ctx.slow_queries << "\n";
        oss << "- 当前 QPS: " << std::fixed << std::setprecision(1) << ctx.current_qps << "\n";
        oss << "- 读 QPS: " << ctx.read_qps << "\n";
        oss << "- 写 QPS: " << ctx.write_qps << "\n";
    } else {
        oss << "## Global Statistics\n\n";
        oss << "- Total Queries: " << ctx.total_queries << "\n";
        oss << "- Failed Queries: " << ctx.failed_queries << "\n";
        oss << "- Slow Queries: " << ctx.slow_queries << "\n";
        oss << "- Current QPS: " << ctx.current_qps << "\n";
    }

    return oss.str();
}

}  // namespace diagnostics
}  // namespace dbproxy
