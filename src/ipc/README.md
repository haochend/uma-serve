# IPC (Inter-Process Communication)

**Component:** `src/ipc`

This directory contains all components related to server-client communication in `umad`. It handles setting up the server, listening for connections, managing client sessions, and processing the wire protocol.

The core of the IPC layer is a non-blocking, single-threaded event loop.

## Key Components

### `uds_server`

- **Purpose:** Manages the lifecycle of the Unix Domain Socket (UDS).
- **Functionality:**
    - Creates, binds, and listens on the socket at the path specified by the `--socket` configuration.
    - Sets appropriate file permissions on the socket.
    - Accepts new incoming client connections.

### `poller` / `poller_kqueue`

- **Purpose:** Provides the high-performance event loop that drives all I/O.
- **Functionality:**
    - Abstracts the underlying OS-specific polling mechanism. Currently, this is `kqueue` on macOS (`poller_kqueue.cpp`), chosen for its efficiency in handling a large number of file descriptors.
    - The main server loop calls `poller.wait()` to block until a socket has a readable or writable event.
    - A future Linux implementation will use `epoll`.

### `session_manager`

- **Purpose:** Manages the state and lifecycle of every connected client. This is the heart of the IPC logic.
- **Functionality:**
    - **Session Tracking:** Maintains a map of file descriptors to `ClientSession` objects.
    - **State Machine:** A `ClientSession` object holds the state of a single client (e.g., `RECV_REQ`, `PREFILL`, `DECODE`). The `session_manager` is responsible for transitioning the session between these states.
    - **RX Handling:** The `on_readable()` method is called by the main loop when a client socket has data to be read. It reads the data into a per-session receive buffer (`rx`).
    - **Protocol Parsing (JSON-only):** Parses a length-prefixed JSON frame from `rx`, validates it, tokenizes the prompt, and transitions the session to `PREFILL`.

### `protocol`

- **Purpose:** A helper module that implements the low-level details of the UMA Serve wire protocol.
- **Functionality:**
    - `try_read_frame()`: Parses a length-prefixed JSON frame from a session's receive buffer.
    - `write_frame()`: Constructs a length-prefixed JSON frame and writes it to a session's transmit buffer (`tx`).
    - Provides helpers (e.g., `append_token_event`) to build standard JSON event objects.

## Data Flow

1.  `umad` starts, and `uds_server` creates and listens on the socket.
2.  `poller` waits for events.
3.  A client connects. The `poller` reports a readable event on the listen socket. `uds_server` accepts the connection, and `session_manager` creates a new `ClientSession`.
4.  The client sends a request. The `poller` reports a readable event on the client's socket.
5.  The main loop calls `session_manager::on_readable()`.
6.  `on_readable()` reads the data into the session's `rx` buffer.
7.  It parses the request as a framed JSON object.
8.  It tokenizes the prompt and transitions the session state to `PREFILL`.
9.  The main loop then calls the `Scheduler`, which sees the session in the `PREFILL` state and begins processing it.
