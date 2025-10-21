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
    GENERATING,
    STREAM,
    DONE,
    ERRORED,
};

struct ClientSession {
    int fd = -1;
    std::vector<uint8_t> rx;
    std::vector<uint8_t> tx;

    llama_context* ctx = nullptr; // owned via deleter
    int32_t seq = 0;
    SessionState state = SessionState::RECV_REQ;
    std::vector<int> prompt_tokens;    // reserved for later
    std::vector<int> generated_tokens; // reserved for later
    uint64_t last_activity_ns = 0;
    bool wants_stream = true;
    std::string last_error;
};

using SessionPool = std::unordered_map<int, std::unique_ptr<ClientSession>>;

} // namespace uma::ipc

