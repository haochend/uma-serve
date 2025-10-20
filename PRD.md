UMA Serve — Product/Tech/Business Brief (Runtime-First)
1) Executive summary

UMA Serve is a local, multi-app LLM runtime for Mac (Apple Silicon) and AMD UMA APUs that turns one machine into a small multi-tenant server. It does not change kernels; it orchestrates them better:

One weights map per node shared across apps.

Token-level continuous batching with a latency guard.

BMT (Bytes-Moved-per-Token) budgeting to keep tail latency flat.

Zero-copy logits + low-overhead IPC so the CPU sampler overlaps the next GPU token.
We ship a daemon, light editor adapters, and a benchmark pack that proves tighter p95 and lower overhead vs stock local servers.

2) Problem & target users

Problem: Local LLM users (devs, small teams) run multiple apps (IDE, terminal, browser) on one machine. Stock servers batch greedily and use HTTP/JSON, so interactive latency spikes and memory is duplicated across processes.

Who:

Individual Mac/AMD APU developers running local assistants/coders.

Small teams with a Mac mini/Studio or mini-PC APUs acting as a shared box.

Tooling vendors that want a low-tail local backend.

3) Value proposition

Snappy under load: Keep interactive p95 ≤ 1.2× solo baseline even while long jobs run.

Higher throughput at the same latency cap: +30–60% tokens/s vs each-app-separate, via continuous micro-batching with a guard.

Lower overhead: −5–15% TTFT, −10–20% CPU, ≈0 copies/token by using UDS + zero-copy logits.

Lower memory: 1× model RSS for N apps (one weights map), not N×.

4) Scope (runtime only)

In scope (v1):

Daemon (“umad”) embedding llama.cpp backends (Metal, Vulkan/HIP).

Scheduler: continuous batching, latency guard, ΣBMT budget, QoS lanes, token-boundary preemption, session pinning.

Zero-copy logits path (shared buffer the CPU sampler reads in-place).

UDS/Named-pipe IPC + thin HTTP gateway (optional, not in hot path).

KV manager (layout packing, optional KV quant selection; wiring, not kernels).

Scorecard metrics and reproducible benchmarks.

Out of scope (v1):

Kernel rewrites/fusions beyond what llama.cpp provides.

Cross-box TP/PP. (We support request-parallel scaling via a simple router.)

NPU offload (optional later; CPU sampler first).

5) Architecture (high level)

Per node:

umad process:

Loads model once (read-only mapping).

Allocates persistent pools/argument buffers (no per-token allocs).

Exposes UDS API and optional HTTP gateway.

Runs the scheduler loop (ready queue → token-merge under guard & BMT budget).

Writes logits to a shared buffer; CPU sampler (BNNS/xnnpack) consumes in-place while GPU starts next token.

Clients (IDE plugin, CLI, agents) connect via UDS; server streams tokens (SSE or framed UDS messages).

Multi-node:

Optional coordinator (router) that places sessions on nodes by p95 headroom and ΣBMT; sessions are sticky.

6) Core features (v1)

Continuous batching + latency guard

Token-level coalescing (2–8) with p95 cap (default 1.20× of solo baseline).

Token-boundary preemption to insert interactive tokens.

BMT budgeting

Compute per-request bytes moved per token (weights slice est., KV, intermediates).

Enforce a ΣBMT budget (GB/s) to avoid saturating UMA bandwidth; throttle background flows.

Zero-copy logits & overlapped sampling

Shared buffer (Metal StorageModeShared / Vulkan HOST_VISIBLE / HIP pinned) for logits.

CPU sampler reads in place while GPU starts the next token.

KV manager (runtime wiring)

Switch for KV layout (head-major) and KV precision (fp16/q8/q6) using llama.cpp hooks.

Page pre-touch hooks (optional) and persistent pools (no per-token allocs).

Low-overhead IPC

Unix domain sockets (or Windows named pipes later).

Optional OpenAI-compatible HTTP gateway for tools that need it (not hot path).

Scorecard & observability

/metrics JSON and CSV logs: tokens/s @ p95 cap, p95/p99, ΣBMT, scheduler efficiency, copy-free ratio, GPU-idle%, residency-churn/token, TTFT, RSS.

7) APIs & CLI (developer-friendly)

CLI

umad serve \
  --model qwen-coder-30b-q8.gguf \
  --latency-cap 1.20 \
  --bmt-budget 320GBps \
  --kv-type f16|q8|q6 \
  --kv-pack head-major \
  --ipc uds \
  --http off


Runtime config (YAML)

latency_cap: 1.20
bmt_budget_gbps: 320
qos:
  interactive: {priority: 10}
  background:  {priority: 1}
scheduler:
  max_merge: 4
  preempt_at_token: true
logging:
  scorecard_interval_s: 5


Endpoints

POST /generate (SSE stream, or UDS framed)

POST /load, POST /unload

GET /metrics (scorecard JSON)

POST /priority (promote/demote session QoS)

Metrics JSON (excerpt)

{
  "model":"qwen-coder-30b-q8",
  "tokens_per_s_at_cap": 88.4,
  "latency_cap": 1.20,
  "p95_ms": 57.2, "p99_ms": 85.1,
  "sum_bmt_mb_per_token": 312.0,
  "scheduler_efficiency": 0.86,
  "copy_free_ratio": 0.97,
  "gpu_idle_pct": 4.3,
  "residency_churn_mb_per_token": 0.0,
  "rss_gb": 31.1
}

8) Benchmarks we will publish (release blog)

A) Mixed-load SLO (3 interactive + 1 long-ctx)
Compare LM Studio / llama.cpp server (CB on) vs UMA Serve on the same box.
Report: tokens/s @ p95 cap, p95/p99 per client, ΣBMT, scheduler efficiency, RSS, GPU-idle%.
Goal: hold interactive p95 ≤ 1.2×, aggregate tokens/s +30–60% vs each-app-separate; near-parity throughput vs stock server but flatter tails; RSS ~1× model.

B) Long-context KV sweep (16k/32k)
KV fp16→q8→q6 with head-major packing.
Report: tokens/s vs ΣBMT; show throughput tracks bytes.
Goal: +15–40% tokens/s at long T by cutting KV bytes (platform-dependent).

C) Overhead microbench
UDS+zero-copy vs HTTP/JSON on single stream.
Goal: TTFT −5–15%, CPU −10–20%, copies/token ≈ 0.

9) Competitive positioning

Versus LM Studio / stock llama.cpp server: Both have CB; we add SLO enforcement (latency guard + BMT budget), token preemption, UDS/zero-copy path, and single-map multi-app UX with a public scorecard. Result: tighter tails under mixed load and lower system overhead.

Versus vLLM/sglang on GPUs: Those win at peak throughput and TP/PP. We interoperate (optional router) and own the local, low-tail, multi-app wedge on UMA nodes.

10) Risks & mitigations

Small or no wins vs tuned stock server → Make p95-at-cap the north-star, not raw T/s; publish mixed-load SLO results.

Driver/SDK quirks (AMD Vulkan/HIP coherence) → Use conservative barriers; zero-copy only for small tensors; keep HTTP gateway as fallback.

Feature creep into kernels → Guardrail: no kernel edits in v1; rely on llama.cpp knobs (KV type/layout).

Adoption friction → Homebrew installer, LaunchAgent/Service, one-click VS Code/Zed adapters.

11) Delivery plan

Week 1–2:

UDS server + SSE gateway; session model loading; continuous batching with latency cap; basic scorecard; Homebrew formula.

Week 3:

ΣBMT estimator + budget governor; token-boundary preemption; zero-copy logits path; CPU sampler overlap; KV layout/quant wiring.

Week 4:

Bench harness + three publishable charts; VS Code/Zed adapter; docs & samples.

Go/No-Go gate: Mixed-load benchmark shows p95 ≤ 1.2× with ≥30% aggregate throughput gain vs each-app-separate and flat RSS. If not, iterate on scheduler/IPC, not kernels.

12) Open questions

Minimum macOS/driver versions for stable shared-buffer handoff? (We’ll document tested combos.)

Default ΣBMT budget per platform (derive from simple roofline or probe at boot).

Whether to expose per-client soft SLOs (e.g., 1.1× vs 1.3×) and dynamic reprioritization from adapters.

AMD first backend: Vulkan (portable) vs HIP (perf on Linux)? (Start with Vulkan + HOST_VISIBLE logits.)

13) What we will not do (to stay focused)

No kernel fusion work in v1 (keep to llama.cpp).

No cross-machine TP/PP; only request-parallel routing.

No NPU offload complexities until we prove the serving wedge.

Bottom line: UMA Serve is a runtime that makes one node feel like a small, low-tail LLM server for multiple local apps. It’s measurable, shippable in a month, and complementary to llama.cpp—exactly the kind of wedge that wins adoption without new kernels.
