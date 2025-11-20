# UMA Serve — Configuration

The `umad` daemon can be configured via command-line arguments and environment variables. Command-line flags take precedence over environment variables.

*Future versions will support a YAML configuration file for more structured and persistent settings.*

## Current Configuration Options

### Model and Context

| Flag               | Environment Variable | Type   | Default                               | Description                                                              |
| ------------------ | -------------------- | ------ | ------------------------------------- | ------------------------------------------------------------------------ |
| `--model <path>`   | `UMA_MODEL`          | string | (none)                                | **Required.** Path to the GGUF model file.                               |
| `--n-ctx <tokens>` | `UMA_N_CTX`          | int    | `4096`                                | The context size in tokens for the model.                                |
| `--threads <n>`    | `UMA_THREADS`        | int    | `0`                                   | Number of CPU threads to use. `0` = use llama.cpp defaults.              |
| `--[no-]mlock`     | `UMA_USE_MLOCK`      | bool   | `false`                               | If set, lock the model in RAM to prevent swapping.                       |
| `--[no-]mmap`      | `UMA_USE_MMAP`       | bool   | `true`                                | If set, use memory-mapping to load the model.                            |
| `--[no-]offload-kqv`| (none)               | bool   | `true`                                | Offload the K, Q, V matrices to the GPU.                                 |

### Server and Session Management

| Flag                      | Environment Variable   | Type   | Default            | Description                                                              |
| ------------------------- | ---------------------- | ------ | ------------------ | ------------------------------------------------------------------------ |
| `--socket <path>`         | `UMA_SOCK`             | string | `/tmp/uma.sock`    | Filesystem path for the Unix Domain Socket.                              |
| `--max-sessions <n>`      | (none)                 | int    | `16`               | Maximum number of concurrent client sessions.                            |
| `--max-prompt-bytes <n>`  | (none)                 | int    | `8192`             | Maximum size in bytes for an incoming prompt (transport safety).         |
| `--max-tokens <n>`        | (none)                 | int    | `64`               | Default maximum number of tokens to generate for a request.              |

| `--idle-timeout-sec <s>`  | (none)                 | int    | `300` (5 min)      | Number of seconds after which an idle client session will be disconnected. |

### Scheduler and SLOs

| Flag                  | Environment Variable | Type | Default | Description                                                                                             |
| --------------------- | -------------------- | ---- | ------- | ------------------------------------------------------------------------------------------------------- |
| `--slo-ttft-ms <ms>`  | `UMA_SLO_TTFT_MS`    | int  | `150`   | **(Experimental)** Service-Level Objective for Time-To-First-Token in milliseconds.                     |
| `--slo-tbt-ms <ms>`   | `UMA_SLO_TBT_MS`     | int  | `80`    | **(Experimental)** Service-Level Objective for inter-token latency (Time-Between-Tokens) in milliseconds. |
| `--max-merge <n>`     | (none)               | int  | `2`     | **(Legacy)** A test-related flag to limit batch merging. May be removed in the future.                  |

## Planned Configuration Options

The following options are described in the design documents and are planned for future releases.

| Flag                     | Type   | Description                                                                                               |
| ------------------------ | ------ | --------------------------------------------------------------------------------------------------------- |
| `--config <path>`        | string | Path to a YAML configuration file.                                                                        |
| `--latency-cap <float>`  | float  | The target latency cap for interactive sessions, as a multiplier of the solo baseline (e.g., `1.2`).      |
| `--bmt-budget <gbps>`    | float  | The memory bandwidth budget in GB/s for the ΣBMT-aware scheduler.                                         |
| `--kv-type <type>`       | enum   | The quantization type for the KV cache (e.g., `f16`, `q8_0`, `q6_k`).                                       |
| `--tick-budget-ms <ms>`  | int    | The target wall-clock time budget for each scheduler tick, used by the adaptive batching algorithm.         |
| `--protocol <mode>`      | enum   | **(Removed)** Superseded by per‑connection automatic protocol detection.                                |

### Environment

- `UMA_DEBUG=1`: Enables verbose server logs (scheduler decisions, protocol detection, I/O hints) written to the test log during runs.
- `UMA_MODEL`: Path to the GGUF model if not passed via `--model`.
