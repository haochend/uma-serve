---

# UMA Serve — System Design (v0.1)

Purpose: define the target architecture, interfaces, and migration plan to evolve UMA‑Serve from the Phase‑1 prototype into a production‑grade serving runtime with a measurable performance wedge.

Scope: local UDS serving on macOS/Linux using llama.cpp backends. Network HTTP/SSE is optional later.

---

## Goals & Success Metrics

- Correctness: stable multi‑client serving with no leaks, clean teardown, and deterministic state transitions.
- Latency: interactive SLOs (e.g., p50 TTFT ≤ 150 ms on small models; p95 inter‑token latency bounded) under mixed load.
- Throughput: maximize tokens/s by maintaining high effective batch sizes without violating latency SLOs.
- Efficiency: keep RSS flat after warm‑up; reuse model/context/KV effectively.
- Operability: metrics snapshot, debug logging, health checks, and graceful shutdown.

Non‑Goals (for now): multi‑model orchestration, remote HTTP API, distributed scheduling.

---

## High‑Level Architecture

Data flow (single process):

client ↔ UDS ↔ I/O thread (poller) ↔ scheduler queue ↔ decode thread (llama_decode) ↔ sampler/output ↔ TX buffer ↔ client

Components:
- runtime/model: model and context lifecycle (RAII); llama.cpp param wiring.
- ipc: socket server, poller abstraction, framing codec, session registry.
- sched: batch builder + policies; state machine advancement.
- sampler: per‑session sampler chain (temp/top‑p, repetition penalties) reading logits.
- metrics: counters, timers, snapshot formatting; admin endpoint.
- cli: simple client for local usage.

---

## Modules & Interfaces

Layering (mechanism vs policy):

- Engine (runtime/)
  - Thin wrapper around llama.cpp providing a stable interface independent of scheduling policy.
  - LlamaBackendGuard; ModelHandle { get(), new_context(), default_ctx_params() }.
  - IModelEngine (concept):
    - capabilities() → { paged_kv, unified_kv, persistent_graph, max_batch, device_kind }
    - tokenize(), token_to_piece()
    - decode(batch) once per tick; logits_view(i)
    - kv: clear(seq), snapshot(prefix_id), restore(prefix_id) [when supported]

- IPC (ipc/)
  - Poller (kqueue/epoll): add/del interests, wait(timeout_ms), future wake().
  - UdsServer: bind/listen/accept; socket lifecycle.
  - Protocol/codec: Phase‑1 newline; Phase‑2 framed JSON (read_frame/write_frame).
  - Session: { fd, rx, tx, seq, state, prompt_tokens, generated_count, last_activity, read_closed }.
  - SessionManager (optional class; helpers now): add/remove/lookup; idle sweeps; teardown.

- Scheduler (sched/)
  - IScheduler: tick(pool, now) → batch plan and I/O intents.
  - Current baseline: two‑phase plan (DECODE 1 token; PREFILL chunks), adaptive budget, latency guard.
  - Policy hooks (injectable later): IBatchPolicy, ILatencyGuard, IAdmissionControl.

- Sampler
  - SamplerChain: greedy/top‑p/top‑k, repetition/frequency penalties, temperature; zero‑copy logits view.

- Metrics
  - Counters/timers; to_json snapshot; exporter later.

- CLI
  - uma‑cli: connects to UDS, sends JSON request, prints streamed events.

Multi‑model & routing (later):
- ModelRegistry managing multiple Engine instances; Router picks target by model id/device/QoS; each engine has its own scheduler or one global scheduler with per‑engine capacity.

---

## Concurrency Model

Threads (Phase‑2 target):
- I/O thread: blocks in poller, handles accept/read/write, parses frames, enqueues work, and triggers wakeups.
- Decode thread: builds global batch, calls llama_decode, posts logits availability.
- Sampler/output thread (optional): reads logits buffers, samples tokens, updates sessions, fills TX.

Wakeups:
- macOS: EVFILT_USER with NOTE_TRIGGER to wake the poller when compute work arrives.
- Linux: eventfd integrated into epoll set.
- Internal: condition variable between decode and sampler threads for overlap.

Thread‑safety:
- Single writer per session (scheduler/output); I/O thread only appends to rx and drains tx behind readiness checks.
- Lockless MPSC ring for work notifications; otherwise coarse‑grained mutex around SessionPool map.

Session Manager (role):
- Own fd→ClientSession map; assign seq ids.
- Teardown: deregister poller interests, clear per‑seq KV, close fd, erase.
- Housekeeping: idle timeout sweep; reset state after STREAM to RECV_REQ.
- Optional helpers: drain_write(), parse_newline(); input guards via small utils.

---

## Scheduling & Batching

State machine per session: RECV_REQ → PREFILL → DECODE → STREAM → DONE/ERROR.

Batching policy (current baseline):
- Two‑phase per tick with a token budget:
  1) DECODE: give exactly 1 token to each decoding session (round‑robin) to ensure progress.
  2) PREFILL: drain remaining budget in large chunks per session (round‑robin). Mark logits=true only on each chunk’s last token.
- Budget = min(target_batch, llama_n_batch(ctx)); adapt target_batch using a decode‑time EWMA toward ~30 ms per tick.
- Latency guard: if any interactive stream exceeded a deadline since last emit, skip PREFILL for the tick.

Next steps:
- Distinguish interactive vs background queues; deadline‑aware dequeue.
- Admission control: caps by tenant/session; drop or postpone background work when SLO at risk.
- Preemption at token boundaries.
- Speculative decoding (draft+target) to reduce single‑stream latency.

Correctness:
- Track the exact batch index for each logits=true entry; call llama_get_logits_ith(ctx, index) post‑decode.

Extensibility hooks:
- Admission/QoS: tag sessions; budget tokens per class; preempt background when interactive SLO at risk.
- Prompt cache: consult cache before PREFILL; on hit, restore KV snapshot and skip prefill work.
- Paged KV: account for residency cost in batch policy (ΣBMT).

---

## Memory & KV Management

- Unified KV enabled; `seq_id` per session.
- Clear KV on teardown or when a response completes.
- KV Snapshot Cache (Phase‑2 wedge):
  - Key: hash(prefix_tokens); Value: KV snapshot handle
  - Restore on matching prefixes to skip PREFILL.
  - LRU with memory cap; metrics for hit ratio.

Paged KV (tiered on UMA):
- IKvManager: allocate/clear(seq), snapshot/restore(prefix), evict; residency hints (device/host/tier‑2).
- Scheduler consults residency/cost; ΣBMT bandwidth‑aware policy shapes batch to balance latency/throughput.

---

## Protocol

Phase‑1: newline framing (single‑line prompt → streamed bytes → newline terminator). Admin `/metrics` returns one JSON line and closes.

Phase‑2: framed JSON over UDS
- Frame: `uint32_le length | JSON`.
- Request:
  - id (uuid), prompt, max_tokens, temperature, top_p/top_k, stream (bool), stop strings, seed, metadata.
- Streamed events:
  - token {id, text, token_id}
  - eos {id, reason}
  - error {id, code, message}
- Cancellation: `{id, event: "cancel"}` handled promptly at token boundaries.

Backpressure: TX ring per session, arm write readiness only when pending bytes; enforce per‑session TX cap.

Transport vs protocol separation:
- Transport: UDS (now), HTTP/SSE (later) over ITransport.
- Protocol: framed JSON over IProtocol (request→events); newline preserved for debug mode.

---

## Metrics & Observability

Metrics plan (phased, low overhead):

Phase 1.5 (now → short term)
- Counters (atomics):
  - `tokens_generated_total`, `prompts_total`, `batch_calls_total`, `decode_errors_total`, `io_errors_total`, `sessions_active`.
- Per‑tick gauges (last values):
  - `last_batch_size`, `decode_ms_last`, `decode_ms_ewma`, `budget_target`, `budget_used`.
- Per‑request SLO fields (kept in session, aggregated globally):
  - `req_start_ns`, `first_emit_ns`, `last_emit_ns`, `tokens_in_response` → derive `ttft_ms` and inter‑token deltas.
- Scheduler decisions:
  - `prefill_tokens_merged_last`, `decode_tokens_merged_last`, `skipped_prefill_due_to_latency_total`.
- /metrics JSON snapshot (cheap):
  - Return counters + last gauges + EWMA + active_sessions; bounded JSON size.

Phase 2 (after refactor)
- Histograms (power‑of‑two buckets; atomic counters):
  - `ttft_ms`, `inter_token_ms`, `decode_ms`, `batch_size`.
  - Implementation: fixed bucket edges (e.g., 1,2,4,…,1024 ms) to avoid locks; snapshot sums per bucket.
- Queues/gauges:
  - `ready_sessions`, `queued_interactive`, `queued_background`, `avg_budget_target`, `avg_budget_used`.
- Cache/KV:
  - Prompt cache: `lookups_total`, `hits_total`, `hit_ratio`, `snapshots_total`, `evictions_total`, `bytes_current`.
  - Paged KV: `pages_in_total`, `pages_out_total`, `resident_dev_bytes`, `resident_host_bytes`.
- I/O:
  - `accepted_total`, `closed_total`, `rx_bytes_total`, `tx_bytes_total`.

Phase 2.5 (optional)
- Percentiles (approx):
  - T‑Digest or HDR‑like structure for `ttft_ms` and `inter_token_ms` if needed; otherwise export histograms only.
- Trace hooks (UMA_DEBUG only):
  - Per‑token timestamps and per‑tick batch composition for short windows.

Measurement points (instrumentation sites):
- Read path (main): when newline parsed → set `req_start_ns`.
- Scheduler tick: surround `llama_decode` for `decode_ms_last`; record `last_batch_size`, `budget_used`, `prefill/decode merged` counts.
- Sampling: on first piece enqueued per request → set `first_emit_ns`; on every piece → update `last_emit_ns`, increment `tokens_generated_total`.
- Write drain: on STREAM completion → finalize per‑request metrics; update histograms.

Overhead guidance:
- Atomics for counters; fixed‑size arrays for histograms; avoid heap allocs in hot path.
- Use `steady_clock` and store raw `ns` in sessions; convert to ms only on snapshot.
- Keep `/metrics` snapshot O(1) in size and time; avoid iterating all sessions.

Exposure:
- `/metrics` UDS admin request (JSON line) — present; extend with new fields.
- Future: optional Prometheus text format endpoint or file dump on signal.

Logs:
- UMA_DEBUG=1 verbose traces for scheduler decisions; otherwise concise info/warn/error lines with session id and seq.

Tracing (optional): token timestamps per session; batch composition.

SLOs & scorecards:
- Track TTFT (p50/p95), inter‑token latency, decode_ms, batch size; expose /metrics snapshot for quick troubleshooting.

---

## Configuration & Tuning

Key knobs:
- Model path, socket path/mode.
- n_ctx, threads, offload_kqv, kv_unified, swa_full.
- Limits: max_sessions, max_prompt_bytes, max_tokens, idle_timeout_sec.
- Scheduler: tick_budget_ms (target), start_target_tokens.

Tuning guidance:
- Start conservative target; let EWMA grow.
- Keep n_ctx realistic for model size; document memory footprint.

Capability detection:
- Engine advertises capabilities(); features (persistent graphs, paged KV) are enabled opportunistically without code churn.

---

## Error Handling

- Input validation: UTF‑8, size caps, NUL rejection; structured error events in JSON mode.
- Decode failures: send error response; clean KV; close session.
- I/O errors: stop monitoring fd; cleanup session.

---

## Testing & Benchmarks

Tests:
- Unit tests for framing codec and scheduler batch mapping.
- Smoke tests: M1/M2/M3 (existing Python scripts) on small and large models.
- Negative tests: oversize prompt, invalid UTF‑8, disconnect mid‑stream, cancellation.

Benchmarks (Phase‑2):
- Mixed load (interactive + background); record TTFT and inter‑token latency percentiles.
- Long‑ctx sweep across n_ctx; tokens/s vs context.
- IPC overhead: UDS framed JSON vs HTTP/SSE (later).

---

## Migration Plan (from current main.cpp)

Phase A (minimal risk):
- Introduce a Poller abstraction (kqueue/epoll); add EVFILT_USER/eventfd wakeups.
- Keep newline protocol; behavior identical.

Phase B (extraction):
- Extract Scheduler class; move batch build + llama_decode + sampling (no behavior change).
- Keep policy and metrics intact; add unit tests for logits mapping.
- Introduce tokenization helpers (runtime/tokens): wrap llama_tokenize/token_to_piece and replace direct calls in scheduler/IPC code.

Phase C (protocol):
- Add framed JSON codec and request router; retain newline fallback behind a flag.

Phase D (threads):
- Split I/O and compute threads; add sampler/output thread optionally; use ring buffers and condition variables.

Phase E (wedge features):
- Prefix/KV snapshot cache; admission/QoS; cancellation; deadline‑aware preemption; ΣBMT‑aware policy.

Each phase should pass M1/M2/M3 smoke tests; add metrics before splitting threads to observe regressions.

---

## Risks & Mitigations

- Latency regressions with larger batches → adaptive target with budget; deadline guard.
- Memory growth (KV/cache) → caps + eviction + metrics.
- I/O stalls on large TX → non‑blocking sockets + write readiness + per‑session TX caps.
- Complexity creep → module boundaries and incremental refactors; tests per module.

---

## Open Questions

- Speculative decoding: draft model vs. assisted heuristics timeline?
- Multi‑tenant isolation and quotas in UDS mode?
- Zero‑copy logits access on GPUs (Metal/Vulkan) practical path?

UMA‑specific wedges:
- ΣBMT bandwidth‑aware scheduler: integrate cost model (weights slice + KV + intermeds) into batch shaping.
- Persistent Metal graph for decode: enable when available via Engine capabilities.
- CPU/GPU coop: offload sampler and small ops; keep device busy.

---

## Roadmap (summary)

1) Poller abstraction + user wakeups; epoll backend.
2) Extract Scheduler; add minimal /metrics with histograms.
3) Framed JSON protocol + uma‑cli.
4) Split threads; sampler overlap.
5) Prefix cache + cancellation + deadline‑aware policy.
6) Speculative decoding prototype.
