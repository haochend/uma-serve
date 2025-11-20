# Scheduler

**Component:** `src/sched`

This directory contains the `Scheduler` class, the core component that orchestrates continuous batching in UMA Serve.

## Purpose

The scheduler's primary responsibility is to intelligently select which tokens from which user sessions to include in the next global batch for processing by the `llama.cpp` backend. Its goal is to maximize throughput by creating large batches while ensuring fairness and low latency for interactive sessions.

## Key Components

### `scheduler.h` / `scheduler.cpp`

This module implements the `Scheduler` class.

- **`Scheduler::tick()`:** This is the main entry point and is called on every iteration of the main server event loop. The method performs the following actions:
    1.  **Collects Ready Work:** It identifies all sessions that are ready for processing (i.e., those in the `PREFILL` or `DECODE` state).
    2.  **Builds a Batch:** It implements the two-phase scheduling policy (Decode-first, then Prefill) to build a `llama_batch` object.
    3.  **Executes the Batch:** It calls `llama_decode()` with the prepared batch.
    4.  **Samples Results:** For each session that had a token sampled, it performs greedy sampling to get the next token.
    5.  **Updates Session State:** It transitions sessions from `PREFILL` to `DECODE`, appends newly generated tokens to the appropriate session's transmit buffer (`tx`), and marks sessions for completion (`EOS`) if necessary.
    6.  **Updates Metrics:** It records key performance metrics, such as decode time and batch size.
    7.  **Returns Work:** It returns a list of session file descriptors that have received new data and need to have their sockets armed for writing by the `poller`.

- **Stateful Logic:** The scheduler is stateful. It maintains internal state across ticks, including:
    - Round-robin cursors to ensure fair processing of sessions in both the Decode and Prefill phases.
    - An Exponentially Weighted Moving Average (EWMA) of `llama_decode` timings, which is used to implement the adaptive batching logic.

## Further Reading

For a detailed, high-level explanation of the scheduling policy (two-phase tick, adaptive batching, TTFT-first prefill) and its goals, see the main documentation file: [`../../docs/SCHEDULER.md`](../../docs/SCHEDULER.md).
