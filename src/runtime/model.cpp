// UMA Serve - ModelHandle (RAII)
#include "runtime/model.h"

#include "llama.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace uma::runtime {

// ---- Backend Guard ----

LlamaBackendGuard::LlamaBackendGuard() {
    llama_backend_init();
}

LlamaBackendGuard::~LlamaBackendGuard() {
    llama_backend_free();
}

// ---- ModelHandle ----

static llama_model_params make_model_params(const RuntimeConfig& cfg) {
    auto mp = llama_model_default_params();
    mp.use_mlock = cfg.use_mlock;
    mp.use_mmap  = cfg.use_mmap;
    // Leave GPU offload defaults as-is to allow Metal/Vulkan auto-routing
    return mp;
}

static llama_context_params make_context_params(const RuntimeConfig& cfg) {
    auto cp = llama_context_default_params();
    if (cfg.n_ctx > 0) {
        cp.n_ctx = cfg.n_ctx;
    }
    if (cfg.n_threads > 0) {
        cp.n_threads = cfg.n_threads;
        cp.n_threads_batch = cfg.n_threads;
    }
    // enable multi-sequence for batching
    cp.n_seq_max = std::max<uint32_t>(cfg.max_sessions, 1);
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;
    cp.offload_kqv = cfg.offload_kqv; // let backend move KQV to device if capable
    cp.kv_unified  = cfg.kv_unified;  // persistent unified KV allocator buffer
    cp.swa_full    = cfg.swa_full;    // persistent SWA cache
    // Keep perf timers off in daemon
    cp.no_perf = true;
    cp.op_offload = true;
    return cp;
}

ModelHandle::ModelHandle(const RuntimeConfig& cfg) : cfg_(cfg) {
    if (cfg_.model_path.empty()) {
        throw std::invalid_argument("Model path not provided. Use --model or UMA_MODEL.");
    }

    const auto mp = make_model_params(cfg_);
    model_ = llama_model_load_from_file(cfg_.model_path.c_str(), mp);
    if (!model_) {
        throw std::runtime_error(std::string("Failed to load model: ") + cfg_.model_path);
    }

    auto cp = make_context_params(cfg_);
    ctx_params_ = std::make_unique<llama_context_params>(cp);
}

ModelHandle::~ModelHandle() {
    if (model_) {
        llama_model_free(model_);
        model_ = nullptr;
    }
}

std::unique_ptr<llama_context, void(*)(llama_context*)> ModelHandle::new_context() const {
    auto* ctx = llama_init_from_model(model_, *ctx_params_);
    if (!ctx) {
        throw std::runtime_error("Failed to create llama_context");
    }
    return std::unique_ptr<llama_context, void(*)(llama_context*)>(ctx, [](llama_context* c){ if (c) llama_free(c); });
}

} // namespace uma::runtime
