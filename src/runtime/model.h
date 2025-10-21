// UMA Serve - ModelHandle (RAII)
#pragma once

#include "runtime/config.h"

#include <atomic>
#include <memory>
#include <string>

// forward decls from llama.h
struct llama_model;
struct llama_context;
struct llama_model_params;
struct llama_context_params;

namespace uma::runtime {

// Llama backend guard (init/free once per process)
class LlamaBackendGuard {
public:
    LlamaBackendGuard();
    ~LlamaBackendGuard();

    LlamaBackendGuard(const LlamaBackendGuard&) = delete;
    LlamaBackendGuard& operator=(const LlamaBackendGuard&) = delete;
};

// RAII handle for a single loaded model
class ModelHandle {
public:
    explicit ModelHandle(const RuntimeConfig& cfg);
    ~ModelHandle();

    ModelHandle(const ModelHandle&) = delete;
    ModelHandle& operator=(const ModelHandle&) = delete;

    // Accessors
    llama_model* get() const { return model_; }
    const RuntimeConfig& cfg() const { return cfg_; }
    const llama_context_params& default_ctx_params() const { return *ctx_params_; }

    // Create a new context bound to the persistent model
    std::unique_ptr<llama_context, void(*)(llama_context*)> new_context() const;

private:
    RuntimeConfig cfg_{};
    llama_model* model_ = nullptr;
    std::unique_ptr<llama_context_params> ctx_params_;
};

} // namespace uma::runtime

