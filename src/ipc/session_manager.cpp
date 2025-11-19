// UMA Serve - Session manager (RX parsing, state transitions to PREFILL, basic guards)
#include "ipc/session_manager.h"

#include "runtime/tokens.h"
#include "util/utf8.h"
#include "util/logging.h"

#include "llama.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace uma::ipc {

ClientSession& SessionManager::add_client(int fd, uint64_t now_ns) {
    auto sess = std::make_unique<ClientSession>();
    sess->fd = fd;
    sess->ctx = nullptr; // global ctx used in M3 batching
    sess->last_activity_ns = now_ns;
    auto& ref = *sess;
    sessions_[fd] = std::move(sess);
    UMA_LOG_DEBUG() << "[accept] fd=" << fd << " sessions=" << sessions_.size();
    return ref;
}

void SessionManager::close(int fd, Poller& poller, llama_context* ctx) {
    // deregister filters
    poller.remove(fd, PollFlags::Read | PollFlags::Write);

    auto it = sessions_.find(fd);
    if (it != sessions_.end()) {
        if (it->second->seq >= 0 && ctx) {
            llama_memory_seq_rm(llama_get_memory(ctx), it->second->seq, -1, -1);
        }
        if (it->second->ctx)
            llama_free(it->second->ctx);
        ::close(fd);
        sessions_.erase(it);
    } else {
        ::close(fd);
    }
}

ClientSession* SessionManager::find(int fd) {
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return nullptr;
    return it->second.get();
}

SessionManager::ReadResult SessionManager::on_readable(int fd, const uma::runtime::RuntimeConfig& cfg,
                                                       const llama_vocab* vocab, uint64_t now_ns) {
    ReadResult rr;
    auto it = sessions_.find(fd);
    if (it == sessions_.end()) return rr;
    auto& s = *it->second;

    uint8_t buf[4096];
    bool saw_eof = false;
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (s.rx.size() + (size_t)n > cfg.max_prompt_bytes) {
                const std::string msg = "error: prompt too large (limit " +
                                        std::to_string(cfg.max_prompt_bytes) + ")\n";
                s.tx.insert(s.tx.end(), msg.begin(), msg.end());
                s.state = SessionState::ERRORED;
                rr.wants_write = true;
                rr.removed_read = true;
                return rr;
            }
            s.rx.insert(s.rx.end(), buf, buf + n);
            s.last_activity_ns = now_ns;
            continue; // drain more
        }
        if (n == 0) { saw_eof = true; break; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        // other error -> close by caller
        rr.removed_read = true;
        return rr;
    }

    if (saw_eof) {
        s.read_closed = true;
        rr.removed_read = true;
        s.last_activity_ns = now_ns;
    }

    // Newline-based protocol: parse one line
    auto nl = std::find(s.rx.begin(), s.rx.end(), (uint8_t)'\n');
    if (nl == s.rx.end()) {
        return rr; // need more
    }

    std::string line(s.rx.begin(), nl);
    s.rx.erase(s.rx.begin(), nl + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find('\0') != std::string::npos) {
        const std::string emsg = "error: invalid input (NUL)\n";
        s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
        s.state = SessionState::ERRORED;
        rr.wants_write = true;
        rr.removed_read = true;
        return rr;
    }
    if (!uma::util::is_valid_utf8(line)) {
        const std::string emsg = "error: invalid utf-8\n";
        s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
        s.state = SessionState::ERRORED;
        rr.wants_write = true;
        rr.removed_read = true;
        return rr;
    }

    if (s.state != SessionState::RECV_REQ) {
        const std::string emsg = "error: busy\n";
        s.tx.insert(s.tx.end(), emsg.begin(), emsg.end());
        rr.wants_write = true;
        return rr;
    }

    // Admin endpoint
    if (line == "/metrics" || line == "metrics") {
        rr.admin_request = true;
        rr.admin_line = line;
        return rr;
    }

    // Tokenize and transition to PREFILL
    s.prompt_tokens.clear();
    auto toks = uma::runtime::tokens::tokenize(vocab, line, /*add_bos*/ true, /*special*/ true);
    if (!toks.empty()) {
        s.prompt_tokens = std::move(toks);
        // Echo prompt pieces for responsiveness
        for (int id : s.prompt_tokens) {
            std::string piece = uma::runtime::tokens::token_to_piece_str(vocab, id, true);
            if (!piece.empty()) {
                s.tx.insert(s.tx.end(), piece.begin(), piece.end());
            }
        }
        rr.wants_write = !s.tx.empty();
        s.prefill_idx = 0;
        s.generated_count = 0;
        s.has_pending_tok = false;
        s.req_start_ns = now_ns;    // start timing for SLO
        s.first_emit_ns = 0;
        s.last_emit_ns = 0;
        if (s.seq < 0) s.seq = next_seq_id_++;
        s.state = SessionState::PREFILL;
        UMA_LOG_DEBUG() << "[prompt] fd=" << fd << " seq=" << s.seq
                        << " n_prompt=" << s.prompt_tokens.size();
    } else {
        // Empty prompt -> immediate newline
        s.tx.push_back('\n');
        rr.wants_write = true;
    }
    return rr;
}

} // namespace uma::ipc
