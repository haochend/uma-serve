# UMA‑Serve — Optimization Catalog (Prioritized)

This document tracks high‑value optimizations for UMA‑Serve. It groups items into UMA‑specific wedges, general serving improvements, and research/frontier ideas. Priorities reflect impact vs. effort for the next phases. Items marked “(blog)” are strong candidates for educational posts.

Legend
- P0: Near‑term (next 1–2 milestones)
- P1: Mid‑term
- P2: Research/Frontier

---

## A) UMA‑Specific Optimizations

1) ΣBMT (Bytes‑Moved‑per‑Token) budgeting — P1 (blog)
- Shape per‑tick work by a bandwidth budget: weights slice + KV R/W + intermediates.
- Goal: stabilize p95 and reduce jitter on UMA devices.

2) Zero‑copy logits + shared‑memory sampler — P2 (blog)
- Metal shared storage for logits + CPU sampler overlap; GPU decodes token N while CPU samples N‑1.

3) UMA‑aware prefill/decode priority — P1
- Throttle PREFILL when bandwidth is saturated; ensure DECODE runs on time.

4) KV precision & layout tuning (head‑major; fp16 → q8/q6) — P1
- Reduce KV traffic and improve effective bandwidth on UMA.

5) CPU+GPU cooperative schedule (UMA overlap) — P2
- Split small ops and sampling to CPU while GPU stays busy; no copies.

---

## B) General Serving Optimizations

6) Continuous batching + latency guard — P0 (blog)
- Policy: decode‑first + budgeted prefill with SLO‑aware guard to protect TTFT/TBT.
- Near‑term: implement guard as a policy transformer with hysteresis.

7) Token‑boundary preemption — P0
- Insert high‑priority tokens at boundaries to protect interactive TBT.

8) Decode fairness + prefill draining — P0 (done: baseline; blog)
- Two‑phase tick (1 token/stream for DECODE; then PREFILL). Good teaching value.

9) Prefix KV snapshot cache — P1 (blog)
- Snapshot KV for common prefixes; restore on hit to skip prefill.

10) Admission control / QoS lanes — P0
- Separate interactive vs background queues; throttle/drop background under guard.

11) UDS + binary/framed protocol — P0 (done; blog)
- Keep JSON frames; avoid HTTP overhead; no JSON in hot loop.

12) Scheduler as policy (planner) — P0 (blog)
- Extract `IBatchPolicy` + transformers (LatencyGuard, QoS, etc.); executor stays minimal.

---

## C) Frontier / Speculative Optimizations

13) Speculative decoding (draft + target, assisted heuristics) — P2 (blog)
- Draft model on CPU + target on GPU (UMA pipeline), or self‑speculation via logits.

14) Logits sharing between decode & sampling — P2
- True sharing/overlap across devices; strong UMA wedge.

15) Multi‑token logits reuse / assisted lookahead — P2
- Heuristics to reuse logits and guess N+1; helps speculation.

16) MoE‑aware scheduling — P2
- Estimate expert activation cost; avoid bandwidth spikes; integrate with ΣBMT.

17) Persistent graphs for decode (Metal) — P1/P2
- Metal graph capture for predictable TTFT/TBT; great UMA fit.

18) Opportunistic parallel decode scheduling — P2
- Back‑to‑back graphs with minimal CPU stalls; pipeline behavior without new kernels.

19) Prompt‑cache with partial‑match (trie) — P2 (blog)
- Partial prefix reuse beyond exact matches; powerful for repeated patterns.

20) Hybrid CPU/GPU execution for small models — P2
- UMA allows efficient split of QKV/attn/MLP across devices with low copy costs.

---

## Near‑Term Plan (P0/P1 roll‑up)

- P0
  - Policy separation (planner + transformers).
  - Latency guard transformer with hysteresis.
  - Token‑boundary preemption.
  - Admission/QoS lanes.
  - Protocol/UDS hardening and CLI (JSON framing finished; CLI next).

- P1
  - Prefix KV snapshot cache (LRU + memory cap).
  - UMA‑aware prefill throttling; initial ΣBMT estimator.
  - KV precision/layout tuning.
  - Persistent graphs (where supported).

- P2
  - Zero‑copy logits + sampler overlap.
  - Speculative decoding (draft/self‑speculation) and assisted lookahead.
  - MoE‑aware heuristics; Paged‑KV + ΣBMT integration; partial‑match prompt cache; hybrid CPU/GPU execution.

This catalog is intentionally succinct to anchor planning and blogging. Detailed designs live in POLICY.md, SCHEDULER.md, MEMORY.md, and the System Design roadmap.

