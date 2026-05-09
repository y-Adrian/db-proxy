#include "core/config.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

namespace dbproxy {

namespace {

// Trim leading/trailing whitespace in-place
void trim(std::string& s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
}

// Return default-initialised config (single "test" database pool)
Config defaultConfig() {
    Config cfg;
    cfg.server.host               = "0.0.0.0";
    cfg.server.port               = 6033;
    cfg.server.max_connections    = 10000;
    cfg.server.worker_threads     = 4;
    cfg.server.backlog            = 128;

    cfg.pool.min_connections           = 10;
    cfg.pool.max_connections           = 100;
    cfg.pool.max_idle_time_ms          = 30000;
    cfg.pool.connection_timeout_ms     = 5000;
    cfg.pool.health_check_interval_ms  = 10000;
    cfg.pool.enable_connection_reuse   = true;

    DatabaseConfig db;
    db.host     = "127.0.0.1";
    db.port     = 3306;
    db.username = "root";
    db.password = "";
    db.database = "test";
    db.charset  = "utf8mb4";
    cfg.databases.push_back(db);

    cfg.monitoring.enable                = true;
    cfg.monitoring.metrics_interval_ms   = 1000;
    cfg.monitoring.slow_query_threshold_ms = 100;
    cfg.monitoring.enable_query_logging  = false;

    cfg.log_level = "INFO";
    cfg.log_file  = "./logs/proxy.log";
    return cfg;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Minimal INI parser
//
// Supported format:
//
//   [section]
//   key = value   ; inline comments allowed after ';'
//   # full-line comment
//
// Recognised sections & keys are listed below.  Unknown keys are silently
// ignored so the file can carry forward-compatible extras.
// ---------------------------------------------------------------------------
Config loadConfig(const std::string& config_file) {
    Config cfg = defaultConfig();

    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cerr << "[Config] '" << config_file
                  << "' not found – using built-in defaults\n";
        return cfg;
    }

    std::string section;
    DatabaseConfig cur_db;  // accumulated while inside a [database.*] section
    bool in_db_section = false;

    auto flush_db = [&]() {
        if (in_db_section && !cur_db.host.empty()) {
            cfg.databases.push_back(cur_db);
        }
        cur_db = DatabaseConfig{};
        in_db_section = false;
    };

    std::string line;
    while (std::getline(file, line)) {
        // Strip inline comments
        auto sc = line.find(';');
        if (sc != std::string::npos) line.erase(sc);
        trim(line);

        if (line.empty() || line.front() == '#') continue;

        // Section header
        if (line.front() == '[' && line.back() == ']') {
            flush_db();
            section = line.substr(1, line.size() - 2);
            trim(section);
            // Convert to lowercase for case-insensitive matching
            std::transform(section.begin(), section.end(), section.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            if (section == "database" || section.rfind("database.", 0) == 0) {
                in_db_section = true;
                cur_db = DatabaseConfig{};
            }
            continue;
        }

        // Key = value
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key   = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        trim(key);
        trim(value);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        // ---- [server] ----
        if (section == "server") {
            if      (key == "host")            cfg.server.host = value;
            else if (key == "port")            cfg.server.port = static_cast<uint16_t>(std::stoi(value));
            else if (key == "max_connections") cfg.server.max_connections = std::stoi(value);
            else if (key == "worker_threads")  cfg.server.worker_threads = std::stoi(value);
            else if (key == "backlog")         cfg.server.backlog = std::stoi(value);
        }
        // ---- [pool] ----
        else if (section == "pool") {
            if      (key == "min_connections")          cfg.pool.min_connections = std::stoi(value);
            else if (key == "max_connections")          cfg.pool.max_connections = std::stoi(value);
            else if (key == "max_idle_time_ms")         cfg.pool.max_idle_time_ms = std::stoi(value);
            else if (key == "connection_timeout_ms")    cfg.pool.connection_timeout_ms = std::stoi(value);
            else if (key == "health_check_interval_ms") cfg.pool.health_check_interval_ms = std::stoi(value);
            else if (key == "enable_connection_reuse")  cfg.pool.enable_connection_reuse = (value == "true" || value == "1");
        }
        // ---- [database] / [database.*] ----
        else if (in_db_section) {
            if      (key == "host")     cur_db.host = value;
            else if (key == "port")     cur_db.port = static_cast<uint16_t>(std::stoi(value));
            else if (key == "user" || key == "username") cur_db.username = value;
            else if (key == "password") cur_db.password = value;
            else if (key == "database" || key == "dbname") cur_db.database = value;
            else if (key == "charset")  cur_db.charset = value;
        }
        // ---- [monitor] ----
        else if (section == "monitor" || section == "monitoring") {
            if      (key == "enable")                    cfg.monitoring.enable = (value == "true" || value == "1");
            else if (key == "metrics_interval_ms")       cfg.monitoring.metrics_interval_ms = std::stoi(value);
            else if (key == "slow_query_threshold_ms")   cfg.monitoring.slow_query_threshold_ms = std::stoi(value);
            else if (key == "enable_query_logging")      cfg.monitoring.enable_query_logging = (value == "true" || value == "1");
        }
        // ---- [log] ----
        else if (section == "log") {
            if      (key == "level") cfg.log_level = value;
            else if (key == "file")  cfg.log_file  = value;
        }
    }

    flush_db();

    // If the file provided a [database] section it replaces the lone default
    // entry; if not, the default entry from defaultConfig() stays.
    if (cfg.databases.size() > 1) {
        // Remove the placeholder default added in defaultConfig()
        cfg.databases.erase(cfg.databases.begin());
    }

    std::cerr << "[Config] Loaded from '" << config_file
              << "' (" << cfg.databases.size() << " database(s))\n";
    return cfg;
}

} // namespace dbproxy
