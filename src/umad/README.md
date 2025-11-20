# UMA Daemon (`umad`)

**Component:** `src/umad`

This directory contains the `main.cpp` file, which is the primary entry point for the `umad` executable.

## Purpose

The `main()` function acts as the top-level orchestrator for the entire UMA Serve application. It is responsible for initializing all components, running the main event loop, dispatching I/O events, and handling graceful shutdown.

## Core Responsibilities

The `main.cpp` file can be thought of as the "conductor" that brings all the other components together. Its sequence of operations is as follows:

1.  **Initialization:**
    - Configures logging.
    - Parses command-line arguments and environment variables into a `RuntimeConfig` object.
    - Installs signal handlers for `SIGINT` and `SIGTERM` to manage graceful shutdown.
    - Initializes the `llama.cpp` backend via the `LlamaBackendGuard`.

2.  **Component Instantiation:**
    - Creates the shared `ModelHandle` to load the model into memory.
    - Creates the `UDSServer`, `Poller`, `SessionManager`, `Scheduler`, and `Metrics` objects.

3.  **Main Event Loop:**
    - Enters the primary `while` loop, which continues until a shutdown signal is received.
    - **Waiting for Events:** On each iteration, it calls `poller.wait()` to block until there is I/O activity on any of the managed sockets (the main listen socket or any client socket). It uses a dynamic timeout to avoid sleeping if there is pending compute work.
    - **Dispatching Events:**
        - If the listen socket is readable, it accepts the new client connection.
        - If a client socket is readable, it calls `session_manager.on_readable()` to read the incoming data and transition the session's state.
        - If a client socket is writable, it writes any pending data from that session's transmit buffer.
    - **Driving Inference:** It calls `scheduler.tick()` on each loop iteration. This crucial step drives the entire inference process by building and executing a batch of tokens.

4.  **Cleanup:**
    - Upon receiving a shutdown signal, the event loop terminates.
    - RAII wrappers for the model and backend ensure that all `llama.cpp` resources are freed correctly.
    - The listening socket is closed.
