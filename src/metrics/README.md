# Metrics

**Component:** `src/metrics`

This directory contains the data structures and serialization logic for the `umad` server's observability metrics.

## Purpose

The metrics component provides a lightweight, thread-safe way to instrument the server's performance. It allows key parts of the system, like the scheduler, to record statistics with minimal overhead.

## Key Components

### `metrics.h` / `metrics.cpp`

- **`Metrics` Class:** This class defines the data structure for holding all server metrics.
    - **Thread Safety:** It uses `std::atomic` for all counters and gauges (e.g., `tokens_generated_total`, `last_batch_size`). This allows different components, potentially running in different threads in the future, to update metrics without the need for mutexes, ensuring low overhead.
    - **Serialization:** The `to_json()` method provides a simple way to serialize the current snapshot of all metrics into a compact JSON string.

## Usage Pattern

1.  A single instance of the `Metrics` class is created in `umad/main.cpp`.
2.  A pointer to this object is passed to other components that need to record data (e.g., the `Scheduler`).
3.  When an administrative request for `/metrics` is received, the main loop calls the `to_json()` method and sends the resulting string back to the client.

This simple, centralized approach keeps the metrics implementation decoupled from the components that produce the data.

## Further Reading

For a detailed list of all current and planned metrics, their definitions, and how to access the metrics endpoint, see the main documentation file: [`../../docs/METRICS.md`](../../docs/METRICS.md).
