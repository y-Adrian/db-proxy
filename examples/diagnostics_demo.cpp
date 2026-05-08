#include "diagnostics/diagnostic_engine.h"
#include "diagnostics/mock_provider.h"
#include "diagnostics/report_generator.h"
#include "monitor/statistics.h"
#include "monitor/metrics.h"
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

// 模拟一些运行时数据，用于演示诊断功能
void simulateMetrics() {
    auto& stats = dbproxy::Statistics::instance();
    auto& metrics = dbproxy::Metrics::instance();

    // 模拟一些查询统计
    stats.recordQuery("SELECT * FROM users WHERE id = 1", "SELECT", "test", 5);
    stats.recordQuery("SELECT * FROM orders WHERE user_id = 100", "SELECT", "test", 120);  // 慢查询
    stats.recordQuery("SELECT * FROM orders WHERE user_id = 200", "SELECT", "test", 85);   // 慢查询
    stats.recordQuery("INSERT INTO logs VALUES (...)", "INSERT", "test", 3);
    stats.recordQuery("UPDATE users SET name = 'test' WHERE id = 1", "UPDATE", "test", 2);

    // 模拟指标
    metrics.incrementCounter("total_queries", 1000);
    metrics.incrementCounter("slow_queries", 50);
    metrics.setGauge("active_connections", 45.0);
    metrics.setGauge("idle_connections", 30.0);
    metrics.setGauge("max_connections", 100.0);
    metrics.recordLatency("query_latency", std::chrono::milliseconds(12));
    metrics.recordLatency("query_latency", std::chrono::milliseconds(85));
    metrics.recordLatency("query_latency", std::chrono::milliseconds(120));
}

int main() {
    std::cout << "========================================\n";
    std::cout << "  DB-Proxy 智能诊断模块演示 (Mock 模式)\n";
    std::cout << "========================================\n\n";

    // 1. 模拟运行时指标
    std::cout << "[1] 模拟运行时指标...\n";
    simulateMetrics();
    std::cout << "  ✓ 已生成模拟数据（含慢查询、连接池压力等）\n\n";

    // 2. 创建诊断引擎（使用 Mock Provider）
    std::cout << "[2] 初始化诊断引擎（Mock 模式）...\n";
    auto provider = std::make_unique<dbproxy::diagnostics::MockProvider>();
    dbproxy::diagnostics::DiagnosticEngine engine(std::move(provider));
    std::cout << "  ✓ 引擎初始化完成\n\n";

    // 3. 执行诊断
    std::cout << "[3] 执行诊断分析...\n";
    auto report = engine.runDiagnosis("zh");
    std::cout << "  ✓ 诊断完成\n\n";

    // 4. 输出控制台报告
    std::cout << "[4] 诊断报告：\n";
    dbproxy::diagnostics::ReportGenerator generator;
    std::cout << generator.toConsole(report);

    // 5. 保存报告文件
    std::cout << "\n[5] 保存报告文件...\n";
    std::string md_file = generator.saveToFile(report, "markdown");
    std::string json_file = generator.saveToFile(report, "json");
    std::cout << "  ✓ Markdown 报告: " << md_file << "\n";
    std::cout << "  ✓ JSON 报告: " << json_file << "\n\n";

    // 6. 显示 JSON 格式（用于 API 集成）
    std::cout << "[6] JSON 格式（用于机器集成）：\n";
    std::cout << generator.toJSON(report) << "\n";

    std::cout << "========================================\n";
    std::cout << "  演示完成！\n";
    std::cout << "  接入真实 LLM：替换 MockProvider 为 OpenAI/Claude Provider\n";
    std::cout << "========================================\n";

    return 0;
}
