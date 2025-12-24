// UMA Serve - ΣBMT estimator (v0)
#include "sched/bmt.h"

#include <climits>

namespace uma::sched::bmt {

static inline uint64_t sum_arith(uint64_t a0, uint64_t d, uint64_t n) {
    // Sum of arithmetic progression: n/2 * (2a0 + (n-1)d)
    return (n * (2 * a0 + (n - 1) * d)) / 2;
}

uint64_t estimate_units(const uma::ipc::SessionPool& sessions, const Plan& plan) {
    uint64_t total = 0;
    for (const auto& it : plan.items) {
        auto fnd = sessions.find(it.fd);
        if (fnd == sessions.end()) continue;
        const auto& s = *fnd->second;
        if (it.phase == Phase::DECODE) {
            // one token; cost ~ (n_past + 1)
            total += (uint64_t)(s.n_past + 1);
        } else {
            // prefill chunk of m tokens at base n_past; sum (base+1 .. base+m)
            uint64_t base = (uint64_t) s.n_past;
            uint64_t m = (uint64_t) (it.n_tokens > 0 ? it.n_tokens : 0);
            total += sum_arith(base + 1, 1, m);
        }
    }
    return total;
}

} // namespace uma::sched::bmt

