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
        // precise decode timing snapshot
        << "\"decode_calls\":" << decode_calls.load(std::memory_order_relaxed) << ','
        << "\"decode_ns_total\":" << decode_ns_total.load(std::memory_order_relaxed) << ','
        << "\"decode_tokens_total\":" << decode_tokens_total.load(std::memory_order_relaxed) << ','
        << "\"decode_ms_min\":" << decode_ms_min.load(std::memory_order_relaxed) << ','
        << "\"decode_ms_max\":" << decode_ms_max.load(std::memory_order_relaxed) << ','
        // derived mean (ms); avoid div by zero
        << "\"decode_ms_mean\":";
    {
        uint64_t calls = decode_calls.load(std::memory_order_relaxed);
        if (calls == 0) {
            oss << 0.0;
        } else {
            long double ns_tot = static_cast<long double>(decode_ns_total.load(std::memory_order_relaxed));
            long double ms_mean = (ns_tot / static_cast<long double>(calls)) / 1.0e6L;
            oss << std::fixed << std::setprecision(3) << static_cast<double>(ms_mean);
        }
    }
    oss << ','
        // mean tokens per call (batch size)
        << "\"decode_tokens_per_call_mean\":";
    {
        uint64_t calls = decode_calls.load(std::memory_order_relaxed);
        if (calls == 0) {
            oss << 0.0;
        } else {
            long double ttot = static_cast<long double>(decode_tokens_total.load(std::memory_order_relaxed));
            long double mean = ttot / static_cast<long double>(calls);
            oss << std::fixed << std::setprecision(3) << static_cast<double>(mean);
        }
    }
    oss << ','
        // split accounting between generation (DECODE) and PREFILL
        << "\"decode_phase_tokens_total\":" << decode_phase_tokens_total.load(std::memory_order_relaxed) << ','
        << "\"prefill_tokens_total\":" << prefill_tokens_total.load(std::memory_order_relaxed) << ','
        << "\"decode_ns_total_gen\":" << decode_ns_total_gen.load(std::memory_order_relaxed) << ','
        << "\"prefill_ns_total\":" << prefill_ns_total.load(std::memory_order_relaxed) << ','
        // derived: ms/token means
        << "\"gen_ms_per_token_mean\":";
    {
        uint64_t gen_toks = decode_phase_tokens_total.load(std::memory_order_relaxed);
        if (gen_toks == 0) {
            oss << 0.0;
        } else {
            long double ns = static_cast<long double>(decode_ns_total_gen.load(std::memory_order_relaxed));
            long double ms_per_tok = (ns / static_cast<long double>(gen_toks)) / 1.0e6L;
            oss << std::fixed << std::setprecision(3) << static_cast<double>(ms_per_tok);
        }
    }
    oss << ','
        << "\"prefill_ms_per_token_mean\":";
    {
        uint64_t pf_toks = prefill_tokens_total.load(std::memory_order_relaxed);
        if (pf_toks == 0) {
            oss << 0.0;
        } else {
            long double ns = static_cast<long double>(prefill_ns_total.load(std::memory_order_relaxed));
            long double ms_per_tok = (ns / static_cast<long double>(pf_toks)) / 1.0e6L;
            oss << std::fixed << std::setprecision(3) << static_cast<double>(ms_per_tok);
        }
    }
    oss << ','
        << "\"active_sessions\":" << active_sessions
        << '}';
    return oss.str();
}

} // namespace uma::metrics
