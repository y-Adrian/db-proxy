#include "diagnostics/diagnostic_engine.h"
#include "diagnostics/mock_provider.h"
#include "diagnostics/ollama_provider.h"
#include "diagnostics/report_generator.h"
#include "monitor/statistics.h"
#include "monitor/metrics.h"
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <cstdlib>

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

void printUsage(const char* prog) {
    std::cout << "用法: " << prog << " [模式] [选项]\n\n";
    std::cout << "模式:\n";
    std::cout << "  mock       使用 Mock Provider（默认）\n";
    std::cout << "  ollama     使用 Ollama 本地大模型\n\n";
    std::cout << "选项（仅 ollama 模式）:\n";
    std::cout << "  --url URL        Ollama 服务地址 (默认 http://localhost:11434)\n";
    std::cout << "  --model MODEL    模型名称 (默认 qwen3:14b)\n";
    std::cout << "  --timeout SEC    请求超时秒数 (默认 120)\n\n";
    std::cout << "示例:\n";
    std::cout << "  " << prog << " mock\n";
    std::cout << "  " << prog << " ollama\n";
    std::cout << "  " << prog << " ollama --model qwen2.5-coder:7b --timeout 60\n";
}

int main(int argc, char* argv[]) {
    // 解析命令行参数
    std::string mode = "mock";
    std::string ollama_url = "http://localhost:11434";
    std::string ollama_model = "qwen3:14b";
    int timeout = 120;

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "--help" || arg1 == "-h") {
            printUsage(argv[0]);
            return 0;
        }
        if (arg1 == "mock" || arg1 == "ollama") {
            mode = arg1;
        } else {
            std::cerr << "未知模式: " << arg1 << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    // 解析额外选项
    for (int i = 2; i < argc; ++i) {
        std::string opt = argv[i];
        if (opt == "--url" && i + 1 < argc) ollama_url = argv[++i];
        else if (opt == "--model" && i + 1 < argc) ollama_model = argv[++i];
        else if (opt == "--timeout" && i + 1 < argc) timeout = std::stoi(argv[++i]);
    }

    std::cout << "========================================\n";
    std::cout << "  DB-Proxy 智能诊断模块演示\n";
    std::cout << "========================================\n\n";

    // 1. 模拟运行时指标
    std::cout << "[1] 模拟运行时指标...\n";
    simulateMetrics();
    std::cout << "  ✓ 已生成模拟数据（含慢查询、连接池压力等）\n\n";

    // 2. 创建诊断引擎
    std::cout << "[2] 初始化诊断引擎";
    std::unique_ptr<dbproxy::diagnostics::LLMProvider> provider;

    if (mode == "ollama") {
        std::cout << "（Ollama 模式, model=" << ollama_model << "）...\n";
        provider = std::make_unique<dbproxy::diagnostics::OllamaProvider>(
            ollama_url, ollama_model, timeout);

        // 检查可用性
        if (!provider->isAvailable()) {
            std::cerr << "  ✗ Ollama 服务不可用！请确认 Ollama 已启动且模型已拉取\n";
            std::cerr << "    检查命令: curl http://localhost:11434/api/tags\n";
            return 1;
        }
        std::cout << "  ✓ Ollama 连接成功\n";
    } else {
        std::cout << "（Mock 模式）...\n";
        provider = std::make_unique<dbproxy::diagnostics::MockProvider>();
        std::cout << "  ✓ 引擎初始化完成（Mock）\n";
    }

    dbproxy::diagnostics::DiagnosticEngine engine(std::move(provider));
    std::cout << "\n";

    // 3. 执行诊断
    std::cout << "[3] 执行诊断分析...\n";
    if (mode == "ollama") {
        std::cout << "  ⏳ 等待 LLM 响应（可能需要 10-60 秒）...\n";
    }

    auto report = engine.runDiagnosis("zh");

    if (!report.success) {
        std::cerr << "  ✗ 诊断失败: " << report.error_message << "\n";
        return 1;
    }
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
    std::cout << "  演示完成！当前模式: " << mode << "\n";
    if (mode == "mock") {
        std::cout << "  切换到 Ollama: ./diagnostics_demo ollama\n";
        std::cout << "  指定模型: ./diagnostics_demo ollama --model qwen2.5-coder:7b\n";
    }
    std::cout << "========================================\n";

    return 0;
}
