# UMA Serve — Metrics & Observability

UMA Serve provides a metrics endpoint to allow for real-time monitoring of the daemon's performance and health. This document outlines how to access the metrics, the currently implemented metrics, and the planned "Scorecard" metrics for more in-depth analysis.

## Accessing Metrics

The metrics endpoint is exposed over the same Unix Domain Socket (UDS) as the inference service. To access it, a client can send a special administrative request.

- **Protocol:** The endpoint is protocol‑aware.
    - In **newline mode**, send the literal string `metrics\n` or `/metrics\n`.
    - In **JSON mode**, send a framed JSON request of the form `{"type": "metrics"}` (or `{"event":"metrics"}`).
- **Response:**
    - Newline: one line JSON and close.
    - JSON: a single framed event `{"event":"metrics","metrics":{...}}` and close.

*Note: This is a low-overhead, one-shot request intended for debugging and monitoring, not for high-frequency polling.*

## Current Metrics (M3 Stub)

The current implementation provides a minimal set of core metrics to gauge the basic activity and performance of the scheduler and decoder.

| Metric                   | Type    | Description                                                                                               |
| ------------------------ | ------- | --------------------------------------------------------------------------------------------------------- |
| `tokens_generated_total` | Counter | A monotonically increasing count of all tokens generated since the daemon started.                        |
| `batch_calls_total`      | Counter | A monotonically increasing count of `llama_decode` calls. This corresponds to the number of scheduler ticks that resulted in a batch. |
| `last_batch_size`        | Gauge   | The number of tokens in the most recently processed batch.                                                |
| `decode_ms_last`         | Gauge   | Generation-only: ms attributed to the DECODE portion of the most recent `llama_decode` call.             |
| `decode_ms_ewma`         | Gauge   | The Exponentially Weighted Moving Average (EWMA) of decode times. This value is used by the adaptive batching algorithm. |
| `decode_calls`           | Counter | Number of decode measurements that included generation (DECODE) work.                                     |
| `decode_ns_total`        | Counter | Sum of generation-attributed durations (ns).                                                              |
| `decode_tokens_total`    | Counter | Total generation (DECODE phase) tokens across all measurements.                                           |
| `decode_phase_tokens_total` | Counter | Alias of `decode_tokens_total` (generation tokens).                                                    |
| `prefill_tokens_total`   | Counter | Total PREFILL tokens included across all measured decode batches.                                         |
| `decode_ns_total_gen`    | Counter | Alias of `decode_ns_total` (generation-attributed durations).                                            |
| `prefill_ns_total`       | Counter | Estimated time spent on prefill tokens (ns), proportionally attributed from batch durations.             |
| `gen_ms_per_token_mean`  | Gauge   | Mean milliseconds per generation token (derived).                                                         |
| `prefill_ms_per_token_mean` | Gauge | Mean milliseconds per prefill token (derived).                                                           |
| `decode_ms_min`          | Gauge   | Minimum observed `llama_decode` duration (ms) since start.                                               |
| `decode_ms_max`          | Gauge   | Maximum observed `llama_decode` duration (ms) since start.                                               |
| `decode_ms_mean`         | Gauge   | Mean generation (DECODE) duration (ms), derived from totals.                                             |
| `decode_tokens_per_call_mean` | Gauge | Mean generation tokens per measured call (tokens/call).                                              |
| `active_sessions`        | Gauge   | The number of currently connected client sessions.                                                        |

### Example Output (newline)

```json
{
  "tokens_generated_total": 12345,
  "batch_calls_total": 678,
  "last_batch_size": 32,
  "decode_ms_last": 28,
  "decode_ms_ewma": 31.458,
  "decode_calls": 678,
  "decode_ns_total": 19234000000,
  "decode_tokens_total": 17500,
  "decode_ms_min": 11,
  "decode_ms_max": 44,
  "decode_ms_mean": 28.376,
  "decode_tokens_per_call_mean": 25.813,
  "active_sessions": 4
}
```

### Example Output (JSON)

```json
{
  "event": "metrics",
  "metrics": {
    "tokens_generated_total": 12345,
    "batch_calls_total": 678,
    "last_batch_size": 32,
    "decode_ms_last": 28,
    "decode_ms_ewma": 31.458,
    "decode_calls": 678,
    "decode_ns_total": 19234000000,
    "decode_tokens_total": 17500,
    "decode_ms_min": 11,
    "decode_ms_max": 44,
    "decode_ms_mean": 28.376,
    "decode_tokens_per_call_mean": 25.813,
    "active_sessions": 4
  }
}
```

## Planned Metrics (Scorecard)

The following metrics are planned for future releases to provide a comprehensive "scorecard" for performance analysis, especially for validating the effectiveness of UMA Serve's scheduling policies under mixed workloads.

### Scheduler & SLO Metrics

- **Counters:**
    - `slo_violations_ttft_total`: Number of times a session's Time-To-First-Token exceeded its SLO.
    - `slo_violations_tbt_total`: Number of times a session's inter-token latency exceeded its SLO.
    - `skipped_prefill_due_to_latency_total`: Number of times the scheduler skipped prefill work because the latency guard was active.
- **Gauges:**
    - `budget_target`: The scheduler's current target batch size.
    - `budget_used`: The actual batch size used in the last tick.
    - `queued_interactive` / `queued_background`: Number of sessions waiting in different QoS queues.

### Latency & Throughput Histograms

- `ttft_ms`: Histogram of Time-To-First-Token durations.
- `inter_token_ms`: Histogram of inter-token generation latencies.
- `decode_ms`: Histogram of `llama_decode` execution times.
- `batch_size`: Histogram of batch sizes.

### Memory & Cache Metrics

- `kv_cache_hit_ratio`: Hit ratio for the planned KV Snapshot (Prefix) Cache.
- `kv_cache_evictions_total`: Total number of items evicted from the prefix cache.
- `kv_cache_resident_bytes`: Current size of the prefix cache in memory.

### UMA-Specific Metrics

- `sum_bmt_mb_per_token`: The estimated sum of Bytes-Moved-per-Token, a measure of memory bandwidth pressure.
- `copy_free_ratio`: The ratio of logits buffers that were read via the zero-copy path vs. those that required a memcpy.
- `gpu_idle_pct`: The percentage of time the GPU was idle, indicating potential scheduling inefficiencies or CPU bottlenecks.
