# UMA Serve — Scheduler Design

The UMA Serve scheduler is the core component responsible for orchestrating concurrent requests to achieve high throughput via continuous batching, while simultaneously ensuring low latency for interactive applications. It operates on a single-threaded, tick-based model within the main event loop.

## Core Concepts

- **Continuous Batching:** The scheduler combines tokens from multiple concurrent sessions (both those being prefilled and those actively decoding) into a single, global `llama_batch` per `llama_decode` call. This maximizes GPU utilization.
- **Session State Machine:** The scheduler acts on sessions based on their state. The primary states it manages are:
    - `PREFILL`: The session's initial prompt is being processed by the model.
    - `DECODE`: The session has finished prefill and is now generating one token at a time.
- **Tick-Based Execution:** On each iteration of the main event loop, the `Scheduler::tick()` method is called. This method builds and executes a single batch.

## The `tick()` Policy

The `tick()` method implements a two-phase policy designed to prioritize responsiveness for active streams over ingesting new work.

### Phase A: Round-Robin Decode

- **Goal:** Ensure all active conversations or generations make progress on every tick.
- **Mechanism:** The scheduler iterates through all sessions currently in the `DECODE` state. For each one, it adds **exactly one token** to the batch. This round-robin approach prevents any single long generation from "hogging" the decode capacity and starving other interactive sessions.

### Phase B: Budgeted Prefill

- **Goal:** Process new prompts efficiently without introducing high latency for the already-active `DECODE` sessions.
- **Mechanism:** After servicing the `DECODE` sessions, the scheduler uses the remaining capacity in its token budget to process sessions in the `PREFILL` state.
- **Fairness & TTFT:** To ensure a fast Time-To-First-Token (TTFT), the prefill phase prioritizes sessions that have not yet produced any output. It also uses a burst limit (`kBurst`) to prevent a single very long new prompt from consuming the entire prefill budget in one tick.

### Example Tick

Imagine a batch budget of 32 tokens and the following sessions:
- **Session A:** `DECODE` state (interactive chat reply)
- **Session B:** `DECODE` state (interactive code completion)
- **Session C:** `PREFILL` state (new request with a 50-token prompt)
- **Session D:** `PREFILL` state (new request with a 100-token prompt)

A `tick()` would compose a batch like this:
1.  **Phase A (Decode):**
    - Add 1 token from Session A.
    - Add 1 token from Session B.
    - *Budget remaining: 30 tokens.*
2.  **Phase B (Prefill):**
    - Add a chunk of up to 16 tokens from Session C (due to the new-session burst limit).
    - Add a chunk of up to 14 tokens from Session D (using the rest of the budget).
    - *Budget remaining: 0 tokens.*
3.  The final 32-token batch is sent to `llama_decode`.

## Adaptive Batching

To maintain a consistent processing interval and avoid overly long `llama_decode` calls that would stall the event loop, the scheduler dynamically tunes its token budget.

- It maintains an Exponentially Weighted Moving Average (EWMA) of recent `llama_decode` execution times.
- If the average decode time exceeds a target budget (e.g., ~30ms), the scheduler reduces the `target_batch_` size for subsequent ticks.
- If the average decode time is well below the budget, it gradually increases the `target_batch_` size to improve throughput.

This adaptive mechanism helps the server stay responsive under varying load and hardware capabilities.

## Future Work & Extensibility

The current scheduler provides a strong baseline. The following features are planned for future iterations, as outlined in the system design documents:

- **SLO-Aware Latency Guard:** A more explicit policy that will actively throttle or pause `PREFILL` work if the inter-token latency of any interactive `DECODE` session violates its configured Service-Level Objective (SLO).
- **QoS and Priority Queues:** Using the `priority` field from the JSON protocol's `slo` object to sort sessions into different queues (e.g., `interactive`, `normal`, `background`). This will allow the scheduler to make more informed decisions about which sessions to prioritize during both `DECODE` and `PREFILL` phases.
- **Token-based Preemption:** A mechanism to interrupt the processing of a low-priority prefill batch to immediately service a high-priority request, further minimizing TTFT for interactive sessions.
- **ΣBMT Budgeting:** Integrating a cost model based on Bytes-Moved-per-Token to make scheduling decisions that are aware of UMA memory bandwidth saturation, not just compute capacity.
