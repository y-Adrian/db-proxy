#ifndef DB_PROXY_PROMPT_BUILDER_H
#define DB_PROXY_PROMPT_BUILDER_H

#include <string>
#include <vector>
#include <chrono>

namespace dbproxy {
// 前置声明已有的监控类
class Statistics;
class Metrics;

namespace diagnostics {

/**
 * @brief 慢查询记录
 */
struct SlowQueryRecord {
    std::string sql_fingerprint;  // 参数化后的 SQL
    std::string sql_type;          // SELECT/INSERT/UPDATE/DELETE
    std::string database;
    uint64_t exec_count{0};
    uint64_t avg_latency_ms{0};
    uint64_t max_latency_ms{0};
    uint64_t total_latency_ms{0};
};

/**
 * @brief 错误分布记录
 */
struct ErrorDistributionRecord {
    std::string error_code;    // MySQL: "1205", PostgreSQL: "40001"
    std::string error_message;
    uint64_t count{0};
    double percentage{0.0};
};

/**
 * @brief 连接池状态快照
 */
struct PoolStatusSnapshot {
    int active_connections{0};
    int idle_connections{0};
    int max_connections{0};
    int waiting_threads{0};
    double pool_usage_percent{0.0};
    uint64_t connections_created{0};
    uint64_t connections_destroyed{0};
};

/**
 * @brief 完整的诊断上下文
 * 
 * 由 PromptBuilder 从运行时指标收集并结构化
 */
struct DiagnosticContext {
    // 时间范围
    std::chrono::system_clock::time_point snapshot_time;
    std::string time_range{"recent 15 min"};

    // 全局统计
    uint64_t total_queries{0};
    uint64_t failed_queries{0};
    uint64_t slow_queries{0};
    double current_qps{0.0};
    double avg_latency_ms{0.0};
    double p99_latency_ms{0.0};

    // 慢查询 Top N
    std::vector<SlowQueryRecord> slow_queries;

    // 错误分布
    std::vector<ErrorDistributionRecord> error_distribution;

    // 连接池状态
    PoolStatusSnapshot pool_status;

    // 查询模式
    uint64_t read_qps{0};
    uint64_t write_qps{0};
    std::string busiest_table;
    std::string busiest_database;
};

/**
 * @brief Prompt 构建器
 * 
 * 将运行时指标结构化为 LLM 可理解的 Prompt 上下文。
 * 
 * 面试亮点：
 * - 结构化 Prompt 工程：将多维指标转化为结构化文本
 * - 模板方法模式：不同场景使用不同 Prompt 模板
 * - 数据清洗：过滤噪声，只保留有价值的诊断上下文
 */
class PromptBuilder {
public:
    PromptBuilder() = default;

    /**
     * @brief 从运行时指标收集诊断上下文
     */
    DiagnosticContext collectContext();

    /**
     * @brief 将上下文构建为 LLM Prompt
     * 
     * @param ctx 诊断上下文
     * @param language 输出语言 ("zh" / "en")
     * @return 结构化的 Prompt 字符串
     */
    std::string buildPrompt(const DiagnosticContext& ctx,
                           const std::string& language = "zh") const;

    /**
     * @brief 构建简短版 Prompt（用于快速诊断）
     */
    std::string buildBriefPrompt(const DiagnosticContext& ctx,
                                const std::string& language = "zh") const;

private:
    /**
     * @brief 格式化慢查询部分
     */
    std::string formatSlowQueries(const std::vector<SlowQueryRecord>& queries,
                                  const std::string& language) const;

    /**
     * @brief 格式化错误分布部分
     */
    std::string formatErrorDistribution(
        const std::vector<ErrorDistributionRecord>& errors,
        const std::string& language) const;

    /**
     * @brief 格式化连接池状态部分
     */
    std::string formatPoolStatus(const PoolStatusSnapshot& pool,
                                const std::string& language) const;

    /**
     * @brief 格式化全局统计部分
     */
    std::string formatGlobalStats(const DiagnosticContext& ctx,
                                 const std::string& language) const;
};

}  // namespace diagnostics
}  // namespace dbproxy

#endif  // DB_PROXY_PROMPT_BUILDER_H
