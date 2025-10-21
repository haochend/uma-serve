// UMA Serve — Daemon + model lifecycle (M1 subset)

#include "runtime/config.h"
#include "runtime/model.h"
#include "runtime/infer.h"
#include "ipc/uds_server.h"

#include "llama.h"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

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
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; // do not set SA_RESTART so accept() is interrupted
        sigaction(SIGINT,  &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    void print_usage() {
        std::cout << "umad - UMA Serve runtime daemon (M1 foundations)\n"
                  << "Usage: umad --model /path/model.gguf [--n-ctx 4096] [--threads N] [--mlock] [--{no-}mmap] [--socket /tmp/uma.sock]\n\n"
                  << "Env: UMA_MODEL, UMA_N_CTX, UMA_THREADS, UMA_USE_MMAP, UMA_USE_MLOCK, UMA_SOCK\n";
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

        // UDS server (single-client blocking, newline-terminated prompt)
        uma::ipc::UDSServer server(cfg.socket_path, cfg.socket_mode);

        auto handler = [&](int cfd) {
            // Read until newline
            std::string prompt;
            char buf[1024];
            while (true) {
                ssize_t n = ::read(cfd, buf, sizeof(buf));
                if (n <= 0) break;
                prompt.append(buf, buf + n);
                auto pos = prompt.find('\n');
                if (pos != std::string::npos) {
                    prompt.resize(pos); // strip newline
                    break;
                }
                // keep reading until newline or EOF
            }

            if (prompt.empty()) {
                return; // nothing to do
            }

            auto write_all = [&](const char* p, size_t nbytes) {
                const char* pcur = p;
                size_t left = nbytes;
                while (left > 0) {
                    ssize_t w = ::write(cfd, pcur, left);
                    if (w <= 0) return false;
                    left -= static_cast<size_t>(w);
                    pcur += w;
                }
                return true;
            };

            // Generate and stream pieces as raw bytes
            try {
                (void)uma::runtime::generate_greedy_stream(
                    admin_ctx.get(), model.get(), prompt, /*max_new_tokens*/256,
                    [&](const char* data, size_t len){
                        (void) write_all(data, len);
                    }
                );
            } catch (const std::exception& e) {
                std::string msg = std::string("error: ") + e.what() + "\n";
                (void) write_all(msg.c_str(), msg.size());
            }
        };

        std::cout << "Ready. Connect with: nc -U " << cfg.socket_path << "\n";
        server.serve(g_shutdown, handler);

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
