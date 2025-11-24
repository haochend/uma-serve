#pragma once

#include "ipc/session.h"
#include "sched/policy.h"
#include "llama.h"
#include "metrics/metrics.h"
#include "runtime/config.h"

namespace uma::sched {

class Scheduler {

  private:
    int32_t batch_cap_;
    int32_t target_batch_;
    size_t rr_decode_idx_;
    size_t rr_prefill_idx_;
    llama_context* ctx_;
    const llama_vocab* vocab_;
    const runtime::RuntimeConfig config_;
    uma::metrics::Metrics* metrics_;
    double decode_ms_ewma_;
    const double tick_budget_ms_ = 30.0;
    BaselinePolicy policy_;

  public:
    Scheduler(llama_context* ctx, const llama_vocab* vocab, const runtime::RuntimeConfig& cfg,
              uma::metrics::Metrics* m = nullptr);

    std::vector<int> tick(ipc::SessionPool& sessions, uint64_t now_ns);
};

} // namespace uma::sched
