// UMA Serve - Minimal metrics (M4 stub)
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace uma::metrics {

struct Metrics {
    // counters
    std::atomic<uint64_t> tokens_generated_total{0};
    std::atomic<uint64_t> batch_calls_total{0};

    // last decode info
    std::atomic<uint32_t> last_batch_size{0};
    std::atomic<uint32_t> decode_ms_last{0};

    // ewma (x1000 for fixed point to avoid double atomics)
    std::atomic<uint32_t> decode_ms_ewma_x1000{0};

    // Write EWMA (ms) in fixed-point x1000
    void set_decode_ms_ewma(double ms);
    double get_decode_ms_ewma() const;

    // Snapshot to compact JSON string
    // active_sessions is provided by caller at snapshot time
    std::string to_json(uint32_t active_sessions) const;
};

} // namespace uma::metrics

