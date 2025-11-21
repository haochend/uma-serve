# Current Findings (Benchmarks + Observations)

This note captures what we’ve measured so far using the local benchmark suite and what we’ve learned about where users feel pain. It is a working document to guide UMA‑Serve design and iteration.

## What We Measured (client‑side UX)

- TTFT, inter‑chunk latency (mean/median/p95), tokens/sec (chunks/sec), total duration.
- Early‑flow “feel” metrics: TTF5 (time to 5th chunk), max stall in first 3s.
- Agent chain makespan (sequential steps) and slowdown factors under background load.

## Key Results (llama.cpp server, representative)

- Continuous batching appears healthy in burst/stagger cases: TTFT ≈ 65–71 ms median, inter‑p95 ≈ 50 ms, stalls ≈ 0.
- Concurrency scaling behaves as expected: per‑stream chunks/sec drops as active slots increase; TTFT rises moderately.
- Long‑only jitter is smooth: inter‑p95 ≈ 28–50 ms with no >500 ms stalls.
- Prefill‑only storm (unique long prompts, background max_tokens=0):
  - Baseline inter‑p95 ≈ 13.5 ms → storm ≈ 175.5 ms (~13×)
  - Baseline max stall(3s) ≈ 14 ms → storm ≈ 561 ms (~40×)
  - Baseline TTF5 ≈ 91 ms → storm ≈ 175 ms (~2×)
  - Takeaway: “freezes” mid‑reply are feelable under heavy prefill.
- Agent chain under prefill‑only load (8 steps, 128 toks/step):
  - Makespan 13.6 s → 17.1 s (~1.26×); per‑turn cadence largely unchanged.
  - Takeaway: prefill‑only pressure doesn’t strongly slow a single active decoder; decode‑concurrent load is the bigger risk for chain time.

## Where Pain Shows Up (user‑felt)

- “Freeze” under load: worst pause mid‑reply jumps from ~10–20 ms baseline to ~500–600 ms under long prefill storms.
- Total time for multi‑step agents: with many active decoders and/or long contexts (and MoE), per‑stream tokens/sec collapses ⇒ chains finish much slower.

## Effects That Skew Results

- Prompt cache / LCP reuse: repeated or similar prompts look unrealistically good (low TTFT/prefill). Use unique prompts to reveal real prefill cost.
- Slot admission: if `parallel < concurrency`, TTFT balloons from queueing (not decode fairness). Match slots to concurrency to isolate decode effects.

## MoE Insight (why scaling degrades faster)

- With MoE, batching more tokens activates more distinct experts per step. The FFN weight set touched per tick grows with slots, so step time inflates more than the dense‑model intuition suggests. Result: per‑stream t/s drops quickly as slots increase.

## “Sweet Spot” Concept

- Throughput per system (tokens/sec) peaks near an operating point where each tick is filled up to a safe wall‑time (e.g., 40–60 ms) without overshooting. Too few decoders under‑utilizes; too many (or MoE/long context) pushes ticks long and reduces overall throughput and UX.

## What This Implies for UMA‑Serve

- Focus on policy and overhead, not kernels:
  - Keep ticks short and predictable (decode‑first, time‑guarded).
  - Throttle/shape prefill and served background decoders to stay under a time/bandwidth budget (ΣBMT guard).
  - Reduce plumbing cost (UDS, zero‑copy logits, overlapped sampling) so we maintain higher throughput at the same latency cap.

## Benchmarks We’ll Use to Validate

- Two‑app “chat vs big paste” (unique prompts): report TTF5, max stall(3s).
- Prefill‑only storm (unique long prompts): confirm stall protection vs stock.
- Agent chain under decode load: chain slowdown factor vs baseline.
- Long‑context sweeps (8k/16k/32k): inter‑p95 and aggregate tokens/sec.
- Dense vs MoE concurrency sweep: earlier plateau and steeper per‑stream drop for MoE.

This document will evolve as we discover optimizations (e.g., sampling overlap gains) and validate UMA‑Serve policy changes on the same benchmarks.

