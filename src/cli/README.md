# uma-cli

Purpose
- Minimal CLI client for UMA Serve over UDS using the framed JSON protocol.

Usage (planned)
- `uma-cli --socket /tmp/uma.sock --prompt "Hello" --max-tokens 64 --temp 0.7 --top-p 0.95 --id req-1`
- Streams JSON events:
  - `{"id":"...","event":"token","text":"...","token_id":123}`
  - `{"id":"...","event":"eos","reason":"stop|length|error"}`
  - `{"id":"...","event":"error","message":"..."}`

Exit Codes
- 0 on success (eos without error)
- 2 on server error
- 3 on protocol error (invalid frames/JSON)

Notes
- CLI will use the same framing codec as the server.
- Newline mode is for debug only; CLI targets the JSON protocol.

