# Engineering Roadmap — SwarmServe-MoE (Apple M Silicon)

## Purpose

Translate the product PRD into an execution plan for the current codebase (`uma-serve`) and the vendored `llama.cpp` submodule, with explicit validation gates before large architecture investments.

This roadmap assumes:

- target hardware: Apple M silicon (start with one machine/config, e.g. M4 Max 128GB)
- target workload: big text-only MoE + 8-16 agent loops
- target model path: `llama.cpp` (MiniMax M2-family supported in current submodule)

## Roadmap Structure

Two tracks run in parallel:

- `Track A (Differentiation)`: MoE expert residency/prefetch experiments and productization
- `Track B (Foundation)`: continuous batching policy, paged KV, chunked prefill, prefix cache

Continuous batching is a dependency for both tracks, not a separate late feature.

## Method Selection for Apple UMA (MoE-Infinity vs fMoE + Add-Ons)

### Decision

Use a staged hybrid:

- start with a `MoE-Infinity`-style offload control loop (residency budget, async page-in/page-out, reactive + near-term prefetch)
- add `fMoE`-style expert-map/trajectory matching only if baseline predictor hit rate is not enough
- layer in practical cache policies (`Fate`/`HOBBIT` style) as first-class modules, not optional tweaks

### Why This Order

- current `llama.cpp` integration point is already a reactive selective expert copy path, so MoE-Infinity-style control is the lowest-risk fit
- fMoE-style matching can improve predictor quality, but it has higher implementation and telemetry complexity
- for Apple UMA, cache policy quality is often higher ROI than adding a complex predictor too early

### Techniques to Include Alongside Prefetch (Priority-Ordered)

1. `P0`: shallow-favoring expert pinning (keep early-layer hot experts resident)
2. `P0`: score-based eviction (miss-cost aware, not plain LRU/LFU)
3. `P1`: mixed-precision cache tiers (hot/warm/cold experts)
4. `P1`: phase-aware policy split (`decode/resume` vs `prefill/chunked prefill`)
5. `P2`: trajectory/map matcher (fMoE-style) for harder workloads
6. `P2`: offline hot-expert profiling for static reservation defaults

### Target Framing

The goal is not theoretical parity with full-DDR residency in all cases.
The goal is to approach DDR-like latency by driving expert cache hit rate high enough that SSD/page-in misses become infrequent on target agent workloads.

## Current Codebase Reuse Map (What We Already Have)

### Reusable Now

- daemon/event loop and model lifecycle: `/Users/davidd/dev/uma-serve/src/umad/main.cpp`
- runtime config/model init wrapper: `/Users/davidd/dev/uma-serve/src/runtime/config.cpp`, `/Users/davidd/dev/uma-serve/src/runtime/model.cpp`
- continuous batching skeleton + policy interface: `/Users/davidd/dev/uma-serve/src/sched/scheduler.cpp`, `/Users/davidd/dev/uma-serve/src/sched/policy.h`
- metrics plumbing and JSON snapshots: `/Users/davidd/dev/uma-serve/src/metrics/metrics.h`, `/Users/davidd/dev/uma-serve/src/metrics/metrics.cpp`
- e2e/unit test harness patterns: `/Users/davidd/dev/uma-serve/tests/cpp`, `/Users/davidd/dev/uma-serve/tests/e2e`

### Relevant `llama.cpp` Hooks Already Present

- MiniMax M2 builder path: `/Users/davidd/dev/uma-serve/external/llama.cpp/src/models/minimax-m2.cpp`
- MoE top-k tensor creation/naming (`ffn_moe_topk`): `/Users/davidd/dev/uma-serve/external/llama.cpp/src/llama-graph.cpp`
- graph/eval callback hook in `llama_context_params`: `/Users/davidd/dev/uma-serve/external/llama.cpp/include/llama.h`
- MoE-aware selective expert copying in backend scheduler: `/Users/davidd/dev/uma-serve/external/llama.cpp/ggml/src/ggml-backend.cpp`

## Phase 0 — Measurement Contract and Baselines (Required)

### Goals

- pin a single reproducible target setup
- establish baseline behavior before any MoE prefetch changes
- verify chunk-size sensitivity and expert-union saturation behavior

### Deliverables

- baseline benchmark config (hardware/software/model/quant/template)
- metrics schema for experiment logs
- chunk-size sweep report (`16/32/64/128`)
- expert-union-per-layer-per-chunk report

### Tasks

1. Pin environment
- record exact Apple chip, RAM, macOS, Xcode/Metal toolchain
- record `llama.cpp` submodule commit and local repo commit
- record model/quant/template and llama flags

2. Baseline decode/prefill measurement
- TTFT, ITL p50/p95/p99, end-to-end tok/s, decode-only tok/s
- expert-copy bytes/time (MoE path only)
- stall attribution (expert copy vs other)

3. Chunk-size sensitivity sweep
- run prefill with chunk sizes `16/32/64/128`
- confirm prior observation: smaller chunks do not materially hurt step performance
- choose default chunk size based on offload/control behavior, not just raw throughput

4. Expert-union saturation logging
- per layer, per chunk:
  - `n_unique_experts`
  - `n_experts_total`
  - `n_experts_used_per_token`
- determine whether rolling chunk-to-chunk prefetch is plausible

### Go / No-Go Gate

- proceed to Track A decode prefetch if expert-copy stall is measurable and decode locality exists
- proceed to chunked prefill work if small chunks remain within acceptable throughput regression

## Track A — MoE Expert Prefetch (Differentiation)

## A1. Decode/Resume Expert Prefetch Validation Spike (bs1, short-step traces)

### Goals

- validate the core hypothesis: expert-prefetch reduces decode/resume stall on target MoE
- avoid overbuilding before proof

### Scope

- decode/resume only (`bs1` and short-step agent traces)
- no prefill-side speculation yet
- feature-flagged and instrumentation-heavy

### Initial Implementation Strategy

Use the existing `llama.cpp` MoE selective-copy path as the primary insertion point:

- observe routed expert IDs where `GGML_OP_MUL_MAT_ID` experts are already identified
- time expert-copy operations
- optionally issue prefetch hints for predicted upcoming expert ranges before demand copy

Primary patch location:

- `/Users/davidd/dev/uma-serve/external/llama.cpp/ggml/src/ggml-backend.cpp`

### Validation Modes

Compare:

- `none`: demand-only expert copy
- `layer`: coarse layer-ish prefetch baseline (expected to lose)
- `expert-basic`: MoE-Infinity-style predicted expert prefetch
- `expert-map`: fMoE-style map/trajectory matcher prefetch

### Metrics

- expert page-in/copy stall time (ms/token, ms/turn)
- ITL p50/p95/p99
- effective tok/s
- prefetch hit rate
- prefetch useful rate / wasted rate
- prefetch bytes/sec and demand bytes/sec

### Go / No-Go Gate

Promote to productization only if:

- prefetch reduces expert-copy stall time meaningfully on decode/resume
- ITL does not regress materially
- gains persist on short-step agent traces (not just synthetic single stream)

## A2. MoE Residency Manager Productization

### Goals

- turn the spike into a runtime subsystem with budgets and guardrails

### Tasks

- expert hotness tables (initially request-class or role, later metadata-aware)
- residency budget and score-based eviction policy (miss-cost aware)
- shallow-favoring layer pinning policy
- async prefetch queue (Metal-aware)
- mixed-precision expert cache tiers (hot/warm/cold)
- latency-aware throttling (disable prefetch when ITL/TTFT regresses)
- telemetry integration in runtime metrics

### Future (Optional / Research)

- gating/hidden-state informed predictor
- chunk-to-chunk prefill prefetch (only if chunk-union saturation remains bounded)
- virtual-expert tuning for strict memory budgets

## Track B — Foundation (Continuous Batching, Paged KV, Chunked Prefill, Cache)

## B1. Continuous Batching Foundation Upgrades

### Goals

- make scheduler behavior stable and observable for MoE experiments
- support chunked prefill as first-class schedulable work

### Existing Code to Extend

- `/Users/davidd/dev/uma-serve/src/sched/policy.h`
- `/Users/davidd/dev/uma-serve/src/sched/policy.cpp`
- `/Users/davidd/dev/uma-serve/src/sched/scheduler.cpp`
- `/Users/davidd/dev/uma-serve/src/metrics/metrics.h`

### Tasks

- keep decode-first / short-step bias as default
- add fairness/starvation counters and queue wait histograms
- treat prefill chunks as explicit schedulable units
- expose per-tick decode/prefill token counts and chunk timings
- preserve operation without role metadata

### Exit Criteria

- stable mixed-load behavior under chunked prefill
- scheduler telemetry sufficient to explain regressions in Track A experiments

## B2. Paged KV + Chunked Prefill

### Goals

- preserve many paused agents without thrash
- create prefill control points compatible with MoE offload constraints

### Tasks

1. Chunked prefill controller
- configurable chunk sizes (start with `16/32/64`)
- scheduler-integrated chunks (interruptible boundaries)
- per-chunk timing and queue impact

2. Paged KV arena
- block allocator
- pause/resume persistence
- eviction policy and telemetry
- resume overhead metrics

3. Optional rolling chunk-union prefetch experiment
- only enabled if Phase 0 shows bounded expert-union sizes for chosen chunk size
- use chunk `N` observed experts to prefetch chunk `N+1`

### Exit Criteria

- chunked prefill does not materially regress total throughput on target model
- improved prefill interruptibility / lower worst-case blocking
- bounded memory behavior with target concurrency

## B3. Aggressive Prompt / Prefix Caching

### Goals

- reduce prefill work entirely where possible
- improve TTFT on shared-prefix agent workloads

### Tasks

- exact token-prefix cache
- memory cap + LRU eviction
- cache hit/miss telemetry
- warm/cold benchmark reporting
- shared workspace/system prompt prefix reuse

### Metrics

- TTFT reduction on shared-prefix workloads
- cache hit rate and hit-bytes
- cache memory usage and eviction rate

### Exit Criteria

- measurable TTFT improvement on agent workloads with shared prefixes
- no correctness regressions on cache miss/fallback paths

## Engineering Experiments (Concrete Sequence)

## Experiment 1 — Decode Expert Prefetch Feasibility (First)

Goal:
- validate expert-prefetch on decode/resume before deeper runtime work

Inputs:
- target MoE model
- bs1 decode traces
- short-step agent traces
- policy variants: `expert-basic` vs `expert-map`

Output:
- decision memo: `expert prefetch viable / not viable`
- recommendation: keep `expert-basic` only, or invest in `expert-map`

## Experiment 2 — Chunked Prefill Sweep + Expert-Union Saturation

Goal:
- choose chunk size based on control + offload behavior

Outputs:
- default chunk size
- decision on whether chunk-to-chunk prefetch is worth attempting

## Experiment 3 — Prompt Cache ROI on Agent Workloads

Goal:
- quantify TTFT and prefill reduction under shared-prefix workloads

Outputs:
- cache sizing defaults
- expected hit-rate range on target workloads

## Reporting and Benchmark Rules (Engineering)

Always include in internal experiment reports:

- exact hardware/software/model/quant/template
- `llama.cpp` submodule commit
- repo commit
- workload shape (agents, prompt lengths, output lengths, tool frequency)
- warm vs cold runs
- trial count and variance
- metric boundaries (TTFT, ITL, tok/s)

## Suggested File Layout for New MoE Work (in This Repo)

If evolving in-place, add:

- `/Users/davidd/dev/uma-serve/src/moe/` for residency/prefetch logic
- `/Users/davidd/dev/uma-serve/tools/` for trace/simulation tools (already started)
- `/Users/davidd/dev/uma-serve/docs/ENGINEERING_ROADMAP_MOE.md` (this file)

Potential initial files:

- `src/moe/residency_manager.h/.cpp`
- `src/moe/prefetch_policy.h/.cpp`
- `src/moe/trace.h/.cpp`

## Notes

- Prefill and decode should not share the same prefetch assumptions.
- Layer prefetch is a useful negative baseline, but not the target design.
- Cache policy quality (pinning/eviction/precision tiers) is part of core design, not polish.
- Prioritize validation gates over feature completeness.
