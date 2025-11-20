# UMA Serve

UMA Serve is a lightweight runtime daemon (`umad`) for local LLMs, built on `llama.cpp`. It is optimized for Unified Memory Architecture (UMA) systems like Apple Silicon and AMD APUs, and is intended for multi-application use on a single host.

- **Shares Resources:** Loads one model and shares it across multiple concurrent applications.
- **Manages Concurrency:** Uses a fairness-oriented, continuous batching scheduler to run interactive and batch requests simultaneously while protecting the latency of interactive sessions.
- **Modern IPC:** Communicates via a Unix Domain Socket (UDS) using a framed JSON protocol.

**Status:** The core foundation is stable. This includes the UDS server, the framed JSON protocol, and a v1 implementation of the continuous batching scheduler. The project is now covered by a `pytest` end-to-end test suite and a C++ unit test suite using GoogleTest.

## Quick Start

### 1. Build

First, ensure the `llama.cpp` submodule is initialized:
```sh
git submodule update --init --recursive
```
Then, build the `umad` daemon:
```sh
./build.sh
```

### 2. Run the Daemon

Run the daemon in a terminal, pointing it to your model file.
```sh
# Required: Set the model path via environment variable or flag
export UMA_MODEL=/path/to/your/model.gguf

# Run the daemon
./build/umad
```

### 3. Interact with the Daemon

In a separate terminal, use a simple Python script to connect and send a request. The default communication protocol is framed JSON.

**`client.py`:**
```python
import socket
import json
import struct

sock_path = "/tmp/uma.sock"
request = {
  "id": "quickstart-1",
  "prompt": "Hello, UMA Serve!",
  "stream": True,
  "max_tokens": 20,
}

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
    s.connect(sock_path)
    
    # Frame and send the request
    payload = json.dumps(request).encode('utf-8')
    header = struct.pack('<I', len(payload))
    s.sendall(header + payload)
    
    # Receive and print response events
    while True:
        header_data = s.recv(4)
        if not header_data: break
        
        payload_len = struct.unpack('<I', header_data)[0]
        payload_data = s.recv(payload_len)
        event = json.loads(payload_data.decode('utf-8'))
        
        print(json.dumps(event, indent=2))
        
        if event.get("event") == "eos":
            break
```

Run the client:
```sh
python3 client.py
```

## Testing

The project uses `pytest` for end-to-end tests and GoogleTest for C++ unit tests.

To run the Python E2E tests:
```sh
# 1. Install test dependencies
python3 -m venv .venv
source .venv/bin/activate
pip install -r tests/requirements.txt

# 2. Set the model path
export UMA_MODEL=/path/to/your/model.gguf

# 3. Run the tests
pytest
```

To run the C++ unit tests:
```sh
# After building, run ctest
ctest --test-dir build
```

## Documentation

For detailed information on the project's design and features, please see the `docs` directory:

- **[System Design](./docs/SYSTEM_DESIGN.md):** High-level architecture and component overview.
- **[Protocol](./docs/PROTOCOL.md):** The framed JSON wire protocol.
- **[Scheduler](./docs/SCHEDULER.md):** The continuous batching and scheduling policy.
- **[Memory Management](./docs/MEMORY.md):** KV cache and memory sharing strategy.
- **[Metrics](./docs/METRICS.md):** The metrics endpoint and available performance data.
- **[Configuration](./docs/CONFIG.md):** All command-line arguments and environment variables.

Component-level `README.md` files are also available in each `src/` subdirectory.

## Key Flags and Environment Variables

Configuration is handled via command-line arguments and environment variables. For a complete list, see [`docs/CONFIG.md`](./docs/CONFIG.md).

- **Required:**
    - `--model /path/to/model.gguf` (or `UMA_MODEL`)
- **Common:**
    - `--n-ctx 4096` (or `UMA_N_CTX`)
    - `--threads N` (or `UMA_THREADS`)
    - `--socket /tmp/uma.sock` (or `UMA_SOCK`)
    - `--max-sessions 32`

## Roadmap

- [x] Continuous batching scheduler (v1)
- [x] Framed JSON protocol
- [x] `pytest` and GoogleTest integration
- [ ] Request cancellation
- [ ] QoS / Priority scheduling
- [ ] `uma-cli` client application
- [ ] Prefix/KV cache for faster prompt processing
- [ ] Linux `epoll` backend for the poller

---
*This project is currently in an early, experimental phase.*
