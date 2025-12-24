// UMA Serve - ΣBMT estimator (v0)
#pragma once

#include "sched/policy.h"
#include "ipc/session.h"

#include <cstdint>

namespace uma::sched::bmt {

// Estimate dimensionless ΣBMT units for a planned tick.
// v0 model: cost per token ≈ (n_past + 1) to reflect attention KV traffic growth.
// - DECODE item cost: (n_past + 1)
// - PREFILL chunk cost: sum_{j=0..m-1} (n_past + j + 1)
uint64_t estimate_units(const uma::ipc::SessionPool& sessions, const Plan& plan);

} // namespace uma::sched::bmt

