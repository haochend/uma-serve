# IPC Layer

Responsibilities
- UDSServer: bind/listen/accept on a Unix domain socket with RAII unlink.
- Poller: kqueue/epoll abstraction to add/remove interests and wait for readiness.
- SessionManager: owns fd→ClientSession map, handles RX parsing/limits, UTF‑8/CRLF/NUL guards, and transitions to PREFILL; queues bytes to `tx` and marks write interest.
- Protocol (planned): framed JSON codec helpers (`read_frame`, `write_frame`) used by SessionManager.

Design Notes
- Main loop wires: poll → accept → `SessionManager::on_readable(fd)` → scheduler.tick(...) → write drain. No llama.cpp calls in IPC.
- Newline protocol is currently used for debug; JSON framing will be introduced behind a `--protocol` flag.
- TX backpressure: per‑session TX cap; arm `EVFILT_WRITE` only when `tx` is non‑empty.

Extending with JSON Protocol
- Add `ipc/protocol.{h,cpp}` with a small codec: `read_frame(rx, out_json)` and `write_frame(tx, json)` with `max_frame_bytes` caps and partial I/O handling.
- In `SessionManager::on_readable`, branch by protocol:
  - JSON: parse a full frame → validate JSON → tokenize and transition to PREFILL → enqueue prompt echo (optional) → arm write if bytes pending.
  - Newline: existing behavior.

Testing
- Use the Python smoke tests in `scripts/tests` to validate multi‑client I/O and interleaving.
- Add focused tests for partial frames, oversize frame rejection, and invalid JSON once codec lands.

