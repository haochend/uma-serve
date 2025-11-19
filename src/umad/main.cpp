// UMA Serve — Daemon + model lifecycle (M1 subset)

#include "ipc/poller.h"
#include "ipc/session.h"
#include "ipc/session_manager.h"
#include "ipc/uds_server.h"
#include "metrics/metrics.h"
#include "runtime/config.h"
#include "runtime/model.h"
#include "runtime/tokens.h"
#include "sched/scheduler.h"
#include "util/logging.h"
#include "util/utf8.h"

#include "llama.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using uma::runtime::LlamaBackendGuard;
using uma::runtime::ModelHandle;
using uma::runtime::RuntimeConfig;

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
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void print_usage() {
    std::cout << "umad - UMA Serve runtime daemon (M1 foundations)\n"
              << "Usage: umad --model /path/model.gguf [--n-ctx 4096] [--threads N] [--mlock] "
                 "[--{no-}mmap] [--socket /tmp/uma.sock]\n\n"
              << "Env: UMA_MODEL, UMA_N_CTX, UMA_THREADS, UMA_USE_MMAP, UMA_USE_MLOCK, UMA_SOCK\n";
}

// Validate basic UTF-8 correctness (reject overlong & invalid ranges)
bool is_valid_utf8(const std::string& s) {
    unsigned int remaining = 0;
    unsigned char lead = 0;
    unsigned int pos = 0;
    for (unsigned char c : s) {
        if (remaining == 0) {
            if (c <= 0x7F) {
                continue;
            } else if (c >= 0xC2 && c <= 0xDF) {
                remaining = 1;
                lead = c;
                pos = 0;
            } else if (c >= 0xE0 && c <= 0xEF) {
                remaining = 2;
                lead = c;
                pos = 0;
            } else if (c >= 0xF0 && c <= 0xF4) {
                remaining = 3;
                lead = c;
                pos = 0;
            } else {
                return false; // overlong leads C0/C1 or > F4
            }
        } else {
            if ((c & 0xC0) != 0x80)
                return false;
            ++pos;
            if (pos == 1) {
                // first continuation has extra constraints for some leads
                if (lead == 0xE0 && c < 0xA0)
                    return false; // overlong 3-byte
                if (lead == 0xED && c > 0x9F)
                    return false; // UTF-16 surrogate
                if (lead == 0xF0 && c < 0x90)
                    return false; // overlong 4-byte
                if (lead == 0xF4 && c > 0x8F)
                    return false; // > U+10FFFF
            }
            if (--remaining == 0) {
                lead = 0;
                pos = 0;
            }
        }
    }
    return remaining == 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        // Configure logging (UMA_LOG_LEVEL / UMA_DEBUG)
        uma::util::Logger::instance().configure_from_env();

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

        UMA_LOG_INFO() << "UMA Serve daemon starting…";
        UMA_LOG_DEBUG() << "llama.cpp system info:\n" << llama_print_system_info();

        install_signal_handlers();

        // Init backend and load model once
        LlamaBackendGuard backend_guard;
        ModelHandle model(cfg);

        UMA_LOG_INFO() << "Model loaded: " << cfg.model_path;
        UMA_LOG_INFO() << "n_ctx=" << cfg.n_ctx << " threads=" << cfg.n_threads
                       << " mmap=" << (cfg.use_mmap ? "on" : "off")
                       << " mlock=" << (cfg.use_mlock ? "on" : "off")
                       << " kv_unified=" << (cfg.kv_unified ? "on" : "off");

        // Create a persistent admin context now so params take effect
        auto admin_ctx = model.new_context();
        UMA_LOG_INFO() << "Context ready: n_ctx_resolved=" << llama_n_ctx(admin_ctx.get())
                       << " n_batch_resolved=" << llama_n_batch(admin_ctx.get())
                       << " n_threads=" << cfg.n_threads;
        UMA_LOG_DEBUG() << "model_has_encoder="
                        << (llama_model_has_encoder(model.get()) ? "true" : "false")
                        << " n_seq_max=" << llama_n_seq_max(admin_ctx.get());

        // UDS server (kqueue, multi-client)
        uma::ipc::UDSServer server(cfg.socket_path, cfg.socket_mode);
        if (!server.open_listen()) {
            UMA_LOG_ERROR() << "Failed to open UDS listen socket";
            return 2;
        }

        uma::ipc::Poller poller;
        poller.add(server.fd(), uma::ipc::PollFlags::Read);

        // sessions
        uma::ipc::SessionManager sessions;
        llama_context* gctx = admin_ctx.get();
        const llama_vocab* vocab = llama_model_get_vocab(model.get());
        // session seq ids are managed by SessionManager

        // Metrics (M4 stub)
        uma::metrics::Metrics mtx;
        // scheduler hook
        uma::sched::Scheduler scheduler(gctx, vocab, cfg, &mtx);

        auto now_ns = []() {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        };

        UMA_LOG_INFO() << "Ready. Connect with: nc -U " << cfg.socket_path;

        // main event loop
        while (!g_shutdown.load(std::memory_order_relaxed)) {
            struct kevent events[64];
            // Dynamic timeout: if any session has ready work, don't sleep; otherwise idle for 200ms
            bool has_ready_work = false;
            for (auto& kv : sessions.map()) {
                auto& s = *kv.second;
                if ((s.state == uma::ipc::SessionState::PREFILL &&
                     s.prefill_idx < s.prompt_tokens.size()) ||
                    (s.state == uma::ipc::SessionState::DECODE && s.has_pending_tok)) {
                    has_ready_work = true;
                    break;
                }
            }
            int time_ms = 0;
            if (!has_ready_work) {
                time_ms = 200 * 1000 * 1000;
            }
            std::vector<uma::ipc::PollEvent> ready_events;
            int nev = poller.wait(time_ms, ready_events);
            if (nev < 0) {
                if (errno == EINTR)
                    continue;
                std::perror("kevent(wait)");
                break;
            }

            for (auto& ev : ready_events) {
                if (ev.fd == server.fd() && ev.readable()) {
                    // accept new client
                    sockaddr_un su;
                    socklen_t sl = sizeof(su);
                    int cfd = ::accept(server.fd(), (sockaddr*)&su, &sl);
                    if (cfd >= 0) {
                        // non-blocking + no-sigpipe
                        int fl = ::fcntl(cfd, F_GETFL, 0);
                        if (fl != -1)
                            ::fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
                        int one = 1;
                        ::setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
                        if (sessions.map().size() >= cfg.max_sessions) {
                            ::close(cfd);
                            continue;
                        }
                        sessions.add_client(cfd, now_ns());
                        poller.add(cfd, uma::ipc::PollFlags::Read);
                        UMA_LOG_DEBUG() << "[accept] fd=" << cfd;
                    }
                } else if (ev.readable()) {
                    // client read via SessionManager
                    auto rr = sessions.on_readable(ev.fd, cfg, vocab, now_ns());
                    auto* sp = sessions.find(ev.fd);
                    if (!sp) goto next_event;
                    auto& s = *sp;
                    if (rr.admin_request) {
                        std::string js = mtx.to_json((uint32_t)sessions.map().size());
                        s.tx.insert(s.tx.end(), js.begin(), js.end());
                        s.tx.push_back('\n');
                        // One-shot admin response: close after flushing
                        s.state = uma::ipc::SessionState::STREAM;
                        s.read_closed = true;
                        poller.remove(ev.fd, uma::ipc::PollFlags::Read);
                        rr.wants_write = true;
                    }
                    if (rr.removed_read) {
                        poller.remove(ev.fd, uma::ipc::PollFlags::Read);
                    }
                    if (rr.wants_write && !s.tx.empty()) {
                        // Try an immediate non-blocking drain; then arm write notifications if needed.
                        ssize_t w = ::write(ev.fd, s.tx.data(), s.tx.size());
                        if (w > 0) {
                            UMA_LOG_DEBUG() << "[write-now] fd=" << ev.fd << " wrote(rx)=" << w;
                            s.tx.erase(s.tx.begin(), s.tx.begin() + w);
                        }
                        if (!s.tx.empty())
                            poller.add(ev.fd, uma::ipc::PollFlags::Write);
                    }
                } else if (ev.writable()) {
                    // client write
                    auto* itp = sessions.find(ev.fd);
                    if (!itp)
                        continue;
                    auto& s = *itp;
                    while (!s.tx.empty()) {
                        ssize_t w = ::write(ev.fd, s.tx.data(), s.tx.size());
                        if (w < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK)
                                break;
                            sessions.close(ev.fd, poller, gctx);
                            goto next_event;
                        }
                        UMA_LOG_DEBUG() << "[write] fd=" << ev.fd << " wrote=" << w
                                        << " tx_left=" << (s.tx.size() - (size_t)w);
                        s.tx.erase(s.tx.begin(), s.tx.begin() + w);
                        s.last_activity_ns = now_ns();
                    }
                    if (s.tx.empty()) {
                        // done streaming, stop write notifications
                        poller.remove(ev.fd, uma::ipc::PollFlags::Write);
                        if (s.state == uma::ipc::SessionState::ERRORED) {
                            // close errored sessions after flushing error
                            sessions.close(ev.fd, poller, gctx);
                        } else if (s.state == uma::ipc::SessionState::STREAM) {
                            // finished a response
                            if (s.read_closed) {
                                sessions.close(ev.fd, poller, gctx);
                            } else {
                                s.state = uma::ipc::SessionState::RECV_REQ;
                                s.prompt_tokens.clear();
                                s.prefill_idx = 0;
                                s.generated_count = 0;
                                s.has_pending_tok = false;
                            }
                        }
                    }
                }
            next_event:;
            }

            // idle timeout cleanup
            uint64_t now = now_ns();
            uint64_t idle_ns = (uint64_t)cfg.idle_timeout_sec * 1000ull * 1000ull * 1000ull;
            std::vector<int> to_close;
            for (auto& kv : sessions.map()) {
                auto& s = *kv.second;
                if (idle_ns > 0 && now - s.last_activity_ns > idle_ns) {
                    to_close.push_back(s.fd);
                }
            }
            for (int fd_c : to_close)
                sessions.close(fd_c, poller, gctx);

            // ---- M3 Scheduler tick: build global batch from ready sessions ----
            // Two-phase policy per tick: (A) 1 token per DECODE session, (B) PREFILL drain in
            // chunks
            {
                auto fds_to_arm = scheduler.tick(sessions.map(), now_ns());
                for (int fd : fds_to_arm) {
                    auto* itp = sessions.find(fd);
                    if (itp && !itp->tx.empty()) {
                        poller.add(fd, uma::ipc::PollFlags::Write);
                    }
                }
            }
        } // end main event loop

        UMA_LOG_INFO() << "Shutdown requested. Draining & cleaning up…";
        // Contexts would be drained here when implemented.
        // Model + backend are freed by RAII destructors.

        UMA_LOG_INFO() << "Goodbye.";
        return 0;

    } catch (const std::exception& e) {
        UMA_LOG_ERROR() << "Fatal error: " << e.what();
        return 1;
    }
}
