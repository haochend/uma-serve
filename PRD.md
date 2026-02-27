# SwarmServe-MoE PRD (Apple Silicon, Big Local MoE)

## 1. One-Sentence Vision

A lean, high-performance local inference runtime purpose-built for real agentic workloads on Apple Silicon, optimized for one very large text-only MoE model (for example MiniMax M2.5-class) serving 8-16+ concurrent agent loops with heavy tool calling and shared workspace.

## 2. Product Thesis (Narrow and Explicit)

SwarmServe-MoE is not a general local serving stack.

It is a specialized runtime for the hardest local workload on Apple M silicon:

- one huge text MoE model
- multiple concurrent agent loops
- tool-heavy short decode bursts
- shared workspace state
- strict memory/offload pressure

The goal is to improve agent-turn throughput and tail latency under MoE memory pressure, not just maximize single-stream tokens/sec.

## 3. Problem Statement (Validated)

Local multi-agent systems using large MoE models are bottlenecked by four coupled problems:

- expert residency churn: different agents/roles activate different experts, causing page-in/page-out stalls
- per-agent KV growth: 8-16 paused/resumed loops accumulate KV quickly
- scheduler inefficiency: generic runtimes treat requests uniformly and can let long/background decodes monopolize progress
- tool-heavy interaction pattern: frequent short turns and handoffs create high TTFT sensitivity and poor batching efficiency

Result:

- thrashing / stalls / OOM before useful concurrency
- weak agent-turn throughput despite high local memory bandwidth
- poor p95 latency for high-priority steps (for example executor/critic)

## 4. Solution & Core Value Proposition

Build **SwarmServe-MoE** as a thin, MoE-aware runtime layer on top of `llama.cpp` that adds:

- MoE expert residency management and speculative prefetch
- offload-aware preemptive continuous batching
- paged KV arena with fast pause/resume
- shared-workspace prompt/prefix reuse (not naive universal KV sharing)
- an agent-step API that can carry scheduling metadata when available

Differentiation:

- Existing Apple-local runtimes can improve batching and KV behavior.
- SwarmServe-MoE is specifically optimized for MoE expert residency/offload behavior under agent concurrency.

## 5. Target Scope (Apple M Silicon Only for v0.x)

### Primary Target

- Hardware: Apple M silicon only (start with one tested machine/config, e.g. M4 Max or M4 Ultra)
- Backend: Metal / UMA only
- Model class: one large text-only MoE at a time (MiniMax M2.5-class first, or closest available compatible path)
- Workload: 8-16 concurrent tool-using agent loops with mixed priorities
- Deployment: single machine only

### Non-Goals (v0.x)

- dense-model optimization as a primary goal
- multimodal (vision/audio/video)
- distributed / multi-node inference
- multi-model serving/router mode
- training or fine-tuning
- full agent framework integration (API only)
- full OpenAI API parity in MVP

## 6. Key Design Principles (from Discussion)

### 6.1 Differentiation Priority Is Not the Same as Execution Order

Feature priority in this PRD is based on expected improvement over existing Apple-local solutions for large-MoE agent workloads, not implementation ease.

Roadmap sequencing may differ due to dependencies, instrumentation, and correctness requirements.

### 6.2 Foundational vs Differentiating Features

`continuous batching` and `paged KV` are important and likely foundational on Apple Silicon. They are not "optional."

However, the unique product wedge for large local MoE workloads is MoE-specific memory/offload control:

- expert residency policies
- prefetch hit rate
- reduced expert page-in stalls

### 6.3 Metadata-Dependent Scheduling Must Be Explicit

True role-aware scheduling requires clients/agents to send metadata such as:

- `role`
- `priority`
- `resume_id`
- optional `deadline_ms` / latency class

Without that metadata, the runtime still benefits from:

- preemptive batching core
- heuristic prioritization
- short-step bias

But it should not claim full role-aware policy behavior.

### 6.4 Prefill and Decode Need Different MoE Strategies

Long prefills behave differently from decode/resume:

- large prefill chunks can cause per-layer expert unions to approach "most experts"
- this reduces the value of speculative expert prefetch and can collapse into layer-like traffic

Implication:

- speculative expert prefetch is a **decode/resume optimization first**
- prefill optimization focuses on **chunked prefill + reactive expert copy + prompt/prefix caching**
- chunked prefill may enable rolling chunk-to-chunk expert prefetch if expert-union saturation stays low

## 7. Feature Priorities (Differentiation-First)

### P1. MoE Expert Residency Manager + Speculative Prefetch (Decode/Resume First)

Goal:

- reduce expert page-in stall time under multi-agent MoE concurrency
- keep likely experts hot using role/history/routing signals

Capabilities:

- expert hotness tracking by role and recent requests
- residency policy and memory budget
- async prefetch queue (Metal)
- decode/resume-first prefetch policy (`bs1` and short-step agent workloads)
- predictor modes:
  - heuristic-first (MVP-safe)
  - gating/hidden-state informed (experimental / later)

Validation note:

- prefill-side speculative prefetch is not assumed to help until chunk-size and expert-union data prove it.

### P2. Offload-Aware Preemptive Continuous Batching

Goal:

- maximize agent-turn throughput while protecting latency for short/high-priority steps

Capabilities:

- token-level or short-quantum scheduling
- preemption at safe boundaries
- fairness/starvation guardrails
- offload-aware scheduling signals (queue cost, expected memory movement)

### P3. Paged KV Arena + Chunked Prefill + Prefix Reuse

Goal:

- sustain more paused/resumed agents without thrash
- reduce TTFT and prompt rebuild cost for shared prefixes
- make long prefill schedulable and compatible with MoE offload constraints

Capabilities:

- KV block allocator
- pause/resume persistence
- eviction policy
- chunked prefill execution (small chunk sizes with scheduler control points)
- chunk-level expert-union telemetry (to decide if rolling prefetch is worth enabling)
- exact token-prefix cache with memory cap + telemetry

Note:

- "shared workspace" does not imply generic KV sharing; reuse is prefix/token compatibility dependent.

### P4. Agent API & Tooling

Goal:

- enable metadata-rich scheduling when available without blocking core runtime gains

Capabilities:

- `/agent-step` endpoint with metadata
- minimal OpenAI-compatible shim later
- tool result fast-path (zero-copy server buffers + token/prefix cache reuse where compatible)

## 8. Execution Plan (Dependency / Risk Managed)

Execution uses two tracks:

- `Track A (Differentiation)`: decode/resume expert-prefetch validation and MoE residency work
- `Track B (Foundation)`: continuous batching policy, paged KV, chunked prefill, prompt cache

Continuous batching is not a separate "later" feature. It is the foundation layer for all phases below.

### Phase 0: Baseline and Measurement Contract (2-3 days)

- pick one exact target: hardware, macOS version, model, quantization, backend build, prompt template
- define "thrash" and "stall" thresholds
- implement benchmark harness for 4/8/16-agent scenarios
- record baseline on stock `llama.cpp` server
- measure prefill chunk-size sensitivity (throughput and latency) on the target model
- measure per-layer expert-union growth vs prefill chunk size (`16/32/64/128`)

Required outputs:

- TTFT, inter-token latency p50/p95/p99, turns/sec, memory footprint, stall counters
- expert-copy bytes/time and expert-union-per-layer-per-chunk (for MoE paths)

### Phase 1: Decode/Resume Expert-Prefetch Validation Spike (5-7 days, Track A)

- instrument MoE expert routing / selected expert IDs on the target model path
- run `bs1` decode and short-step agent traces to validate expert locality and stall behavior
- implement heuristic expert prefetch (feature-flagged) for decode/resume only
- compare `none` vs `layer` vs `expert` prefetch behavior (stall time, ITL, tok/s)

Why first:

- validates the highest-differentiation hypothesis before deeper runtime changes
- avoids over-investing in a prefetch system that may not pay off on target workloads

### Phase 1B: Continuous Batching Foundation Upgrades (parallel/overlapping, Track B)

- strengthen preemptive continuous batching (works without role metadata)
- add short-step bias, fairness/starvation guardrails, and queue telemetry
- treat prefill chunks as schedulable units (not monolithic prefill bursts)
- expose scheduler counters needed by Track A experiments

### Phase 2: Paged KV Arena + Chunked Prefill (7-10 days, Track B)

- KV block allocator and resume path
- chunked prefill controller (start with small chunk sizes if throughput remains flat-ish)
- resume latency and prefill interruptibility telemetry
- optional rolling chunk-union prefetch experiment (only if expert-union saturation stays bounded)

### Phase 3: Aggressive Prompt/Prefix Caching (5-7 days, Track B)

- exact token-prefix cache with memory cap + LRU eviction
- shared workspace/system prompt prefix reuse
- cache hit/miss telemetry and TTFT attribution
- aggressive cache policy tuning for agent swarm workloads (warm/cold reporting required)

### Phase 4: MoE Residency Manager Productization + Metadata-Rich Policy (ongoing)

- expand decode prefetch from validation spike to runtime residency manager
- expert hotness tables by role/request class
- residency budget and eviction
- async Metal prefetch queue with latency guard / budget throttling
- role-aware scheduling policy when clients send metadata
- gating/hidden-state informed predictor if hooks are available
- upstream minimal hooks to `llama.cpp`
- OpenAI compatibility shim and examples

Detailed milestone sequencing and engineering tasks live in `docs/ENGINEERING_ROADMAP_MOE.md`.

## 9. API Surface (MVP and Forward-Compatible)

### Core Endpoint (MVP)

`POST /agent-step`

Required request fields (MVP):

- `agent_id`
- `workspace_id`
- `messages` or prompt payload

Optional scheduling metadata (enables role-aware policy):

- `role`
- `priority`
- `resume_id`
- `deadline_ms`
- `latency_class`
- `shared_prefix_id`

### Compatibility Notes

- Runtime provides value without metadata via generic preemptive scheduling.
- Full role-aware claims require client support for metadata.

## 10. Success Metrics (Apple M + Big-MoE Specific)

### Primary Product Metrics

- agent turns/sec at 8 and 16 concurrent loops
- `agent-step` latency p50/p95/p99 (overall and by role when metadata is present)
- sustained no-thrash operating point (30+ minute run, no OOM/runaway stalls)

### Latency and Streaming Metrics (must report)

- TTFT p50/p95
- inter-token latency (ITL/TBT) p50/p95/p99
- decode-only throughput and end-to-end throughput (reported separately)

### MoE / Memory Metrics (differentiation metrics)

- expert prefetch hit rate
- expert page-in stall time (ms/turn, ms/token)
- offload bytes/sec and read amplification
- KV resume overhead (ms)
- cache hit/miss rate (prefix cache, KV resume path)
- SSD stall percentage (if disk-backed offload is used)
- per-layer unique experts per prefill chunk (chunk-size sweep)
- chunked prefill interruptibility / pause overhead

### Scheduler Quality Metrics

- fairness/starvation incidents
- queue wait time by class
- preemption count and resume success rate

## 11. Benchmark Methodology (Required for Credible Claims)

This section is mandatory for any benchmark claim in release docs or PRs.

### 11.1 Baselines (Apple-Specific)

At minimum:

- `llama.cpp` server (matched model, quantization, context, and generation settings)

If feasible on the same hardware/model path:

- an MLX-based local server/runtime
- MLC-LLM (or comparable Apple-focused runtime)

### 11.2 Reporting Rules

Always report:

- hardware (exact Apple chip, RAM, storage)
- macOS version
- Xcode/Metal toolchain version (if relevant)
- `llama.cpp` commit and SwarmServe-MoE commit
- model name + quantization + template
- prompt length / output length distributions
- concurrency level and workload mix
- warm vs cold runs
- number of trials and variance/error bars

### 11.3 Workload Classes (to prevent cherry-picking)

- unique prompts (prompt cache disabled or neutralized)
- shared-prefix prompts (agent workspace/system prompt reuse)
- tool-heavy short-turn workloads
- long-context stress workloads
- mixed-priority agent loops (when metadata is available)

### 11.4 Metric Boundaries

Define and report exact measurement boundaries for:

- TTFT (client-perceived)
- inter-token latency
- decode-only throughput
- end-to-end throughput
- cache warm/cold state

## 12. Architecture (High-Level)

```text
SwarmServe-MoE
├── external/llama.cpp/             # upstream submodule (minimal hooks only)
├── core/                           # C++ hot path
│   ├── moe_residency_manager.cpp   # expert hotness, residency policy, prefetch queue
│   ├── preemptive_batcher.cpp      # preemption, fairness, scheduling lanes
│   ├── paged_kv_arena.cpp          # KV blocks, eviction, pause/resume
│   ├── prefix_cache.cpp            # exact token-prefix cache + LRU budget
│   └── telemetry.cpp               # stall, queue, cache, latency metrics
├── server/                         # thin control plane (C++ or Python)
│   └── agent_step_api.*            # /agent-step, metadata handling, tool dispatch
├── agents/                         # examples only (not a framework)
└── benchmarks/                     # reproducible swarm workloads and traces
```

## 13. Risks and Assumptions

### Key Assumptions

- target MoE model is runnable via a `llama.cpp`-compatible path on Apple Metal
- a single-model deployment is sufficient for product validation
- role metadata may not be available initially from existing agent clients

### Risks

- `llama.cpp` internal API churn complicates hooks
- predictor accuracy is weak at first, reducing prefetch gains
- prefetch can regress latency if it contends with active decode
- benchmark comparability is difficult across runtimes if templates/settings differ

### Mitigations

- keep upstream patches minimal and generic
- ship heuristic predictor first with feature flags
- instrument prefetch hit/miss and disable automatically if regressions are detected
- publish strict benchmark configs and scripts

## 14. References (for Scope and Bench Methodology)

- `arXiv:2601.19139` (Apple Silicon local serving / prefix cache findings)
- `arXiv:2511.05502` (Apple Silicon runtime comparisons, batching/paged-KV/latency methodology)
