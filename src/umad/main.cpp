// UMA Serve — Daemon + model lifecycle (M1 subset)

#include "runtime/config.h"
#include "runtime/model.h"

#include "llama.h"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

using uma::runtime::RuntimeConfig;
using uma::runtime::LlamaBackendGuard;
using uma::runtime::ModelHandle;

namespace {
    std::atomic<bool> g_shutdown{false};

    void signal_handler(int sig) {
        (void)sig;
        g_shutdown.store(true, std::memory_order_relaxed);
    }

    void install_signal_handlers() {
        std::signal(SIGINT,  signal_handler);
        std::signal(SIGTERM, signal_handler);
    }

    void print_usage() {
        std::cout << "umad - UMA Serve runtime daemon (M1 foundations)\n"
                  << "Usage: umad --model /path/model.gguf [--n-ctx 4096] [--threads N] [--mlock] [--{no-}mmap]\n\n"
                  << "Env: UMA_MODEL, UMA_N_CTX, UMA_THREADS, UMA_USE_MMAP, UMA_USE_MLOCK\n";
    }
}

int main(int argc, char** argv) {
    try {
        // Parse config (YAML later)
        RuntimeConfig cfg;
        try {
            cfg = RuntimeConfig::from_args(argc, argv);
        } catch (const std::invalid_argument& e) {
            if (std::string(e.what()) == "help") {
                print_usage();
                return 0;
            }
            std::cerr << "Argument error: " << e.what() << "\n";
            print_usage();
            return 2;
        }

        if (cfg.model_path.empty()) {
            std::cerr << "Error: --model or UMA_MODEL is required.\n";
            print_usage();
            return 2;
        }
        if (!std::filesystem::exists(cfg.model_path)) {
            std::cerr << "Error: model file not found: " << cfg.model_path << "\n";
            return 2;
        }

        std::cout << "UMA Serve daemon starting…\n";
        std::cout << "llama.cpp system info:\n" << llama_print_system_info() << "\n";

        install_signal_handlers();

        // Init backend and load model once
        LlamaBackendGuard backend_guard;
        ModelHandle model(cfg);

        std::cout << "Model loaded: " << cfg.model_path << "\n";
        std::cout << "n_ctx=" << cfg.n_ctx
                  << " threads=" << cfg.n_threads
                  << " mmap=" << (cfg.use_mmap ? "on" : "off")
                  << " mlock=" << (cfg.use_mlock ? "on" : "off")
                  << " kv_unified=" << (cfg.kv_unified ? "on" : "off")
                  << "\n";

        // Create a persistent admin context now so params take effect
        auto admin_ctx = model.new_context();
        std::cout << "Context ready: n_ctx_resolved=" << llama_n_ctx(admin_ctx.get())
                  << " n_batch_resolved=" << llama_n_batch(admin_ctx.get())
                  << " n_threads=" << cfg.n_threads
                  << "\n";

        // For M1 section, just keep the daemon alive until signal.
        // Later sections will add UDS server & I/O loops.
        while (!g_shutdown.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        std::cout << "Shutdown requested. Draining & cleaning up…\n";
        // Contexts would be drained here when implemented.
        // Model + backend are freed by RAII destructors.

        std::cout << "Goodbye.\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
