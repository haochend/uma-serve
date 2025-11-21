# Iteration Plan (Hypotheses + A/Bs)

We will iterate UMA‑Serve on the same benchmark scenarios to validate improvements and avoid regressions. This document lists hypotheses, toggles, and success criteria.

## Hypotheses

- Overlapped CPU sampling reduces non‑model overhead: +10–25% tokens/sec at same latency cap; −5–15% TTF5.
- UDS vs HTTP/SSE cuts hot‑path cost: −5–15% TTFT; −10–20% CPU.
- Time‑guarded ticks keep worst pause near baseline with minimal throughput loss (<5–10%).
- ΣBMT guard flattens long‑context slowdown and increases aggregate tokens/sec vs naive “serve everything”.
- MoE shaping (cap served decoders, small bursts, top‑1 gating when possible) improves scaling vs naive scheduling.

## Feature Flags (A/B)

- overlap_sampling: on/off
- ipc: uds/http
- zero_copy_logits: on/off
- prefill_guard: on/off (hysteresis H)
- served_decoders_cap: auto / fixed N / unlimited
- bg_burst_k: 1 / 2..4
- moe_topk: 1 / 2 (where exposed)

## Benchmark Matrix

- Single‑stream microbench: TTFT, TTF5, CPU% — ipc × overlap on/off.
- Two‑app “chat vs big paste” (unique): TTF5, max_stall_3s — guard on/off.
- Prefill‑only storm (unique long): worst pause with/without guard; confirm UMA protection.
- Agent chain under decode load: makespan slowdown — guard/overlap on/off.
- Long‑context sweep (8k/16k/32k): inter‑p95 and aggregate tokens/sec — ΣBMT guard on/off.
- Dense vs MoE concurrency sweep: per‑stream and aggregate scaling — MoE shaping on/off.

## Success Criteria (examples)

- Feel: worst pause (first 3s) under load ≤ 2× baseline; TTF5 near baseline (±20%).
- Finish: agent chain slowdown factor improves materially (e.g., 3–8× → ≤1.5–2×).
- Throughput: aggregate tokens/sec at fixed time‑budget increases, or stays flat while feel improves.
- Overhead: TTFT and CPU drop in UDS/overlap A/Bs (target −10–20%).

We will record UMA metrics (decode_ms_*, tokens_per_tick, ΣBMT_estimate, guard state) alongside client results for each run to correlate policy behavior with user‑visible outcomes.

