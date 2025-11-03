// UMA Serve - Session state (M2)
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct llama_context;

namespace uma::ipc {

enum class SessionState {
    RECV_REQ,
    PREFILL,
    DECODE,
    STREAM,
    DONE,
    ERRORED,
};

struct ClientSession {
    int fd = -1;
    std::vector<uint8_t> rx;
    std::vector<uint8_t> tx;

    llama_context* ctx = nullptr; // unused in M3 (global ctx); kept for compatibility
    int32_t seq = -1;             // assigned on first request
    SessionState state = SessionState::RECV_REQ;
    std::vector<int> prompt_tokens;    // tokenized prompt (llama_token ids)
    size_t prefill_idx = 0;            // next index into prompt_tokens
    bool   has_pending_tok = false;    // if true, pending_tok will be appended next tick
    int32_t pending_tok = 0;           // last sampled token to feed
    uint32_t generated_count = 0;      // number of generated tokens so far
    uint64_t last_activity_ns = 0;
    bool wants_stream = true;
    bool read_closed = false;           // peer sent EOF on read side
    std::string last_error;
};

using SessionPool = std::unordered_map<int, std::unique_ptr<ClientSession>>;

} // namespace uma::ipc
