---

# UMA Serve — Framed JSON Protocol (UDS)

Purpose: replace the temporary newline protocol with a robust, length‑prefixed JSON protocol over Unix domain sockets. Enables structured requests, streamed events, and future features (cancellation, SLOs, sampling params) without changing transport.

Scope: single‑process UDS transport. HTTP/SSE may reuse the same JSON event schema later.

---

## Motivation

- Robust framing: length‑prefix prevents ambiguities, supports NULs, and handles partial I/O cleanly.
- Structured requests: carry generation params, SLO hints, and metadata safely.
- Streamed events: typed token/eos/error events simplify clients and tests.
- Compatibility: keep newline mode behind a flag for quick debugging.

---

## Transport & Framing

- Socket: Unix domain socket (stream) at `--socket` path.
- Frame format: `uint32_le length | JSON bytes`.
  - `length` is the number of bytes in the JSON payload (no terminator).
  - Cap: `max_frame_bytes` (config; default 1 MiB). Frames over cap → protocol error.
  - Partial I/O: server buffers into `rx`; once `rx.size() >= 4 + length`, a full frame is parsed.

---

## Request Schema (v1)

One request at a time per session. A new request is allowed only after the server signals eos or error.

Required
- `id` (string): client request id.
- `prompt` (string): UTF‑8 text to generate from.

Optional
- `max_tokens` (int, default server limit)
- `temperature` (float, default=0.0)
- `top_p` (float), `top_k` (int) — reserved; may be ignored for now
- `stream` (bool, default=true): if false, server may buffer and send a single `eos` event at end
- `slo` (object): `{ "target_ttft_ms": 150, "target_tbt_ms": 80 }` (advisory; used by future policy)
- `metadata` (object): user data echoed in events (later)

Admin
- `type: "metrics"` requests a one‑shot metrics snapshot frame and server closes the session.

Cancellation
- `{ "event": "cancel", "id": "..." }` signals cancellation for an in‑flight request (best‑effort at token boundaries).

---

## Streamed Events

All responses are frames carrying a JSON object with an `event` field.

- Token:
  - `{ "id": "...", "event": "token", "text": "...", "token_id": 123 }`
- End of stream:
  - `{ "id": "...", "event": "eos", "reason": "stop|length|error" }`
- Error:
  - `{ "id": "...", "event": "error", "code": "E_...", "message": "..." }`

Admin metrics (one frame):
- `{ "event": "metrics", ... snapshot fields ... }`

Notes
- The server closes the socket after sending `eos` or `error` unless the client explicitly keeps the session open (future multi‑request sessions). For Phase 1, one request per connection.

---

## State Machine (per session)

`RECV_REQ → PREFILL → DECODE → STREAM → DONE|ERRORED`

- JSON mode: `RECV_REQ` parses request frames (may buffer multiple) and rejects a second request while busy.
- Newline mode: unchanged; kept behind `--protocol newline` for debug.

---

## Error Handling

- Frame too large: close with `error { code: "E_PROTO_FRAME_TOO_LARGE" }`.
- Invalid JSON: `E_PROTO_INVALID_JSON`.
- Missing/invalid fields: `E_PROTO_BAD_REQUEST`.
- Prompt too large (after UTF‑8 validation): `E_LIMIT_PROMPT_TOO_LARGE`.
- Decode failure: `E_RUNTIME_DECODE`.

On error: enqueue error event, flush, then close.

---

## Backpressure & Limits

- RX: cap `max_frame_bytes`; reject oversize before allocating payload.
- TX: per‑session TX cap (existing), write‑interest is armed only when bytes are pending.
- Scheduler remains single decode per tick; framing does not change compute behavior.

---

## Compatibility & Versioning

- Mode flag: `--protocol json|newline` (default `json`).
- JSON v1 schema is stable for Phase 1; future fields are optional and ignored by server until implemented.
- CLI uses the same frames over UDS.

---

## Implementation Plan

1) Add protocol flag and caps in `RuntimeConfig`.
2) Implement codec `ipc/protocol.{h,cpp}` with `read_frame()` and `write_frame()` and caps.
3) Extend `SessionManager::on_readable()` to branch by protocol; parse request frames, validate, tokenize, transition to PREFILL.
4) Stream events as frames in JSON mode; keep newline mode as is.
5) Add `uma-cli` to exercise happy path and partial I/O scenarios.
6) Tests: partial frames, oversize frame rejection, invalid JSON, busy request, and admin metrics.

---

## Open Questions

- Multiple requests per connection vs one‑shot sessions? (Phase 1: one request)
- Cancellation timing and delivery guarantees? (best‑effort, token boundaries)
- Echoing `metadata` in events? (later)

---

