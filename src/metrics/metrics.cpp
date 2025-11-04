// UMA Serve - Minimal metrics (M4 stub)
#include "metrics/metrics.h"

#include <sstream>
#include <iomanip>

namespace uma::metrics {

void Metrics::set_decode_ms_ewma(double ms) {
    if (ms < 0) ms = 0;
    uint32_t fx = (uint32_t) (ms * 1000.0);
    decode_ms_ewma_x1000.store(fx, std::memory_order_relaxed);
}

double Metrics::get_decode_ms_ewma() const {
    return decode_ms_ewma_x1000.load(std::memory_order_relaxed) / 1000.0;
}

static void json_escape(std::ostringstream &oss, const std::string &s) {
    for (unsigned char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (c < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c << std::dec;
                } else {
                    oss << c;
                }
        }
    }
}

std::string Metrics::to_json(uint32_t active_sessions) const {
    std::ostringstream oss;
    oss << '{'
        << "\"tokens_generated_total\":" << tokens_generated_total.load(std::memory_order_relaxed) << ','
        << "\"batch_calls_total\":" << batch_calls_total.load(std::memory_order_relaxed) << ','
        << "\"last_batch_size\":" << last_batch_size.load(std::memory_order_relaxed) << ','
        << "\"decode_ms_last\":" << decode_ms_last.load(std::memory_order_relaxed) << ','
        << "\"decode_ms_ewma\":" << std::fixed << std::setprecision(3) << get_decode_ms_ewma() << ','
        << "\"active_sessions\":" << active_sessions
        << '}';
    return oss.str();
}

} // namespace uma::metrics

