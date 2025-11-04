// UMA Serve — Daemon + model lifecycle (M1 subset)

#include "runtime/config.h"
#include "runtime/model.h"
#include "ipc/uds_server.h"
#include "metrics/metrics.h"
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
#include <sstream>

using uma::runtime::RuntimeConfig;
using uma::runtime::LlamaBackendGuard;
using uma::runtime::ModelHandle;

namespace {
    std::atomic<bool> g_shutdown{false};
    bool g_debug = false;

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

    // Validate basic UTF-8 correctness (reject overlong & invalid ranges)
    bool is_valid_utf8(const std::string &s) {
        unsigned int remaining = 0;
        unsigned char lead = 0;
        unsigned int pos = 0;
        for (unsigned char c : s) {
            if (remaining == 0) {
                if (c <= 0x7F) {
                    continue;
                } else if (c >= 0xC2 && c <= 0xDF) {
                    remaining = 1; lead = c; pos = 0;
                } else if (c >= 0xE0 && c <= 0xEF) {
                    remaining = 2; lead = c; pos = 0;
                } else if (c >= 0xF0 && c <= 0xF4) {
                    remaining = 3; lead = c; pos = 0;
                } else {
                    return false; // overlong leads C0/C1 or > F4
                }
            } else {
                if ((c & 0xC0) != 0x80) return false;
                ++pos;
                if (pos == 1) {
                    // first continuation has extra constraints for some leads
                    if (lead == 0xE0 && c < 0xA0) return false;      // overlong 3-byte
                    if (lead == 0xED && c > 0x9F) return false;      // UTF-16 surrogate
                    if (lead == 0xF0 && c < 0x90) return false;      // overlong 4-byte
                    if (lead == 0xF4 && c > 0x8F) return false;      // > U+10FFFF
                }
                if (--remaining == 0) { lead = 0; pos = 0; }
            }
        }
        return remaining == 0;
    }
}

int main(int argc, char** argv) {
    try {
        // Debug toggle via env UMA_DEBUG
        if (const char *d = std::getenv("UMA_DEBUG")) {
            std::string v(d);
            for (auto & c : v) c = (char) std::tolower(c);
            g_debug = (v == "1" || v == "true" || v == "yes" || v == "on");
        }

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
        if (g_debug) {
            std::cout << "llama.cpp system info:\n" << llama_print_system_info() << "\n";
        }

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
        if (g_debug) {
            std::cerr << "[debug] model_has_encoder=" << (llama_model_has_encoder(model.get()) ? "true" : "false")
                      << " n_seq_max=" << llama_n_seq_max(admin_ctx.get()) << "\n";
        }

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
                if ((flags & EV_DELETE) && (errno == ENOENT || errno == EBADF)) {
                    return; // benign: nothing to delete or fd already closed
                }
                std::perror("kevent(reg)");
            }
        };


        // register listen fd for read
        reg_ev(server.fd(), EVFILT_READ, EV_ADD);

        // session pool
        uma::ipc::SessionPool sessions;
        llama_context * gctx = admin_ctx.get();
        const llama_vocab * vocab = llama_model_get_vocab(model.get());
        int32_t next_seq_id = 1;
        size_t rr_cursor_decode = 0; // RR cursor for decode fairness
        size_t rr_cursor_prefill = 0; // RR cursor for prefill fairness

        // Micro-batch capacity resolved by the context
        const int32_t ubatch_cap = llama_n_batch(gctx);
        // Adaptive target size (tokens per tick), start conservative
        int32_t target_batch = std::min<int32_t>(ubatch_cap, 32);
        const double tick_budget_ms = 30.0; // soft budget per decode
        double decode_ms_ewma = tick_budget_ms; // seed EWMA

        // Metrics (M4 stub)
        uma::metrics::Metrics mtx;
        mtx.set_decode_ms_ewma(decode_ms_ewma);

        auto now_ns = [](){
            using namespace std::chrono;
            return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
        };

        auto close_session = [&](int cfd){
            // deregister filters
            reg_ev(cfd, EVFILT_READ, EV_DELETE);
            reg_ev(cfd, EVFILT_WRITE, EV_DELETE);

            auto it = sessions.find(cfd);
            if (it != sessions.end()) {
                // clear sequence KV memory for reuse
                if (it->second->seq >= 0) {
                    llama_memory_seq_rm(llama_get_memory(gctx), it->second->seq, -1, -1);
                }
                if (it->second->ctx) llama_free(it->second->ctx);
                ::close(cfd);
                sessions.erase(it);
            } else {
                ::close(cfd);
            }
        };

        std::cout << "Ready. Connect with: nc -U " << cfg.socket_path << "\n";

        // main event loop
        while (!g_shutdown.load(std::memory_order_relaxed)) {
            struct kevent events[64];
            // Dynamic timeout: if any session has ready work, don't sleep; otherwise idle for 200ms
            bool has_ready_work = false;
            for (auto & kv : sessions) {
                auto & s = *kv.second;
                if ((s.state == uma::ipc::SessionState::PREFILL && s.prefill_idx < s.prompt_tokens.size()) ||
                    (s.state == uma::ipc::SessionState::DECODE && s.has_pending_tok)) {
                    has_ready_work = true; break;
                }
            }
            timespec ts;
            if (has_ready_work) { ts = timespec{0, 0}; }
            else                { ts = timespec{0, 200 * 1000 * 1000}; }
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
                        sess->ctx = nullptr; // using global context for M3 batching
                        sess->last_activity_ns = now_ns();
                        sessions[cfd] = std::move(sess);
                        reg_ev(cfd, EVFILT_READ, EV_ADD);
                        if (g_debug) std::cerr << "[accept] fd=" << cfd << " sessions=" << sessions.size() << "\n";
                    }
                } else if (ev.filter == EVFILT_READ) {
                    // client read
                    auto it = sessions.find(fd);
                    if (it == sessions.end()) continue;
                    auto & s = *it->second;
                    uint8_t buf[4096];
                    bool saw_eof = false;
                    for (;;) {
                        ssize_t n = ::read(fd, buf, sizeof(buf));
                        if (n > 0) {
                            if (s.rx.size() + (size_t)n > cfg.max_prompt_bytes) {
                                const std::string msg = "error: prompt too large (limit " + std::to_string(cfg.max_prompt_bytes) + ")\n";
                                s.tx.insert(s.tx.end(), msg.begin(), msg.end());
                                s.state = uma::ipc::SessionState::ERRORED;
                                reg_ev(fd, EVFILT_READ, EV_DELETE);
                                reg_ev(fd, EVFILT_WRITE, EV_ADD);
                                break;
                            }
                            s.rx.insert(s.rx.end(), buf, buf + n);
                            s.last_activity_ns = now_ns();
                            continue; // try to drain more
                        }
                        if (n == 0) { saw_eof = true; break; }
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        // other error
                        close_session(fd);
                        goto next_event;
                    }
                    if (saw_eof) {
                        s.read_closed = true;
                        reg_ev(fd, EVFILT_READ, EV_DELETE);
                        s.last_activity_ns = now_ns();
                    }
                    // Check for newline-terminated prompt
                    auto nl = std::find(s.rx.begin(), s.rx.end(), (uint8_t) '\n');
                    if (nl != s.rx.end()) {
                        std::string prompt(s.rx.begin(), nl);
                        s.rx.erase(s.rx.begin(), nl + 1);
                        if (s.state != uma::ipc::SessionState::RECV_REQ) {
                            const std::string emsg = "error: busy\n";
                            s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
                            reg_ev(fd, EVFILT_WRITE, EV_ADD);
                            continue;
                        }
                        // Admin: metrics endpoint via UDS prompt
                        if (prompt == "/metrics" || prompt == "metrics") {
                            std::string js = mtx.to_json((uint32_t) sessions.size());
                            s.tx.insert(s.tx.end(), js.begin(), js.end());
                            s.tx.push_back('\n');
                            // One-shot admin response: close after flushing
                            s.state = uma::ipc::SessionState::STREAM;
                            s.read_closed = true; // trigger close after tx drains
                            reg_ev(fd, EVFILT_READ, EV_DELETE);
                            reg_ev(fd, EVFILT_WRITE, EV_ADD);
                            continue;
                        }
                        // trim trailing CR
                        if (!prompt.empty() && prompt.back() == '\r') prompt.pop_back();
                        // reject embedded NUL
                        if (prompt.find('\0') != std::string::npos) {
                            const std::string emsg = "error: invalid input (NUL)\n";
                            s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
                            s.state = uma::ipc::SessionState::ERRORED;
                            reg_ev(fd, EVFILT_WRITE, EV_ADD);
                            reg_ev(fd, EVFILT_READ, EV_DELETE);
                            continue;
                        }
                        // UTF-8 validation
                        if (!is_valid_utf8(prompt)) {
                            const std::string emsg = "error: invalid utf-8\n";
                            s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
                            s.state = uma::ipc::SessionState::ERRORED;
                            reg_ev(fd, EVFILT_WRITE, EV_ADD);
                            reg_ev(fd, EVFILT_READ, EV_DELETE);
                            continue;
                        }
                        // tokenize prompt and transition to PREFILL
                        s.prompt_tokens.clear();
                        const int n_prompt = -llama_tokenize(vocab, prompt.c_str(), (int)prompt.size(), nullptr, 0, true, true);
                        if (n_prompt > 0) {
                            s.prompt_tokens.resize((size_t)n_prompt);
                            if (llama_tokenize(vocab, prompt.c_str(), (int)prompt.size(), (llama_token*)s.prompt_tokens.data(), n_prompt, true, true) < 0) {
                                const std::string emsg = "error: tokenize failed\n";
                                s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
                                s.state = uma::ipc::SessionState::ERRORED;
                                reg_ev(fd, EVFILT_WRITE, EV_ADD);
                                reg_ev(fd, EVFILT_READ, EV_DELETE);
                                continue;
                            }
                            // For responsiveness (esp. on large models), echo the prompt pieces immediately.
                            // This guarantees clients see bytes even before first decode completes.
                            for (int id : s.prompt_tokens) {
                                char pbuf[256];
                                int pn = llama_token_to_piece(vocab, (llama_token) id, pbuf, sizeof(pbuf), 0, true);
                                if (pn > 0) s.tx.insert(s.tx.end(), (uint8_t*)pbuf, (uint8_t*)pbuf + pn);
                            }
                            if (!s.tx.empty()) {
                                // Try an immediate non-blocking drain; then arm write notifications if needed.
                                ssize_t w = ::write(fd, s.tx.data(), s.tx.size());
                                if (w > 0) {
                                    if (g_debug) std::cerr << "[write-now] fd=" << fd << " wrote(prompt)=" << w << "\n";
                                    s.tx.erase(s.tx.begin(), s.tx.begin() + w);
                                }
                                if (!s.tx.empty()) reg_ev(fd, EVFILT_WRITE, EV_ADD);
                            }
                            s.prefill_idx = 0;
                            s.generated_count = 0;
                            s.has_pending_tok = false;
                            if (s.seq < 0) s.seq = next_seq_id++;
                            s.state = uma::ipc::SessionState::PREFILL;
                            if (g_debug) std::cerr << "[prompt] fd=" << fd << " seq=" << s.seq
                                                    << " n_prompt=" << n_prompt << "\n";
                        } else {
                            // empty prompt -> immediate newline
                            s.tx.push_back('\n');
                            reg_ev(fd, EVFILT_WRITE, EV_ADD);
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
                        if (g_debug) std::cerr << "[write] fd=" << fd << " wrote=" << w << " tx_left=" << (s.tx.size() - (size_t)w) << "\n";
                        s.tx.erase(s.tx.begin(), s.tx.begin() + w);
                        s.last_activity_ns = now_ns();
                    }
                    if (s.tx.empty()) {
                        // done streaming, stop write notifications
                        reg_ev(fd, EVFILT_WRITE, EV_DELETE);
                        if (s.state == uma::ipc::SessionState::ERRORED) {
                            // close errored sessions after flushing error
                            close_session(fd);
                        } else if (s.state == uma::ipc::SessionState::STREAM) {
                            // finished a response
                            if (s.read_closed) {
                                close_session(fd);
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

        // ---- M3 Scheduler tick: build global batch from ready sessions ----
        // Two-phase policy per tick: (A) 1 token per DECODE session, (B) PREFILL drain in chunks
        {
            // Build containers with capacity up to ubatch_cap
            std::vector<llama_token> tokens;               tokens.reserve(ubatch_cap);
            std::vector<int32_t>     n_seq_id;             n_seq_id.reserve(ubatch_cap);
            std::vector<llama_seq_id> seq_id_vals;         seq_id_vals.reserve(ubatch_cap);
            std::vector<llama_seq_id*> seq_ids;            seq_ids.reserve(ubatch_cap);
            std::vector<int8_t>      logits;               logits.reserve(ubatch_cap);
            struct SampleRef { int fd; int batch_index; uma::ipc::SessionState state_before; };
            std::vector<SampleRef> samples; samples.reserve(ubatch_cap);

            // Split ready sessions
            std::vector<int> ready_decode;
            std::vector<int> ready_prefill;
            ready_decode.reserve(sessions.size());
            ready_prefill.reserve(sessions.size());
            for (auto & kv : sessions) {
                auto & s = *kv.second;
                if (s.state == uma::ipc::SessionState::DECODE && s.has_pending_tok) {
                    ready_decode.push_back(s.fd);
                } else if (s.state == uma::ipc::SessionState::PREFILL && s.prefill_idx < s.prompt_tokens.size()) {
                    ready_prefill.push_back(s.fd);
                }
            }

            // Adaptive budget for this tick
            int32_t budget = std::min<int32_t>(target_batch, ubatch_cap);
            if (budget <= 0) budget = 1;

            // (A) DECODE: give exactly 1 token to each ready DECODE session (RR), up to budget
            if (!ready_decode.empty() && budget > 0) {
                for (size_t i = 0; i < ready_decode.size() && budget > 0; ++i) {
                    int fd_pick = ready_decode[(rr_cursor_decode + i) % ready_decode.size()];
                    auto it = sessions.find(fd_pick); if (it == sessions.end()) continue;
                    auto & s = *it->second;
                    // Append the pending token
                    llama_token t = (llama_token) s.pending_tok;
                    s.has_pending_tok = false;
                    tokens.push_back(t);
                    n_seq_id.push_back(1);
                    seq_id_vals.push_back((llama_seq_id)s.seq);
                    seq_ids.push_back(&seq_id_vals.back());
                    logits.push_back(1);
                    samples.push_back({s.fd, (int)tokens.size()-1, uma::ipc::SessionState::DECODE});
                    budget--;
                }
                rr_cursor_decode = (rr_cursor_decode + 1) % ready_decode.size();
            }

            // Latency guard: if any DECODE session has waited too long for emit, skip PREFILL this tick
            bool skip_prefill = false;
            if (!ready_decode.empty()) {
                uint64_t nowt = now_ns();
                for (int fd : ready_decode) {
                    auto it = sessions.find(fd); if (it == sessions.end()) continue;
                    auto & s = *it->second;
                    // 150ms inactivity on writes is a simple heuristic for interactivity
                    if (nowt - s.last_activity_ns > 150ull * 1000ull * 1000ull) { skip_prefill = true; break; }
                }
            }

            // (B) PREFILL: drain large chunks up to remaining budget (RR over prefill sessions)
            if (!skip_prefill && !ready_prefill.empty() && budget > 0) {
                for (size_t i = 0; i < ready_prefill.size() && budget > 0; ++i) {
                    int fd_pick = ready_prefill[(rr_cursor_prefill + i) % ready_prefill.size()];
                    auto it = sessions.find(fd_pick); if (it == sessions.end()) continue;
                    auto & s = *it->second;
                    size_t remain = s.prompt_tokens.size() - s.prefill_idx;
                    int32_t chunk = (int32_t) std::min<size_t>(remain, (size_t) budget);
                    if (chunk <= 0) continue;
                    // append chunk tokens; logits only on last
                    for (int32_t j = 0; j < chunk; ++j) {
                        llama_token t = (llama_token) s.prompt_tokens[s.prefill_idx++];
                        tokens.push_back(t);
                        n_seq_id.push_back(1);
                        seq_id_vals.push_back((llama_seq_id)s.seq);
                        seq_ids.push_back(&seq_id_vals.back());
                        int8_t lg = (j == chunk - 1) ? 1 : 0;
                        logits.push_back(lg);
                        if (lg) samples.push_back({s.fd, (int)tokens.size()-1, uma::ipc::SessionState::PREFILL});
                    }
                    budget -= chunk;
                }
                rr_cursor_prefill = (rr_cursor_prefill + 1) % ready_prefill.size();
            }

            if (!tokens.empty()) {
                if (g_debug) {
                    std::ostringstream oss;
                    oss << "[batch] n=" << tokens.size() << " cap=" << ubatch_cap << " target=" << target_batch << " items=";
                    for (size_t i = 0; i < tokens.size(); ++i) {
                        oss << " (seq=" << (int)seq_id_vals[i] << ",log=" << (int)logits[i] << ")";
                    }
                    std::cerr << oss.str() << "\n";
                }

                llama_batch batch{};
                batch.n_tokens = (int32_t) tokens.size();
                batch.token    = tokens.data();
                batch.embd     = nullptr;
                batch.pos      = nullptr; // auto-track positions per seq
                batch.n_seq_id = n_seq_id.data();
                batch.seq_id   = seq_ids.data();
                batch.logits   = logits.data();

                auto t0 = std::chrono::steady_clock::now();
                int dec_rc = llama_decode(gctx, batch);
                auto t1 = std::chrono::steady_clock::now();
                double ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
                // update metrics
                mtx.batch_calls_total.fetch_add(1, std::memory_order_relaxed);
                mtx.last_batch_size.store((uint32_t) tokens.size(), std::memory_order_relaxed);
                mtx.decode_ms_last.store((uint32_t) (ms + 0.5), std::memory_order_relaxed);
                // EWMA toward observed decode time
                decode_ms_ewma = 0.8 * decode_ms_ewma + 0.2 * ms;
                mtx.set_decode_ms_ewma(decode_ms_ewma);
                // Simple adaptive tuning
                if (decode_ms_ewma > 1.3 * tick_budget_ms) {
                    target_batch = std::max<int32_t>(8, (int32_t) (target_batch * 0.7));
                } else if (decode_ms_ewma < 0.8 * tick_budget_ms) {
                    target_batch = std::min<int32_t>(ubatch_cap, target_batch + std::max<int32_t>(1, target_batch / 8));
                }

                if (dec_rc != 0) {
                    // On decode error, mark sessions errored
                    for (auto & samp : samples) {
                        auto it = sessions.find(samp.fd); if (it == sessions.end()) continue;
                        auto & s = *it->second;
                        s.last_error = "decode error";
                        s.state = uma::ipc::SessionState::ERRORED;
                        const std::string emsg = "error: decode failed\n";
                        s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
                        reg_ev(s.fd, EVFILT_WRITE, EV_ADD);
                    }
                } else {
                    // Read logits rows only for positions where logits==1, in push order
                    const int32_t n_vocab = llama_vocab_n_tokens(vocab);
                    for (size_t k = 0; k < samples.size(); ++k) {
                        auto & samp = samples[k];
                        auto it = sessions.find(samp.fd);
                        if (it == sessions.end()) continue;
                        auto & s = *it->second;
                        float * logits_row = llama_get_logits_ith(gctx, samp.batch_index);
                        if (!logits_row) continue; // safety in non-DEBUG
                        // greedy argmax
                        int best = 0; float bestv = logits_row[0];
                        for (int i = 1; i < n_vocab; ++i) {
                            float v = logits_row[i];
                            if (v > bestv) { bestv = v; best = i; }
                        }
                        llama_token new_id = (llama_token) best;
                        if (g_debug) std::cerr << "[sample] fd=" << s.fd
                                                << " state_before=" << (samp.state_before == uma::ipc::SessionState::PREFILL ? "PREFILL" : "DECODE")
                                                << " tok=" << new_id << "\n";
                        if (samp.state_before == uma::ipc::SessionState::PREFILL) {
                            // transition to DECODE; feed this token next tick
                            s.pending_tok = new_id;
                            s.has_pending_tok = true;
                            s.state = uma::ipc::SessionState::DECODE;
                            // stream first sampled token immediately
                            char buf[256];
                            int n = llama_token_to_piece(vocab, new_id, buf, sizeof(buf), 0, true);
                            if (n > 0) s.tx.insert(s.tx.end(), (uint8_t*)buf, (uint8_t*)buf + n);
                            mtx.tokens_generated_total.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            // stream piece or finish
                            if (llama_vocab_is_eog(vocab, new_id) || s.generated_count >= cfg.max_tokens) {
                                s.tx.push_back('\n');
                                s.state = uma::ipc::SessionState::STREAM; // finished for now
                                // Clear sequence memory for reuse
                                llama_memory_seq_rm(llama_get_memory(gctx), s.seq, -1, -1);
                            } else {
                                char buf[256];
                                int n = llama_token_to_piece(vocab, new_id, buf, sizeof(buf), 0, true);
                                if (n > 0) s.tx.insert(s.tx.end(), (uint8_t*)buf, (uint8_t*)buf + n);
                                s.generated_count++;
                                s.pending_tok = new_id;
                                s.has_pending_tok = true;
                                s.state = uma::ipc::SessionState::DECODE;
                                mtx.tokens_generated_total.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        // try immediate drain and/or arm write readiness
                        if (!s.tx.empty()) {
                            ssize_t w = ::write(s.fd, s.tx.data(), s.tx.size());
                            if (w > 0) {
                                if (g_debug) std::cerr << "[write-now] fd=" << s.fd << " wrote=" << w << "\n";
                                s.tx.erase(s.tx.begin(), s.tx.begin() + w);
                                s.last_activity_ns = now_ns();
                            }
                            if (!s.tx.empty()) reg_ev(s.fd, EVFILT_WRITE, EV_ADD);
                            else if (s.state == uma::ipc::SessionState::STREAM) {
                                if (s.read_closed) { close_session(s.fd); }
                                else {
                                    s.state = uma::ipc::SessionState::RECV_REQ;
                                    s.prompt_tokens.clear();
                                    s.prefill_idx = 0;
                                    s.generated_count = 0;
                                    s.has_pending_tok = false;
                                }
                            }
                        }
                    }
                }
            }
        }
        } // end main event loop

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
