// UMA Serve - Scheduling policy (baseline)
#include "sched/policy.h"

#include <algorithm>
#include <climits>

namespace uma::sched {

Plan BaselinePolicy::schedule_tick(const uma::ipc::SessionPool& sessions, int32_t batch_cap,
                                   int32_t target_batch, size_t rr_decode_idx,
                                   size_t rr_prefill_idx) {
    Plan plan{};
    const int32_t budget0 = std::min<int32_t>(target_batch, batch_cap);
    int32_t budget = budget0;

    // Build lists of session fds for decode and prefill pools
    std::vector<int> decode_pool;
    std::vector<int> prefill_pool;
    decode_pool.reserve(sessions.size());
    prefill_pool.reserve(sessions.size());
    for (auto& kv : sessions) {
        const auto& s = *kv.second;
        if (s.state == uma::ipc::SessionState::DECODE && s.has_pending_tok) {
            decode_pool.push_back(s.fd);
        } else if (s.state == uma::ipc::SessionState::PREFILL &&
                   s.prefill_idx < s.prompt_tokens.size()) {
            prefill_pool.push_back(s.fd);
        }
    }

    // Phase A: round-robin decode (1 token per ready DECODE session)
    if (!decode_pool.empty() && budget > 0) {
        const size_t N = decode_pool.size();
        for (size_t i = 0; i < N && budget > 0; ++i) {
            int fd = decode_pool[(rr_decode_idx + i) % N];
            plan.items.push_back({fd, Phase::DECODE, 1});
            budget -= 1;
            plan.decode_tok_count += 1;
        }
        // rotate cursor by one position (legacy behavior)
        plan.next_rr_decode_idx = (N > 0) ? (rr_decode_idx + 1) % N : 0;
    } else {
        plan.next_rr_decode_idx = 0;
    }

    // Phase B: budgeted prefill (TTFT-first, small burst for first-token sessions)
    if (!prefill_pool.empty() && budget > 0) {
        std::vector<int> ttft_pool;
        std::vector<int> rest_pool;
        ttft_pool.reserve(prefill_pool.size());
        rest_pool.reserve(prefill_pool.size());
        const size_t N = prefill_pool.size();
        for (size_t i = 0; i < N; ++i) {
            int fd = prefill_pool[(rr_prefill_idx + i) % N];
            const auto it = sessions.find(fd);
            if (it == sessions.end()) continue;
            const auto& s = *it->second;
            if (s.first_emit_ns == 0)
                ttft_pool.push_back(fd);
            else
                rest_pool.push_back(fd);
        }

        auto schedule_pool = [&](const std::vector<int>& pool) {
            for (int fd : pool) {
                if (budget <= 0) break;
                const auto it = sessions.find(fd);
                if (it == sessions.end()) continue;
                const auto& s = *it->second;
                const size_t remain_sz = s.prompt_tokens.size() - s.prefill_idx;
                int32_t remain = static_cast<int32_t>(
                        std::min<size_t>(remain_sz, static_cast<size_t>(INT_MAX)));
                int32_t chunk = std::min<int32_t>(remain, budget);
                if (s.first_emit_ns == 0) {
                    const int32_t kBurst = 16;
                    chunk = std::min<int32_t>(chunk, kBurst);
                }
                if (chunk <= 0) continue;
                plan.items.push_back({fd, Phase::PREFILL, chunk});
                budget -= chunk;
                plan.prefill_tok_count += chunk;
            }
        };
        schedule_pool(ttft_pool);
        if (budget > 0) schedule_pool(rest_pool);
        plan.next_rr_prefill_idx = (N > 0) ? (rr_prefill_idx + 1) % N : 0;
    } else {
        plan.next_rr_prefill_idx = 0;
    }

    return plan;
}

} // namespace uma::sched

