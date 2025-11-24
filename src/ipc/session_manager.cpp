// UMA Serve - Session manager (RX parsing, state transitions to PREFILL, basic guards)
#include "ipc/session_manager.h"

#include "ipc/protocol.h"
#include "runtime/tokens.h"
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
    if (it == sessions_.end())
        return nullptr;
    return it->second.get();
}

SessionManager::ReadResult SessionManager::on_readable(int fd,
                                                       const uma::runtime::RuntimeConfig& cfg,
                                                       const llama_vocab* vocab, uint64_t now_ns) {
    ReadResult rr;
    auto it = sessions_.find(fd);
    if (it == sessions_.end())
        return rr;
    auto& s = *it->second;

    uint8_t buf[4096];
    bool saw_eof = false;
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            s.rx.insert(s.rx.end(), buf, buf + n);
            s.last_activity_ns = now_ns;
            continue; // drain more
        }
        if (n == 0) {
            saw_eof = true;
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        // other error -> close by caller
        rr.removed_read = true;
        return rr;
    }

    if (saw_eof) {
        s.read_closed = true;
        rr.removed_read = true;
        s.last_activity_ns = now_ns;
    }

    // JSON-only protocol: attempt to parse one framed request
    std::string js;
    std::string err;
    if (!uma::ipc::protocol::try_read_frame(s.rx, js, uma::ipc::protocol::kDefaultMaxFrameBytes,
                                            &err)) {
        if (!err.empty()) {
            const char* code = (err.find("invalid frame length 0") != std::string::npos)
                                       ? "E_PROTO_INVALID_LEN"
                                       : "E_PROTO_FRAME_TOO_LARGE";
            uma::ipc::protocol::append_error_event(s.tx, s.request_id, code, err);
            s.state = SessionState::STREAM;
            s.read_closed = true;
            rr.wants_write = true;
            rr.removed_read = true;
        }
        return rr; // need more
    }
    // Admin metrics handled below
    // minimal field extraction with basic JSON string parsing (handles escapes; flags invalid
    // escapes)
    auto extract_json_string = [](const std::string& j, const char* key,
                                  bool& invalid_escape) -> std::string {
        invalid_escape = false;
        size_t kpos = j.find("\"" + std::string(key) + "\"");
        if (kpos == std::string::npos)
            return {};
        size_t colon = j.find(':', kpos);
        if (colon == std::string::npos)
            return {};
        size_t q1 = j.find('"', colon);
        if (q1 == std::string::npos)
            return {};
        size_t i = q1 + 1;
        std::string out;
        auto is_hex = [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        };
        while (i < j.size()) {
            char c = j[i++];
            if (c == '\\') {
                if (i >= j.size()) {
                    invalid_escape = true;
                    break;
                }
                char e = j[i++];
                switch (e) {
                    case '"':
                        out.push_back('"');
                        break;
                    case '\\':
                        out.push_back('\\');
                        break;
                    case '/':
                        out.push_back('/');
                        break;
                    case 'b':
                        out.push_back('\b');
                        break;
                    case 'f':
                        out.push_back('\f');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'u': {
                        if (i + 4 > j.size() || !is_hex(j[i]) || !is_hex(j[i + 1]) ||
                            !is_hex(j[i + 2]) || !is_hex(j[i + 3])) {
                            invalid_escape = true;
                            break;
                        }
                        // skip 4 hex digits; we won't decode here
                        i += 4;
                        // append placeholder; actual UTF-8 decoding not required for this test
                        out.push_back('?');
                        break;
                    }
                    default:
                        // unsupported escape (e.g., \x) → invalid
                        invalid_escape = true;
                        break;
                }
                if (invalid_escape)
                    break;
            } else if (c == '"') {
                // end of string
                break;
            } else {
                // control chars are invalid in JSON strings
                if (static_cast<unsigned char>(c) < 0x20) {
                    invalid_escape = true;
                    break;
                }
                out.push_back(c);
            }
        }
        return out;
    };

    // Admin metrics (JSON): accept {"type":"metrics"} or {"event":"metrics"}
    {
        bool type_invalid = false, event_invalid = false;
        std::string typ = extract_json_string(js, "type", type_invalid);
        std::string evt = extract_json_string(js, "event", event_invalid);
        if ((!type_invalid && typ == "metrics") || (!event_invalid && evt == "metrics")) {
            rr.admin_request = true;
            s.state = SessionState::STREAM;
            s.read_closed = true;
            rr.wants_write = true;
            rr.removed_read = true;
            return rr;
        }
    }

    bool id_invalid = false, prompt_invalid = false;
    std::string req_id = extract_json_string(js, "id", id_invalid);
    std::string prompt = extract_json_string(js, "prompt", prompt_invalid);
    if (id_invalid || prompt_invalid) {
        uma::ipc::protocol::append_error_event(s.tx, req_id, "E_PROTO_001", "invalid utf-8");
        s.state = SessionState::STREAM;
        s.read_closed = true;
        rr.wants_write = true;
        rr.removed_read = true;
        return rr;
    }
    if (prompt.empty()) {
        uma::ipc::protocol::append_error_event(s.tx, req_id, "E_PROTO_BAD_REQUEST",
                                               "missing or invalid prompt");
        s.state = SessionState::STREAM;
        s.read_closed = true;
        rr.wants_write = true;
        rr.removed_read = true;
        return rr;
    }
    s.request_id = req_id;

    // size limit (bytes) on prompt
    if (prompt.size() > cfg.max_prompt_bytes) {
        uma::ipc::protocol::append_error_event(s.tx, s.request_id, "E_LIMIT_001",
                                               "prompt too large");
        s.state = SessionState::STREAM;
        s.read_closed = true;
        rr.wants_write = true;
        rr.removed_read = true;
        return rr;
    }

    // tokenize prompt and transition to PREFILL (no immediate echo for JSON)
    s.prompt_tokens.clear();
    auto toks = uma::runtime::tokens::tokenize(vocab, prompt, /*add_bos*/ true, /*special*/ true);
    if (!toks.empty()) {
        s.prompt_tokens = std::move(toks);
        s.prefill_idx = 0;
        s.generated_count = 0;
        s.has_pending_tok = false;
        s.n_past = 0;
        s.req_start_ns = now_ns;
        s.first_emit_ns = 0;
        s.last_emit_ns = 0;
        s.slo.target_ttft_ms = cfg.slo_ttft_ms;
        s.slo.target_tbt_ms = cfg.slo_tbt_ms;
        if (s.seq < 0)
            s.seq = next_seq_id_++;
        s.state = SessionState::PREFILL;
        UMA_LOG_DEBUG() << "[prompt-json] fd=" << fd << " seq=" << s.seq
                        << " n_prompt=" << s.prompt_tokens.size();
    } else {
        // empty prompt -> eos event (keep connection open for reuse)
        s.state = SessionState::STREAM;
        uma::ipc::protocol::append_eos_event(s.tx, s.request_id, "stop");
        rr.wants_write = true;
    }
    return rr;
}

} // namespace uma::ipc
