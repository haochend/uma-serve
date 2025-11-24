# Testing UMA Serve

This guide covers how to run and understand the project’s tests.

Test types
- C++ unit tests (GoogleTest): fast, hermetic logic tests (e.g., protocol framing, policy planner).
- Python end-to-end (pytest): spins up `umad`, connects over UDS, verifies protocol and streaming behavior.
- Perf smoke (pytest): quick metrics sanity to guard regressions during scheduler/policy changes.

## Prerequisites

- Build the project once: `./build.sh` (Debug by default; `./build.sh release` for Release).
- Python 3 with pytest installed:
  - `python3 -m venv .venv && source .venv/bin/activate && pip install -r tests/requirements.txt`
- A local GGUF model file and `UMA_MODEL` set to its path for E2E tests:
  - `export UMA_MODEL=/path/to/model.gguf`

## C++ Unit Tests (gtest)

Run all gtests:
```
ctest --test-dir build
```

Useful filters:
```
# Only protocol tests
ctest --test-dir build -R ProtocolTest

# Only policy tests
ctest --test-dir build -R PolicyTest
```

What’s covered:
- `ProtocolTest.*`: framed JSON codec edge cases (oversize, incomplete, roundtrip).
- `PolicyTest.*`: baseline planner behavior (decode‑first, TTFT‑first prefill, budget, round‑robin).

## Python E2E Tests (pytest)

Run the full E2E suite (requires `UMA_MODEL`):
```
pytest
```

Common selections:
```
# Only v1 JSON protocol tests
pytest -k m4_json_protocol

# Only interleaving/concurrency tests
pytest -k interleaving
```

Logs: daemon output is saved under `build/` (e.g., `build/umad_e2e_tests.log`). See `tests/e2e/conftest.py` for exact paths.

## Perf Smoke (metrics)

Lightweight guard to ensure metrics are wired and monotonic when we change scheduling/policy:
```
pytest -k perf_smoke -q
```

It runs a short generation, fetches `/metrics` (framed JSON), and checks fields like `batch_calls_total`, `tokens_generated_total`, and `decode_ms_ewma` for presence and monotonicity.

## Tips

- Use a small GGUF for faster E2E runs.
- Reduce daemon log noise by exporting:
  - `LLAMA_LOG_VERBOSITY=2`, `LLAMA_LOG_PREFIX=1`, `LLAMA_LOG_TIMESTAMPS=0` (defaults used in tests).
- If `build/umad` is missing, E2E fixtures will automatically invoke `./build.sh`.

