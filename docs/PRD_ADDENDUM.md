# PRD Addendum — Updated Priorities From Findings

Purpose
- Incorporate insights from docs/FINDINGS.md into the product plan so we target the highest user‑felt wins under mixed workloads on UMA.

North‑Star Outcomes
- Protect interactive UX under load:
  - inter‑token p95 ≤ 1.2× solo baseline
  - worst pause (first 3s) ≤ 2× baseline
- Maintain throughput within −5–10% of llama.cpp at the same latency cap.
- Avoid allocator failures (no “failed to find a memory slot …” warnings) under realistic stress.

Key Findings (summary)
- Prefill‑only storms cause visible freezes: worst pause jumps from ~10–20 ms baseline to ~500–600 ms; TTF5 ~2×.
- Concurrency scaling behaves as expected; per‑stream t/s drops as active slots rise.
- Agent chains slow down under concurrent decoders; decode‑concurrent load is the bigger risk than prefill‑only.
- Prompt cache can mask real costs; use unique prompts when validating.
- MoE degrades faster as slots rise due to expert activation breadth.
- There is a “sweet spot”: short, predictable ticks (e.g., 40–60 ms) maximize throughput at acceptable latency.

Scope (this iteration)
- Align parallel sequences with llama‑server: default `--parallel 4` (env `UMA_N_SEQ`).
- Baseline policy in planner (done): decode‑first + TTFT‑first prefill with burst limit.
- ΣBMT guard v0 (added): coarse estimator + prefill trimming to keep ticks within a bandwidth budget.
- Admission shape: recommend safe batch defaults (`UMA_N_BATCH`, `UMA_N_UBATCH`) and document tuning.
- Soft‑fail handling: treat llama decode rc==1 (FAILED_PREPARE) as a soft condition for the scheduler (reduce/skip prefill), not a session error.

Next Up (short‑term)
- LatencyGuard (SLO‑aware): disable/throttle prefill when `decode_ms_ewma` exceeds target; hysteresis.
- QoS lanes: reserve per‑tick budget for interactive; cap background decoders.
- Concurrency perf sweeps and storm scenarios wired into pytest marks; report scorecard metrics.

Validation Matrix (bench harness)
- Two‑app “chat vs big paste” (unique prompts): TTF5 and worst‑pause protection.
- Prefill‑only storm: worst pause and inter‑p95 flattening; allocator stability.
- Agent chain under decode load: makespan slowdown improvement (target ≤ 1.5–2×).
- Long‑context sweeps (8k/16k/32k): aggregate tokens/s and inter‑p95 under a fixed time/bandwidth cap.
- Dense vs MoE concurrency sweep: earlier plateau for MoE; policy keeps UX stable.

Success Criteria
- Meets North‑Star outcomes above.
- Tokens/s within −5–10% of llama.cpp server on matched params when guards are off; with guards on, trades background throughput for clearly improved interactive UX.
- No allocator warnings under storm/mixed scenarios with documented defaults.

Defaults & Knobs (documented)
- `--parallel 4` (env `UMA_N_SEQ`) to match llama‑server behavior.
- Batch tuning: `UMA_N_BATCH`, `UMA_N_UBATCH` starting points per model class.
- ΣBMT guard: `--bmt-budget` / `UMA_BMT_BUDGET` (off by default; calibrated per model family).
- SLO targets: `UMA_SLO_TTFT_MS`, `UMA_SLO_TBT_MS` (advisory today; guard soon).

Risks & Mitigations
- Over‑trimming prefill reduces background throughput → tunable budgets; scenario‑specific presets.
- Model variance (MoE vs dense) → provide conservative defaults and per‑model notes.
- Test flakiness under large models → keep a fast smoke metric test; mark heavier sweeps.

