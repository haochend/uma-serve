# UMA Serve — Memory Management

UMA Serve is designed for efficiency on Unified Memory Architecture (UMA) systems like Apple Silicon and AMD APUs. Its memory management strategy is central to providing a multi-tenant experience with a low resource footprint.

## Core Principles

The memory model is built on two key principles:
1.  **Share Everything:** Resources that can be shared between sessions are loaded only once and accessed by all.
2.  **Minimize Copies:** Data transfer between the GPU and CPU is minimized to reduce latency and overhead, a critical concern on UMA platforms where both units access the same physical memory.

## Key Components

### Single Model Weights

The `umad` daemon loads the specified model into memory a single time when it starts up. This single set of model weights is mapped as read-only and is shared by all concurrent user sessions.

This provides a significant memory saving compared to running separate model instances for each application, reducing the Resident Set Size (RSS) from `N × model_size` to `1 × model_size` for N concurrent applications.

### Unified KV Cache

All sessions operate within a single `llama_context`. To manage concurrent requests, UMA Serve uses `llama.cpp`'s sequencing feature. Each session is assigned a unique `llama_seq_id`. This allows the KV cache data for all active sessions to coexist in one unified buffer, managed by the `llama.cpp` backend.

- **Lifecycle:** When a new session is created, it is assigned a sequence ID. When the session's request is complete or the client disconnects, `llama_memory_seq_rm` is called to clear that sequence's data from the KV cache, freeing space for other sessions.
- **Benefits:** This approach avoids the overhead of creating and managing separate `llama_context` objects for each session, which would duplicate internal state and fragment the KV cache.

## Planned Optimizations (The "Wedge")

The following memory-related features are key to UMA Serve's performance wedge and are planned for future implementation.

### KV Snapshot Cache (Prefix Caching)

- **Goal:** Eliminate the expensive `PREFILL` (prompt processing) step for common prefixes.
- **Mechanism:** After a prompt is processed, the state of the KV cache can be saved (snapshotted) and associated with the tokenized prefix. If a new request arrives with the same prefix, the server can restore this snapshot directly instead of re-computing the prompt from scratch.
- **Impact:** This will dramatically improve Time-to-First-Token (TTFT) for applications that frequently use templates or boilerplate, such as instruction-following agents or RAG (Retrieval-Augmented Generation) systems. The cache will be managed with an LRU (Least Recently Used) eviction policy.

### ΣBMT-Aware Scheduling (Paged KV)

- **Goal:** Treat memory bandwidth as a first-class resource to be managed by the scheduler, preventing latency spikes caused by memory bus saturation on UMA systems.
- **Mechanism:** This advanced feature involves two parts:
    1.  **Paged KV:** The KV cache will be managed in pages, similar to an operating system's virtual memory. This allows the runtime to make fine-grained decisions about which parts of the cache are "hot" and need to be in the fastest memory tier.
    2.  **ΣBMT Cost Model:** The scheduler will use a cost model to estimate the **ΣBMT (sum of Bytes-Moved-per-Token)** for each request. This includes the bytes for model weights, the KV cache, and other intermediate data. The scheduler's batching decisions will be shaped by a global BMT budget, throttling background or low-priority requests that are bandwidth-intensive to protect the latency of interactive ones.

### Zero-Copy Logits

- **Goal:** Eliminate the copy of the final logits tensor from the GPU to the CPU.
- **Mechanism:** The GPU will write the logits directly into a shared, host-visible memory buffer (e.g., using `MTLStorageModeShared` on Metal). The CPU-based sampler can then read from this buffer in-place, without waiting for a data transfer to complete.
- **Impact:** This reduces system overhead and can improve inter-token latency by allowing the CPU sampler to begin its work while the GPU starts processing the next token.
