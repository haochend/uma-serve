# UMA Serve — Policy Layer (Planner)

This document tracks the design of the scheduling policy as a standalone, pluggable component separate from the scheduler executor.

## Purpose

Decouple “what to do” from “how to execute.” The scheduler remains a minimal executor that builds a batch and calls `llama_decode`; all decision‑making about which sessions to serve and how to shape the batch is handled by the policy layer.

## Contracts (planned)

- `SchedulerState`: immutable snapshot with `SessionView` (phase, token counts, SLO timing), device info, and `now_ns`.
- `Plan`: list of `BatchItem { seq_id, phase(PREFILL|DECODE), logits_flag }` for a single tick.
- `IBatchPolicy::schedule_tick(state, constraints) -> Plan`.

## Baseline Policy (current behavior)

- Decode‑first fairness: 1 token to each `DECODE` session per tick.
- Budgeted prefill: use remaining capacity for `PREFILL` work.
- TTFT‑first: prioritize sessions that haven’t emitted their first token; apply a small per‑session prefill burst.
- Adaptive batching: tune batch target via decode‑time EWMA.

## Transformer Pipeline (planned)

Policies are composed via a sequence of transformers operating on the `Plan`:

- `LatencyGuard`: Throttle prefill when TTFT/TBT deadlines are exceeded; apply hysteresis.
- `Admission/QoS`: Classify sessions (interactive/background) and enforce per‑class budgets.
- `PrefixCache`: Replace prefill with KV snapshot restore on cache hits.
- `PagedKV`: Bias selections based on residency/cost to manage UMA bandwidth.
- `SpeculativeDecode`: Expand `Plan` using draft/verify tokens when enabled.

## Roadmap

- Phase 1: define `SchedulerState`, `Plan`, and `IBatchPolicy` interfaces; move current logic into `BaselinePolicy`.
- Phase 2: add `LatencyGuard` and `Admission/QoS` transformers.
- Phase 3: integrate `PrefixCache` and `PagedKV` (with a ΣBMT cost model).
- Phase 4: prototype `SpeculativeDecode` and evaluate TTFT/throughput gains.

The policy layer enables iteration on fairness and performance without destabilizing the executor, and provides clean hooks for advanced UMA‑specific optimizations.

