// UMA Serve - RuntimeConfig (Week 1 minimal)
#pragma once

#include <cstdint>
#include <string>

namespace uma::runtime {

struct RuntimeConfig {
    // Required
    std::string model_path; // path to .gguf

    // Optional knobs (minimal W1)
    uint32_t n_ctx = 4096; // tokens of context window
    int32_t n_threads = 0; // 0 = use ggml default
    uint32_t n_batch = 0;  // 0 = llama.cpp default; otherwise logical max batch
    uint32_t n_ubatch = 0; // 0 = llama.cpp default; otherwise physical micro-batch size

    // IPC (UDS)
    std::string socket_path = "/tmp/uma.sock"; // UDS path
    uint16_t socket_mode = 0600;               // file mode for socket

    // Limits (M2)
    uint32_t max_sessions = 16;
    uint32_t max_prompt_bytes = 8192; // per request
    uint32_t max_tokens = 64;         // per request (default small for responsiveness)
    uint32_t idle_timeout_sec = 300;  // close idle sessions

    // Scheduling (M3)
    uint32_t max_merge = 4; // max sessions to merge per tick
    // Max concurrent sequences in llama context (align with llama-server's --parallel)
    uint32_t n_seq_max = 4; // default 4 to match server behavior

    // SLO instrumentation (for future policy)
    uint32_t slo_ttft_ms = 150; // target TTFT in ms (unused by executor)
    uint32_t slo_tbt_ms = 80;   // target inter-token budget in ms (unused by executor)

    // Bandwidth guard (ΣBMT) experimental budget in dimensionless "token-attention units".
    // 0 disables the guard.
    uint64_t bmt_budget_units = 0;

    // llama.cpp model params
    bool use_mmap = true;
    bool use_mlock = false;

    // llama.cpp context params
    bool offload_kqv = true; // default true in llama.cpp
    bool kv_unified = true;  // enable unified KV buffer for persistence
    bool swa_full = true;    // persistent full-size SWA cache
    bool enable_perf =
            false; // enable llama internal perf counters for debugging (gated by UMA_DEBUG)

    // load from CLI/env (YAML later)
    static RuntimeConfig from_args(int argc, char** argv);
};

} // namespace uma::runtime
