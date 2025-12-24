#include "gtest/gtest.h"
#include "sched/bmt.h"
#include "sched/policy.h"
#include "ipc/session.h"

#include <memory>

using uma::sched::Plan;
using uma::sched::BatchItem;
using uma::sched::Phase;

TEST(BmtTest, EstimateSimple) {
    uma::ipc::SessionPool sessions;

    // DECODE session with n_past=10
    {
        auto s = std::make_unique<uma::ipc::ClientSession>();
        s->fd = 1; s->state = uma::ipc::SessionState::DECODE; s->has_pending_tok = true; s->n_past = 10;
        sessions.emplace(s->fd, std::move(s));
    }
    // PREFILL session with base n_past=5, chunk m=3 -> sum (6+7+8)=21
    {
        auto s = std::make_unique<uma::ipc::ClientSession>();
        s->fd = 2; s->state = uma::ipc::SessionState::PREFILL; s->n_past = 5; s->prefill_idx = 0;
        sessions.emplace(s->fd, std::move(s));
    }

    Plan plan;
    plan.items.push_back({1, Phase::DECODE, 1});
    plan.items.push_back({2, Phase::PREFILL, 3});

    uint64_t est = uma::sched::bmt::estimate_units(sessions, plan);
    // decode cost = 11; prefill sum = 21; total = 32
    EXPECT_EQ(est, 32u);
}

