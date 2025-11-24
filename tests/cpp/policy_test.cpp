#include "gtest/gtest.h"
#include "sched/policy.h"
#include "ipc/session.h"

#include <memory>
#include <unordered_map>

using uma::sched::BaselinePolicy;
using uma::sched::IBatchPolicy;
using uma::sched::Phase;
using uma::sched::Plan;

static uma::ipc::SessionPool make_pool() {
    return uma::ipc::SessionPool{};
}

TEST(PolicyTest, DecodeFirstOneEach) {
    auto sessions = make_pool();
    // Two decode-ready sessions
    {
        auto s = std::make_unique<uma::ipc::ClientSession>();
        s->fd = 3; s->state = uma::ipc::SessionState::DECODE; s->has_pending_tok = true; s->seq = 1; s->n_past = 10;
        sessions.emplace(s->fd, std::move(s));
    }
    {
        auto s = std::make_unique<uma::ipc::ClientSession>();
        s->fd = 4; s->state = uma::ipc::SessionState::DECODE; s->has_pending_tok = true; s->seq = 2; s->n_past = 20;
        sessions.emplace(s->fd, std::move(s));
    }

    BaselinePolicy pol;
    Plan plan = pol.schedule_tick(sessions, /*batch_cap*/32, /*target*/32, /*rrd*/0, /*rrp*/0);
    ASSERT_EQ(plan.items.size(), 2u);
    EXPECT_EQ(plan.decode_tok_count, 2);
    EXPECT_EQ(plan.prefill_tok_count, 0);
    EXPECT_EQ(plan.items[0].phase, Phase::DECODE);
    EXPECT_EQ(plan.items[1].phase, Phase::DECODE);
}

TEST(PolicyTest, PrefillTtftFirstBurstLimited) {
    auto sessions = make_pool();
    // TTFT session (no first emit yet), long prompt
    {
        auto s = std::make_unique<uma::ipc::ClientSession>();
        s->fd = 5; s->state = uma::ipc::SessionState::PREFILL; s->first_emit_ns = 0; s->prefill_idx = 0; s->seq = 3;
        s->prompt_tokens.resize(100, 1);
        sessions.emplace(s->fd, std::move(s));
    }
    // Non-TTFT session (already emitted), short remaining prompt
    {
        auto s = std::make_unique<uma::ipc::ClientSession>();
        s->fd = 6; s->state = uma::ipc::SessionState::PREFILL; s->first_emit_ns = 42; s->prefill_idx = 0; s->seq = 4;
        s->prompt_tokens.resize(8, 1);
        sessions.emplace(s->fd, std::move(s));
    }

    BaselinePolicy pol;
    Plan plan = pol.schedule_tick(sessions, /*batch_cap*/64, /*target*/64, /*rrd*/0, /*rrp*/0);
    ASSERT_GE(plan.items.size(), 1u);
    // First prefill item should be TTFT session (fd=5) with burst limit 16
    EXPECT_EQ(plan.items[0].fd, 5);
    EXPECT_EQ(plan.items[0].phase, Phase::PREFILL);
    EXPECT_EQ(plan.items[0].n_tokens, 16);
}

TEST(PolicyTest, BudgetRespectedAcrossPhases) {
    auto sessions = make_pool();
    // One DECODE and one PREFILL; target budget 3 should allocate 1 + 2
    {
        auto s = std::make_unique<uma::ipc::ClientSession>();
        s->fd = 7; s->state = uma::ipc::SessionState::DECODE; s->has_pending_tok = true; s->seq = 5;
        sessions.emplace(s->fd, std::move(s));
    }
    {
        auto s = std::make_unique<uma::ipc::ClientSession>();
        s->fd = 8; s->state = uma::ipc::SessionState::PREFILL; s->first_emit_ns = 42; s->prefill_idx = 0; s->seq = 6;
        s->prompt_tokens.resize(10, 1);
        sessions.emplace(s->fd, std::move(s));
    }

    BaselinePolicy pol;
    Plan plan = pol.schedule_tick(sessions, /*batch_cap*/32, /*target*/3, /*rrd*/0, /*rrp*/0);
    ASSERT_EQ(plan.decode_tok_count, 1);
    ASSERT_EQ(plan.prefill_tok_count, 2);
    ASSERT_EQ(plan.items.size(), 2u);
}

TEST(PolicyTest, RoundRobinCursorsAdvance) {
    auto sessions = make_pool();
    // Three DECODE sessions
    for (int i = 0; i < 3; ++i) {
        auto s = std::make_unique<uma::ipc::ClientSession>();
        s->fd = 10 + i; s->state = uma::ipc::SessionState::DECODE; s->has_pending_tok = true; s->seq = 100 + i;
        sessions.emplace(s->fd, std::move(s));
    }
    BaselinePolicy pol;
    Plan plan = pol.schedule_tick(sessions, /*batch_cap*/32, /*target*/32, /*rrd*/0, /*rrp*/0);
    // Next rr index should advance by 1 across ticks
    EXPECT_EQ(plan.next_rr_decode_idx, 1u);
}

