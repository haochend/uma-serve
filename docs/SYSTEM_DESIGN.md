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

runtime/model
- LlamaBackendGuard
- ModelHandle { get(), new_context(), default_ctx_params() }

ipc
- Poller (kqueue/epoll): register(fd, read/write), wait(timeout), wake() [user event/eventfd]
- UdsServer: open(path, mode), accept(), close()
- Codec (Phase‑1: newline; Phase‑2: framed JSON): read_frame(), write_frame()
- Session { fd, rx, tx, seq, state, prompt_tokens, generated_count, last_activity }
- SessionManager: add/remove/find; idle sweeps

sched
- Scheduler: tick(pool) → vector<fd_to_write>
  - Inputs: SessionPool, llama_context, llama_vocab, config, metrics
  - Outputs: per‑session mutations (state, tx, pending_tok)
  - Policy hooks: budget provider, fairness, latency guard, admission

sampler
- SamplerChain: greedy/top‑p/top‑k, repetition/frequency penalties, temperature

metrics
- Metrics: counters/timers; to_json(); optional exporter later

cli
- uma‑cli: connects to UDS, sends JSON request, prints streamed events

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

---

## Memory & KV Management

- Unified KV enabled; `seq_id` per session.
- Clear KV on teardown or when a response completes.
- KV Snapshot Cache (Phase‑2 wedge):
  - Key: hash(prefix_tokens); Value: KV snapshot handle
  - Restore on matching prefixes to skip PREFILL.
  - LRU with memory cap; metrics for hit ratio.

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

---

## Metrics & Observability

Metrics (initial): tokens_generated_total, batch_calls_total, last_batch_size, decode_ms_last, decode_ms_ewma, active_sessions.

Later:
- Histograms: ttft_ms, inter_token_ms, decode_ms, batch_size.
- Gauges: kv_bytes, cache_entries, ready_sessions, queued_background.
- /metrics endpoint (JSON) without blocking compute; option to dump on signal.

Logs:
- UMA_DEBUG=1 verbose traces for scheduler decisions; otherwise concise info/warn/error lines with session id and seq.

Tracing (optional): token timestamps per session; batch composition.

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
- Extract Scheduler class; move batch build + llama_decode + sampling.
- Keep policy and metrics intact.

Phase C (protocol):
- Add framed JSON codec and request router; retain newline fallback behind a flag.

Phase D (threads):
- Split I/O and compute threads; add sampler/output thread optionally; use ring buffers and condition variables.

Phase E (wedge features):
- Prefix/KV snapshot cache; admission control; cancellation; deadline‑aware preemption.

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

---

## Roadmap (summary)

1) Poller abstraction + user wakeups; epoll backend.
2) Extract Scheduler; add minimal /metrics with histograms.
3) Framed JSON protocol + uma‑cli.
4) Split threads; sampler overlap.
5) Prefix cache + cancellation + deadline‑aware policy.
6) Speculative decoding prototype.

