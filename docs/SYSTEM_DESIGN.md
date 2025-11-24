# UMA Serve — System Design

This document provides a high-level overview of the UMA Serve architecture. It is intended to be a starting point for understanding the major components of the system. For more detailed information on specific features, please refer to the documents in the list below.

## Documentation Structure

- **[SYSTEM_DESIGN.md](./SYSTEM_DESIGN.md) (This document):** A high-level overview of the components and their interactions.
- **[PROTOCOL.md](./PROTOCOL.md):** A detailed specification of the framed JSON wire protocol.
- **[SCHEDULER.md](./SCHEDULER.md):** A deep dive into the continuous batching and scheduling policy.
- **[POLICY.md](./POLICY.md):** The scheduling policy as a standalone, pluggable component and its roadmap.
- **[MEMORY.md](./MEMORY.md):** An explanation of the memory sharing and KV cache management strategy.
- **[METRICS.md](./METRICS.md):** A guide to the observability metrics available from the server.
- **[CONFIG.md](./CONFIG.md):** A complete reference for all configuration options.

---

## High-Level Architecture

UMA Serve operates as a single `umad` process that orchestrates the `llama.cpp` backend to serve multiple concurrent clients.

The data flow is as follows:

**`Client Application`** ↔ `Unix Domain Socket` ↔ **`IPC Layer`** ↔ **`Scheduler`** ↔ **`Runtime (llama.cpp)`**

### Components

- **`runtime` (`src/runtime`):** A layer that wraps the `llama.cpp` backend, managing the model and context lifecycle via RAII and providing helper functions for tokenization.
- **`ipc` (`src/ipc`):** The Inter-Process Communication layer. It manages the UDS server, the `kqueue`-based event loop (`poller`), session state, and the protocol parsing logic.
- **`sched` (`src/sched`):** The executor that drives continuous batching. It builds a batch per tick and executes `llama_decode`.
- **Policy (planned as a separate layer):** A standalone planning component that takes a `SchedulerState` snapshot and produces a `Plan` for the scheduler to execute. This separation enables policy experimentation (e.g., SLO‑aware guard, QoS) without touching the executor.
- **`metrics` (`src/metrics`):** A lightweight, thread-safe component for collecting and exposing performance and health metrics.
- **`cli` (`src/cli`):** The implemented command-line interface for interacting with the daemon.
- **`umad` (`src/umad`):** The main entry point that initializes all components and runs the primary event loop.

---

## Concurrency Model

The server currently operates on a **single main thread** that runs an I/O event loop.

- The `poller` (using `kqueue` on macOS) waits for events on all active sockets.
- The main loop dispatches these events, calling the `SessionManager` to read incoming data or writing outgoing data to clients.
- On each iteration, the main loop calls `scheduler.tick()` to build a batch and execute a single `llama_decode` call.

A future version will evolve this into a multi-threaded model to separate I/O from computation, likely with a dedicated I/O thread and a decode/sampler thread pool to enable features like zero-copy logits and further reduce latency.

---

## Scheduling & Batching

The scheduler's goal is to maximize throughput while protecting the latency of interactive sessions. It does this via a two-phase, tick-based policy. A future version will extract the policy into a standalone component (see [POLICY.md](./POLICY.md)).

1.  **Decode Phase:** All active sessions get one token processed to ensure forward progress.
2.  **Prefill Phase:** The remaining batch capacity is used to ingest new prompts, prioritizing those that have not yet received a first token (TTFT-first).

The scheduler also uses an **adaptive batching** algorithm to dynamically tune the batch size based on the observed execution time of `llama_decode`, helping to maintain a consistent tick rate.

> For a detailed explanation of the policy, fairness rules, and future plans for QoS, see **[SCHEDULER.md](./SCHEDULER.md)**.

---

## Memory & KV Management

Efficiency on UMA systems requires careful memory management. UMA Serve's strategy is based on sharing and minimizing data copies.

- **Single Model & Unified KV Cache:** The model is loaded once, and all sessions share a single KV cache, with individual session data managed via `llama_seq_id`.
- **Planned Optimizations:** The long-term performance wedge relies on advanced memory strategies like a **KV Snapshot (Prefix) Cache** to reduce prefill costs and **ΣBMT-aware scheduling** to manage memory bandwidth as a first-class resource.

> For a deep dive into the memory model, KV cache lifecycle, and planned features, see **[MEMORY.md](./MEMORY.md)**.

---

## Protocol

Communication occurs over a Unix Domain Socket using a single, JSON‑only protocol.

- **Framed JSON:** The wire format is length‑prefixed JSON. Requests are JSON objects containing a prompt and parameters, and the server replies with a stream of JSON `event` objects (`token`, `eos`, `error`). Multiple requests per connection are allowed; send the next request after `eos`.

> For the complete wire format, JSON schemas, and event types, see **[PROTOCOL.md](./PROTOCOL.md)**.

---

## Metrics & Configuration

- **Metrics:** The server exposes a rich set of performance and health metrics via a special `/metrics` or `{"type": "metrics"}` request.
- **Configuration:** The daemon is configured via command-line flags and environment variables.

> For a full list of available metrics and configuration options, see **[METRICS.md](./METRICS.md)** and **[CONFIG.md](./CONFIG.md)**.

---

## Roadmap Snapshot (North Star)

- Policy separation: `SchedulerState` → `Plan` via `IBatchPolicy` with a transformer pipeline (LatencyGuard, Admission/QoS, PrefixCache, PagedKV, SpeculativeDecode), executed by a minimal scheduler.
- Prefix (KV snapshot) cache to skip repeated prefill (TTFT wedge).
- ΣBMT‑aware scheduling and paged KV to manage bandwidth on UMA.
- Speculative decoding (draft/verify) to reduce TTFT in single‑stream scenarios.
- Zero‑copy logits (when supported by backend) and sampler overlap.
