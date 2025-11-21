# Directions (Serving Policy + Overheads)

This note outlines how UMA‑Serve should shape work to keep user‑visible latency tight and improve system throughput without changing kernels.

## Design Pillars

- Decode‑first ticks with a per‑tick wall‑time guard (e.g., 40–60 ms).
- UMA‑aware scheduling via a simple ΣBMT (bytes/token) guard.
- Low‑overhead IPC and overlapped sampling to reduce non‑model costs.

## Scheduler Policy (first version)

- Tick budget: maintain `decode_ms_ewma`; set a target (40–60 ms). Each tick should complete under budget.
- Decode‑first: always add 1 token for each interactive decoder first.
- Prefill throttle + hysteresis: estimate predicted step time; only add prefill if under budget. If a tick exceeds budget, disable prefill for H ticks, then re‑enable gradually.
- Served decoders cap (admission): if step time creeps up, reduce background decoders served per tick; rotate fairly. Interactive decoders are always served.
- Token‑boundary preemption: new interactive requests join on the next tick; no long waits behind prefill.
- QoS lanes: reserve a minimum budget for interactive class per tick.

## ΣBMT Guard (bandwidth‑aware)

- Estimate bytes moved per token (attention K/V up to n_past, matmuls, MoE FFN experts). Keep ΣBMT_per_tick ≤ BW_budget × time_budget by trimming prefill and/or served decoders. This prevents entering long‑tick regimes that hurt both UX and throughput.

## Throughput “Sweet Spot” (auto‑tune)

- Find the operating point that maximizes tokens/tick ÷ tick_time under the time budget.
- Adjust either:
  - Served background decoders S (1 token each), and/or
  - Background burst length k (2–4 tokens for fewer decoders) when interactive load is low.
- Hill‑climb with hysteresis: nudge S/k; keep changes that increase throughput while staying under the guard.

## MoE‑Aware Shaping

- Cap served decoders at a lower S for MoE models (expert explosion). Prefer small bursts for fewer decoders over many decoders with 1 token.
- Where possible, expose top‑k gating control (prefer k=1) to reduce experts per step.

## Overhead Reductions (UMA)

- Default IPC: UDS + framed messages; keep HTTP/SSE as a gateway.
- Zero‑copy logits: shared buffer that the CPU sampler reads in place; overlap sampling with next GPU decode.
- Early, small flushes; avoid per‑chunk JSON churn in the hot path.
- Residency friendliness (macOS Metal): pre‑touch pools; use residency sets and stable heaps to minimize page churn.

## Metrics To Export

- decode_ms_last, decode_ms_ewma, tokens_per_tick (decode vs prefill), served_decoders, parked_sessions.
- ΣBMT_estimate_last, ΣBMT_budget, prefill_guard_active, ticks_over_budget.
- Sampling overlap telemetry: sampling_ms_last, time_logits_to_first_chunk, overlap_active.
- Optional client‑centric snapshots: worst_pause_ms (first 3s), ttf5_ms.

## Validation Targets

- Prefill‑only storm: worst pause near baseline (≤2×), no 500–600 ms freezes.
- Agent chain under decode load: slowdown factor reduced (e.g., 3–8× → ≤1.5–2×).
- Long‑context sweeps: flatter inter‑p95 curve; higher aggregate tokens/sec at equal guard.
- Dense vs MoE: UMA operates near the sweet spot automatically; avoids expert‑explosion slowdowns.

This policy is intentionally simple and measurable; we can iterate (e.g., better ΣBMT model, speculative decode) once we lock the basic UX + throughput wins.

