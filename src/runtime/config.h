// UMA Serve - RuntimeConfig (Week 1 minimal)
#pragma once

#include <cstdint>
#include <string>

namespace uma::runtime {

struct RuntimeConfig {
    // Required
    std::string model_path; // path to .gguf

    // Optional knobs (minimal W1)
    uint32_t n_ctx = 4096;      // tokens of context window
    int32_t  n_threads = 0;     // 0 = use ggml default

    // llama.cpp model params
    bool use_mmap  = true;
    bool use_mlock = false;

    // llama.cpp context params
    bool offload_kqv = true;   // default true in llama.cpp
    bool kv_unified  = true;   // enable unified KV buffer for persistence
    bool swa_full    = true;   // persistent full-size SWA cache

    // load from CLI/env (YAML later)
    static RuntimeConfig from_args(int argc, char** argv);
};

} // namespace uma::runtime

