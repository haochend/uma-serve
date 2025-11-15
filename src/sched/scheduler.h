#pragma once

#include "ipc/session.h"
#include "llama.h"
#include "metrics/metrics.h"
#include "runtime/config.h"

namespace uma::sched {

class Scheduler {

  private:
    llama_context* ctx_;
    const llama_vocab* vocab_;
    const runtime::RuntimeConfig config_;
    uma::metrics::Metrics* metrics_;

  public:
    Scheduler(llama_context* ctx, const llama_vocab* vocab, const runtime::RuntimeConfig cfg,
              uma::metrics::Metrics* m = nullptr);

    std::vector<int> tick(ipc::SessionPool& p, uint64_t now_ns);
};

} // namespace uma::sched
