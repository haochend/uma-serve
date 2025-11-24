// UMA Serve - minimal logging (header-only)
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>

namespace uma::util {

enum class LogLevel : uint8_t { Error = 0, Warn = 1, Info = 2, Debug = 3 };

class Logger {
  public:
    static Logger& instance() {
        static Logger g;
        return g;
    }

    void set_level(LogLevel lvl) {
        level_.store(static_cast<uint8_t>(lvl), std::memory_order_relaxed);
    }
    LogLevel level() const {
        return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
    }
    bool should(LogLevel lvl) const {
        return static_cast<uint8_t>(lvl) <= level_.load(std::memory_order_relaxed);
    }

    void configure_from_env() {
        // UMA_LOG_LEVEL = debug|info|warn|error
        const char* lvl = std::getenv("UMA_LOG_LEVEL");
        if (lvl && *lvl) {
            std::string s(lvl);
            for (auto& c : s)
                c = (char)std::tolower(c);
            if (s == "debug")
                set_level(LogLevel::Debug);
            else if (s == "info")
                set_level(LogLevel::Info);
            else if (s == "warn" || s == "warning")
                set_level(LogLevel::Warn);
            else if (s == "error" || s == "err")
                set_level(LogLevel::Error);
        }
    }

    void emit(LogLevel lvl, const std::string& msg) {
        if (!should(lvl))
            return;
        std::ostream& os = std::cerr;
        // timestamp (seconds)
        std::time_t t = std::time(nullptr);
        os << '[' << level_tag(lvl) << ' ' << std::put_time(std::localtime(&t), "%H:%M:%S") << "] "
           << msg << '\n';
    }

  private:
    std::atomic<uint8_t> level_{static_cast<uint8_t>(LogLevel::Info)};

    static const char* level_tag(LogLevel lvl) {
        switch (lvl) {
            case LogLevel::Error:
                return "error";
            case LogLevel::Warn:
                return "warn";
            case LogLevel::Info:
                return "info";
            case LogLevel::Debug:
                return "debug";
        }
        return "log";
    }
};

class LogLine {
  public:
    LogLine(LogLevel lvl) : lvl_(lvl), enabled_(Logger::instance().should(lvl)) {}
    ~LogLine() {
        if (enabled_)
            Logger::instance().emit(lvl_, oss_.str());
    }

    template <typename T> LogLine& operator<<(const T& v) {
        if (enabled_)
            oss_ << v;
        return *this;
    }

  private:
    LogLevel lvl_;
    bool enabled_;
    std::ostringstream oss_;
};

} // namespace uma::util

#define UMA_LOG_DEBUG() ::uma::util::LogLine(::uma::util::LogLevel::Debug)
#define UMA_LOG_INFO() ::uma::util::LogLine(::uma::util::LogLevel::Info)
#define UMA_LOG_WARN() ::uma::util::LogLine(::uma::util::LogLevel::Warn)
#define UMA_LOG_ERROR() ::uma::util::LogLine(::uma::util::LogLevel::Error)
