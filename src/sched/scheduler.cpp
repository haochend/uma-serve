#include "sched/scheduler.h"

#include <vector>

namespace uma::sched {

Scheduler::Scheduler(llama_context* ctx, const llama_vocab* vocab, const runtime::RuntimeConfig cfg,
                     uma::metrics::Metrics* m)
    : ctx_(ctx), vocab_(vocab), config_(cfg), metrics_(m) {
    // Initialize any scheduler-specific state here
}

std::vector<int> Scheduler::tick(ipc::SessionPool& pool, uint64_t now_ns) {
    return {};
}

} // namespace uma::sched
