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

    // precise decode timing aggregation (for analysis)
    // All times are wall-clock durations for llama_decode + synchronize
    std::atomic<uint64_t> decode_ns_total{0};  // sum of decode durations in nanoseconds
    std::atomic<uint64_t> decode_calls{0};     // number of decode measurements (mirrors batch_calls_total)
    std::atomic<uint64_t> decode_tokens_total{0}; // total tokens processed in decode batches
    std::atomic<uint32_t> decode_ms_min{0xFFFFFFFFu};
    std::atomic<uint32_t> decode_ms_max{0};

    // Split accounting: generation (DECODE phase) vs PREFILL tokens
    std::atomic<uint64_t> decode_phase_tokens_total{0}; // tokens that were part of DECODE phase
    std::atomic<uint64_t> prefill_tokens_total{0};      // tokens that were part of PREFILL phase
    std::atomic<uint64_t> decode_ns_total_gen{0};       // estimated time spent on DECODE tokens (ns)
    std::atomic<uint64_t> prefill_ns_total{0};          // estimated time spent on PREFILL tokens (ns)

    // per-tick breakdown observability
    std::atomic<uint32_t> last_decode_tokens{0};
    std::atomic<uint32_t> last_prefill_tokens{0};
    std::atomic<uint32_t> max_batch_size_seen{0};
    std::atomic<uint64_t> prefill_calls{0};

    // llama internal perf (optional: when perf enabled)
    std::atomic<uint32_t> eval_ms_last{0};
    std::atomic<uint32_t> p_eval_ms_last{0};
    std::atomic<uint64_t> eval_ns_total{0};
    std::atomic<uint64_t> p_eval_ns_total{0};
    std::atomic<uint64_t> eval_calls{0};
    std::atomic<uint64_t> p_eval_calls{0};

    // ΣBMT guard observability (experimental)
    std::atomic<uint64_t> bmt_units_last{0};
    std::atomic<uint64_t> bmt_budget_units{0};
    std::atomic<uint32_t> bmt_guard_activations{0};
    std::atomic<uint8_t> bmt_guard_active{0};

    // Write EWMA (ms) in fixed-point x1000
    void set_decode_ms_ewma(double ms);
    double get_decode_ms_ewma() const;

    // Snapshot to compact JSON string
    // active_sessions is provided by caller at snapshot time
    // When debug=true, include extended fields (perf + batch-shape)
    std::string to_json(uint32_t active_sessions, bool debug = false) const;
};

} // namespace uma::metrics
