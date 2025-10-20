---

# UMA Serve — Phase 1 (Runtime MVP) • Engineering Tracker

> **Phase 1 mission:** Ship a persistent UMA runtime daemon (`umad`) that (1) loads one model once, (2) serves multiple concurrent sessions over UDS, (3) proves continuous batching with a latency guard, and (4) lays the plumbing for zero-copy logits + metrics + bench harness.
> **Scope guard:** No kernel edits; llama.cpp backends only.

---

## Milestones & Gates

* **M1 — Single-client daemon (Week 1):** UDS server, one client end-to-end stream, model loaded once.
* **M2 — Multi-client I/O + session state (Week 2):** `kqueue`/`poll` loop, session pool, stable teardown.
* **M3 — Concurrent batching v0 (Week 3):** global `llama_batch` w/ per-session `seq_id`, interleaved streaming.
* **M4 — Protocol + CLI + metrics stub (Week 4):** framed JSON, `uma-cli`, `/metrics` JSON tick.
* **Go/No-Go Gate (end of Phase 1):** Mixed-load demo shows interleaving + stable tails (prep for Phase 2 KPIs).

---

## Repo Layout (proposed)

```
uma-serve/
├─ src/
│  ├─ umad/            # daemon entry + server
│  ├─ ipc/             # uds framing, codec, I/O loop
│  ├─ runtime/         # llama model/context wrappers
│  ├─ sched/           # scheduler + batch builder
│  ├─ metrics/         # counters, timers, /metrics
│  ├─ sampler/         # CPU sampler (BNNS/xnnpack) interface
│  └─ cli/             # uma-cli
├─ benches/            # python harnesses + CSV
├─ include/            # public headers (if any)
├─ cmake/
├─ CMakeLists.txt
└─ docs/
   ├─ PROTOCOL.md
   ├─ METRICS.md
   ├─ BENCHMARKS.md
   └─ SCORECARD.md
```

---

## Work Breakdown (checklists)

### Week 1 — Foundations (M1)

**Daemon + model lifecycle**

* [ ] `RuntimeConfig` loader (YAML/env/flags; minimal in W1).
* [ ] `ModelHandle` (RAII)

  * [ ] `llama_model* load(const std::string& path)` once in parent.
  * [ ] `llama_context_params` baseline (Metal/Vulkan auto).
  * [ ] Persistent allocator pools enabled (llama.cpp flags).
* [ ] Graceful signal handling (SIGINT/SIGTERM) → drain & shutdown.

**UDS server (macOS first)**

* [ ] Path `/tmp/uma.sock` (configurable).
* [ ] `socket(AF_UNIX)`, `bind`, `listen`, `accept`.
* [ ] `umask` + `chmod` secure (0600 default).
* [ ] Remove stale socket on boot.

**Wire a single request**

* [ ] Blocking loop: read prompt (newline-terminated for W1).
* [ ] Minimal inference: `tokenize → batch(-1) → decode()/sample → stream`.
* [ ] Stream tokens as raw bytes (W1 only), flush on every piece.

**Build & run**

* [ ] CMake target `umad`; RelWithDebInfo; LTO off initially.
* [ ] `scripts/run_dev.sh` (sets `LLAMA_*` env if needed).

**Acceptance (M1)**

* [ ] `nc -U /tmp/uma.sock` → send a prompt → streamed reply.
* [ ] Model loads exactly once; RSS stable across 3 prompts.

---

### Week 2 — Multi-client I/O & Sessions (M2)

**I/O model**

* [ ] Switch to **`kqueue`** (macOS) with fallbacks: `poll` (Linux).
* [ ] Register: listen FD (read), per-client FDs (read/write, HUP/ERR).
* [ ] Non-blocking sockets (`O_NONBLOCK`), backpressure via per-session TX ring.

**Session state**

* [ ] `struct ClientSession {`

  * [ ] `int fd;`
  * [ ] `std::vector<uint8_t> rx, tx;`
  * [ ] `llama_context* ctx;`
  * [ ] `llama_seq_id seq;`
  * [ ] `StateMachine state; // RECV_REQ → PREFILL → DECODE → STREAM → DONE`
  * [ ] `std::vector<llama_token> prompt, generated;`
  * [ ] `uint64_t last_activity_ns;`
  * [ ] `bool wants_stream;`
  * [ ] `Error last_error;`
  * [ ] `};`
* [ ] `SessionPool` (`std::unordered_map<int, ClientSession>`).
* [ ] Cleanup: on HUP/ERR remove from poller, free `ctx`, close fd.

**Safety & limits**

* [ ] Configurable caps: max sessions, max prompt bytes, max tokens.
* [ ] Idle timeout (e.g., 5 min).
* [ ] Input parser guards (UTF-8, reasonable JSON size later).

**Acceptance (M2)**

* [ ] 4 terminals connect simultaneously; sequential compute is okay.
* [ ] No crashes/leaks (`leaks`, `Activity Monitor` RSS flat except contexts).

---

### Week 3 — Concurrent Batching v0 (M3)

**Scheduler loop (single thread to start)**

* [ ] `ReadyQueue` = sessions in `PREFILL|DECODE` with tokens pending.
* [ ] Build **global `llama_batch`** per tick:

  * [ ] For each ready session, append next token(s).
  * [ ] Annotate with `llama_seq_id = session.seq`.
  * [ ] Keep **max_merge** (config; start 2–4).
* [ ] `llama_decode(model, batch)` **once** per tick.
* [ ] Per session: pluck logits view, sample next token (greedy at first).
* [ ] Push streamed piece into `tx`; advance state.

**Latency guard (lightweight)**

* [ ] Record per-session per-token latency EWMA.
* [ ] If predicted p95 would exceed **`latency_cap`** (default 1.20× of solo), **skip merging** that adds more than `max_merge` or temporarily deprioritize background sessions.
* [ ] Simple heuristic: cap batch size if `now - session.last_emit > cap_ms`.

**Metrics (stub)**

* [ ] Counters: tokens_generated_total, batch_calls_total, avg_batch_size.
* [ ] Timers: decode_ms, sample_ms, per-token_latency_ms.
* [ ] Expose `/metrics` JSON (single snapshot) on a debug UDS path or `SIGUSR1` dump to file (W3 minimal).

**Acceptance (M3)**

* [ ] Run long job + short Q/A → visible interleaving at terminal(s).
* [ ] Average batch size ≥ 1.5 while short job maintains snappy tokens.

---

### Week 4 — Protocol, CLI, Metrics Tick (M4)

**Framed JSON protocol (UDS)**

* [ ] **Message framing:** `uint32_le length | json payload`.
* [ ] Request schema:

  ```json
  {
    "id": "uuid",
    "prompt": "string",
    "max_tokens": 512,
    "temperature": 0.0,
    "top_p": 1.0,
    "stream": true
  }
  ```
* [ ] Stream response events (each framed):

  * `{"id":"...", "event":"token", "text":"...","token_id":123}`
  * `{"id":"...", "event":"eos","reason":"stop|length|error"}`
  * `{"id":"...", "event":"error","message":"..."}`
* [ ] Library: `nlohmann/json` (header-only).

**CLI**

* [ ] `uma-cli`: connects to `/tmp/uma.sock`, builds JSON request, prints streamed pieces to stdout.
* [ ] Flags: `--prompt`, `--max-tokens`, `--temp`, `--top-p`, `--no-stream`.
* [ ] Exit codes: 0 success; 2 server error; 3 protocol error.

**Metrics endpoint v0**

* [ ] `/metrics` framed JSON dump on request:

  ```json
  {
    "model":"qwen-coder-30b-q8.gguf",
    "sessions":4,
    "tokens_total": 12345,
    "avg_batch": 1.8,
    "p50_ms": 45.2,
    "p95_ms": 60.1,
    "decode_ms_avg": 12.3
  }
  ```

**Acceptance (M4)**

* [ ] `umad` + `uma-cli` happy path; JSON framing robust to partial reads.
* [ ] `/metrics` returns without blocking the scheduler.

---

## Design Details (ready-to-implement)

### 1) Concurrency & Threads (Phase 1)

* **Main thread:** event loop (`kqueue`/`poll`) + scheduler tick + `llama_decode`.
* **Optional sampler thread:** can be added in Phase 2 to overlap CPU sampling; P1 uses main thread for simplicity.
* **No locks** on hot path in P1 (single producer/consumer semantics inside the scheduler tick).

### 2) State Machine (per session)

```
RECV_REQ
  └─parse ok→ PREFILL
PREFILL
  └─decode batch (prompt)→ DECODE
DECODE
  ├─sample→ STREAM
STREAM
  ├─write tx→ (ok) DECODE
  └─eos/limit→ DONE
DONE → teardown
ERROR → teardown (send error event if possible)
```

### 3) Memory & Residency

* **One weights map**: `llama_model*` owned by process.
* **One context per session**: `llama_new_context_with_model(model, params)`; consider a **context pool** later if churn observed.
* **Persistent buffers**: set `n_batch`, `n_ctx` once where possible; avoid per-token allocs.
* **KV layout**: default llama.cpp; Phase 2 adds head-major selectable.

### 4) Scheduler v0 (merge policy)

* **Token-merge window:** up to `max_merge` tokens when multiple sessions are ready, otherwise 1.
* **Fairness:** round-robin over ready sessions; deprioritize sessions with age < `min_inter_token_ms`.
* **Guard:** if `(now - last_emit_ms) > cap_ms` for an interactive session, include it solo next tick.

### 5) Error Handling & Codes

* `E_PROTO_001` invalid JSON
* `E_LIMIT_001` prompt too large
* `E_MODEL_001` model not loaded
* `E_RUNTIME_001` llama_decode failure
* `E_IO_001` short write after N retries
  Return `event:"error"` with `code`, close session.

### 6) Logging

* **Binary perf log (Phase 2);** in Phase 1:

  * [ ] Text log (INFO/WARN/ERR) with timestamps, session id, seq.
  * [ ] Per-tick counters: batch size, tokens emitted, decode_ms.

### 7) Build Flags (CMake)

* `-O3 -DNDEBUG -fno-exceptions -fno-rtti` (where viable).
* `-march=armv8.5-a+simd+crypto` on mac where safe.
* LTO off initially; turn on after correctness.
* `LLAMA_METAL=1` on macOS; `LLAMA_VULKAN=1` for AMD path.

### 8) Config (YAML; W4 minimal)

```yaml
model: "/models/qwen-coder-30b-q8.gguf"
ipc:
  uds_path: "/tmp/uma.sock"
  backlog: 64
limits:
  max_sessions: 32
  max_prompt_bytes: 65536
  max_tokens: 2048
scheduler:
  max_merge: 2
  latency_cap: 1.20
metrics:
  tick_interval_ms: 1000
```

---

## Bench Harness (skeleton to land in Phase 2, but prep now)

* **Mixed-load SLO (`benches/mixed_load.py`)**

  * 3 interactive prompts (short) + 1 long doc summarization.
  * Capture token timestamps per stream; compute p50/p95 inter-token latency.
  * CSV fields: `session_id, token_idx, ts_ms, is_interactive`.

* **Long-ctx KV sweep (`benches/long_ctx.py`)**

  * 16k/32k ctx; record tokens/s vs `n_ctx`; CSV `ctx,tps`.

* **IPC overhead (`benches/ipc_overhead.py`)**

  * Compare UDS framed JSON vs HTTP/JSON SSE (later); CSV `path,ttft_ms,cpu_pct`.

---

## Test Matrix

* **macOS 15.x**, **Xcode 16** toolchain, Apple Silicon M-series (M3/M4).
* **Linux (Ubuntu 22.04)** + AMD APU (when available) with Vulkan enabled (compile-only sanity in P1 if HW N/A).
* **Model sizes:** 7B/13B smoke; 30B for stress (q8_0).
* **Prompts:** ASCII, UTF-8 w/ emojis, 32k ctx boundary.

---

## Engineering Tasks by File/Module

**`src/runtime/llm_runtime.{h,cc}`**

* [ ] `class LlmRuntime { load_model(path); new_context(); tokenize(); decode(batch); token_to_piece(); }`
* [ ] Backend auto-detect (Metal first).

**`src/ipc/uds_server.{h,cc}`**

* [ ] `bind_listen(path)`, `accept_nonblock()`, `set_nonblock(fd)`, `close_fd(fd)`.
* [ ] Framing codec: `read_frame()`, `write_frame()` w/ partial IO handling.

**`src/umad/main.cc`**

* [ ] Argparse (model path, uds path).
* [ ] Boot: load model, start server, event loop.

**`src/sched/scheduler.{h,cc}`**

* [ ] `tick(SessionPool&)` → `llama_batch` → `decode()` → per-session sample.

**`src/metrics/metrics.{h,cc}`**

* [ ] Atomic counters/timers; `snapshot()` → JSON.

**`src/cli/main.cc`**

* [ ] Build JSON request; connect; stream events to stdout.

---

## Definition of Done (Phase 1)

* [ ] `umad` runs; loads model once; survives connect/disconnect churn.
* [ ] 4 concurrent sessions stream tokens; no memory growth > contexts.
* [ ] Interleaved tokens across sessions with global `llama_decode`.
* [ ] Framed JSON protocol stable to partial reads/writes.
* [ ] `uma-cli` usable; `/metrics` returns JSON without blocking.
* [ ] Minimal docs: `README`, `PROTOCOL`, quickstart.

---

## Risks & Mitigations

* **Event loop stalls due to big writes** → non-blocking + TX ring + `EVFILT_WRITE` readiness.
* **Context memory blow-up** → document recommended `n_ctx`, warn + reject over-large prompts.
* **Latency regressions** → keep `max_merge` small; implement simple guard; measure per-token latencies early.
* **Sampler CPU spikes** → Phase 2: move to separate thread and read logits in-place (zero-copy path).

---

## Stretch (Phase 2 Preview Hooks You Can Leave TODOs For)

* [ ] **ΣBMT estimator** (weights slice + KV + intermeds) per token.
* [ ] **Token-boundary preemption** (insert interactive tokens).
* [ ] **Zero-copy logits buffer**: `StorageModeShared` (Metal) view + sampler thread.
* [ ] **/metrics scorecard** with p95/p99, copy-free ratio, GPU-idle%.

---

## Quick Scaffolding Snippets

**Framing (read)**

```cpp
bool read_frame(int fd, std::vector<uint8_t>& buf, std::string& out_json) {
  while (true) {
    uint8_t tmp[4096];
    ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (n <= 0) return false; // peer closed or error
    buf.insert(buf.end(), tmp, tmp + n);
  }
  if (buf.size() < 4) return true; // need more
  uint32_t len = 0; memcpy(&len, buf.data(), 4);
  if (buf.size() < 4 + len) return true; // need more
  out_json.assign(reinterpret_cast<char*>(buf.data()+4), len);
  buf.erase(buf.begin(), buf.begin() + 4 + len);
  return true;
}
```

**Batch build sketch**

```cpp
llama_batch batch = llama_batch_init(/*n_tokens*/ 0, /*embd*/ 0, /*n_seq_max*/ 0);
for (auto sid : ready_sessions) {
  auto& s = pool[sid];
  llama_token tok = next_token_for(s);
  llama_batch_add(batch, tok, /*pos*/ s.generated.size(), /*seq_id*/ s.seq, /*logits*/ true);
}
// one decode for all
llama_decode(ctx0, batch);
```

---
