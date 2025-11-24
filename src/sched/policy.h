// UMA Serve - Scheduling policy interfaces
#pragma once

#include "ipc/session.h"

#include <cstddef>
#include <vector>

namespace uma::sched {

enum class Phase { PREFILL, DECODE };

struct BatchItem {
    int fd = -1;
    Phase phase = Phase::DECODE;
    int32_t n_tokens = 1; // for PREFILL chunks; DECODE is always 1
};

struct Plan {
    std::vector<BatchItem> items;
    // Round-robin cursors for next tick
    size_t next_rr_decode_idx = 0;
    size_t next_rr_prefill_idx = 0;
    // Accounting helpers
    int32_t decode_tok_count = 0;
    int32_t prefill_tok_count = 0;
};

class IBatchPolicy {
  public:
    virtual ~IBatchPolicy() = default;
    // Build a plan for a single tick given the current sessions and scheduler cursors/budget.
    virtual Plan schedule_tick(const uma::ipc::SessionPool& sessions, int32_t batch_cap,
                               int32_t target_batch, size_t rr_decode_idx,
                               size_t rr_prefill_idx) = 0;
};

// Baseline policy that mirrors the current scheduler behavior:
// - Decode-first: 1 token per DECODE session (round-robin)
// - Budgeted prefill: fill remaining capacity, TTFT-first with small burst
class BaselinePolicy : public IBatchPolicy {
  public:
    Plan schedule_tick(const uma::ipc::SessionPool& sessions, int32_t batch_cap,
                       int32_t target_batch, size_t rr_decode_idx,
                       size_t rr_prefill_idx) override;
};

} // namespace uma::sched

