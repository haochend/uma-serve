# UMA Serve

UMA Serve is a lightweight runtime daemon (umad) built on top of llama.cpp that:
- Loads one model once and keeps it hot in memory.
- Serves multiple concurrent clients over a Unix domain socket (UDS).
- Streams tokens as raw bytes with a simple, newline‑delimited protocol.
- Uses a capacity‑aware, fairness‑oriented scheduler to batch work across sessions.

Status: Phase 1 foundations are in place (M1/M2 done, M3 scheduler v1). The wire protocol is intentionally minimal for now; metrics and a framed JSON protocol land next. The runtime tracks simple per‑session SLO timing (TTFT/TBT) for observability and future policy work.

**Quick Start**
- Build: `./build.sh`
- Run tests (smoke):
  - `UMA_MODEL=/path/to/model.gguf python3 scripts/tests/run_m1.py`
  - `UMA_MODEL=/path/to/model.gguf python3 scripts/tests/run_m3.py`
  
- Manual run:
  - Start: `build/umad --model /path/to/model.gguf`
  - Connect: `nc -U /tmp/uma.sock`, type a prompt, then press Enter

**Requirements**
- macOS on Apple Silicon (tested on macOS 15 / M3/M4). Metal backend is enabled by default.
- CMake ≥ 3.13, Clang (via Xcode tools), Python 3 for helper scripts.
- The llama.cpp submodule is included under `external/llama.cpp` (fetched by CMake).

**Build**
- `./build.sh` configures CMake and builds `build/umad` (Debug by default).
- Environment variables `LLAMA_*` can control llama.cpp logging; the tests set sane defaults.

**Run**
- Minimal: `build/umad --model /path/to/model.gguf`
- Defaults:
  - Socket path: `/tmp/uma.sock`
  - Socket mode: `0600`
  - Context tokens: `--n-ctx 4096` (change via flag or `UMA_N_CTX`)
  - SLO defaults: TTFT target 150 ms, inter‑token target 80 ms (configurable)

**Protocol (temporary, W1/W2)**
- Each connection is a session. Send a single line terminated by `\n`.
- Server streams token pieces (UTF‑8 bytes) as they are generated and ends the stream with `\n`.
- Errors are sent as one line, e.g.:
  - `error: prompt too large\n`
  - `error: invalid utf-8\n`
  - `error: decode failed\n`
- Session reuse: you may send another line on the same connection after a reply completes.

**Admin/Debug**
- Set `UMA_DEBUG=1` for verbose daemon traces (batch composition, samples, I/O). The test harness captures these in `build/umad_test.log`.
- Basic metrics (temporary) are available by sending `/metrics\n` on the UDS; the server replies with a single JSON line with fields like `tokens_generated_total`, `last_batch_size`, and `decode_ms_ewma`.
  

**Key Flags and Env Vars**
- Required:
  - `--model /path/to/model.gguf` (or `UMA_MODEL`)
- Useful:
  - `--n-ctx 4096` (or `UMA_N_CTX`)
  - `--threads N` (or `UMA_THREADS`)
  - `--socket /tmp/uma.sock` (or `UMA_SOCK`)
  - `--mlock` / `--no-mlock`, `--mmap` / `--no-mmap`
  - `--max-sessions`, `--max-prompt-bytes`, `--max-tokens`, `--idle-timeout-sec`
  - `--slo-ttft-ms 150`, `--slo-tbt-ms 80` (or env: `UMA_SLO_TTFT_MS`, `UMA_SLO_TBT_MS`)
  - `UMA_DEBUG=1` for verbose logs

Note: some flags are experimental and may be ignored in early phases as the scheduler evolves.

**Design at a Glance**
- Single process, single llama model.
- One persistent `llama_context` with multi‑sequence enabled (unified KV).
- Kqueue‑based I/O loop (macOS first) with non‑blocking sockets; write‑interest is armed only when TX is pending.
- Scheduler tick per loop iteration:
  - 1 token per DECODE session (keeps streams moving), then drain PREFILL in large chunks up to a device‑capacity budget.
  - Single `llama_decode` per tick with correct logits‑row mapping.
  - Batch target adapts toward a ~30 ms decode budget. SLO‑aware policy will be added in a future refactor.

Refactor highlights (Phase B extraction):
- I/O loop uses a `SessionManager` to own sessions and RX parsing (limits, UTF‑8/CR trim/NUL) and to transition to PREFILL. Main no longer performs parsing/tokenization.
- Tokenization utilities are in `runtime/tokens.{h,cpp}` for consistent usage by I/O and scheduler.

**Known Limitations (Phase 1)**
- Newline protocol only; framed JSON and a CLI are upcoming.
- Metrics are minimal; no persistent counters/histograms yet.
- Single main thread for I/O + decode; no sampler thread overlap.
- macOS path (kqueue) implemented; Linux epoll path to follow.

**Contributing / Hacking**
- Read `first-phase-design.md` for the plan and acceptance tests.
- The Python smoke tests in `scripts/tests` are a good way to validate changes.
- Keep PRs focused; prefer small, incremental refactors (e.g., extract scheduler/poller modules) with behavior unchanged.

**Troubleshooting**
- “client got no output”: enable `UMA_DEBUG=1` and check `build/umad_test.log` for `[prompt]` and `[write]` lines; large models can have long TTFT without early bytes.
- `get_logits_ith: invalid logits id`: ensure logits mapping follows batch indices where `logits==1`.

**Roadmap (short)**
- Framed JSON protocol over UDS with robust partial I/O and backpressure.
- `/metrics` snapshot with decode/batch histograms and per‑session latency.
- EVFILT_USER/eventfd wakeups and epoll backend; optional compute thread.
- Prefix/KV cache; speculative decoding path for lower TTFT.

**Testing Notes**
- Run `scripts/tests/run_m1.py` and `scripts/tests/run_m3.py` with `UMA_MODEL` set for smoke coverage.
JSON protocol (M4): see `docs/PROTOCOL.md` for design. A `--protocol json|newline` flag will select the mode (default `json`).
