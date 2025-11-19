// UMA Serve — Daemon + model lifecycle (M1 subset)

#include "ipc/poller.h"
#include "ipc/session.h"
#include "ipc/uds_server.h"
#include "metrics/metrics.h"
#include "runtime/config.h"
#include "runtime/model.h"
#include "sched/scheduler.h"
#include "util/logging.h"

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

        // session pool
        uma::ipc::SessionPool sessions;
        llama_context* gctx = admin_ctx.get();
        const llama_vocab* vocab = llama_model_get_vocab(model.get());
        int32_t next_seq_id = 1;

        // Metrics (M4 stub)
        uma::metrics::Metrics mtx;
        // scheduler hook
        uma::sched::Scheduler scheduler(gctx, vocab, cfg, &mtx);

        auto now_ns = []() {
            using namespace std::chrono;
            return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        };

        auto close_session = [&](int cfd) {
            // deregister filters
            poller.remove(cfd, uma::ipc::PollFlags::Read | uma::ipc::PollFlags::Write);

            auto it = sessions.find(cfd);
            if (it != sessions.end()) {
                // clear sequence KV memory for reuse
                if (it->second->seq >= 0) {
                    llama_memory_seq_rm(llama_get_memory(gctx), it->second->seq, -1, -1);
                }
                if (it->second->ctx)
                    llama_free(it->second->ctx);
                ::close(cfd);
                sessions.erase(it);
            } else {
                ::close(cfd);
            }
        };

        UMA_LOG_INFO() << "Ready. Connect with: nc -U " << cfg.socket_path;

        // main event loop
        while (!g_shutdown.load(std::memory_order_relaxed)) {
            struct kevent events[64];
            // Dynamic timeout: if any session has ready work, don't sleep; otherwise idle for 200ms
            bool has_ready_work = false;
            for (auto& kv : sessions) {
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
                        if (sessions.size() >= cfg.max_sessions) {
                            ::close(cfd);
                            continue;
                        }
                        auto sess = std::make_unique<uma::ipc::ClientSession>();
                        sess->fd = cfd;
                        sess->ctx = nullptr; // using global context for M3 batching
                        sess->last_activity_ns = now_ns();
                        sessions[cfd] = std::move(sess);
                        poller.add(cfd, uma::ipc::PollFlags::Read);
                        UMA_LOG_DEBUG() << "[accept] fd=" << cfd << " sessions=" << sessions.size();
                    }
                } else if (ev.readable()) {
                    // client read
                    auto it = sessions.find(ev.fd);
                    if (it == sessions.end())
                        continue;
                    auto& s = *it->second;
                    uint8_t buf[4096];
                    bool saw_eof = false;
                    for (;;) {
                        ssize_t n = ::read(ev.fd, buf, sizeof(buf));
                        if (n > 0) {
                            if (s.rx.size() + (size_t)n > cfg.max_prompt_bytes) {
                                const std::string msg = "error: prompt too large (limit " +
                                                        std::to_string(cfg.max_prompt_bytes) +
                                                        ")\n";
                                s.tx.insert(s.tx.end(), msg.begin(), msg.end());
                                s.state = uma::ipc::SessionState::ERRORED;
                                poller.remove(ev.fd, uma::ipc::PollFlags::Read);
                                poller.add(ev.fd, uma::ipc::PollFlags::Write);
                                break;
                            }
                            s.rx.insert(s.rx.end(), buf, buf + n);
                            s.last_activity_ns = now_ns();
                            continue; // try to drain more
                        }
                        if (n == 0) {
                            saw_eof = true;
                            break;
                        }
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        // other error
                        close_session(ev.fd);
                        goto next_event;
                    }
                    if (saw_eof) {
                        s.read_closed = true;
                        poller.remove(ev.fd, uma::ipc::PollFlags::Read);
                        s.last_activity_ns = now_ns();
                    }
                    // Check for newline-terminated prompt
                    auto nl = std::find(s.rx.begin(), s.rx.end(), (uint8_t)'\n');
                    if (nl != s.rx.end()) {
                        std::string prompt(s.rx.begin(), nl);
                        s.rx.erase(s.rx.begin(), nl + 1);
                        if (s.state != uma::ipc::SessionState::RECV_REQ) {
                            const std::string emsg = "error: busy\n";
                            s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
                            poller.add(ev.fd, uma::ipc::PollFlags::Write);
                            continue;
                        }
                        // Admin: metrics endpoint via UDS prompt
                        if (prompt == "/metrics" || prompt == "metrics") {
                            std::string js = mtx.to_json((uint32_t)sessions.size());
                            s.tx.insert(s.tx.end(), js.begin(), js.end());
                            s.tx.push_back('\n');
                            // One-shot admin response: close after flushing
                            s.state = uma::ipc::SessionState::STREAM;
                            s.read_closed = true; // trigger close after tx drains
                            poller.remove(ev.fd, uma::ipc::PollFlags::Read);
                            poller.add(ev.fd, uma::ipc::PollFlags::Write);
                            continue;
                        }
                        // trim trailing CR
                        if (!prompt.empty() && prompt.back() == '\r')
                            prompt.pop_back();
                        // reject embedded NUL
                        if (prompt.find('\0') != std::string::npos) {
                            const std::string emsg = "error: invalid input (NUL)\n";
                            s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
                            s.state = uma::ipc::SessionState::ERRORED;
                            poller.remove(ev.fd, uma::ipc::PollFlags::Read);
                            poller.add(ev.fd, uma::ipc::PollFlags::Write);
                            continue;
                        }
                        // UTF-8 validation
                        if (!is_valid_utf8(prompt)) {
                            const std::string emsg = "error: invalid utf-8\n";
                            s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
                            s.state = uma::ipc::SessionState::ERRORED;
                            poller.remove(ev.fd, uma::ipc::PollFlags::Read);
                            poller.add(ev.fd, uma::ipc::PollFlags::Write);
                            continue;
                        }
                        // tokenize prompt and transition to PREFILL
                        s.prompt_tokens.clear();
                        const int n_prompt = -llama_tokenize(
                                vocab, prompt.c_str(), (int)prompt.size(), nullptr, 0, true, true);
                        if (n_prompt > 0) {
                            s.prompt_tokens.resize((size_t)n_prompt);
                            if (llama_tokenize(vocab, prompt.c_str(), (int)prompt.size(),
                                               (llama_token*)s.prompt_tokens.data(), n_prompt, true,
                                               true) < 0) {
                                const std::string emsg = "error: tokenize failed\n";
                                s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
                                s.state = uma::ipc::SessionState::ERRORED;
                                poller.remove(ev.fd, uma::ipc::PollFlags::Read);
                                poller.add(ev.fd, uma::ipc::PollFlags::Write);
                                continue;
                            }
                            // For responsiveness (esp. on large models), echo the prompt pieces
                            // immediately. This guarantees clients see bytes even before first
                            // decode completes.
                            for (int id : s.prompt_tokens) {
                                char pbuf[256];
                                int pn = llama_token_to_piece(vocab, (llama_token)id, pbuf,
                                                              sizeof(pbuf), 0, true);
                                if (pn > 0)
                                    s.tx.insert(s.tx.end(), (uint8_t*)pbuf, (uint8_t*)pbuf + pn);
                            }
                            if (!s.tx.empty()) {
                                // Try an immediate non-blocking drain; then arm write notifications
                                // if needed.
                                ssize_t w = ::write(ev.fd, s.tx.data(), s.tx.size());
                                if (w > 0) {
                                    UMA_LOG_DEBUG()
                                            << "[write-now] fd=" << ev.fd << " wrote(prompt)=" << w;
                                    s.tx.erase(s.tx.begin(), s.tx.begin() + w);
                                }
                                if (!s.tx.empty())
                                    poller.add(ev.fd, uma::ipc::PollFlags::Write);
                            }
                            s.prefill_idx = 0;
                            s.generated_count = 0;
                            s.has_pending_tok = false;
                            if (s.seq < 0)
                                s.seq = next_seq_id++;
                            s.state = uma::ipc::SessionState::PREFILL;
                            UMA_LOG_DEBUG() << "[prompt] fd=" << ev.fd << " seq=" << s.seq
                                            << " n_prompt=" << n_prompt;
                        } else {
                            // empty prompt -> immediate newline
                            s.tx.push_back('\n');
                            poller.add(ev.fd, uma::ipc::PollFlags::Write);
                        }
                    }
                } else if (ev.writable()) {
                    // client write
                    auto it = sessions.find(ev.fd);
                    if (it == sessions.end())
                        continue;
                    auto& s = *it->second;
                    while (!s.tx.empty()) {
                        ssize_t w = ::write(ev.fd, s.tx.data(), s.tx.size());
                        if (w < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK)
                                break;
                            close_session(ev.fd);
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
                            close_session(ev.fd);
                        } else if (s.state == uma::ipc::SessionState::STREAM) {
                            // finished a response
                            if (s.read_closed) {
                                close_session(ev.fd);
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
            for (auto& kv : sessions) {
                auto& s = *kv.second;
                if (idle_ns > 0 && now - s.last_activity_ns > idle_ns) {
                    to_close.push_back(s.fd);
                }
            }
            for (int fd_c : to_close)
                close_session(fd_c);

            // ---- M3 Scheduler tick: build global batch from ready sessions ----
            // Two-phase policy per tick: (A) 1 token per DECODE session, (B) PREFILL drain in
            // chunks
            {
                auto fds_to_arm = scheduler.tick(sessions, now_ns());
                for (int fd : fds_to_arm) {
                    auto it = sessions.find(fd);
                    if (it != sessions.end() && !it->second->tx.empty()) {
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
