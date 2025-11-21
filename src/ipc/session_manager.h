// UMA Serve - Session manager (RX parsing, state transitions to PREFILL, basic guards)
#pragma once

#include "ipc/poller.h"
#include "ipc/session.h"
#include "runtime/config.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct llama_context;
struct llama_vocab;

namespace uma::ipc {

class SessionManager {
  public:
    SessionManager() = default;

    // Create and register a new session for fd; returns reference.
    ClientSession& add_client(int fd, uint64_t now_ns);

    // Close and remove a session; clears KV memory and deregisters with poller.
    void close(int fd, Poller& poller, llama_context* ctx);

    // Lookup
    ClientSession* find(int fd);

    // Access underlying map (for scheduler and iteration)
    SessionPool& map() { return sessions_; }

    struct ReadResult {
        bool wants_write = false;   // true if tx now has pending bytes
        bool removed_read = false;  // caller should remove Read interest
        bool admin_request = false; // true if line was an admin command (e.g., /metrics)
        std::string admin_line;     // the raw line parsed
    };

    // Handle readable event: read bytes, parse framed JSON, validate and tokenize prompt.
    // On prompt, transitions session to PREFILL.
    // Returns what actions the caller should take.
    ReadResult on_readable(int fd, const uma::runtime::RuntimeConfig& cfg, const llama_vocab* vocab,
                           uint64_t now_ns);

  private:
    SessionPool sessions_;
    int32_t next_seq_id_ = 1;
};

} // namespace uma::ipc
