// UMA Serve — Daemon + model lifecycle (M1 subset)

#include "runtime/config.h"
#include "runtime/model.h"
#include "runtime/infer.h"
#include "ipc/uds_server.h"
#include "ipc/session.h"

#include "llama.h"

#include <atomic>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <cerrno>

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

        // UDS server (kqueue, multi-client)
        uma::ipc::UDSServer server(cfg.socket_path, cfg.socket_mode);
        if (!server.open_listen()) {
            std::cerr << "Failed to open UDS listen socket\n";
            return 2;
        }

        int kq = ::kqueue();
        if (kq < 0) {
            std::perror("kqueue");
            return 2;
        }

        auto reg_ev = [&](int fd, int16_t filter, uint16_t flags) {
            struct kevent kev;
            EV_SET(&kev, fd, filter, flags, 0, 0, nullptr);
            if (::kevent(kq, &kev, 1, nullptr, 0, nullptr) < 0) {
                std::perror("kevent(reg)");
            }
        };

        // register listen fd for read
        reg_ev(server.fd(), EVFILT_READ, EV_ADD);

        // session pool
        uma::ipc::SessionPool sessions;

        auto now_ns = [](){
            using namespace std::chrono;
            return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        };

        auto close_session = [&](int cfd){
            auto it = sessions.find(cfd);
            if (it != sessions.end()) {
                if (it->second->ctx) llama_free(it->second->ctx);
                ::close(cfd);
                sessions.erase(it);
            } else {
                ::close(cfd);
            }
            // deregister filters
            reg_ev(cfd, EVFILT_READ, EV_DELETE);
            reg_ev(cfd, EVFILT_WRITE, EV_DELETE);
        };

        std::cout << "Ready. Connect with: nc -U " << cfg.socket_path << "\n";

        // main event loop
        while (!g_shutdown.load(std::memory_order_relaxed)) {
            struct kevent events[64];
            timespec ts{0, 200 * 1000 * 1000}; // 200ms tick
            int nev = ::kevent(kq, nullptr, 0, events, 64, &ts);
            if (nev < 0) {
                if (errno == EINTR) continue;
                std::perror("kevent(wait)");
                break;
            }

            for (int i = 0; i < nev; ++i) {
                auto &ev = events[i];
                int fd = static_cast<int>(ev.ident);
                if (fd == server.fd() && ev.filter == EVFILT_READ) {
                    // accept new client
                    sockaddr_un su; socklen_t sl = sizeof(su);
                    int cfd = ::accept(server.fd(), (sockaddr*)&su, &sl);
                    if (cfd >= 0) {
                        // non-blocking + no-sigpipe
                        int fl = ::fcntl(cfd, F_GETFL, 0);
                        if (fl != -1) ::fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
                        int one = 1; ::setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
                        if (sessions.size() >= cfg.max_sessions) {
                            ::close(cfd);
                            continue;
                        }
                        auto sess = std::make_unique<uma::ipc::ClientSession>();
                        sess->fd = cfd;
                        sess->ctx = model.new_context().release();
                        sess->last_activity_ns = now_ns();
                        sessions[cfd] = std::move(sess);
                        reg_ev(cfd, EVFILT_READ, EV_ADD);
                    }
                } else if (ev.filter == EVFILT_READ) {
                    // client read
                    auto it = sessions.find(fd);
                    if (it == sessions.end()) continue;
                    auto & s = *it->second;
                    uint8_t buf[4096];
                    ssize_t n = ::read(fd, buf, sizeof(buf));
                    if (n <= 0) {
                        close_session(fd);
                        continue;
                    }
                    s.rx.insert(s.rx.end(), buf, buf + n);
                    s.last_activity_ns = now_ns();
                    if (s.rx.size() > cfg.max_prompt_bytes) {
                        s.last_error = "prompt too large";
                        s.state = uma::ipc::SessionState::ERRORED;
                    }
                    // Check for newline-terminated prompt
                    auto nl = std::find(s.rx.begin(), s.rx.end(), (uint8_t) '\n');
                    if (nl != s.rx.end()) {
                        std::string prompt(s.rx.begin(), nl);
                        s.rx.erase(s.rx.begin(), nl + 1);
                        // run generation synchronously (sequential compute OK in M2)
                        try {
                            (void)uma::runtime::generate_greedy_stream(
                                s.ctx, model.get(), prompt, (int)cfg.max_tokens,
                                [&](const char* data, size_t len){
                                    s.tx.insert(s.tx.end(), (const uint8_t*)data, (const uint8_t*)data + len);
                                }
                            );
                            s.state = uma::ipc::SessionState::STREAM;
                            // enable write notifications
                            reg_ev(fd, EVFILT_WRITE, EV_ADD);
                        } catch (...) {
                            s.last_error = "generation error";
                            s.state = uma::ipc::SessionState::ERRORED;
                        }
                    }
                } else if (ev.filter == EVFILT_WRITE) {
                    // client write
                    auto it = sessions.find(fd);
                    if (it == sessions.end()) continue;
                    auto & s = *it->second;
                    while (!s.tx.empty()) {
                        ssize_t w = ::write(fd, s.tx.data(), s.tx.size());
                        if (w < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            close_session(fd);
                            goto next_event;
                        }
                        s.tx.erase(s.tx.begin(), s.tx.begin() + w);
                        s.last_activity_ns = now_ns();
                    }
                    if (s.tx.empty()) {
                        // done streaming, stop write notifications
                        reg_ev(fd, EVFILT_WRITE, EV_DELETE);
                        // keep connection open for now to allow multiple prompts per session
                    }
                }
            next_event: ;
            }

            // idle timeout cleanup
            uint64_t now = now_ns();
            uint64_t idle_ns = (uint64_t) cfg.idle_timeout_sec * 1000ull * 1000ull * 1000ull;
            std::vector<int> to_close;
            for (auto & kv : sessions) {
                auto & s = *kv.second;
                if (idle_ns > 0 && now - s.last_activity_ns > idle_ns) {
                    to_close.push_back(s.fd);
                }
            }
            for (int fd_c : to_close) close_session(fd_c);
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
