#include "monitor/metrics.h"
#include "core/logger.h"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace dbproxy {

Metrics& Metrics::instance() {
    static Metrics instance_;
    return instance_;
}

void Metrics::incrementCounter(const std::string& name) {
    incrementCounter(name, 1);
}

void Metrics::incrementCounter(const std::string& name, int64_t delta) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = counters_.find(name);
    if (it == counters_.end()) {
        lock.unlock();
        std::unique_lock<std::shared_mutex> ulock(mutex_);
        auto result = counters_.emplace(name, std::make_unique<Counter>());
        it = result.first;
    }
    
    it->second->value.fetch_add(delta);
}

int64_t Metrics::getCounter(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = counters_.find(name);
    if (it != counters_.end()) {
        return it->second->value.load();
    }
    return 0;
}

void Metrics::setGauge(const std::string& name, double value) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = gauges_.find(name);
    if (it == gauges_.end()) {
        lock.unlock();
        std::unique_lock<std::shared_mutex> ulock(mutex_);
        auto result = gauges_.emplace(name, std::make_unique<Gauge>());
        it = result.first;
    }
    
    it->second->value.store(value);
}

void Metrics::incrementGauge(const std::string& name) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = gauges_.find(name);
    if (it == gauges_.end()) {
        lock.unlock();
        std::unique_lock<std::shared_mutex> ulock(mutex_);
        gauges_.emplace(name, std::make_unique<Gauge>());
        it = gauges_.find(name);
    }
    
    it->second->value.fetch_add(1.0);
}

void Metrics::decrementGauge(const std::string& name) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = gauges_.find(name);
    if (it == gauges_.end()) {
        lock.unlock();
        std::unique_lock<std::shared_mutex> ulock(mutex_);
        gauges_.emplace(name, std::make_unique<Gauge>());
        it = gauges_.find(name);
    }
    
    it->second->value.fetch_sub(1.0);
}

double Metrics::getGauge(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = gauges_.find(name);
    if (it != gauges_.end()) {
        return it->second->value.load();
    }
    return 0.0;
}

void Metrics::recordLatency(const std::string& name, std::chrono::milliseconds ms) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    auto it = histograms_.find(name);
    if (it == histograms_.end()) {
        lock.unlock();
        std::unique_lock<std::shared_mutex> ulock(mutex_);
        auto result = histograms_.emplace(name, std::make_unique<Histogram>());
        it = result.first;
    }
    
    it->second->record(ms.count());
}

void Metrics::Histogram::record(double value) {
    count.fetch_add(1);
    sum.fetch_add(value);
    
    // 更新 min/max
    double current_min = min.load();
    while (value < current_min && !min.compare_exchange_weak(current_min, value)) {}
    
    double current_max = max.load();
    while (value > current_max && !max.compare_exchange_weak(current_max, value)) {}
    
    // 更新 bucket
    for (int i = 0; i < BUCKETS; ++i) {
        if (value <= buckets[i].le) {
            buckets[i].count.fetch_add(1);
        }
    }
}

Metrics::HistogramData Metrics::Histogram::getData() const {
    HistogramData data;
    data.count = count.load();
    data.sum = sum.load();
    data.min = min.load();
    data.max = max.load();
    
    if (data.count > 0) {
        data.sum = sum.load();
    }
    
    // 计算百分位数（简化实现）
    int64_t total = 0;
    int64_t p50_target = data.count * 0.5;
    int64_t p90_target = data.count * 0.9;
    int64_t p99_target = data.count * 0.99;
    int64_t p999_target = data.count * 0.999;
    
    for (int i = 0; i < BUCKETS && data.count > 0; ++i) {
        int64_t bucket_count = buckets[i].count.load();
        if (bucket_count == 0) continue;
        
        total += bucket_count;
        
        if (data.p50 == 0 && total >= p50_target) {
            data.p50 = buckets[i].le;
        }
        if (data.p90 == 0 && total >= p90_target) {
            data.p90 = buckets[i].le;
        }
        if (data.p99 == 0 && total >= p99_target) {
            data.p99 = buckets[i].le;
        }
        if (data.p999 == 0 && total >= p999_target) {
            data.p999 = buckets[i].le;
        }
    }
    
    return data;
}

Metrics::HistogramData Metrics::getHistogram(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = histograms_.find(name);
    if (it != histograms_.end()) {
        return it->second->getData();
    }
    return HistogramData{};
}

void Metrics::reset() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    counters_.clear();
    gauges_.clear();
    histograms_.clear();
}

std::string Metrics::toPrometheusFormat() const {
    std::ostringstream oss;
    
    std::shared_lock<std::shared_mutex> lock(mutex_);
    
    // 输出计数器
    for (const auto& [name, counter] : counters_) {
        oss << "# TYPE " << name << " counter\n";
        oss << name << " " << counter->value.load() << "\n";
    }
    
    // 输出仪表
    for (const auto& [name, gauge] : gauges_) {
        oss << "# TYPE " << name << " gauge\n";
        oss << name << " " << std::fixed << std::setprecision(2) << gauge->value.load() << "\n";
    }
    
    // 输出直方图
    for (const auto& [name, histogram] : histograms_) {
        auto data = histogram->getData();
        
        oss << "# TYPE " << name << " histogram\n";
        for (int i = 0; i < Histogram::BUCKETS; ++i) {
            int64_t count = histogram->buckets[i].count.load();
            oss << name << "_bucket{le=\"" << histogram->buckets[i].le << "\"} " << count << "\n";
        }
        oss << name << "_bucket{le=\"+Inf\"} " << data.count << "\n";
        oss << name << "_sum " << data.sum << "\n";
        oss << name << "_count " << data.count << "\n";
    }
    
    return oss.str();
}

}  // namespace dbproxy
