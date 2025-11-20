# Runtime

**Component:** `src/runtime`

This directory contains the core abstractions for interacting with the `llama.cpp` backend. It provides a stable, high-level interface for the rest of the `umad` application to use, encapsulating the details of model loading, context management, and tokenization.

## Purpose

The runtime component acts as a bridge between the UMA Serve application logic (like the scheduler and session manager) and the underlying `llama.cpp` inference engine. Its primary goals are:
- To manage the lifecycle of `llama.cpp` objects safely.
- To provide a consistent API for common operations like tokenization.
- To centralize configuration management related to the model and backend.

## Key Components

### `config.{h,cpp}`

- **Purpose:** Parses and stores all runtime configuration options.
- **Functionality:** It reads configuration from command-line arguments and environment variables, providing a single `RuntimeConfig` struct that can be passed throughout the application.
- **Further Reading:** For a detailed list of all available options, see [`../../docs/CONFIG.md`](../../docs/CONFIG.md).

### `model.{h,cpp}`

- **Purpose:** Manages the lifecycle of the `llama_model` and `llama_context` objects.
- **Functionality:**
    - `LlamaBackendGuard`: An RAII-compliant class that ensures `llama_backend_init` is called on startup and `llama_backend_free` is called on shutdown.
    - `ModelHandle`: An RAII wrapper around `llama_model_load` and `llama_free_model`. It ensures the model is loaded correctly based on the `RuntimeConfig` and is always freed when it goes out of scope.
    - `LlamaContextHandle`: A `std::unique_ptr` with a custom deleter that ensures `llama_free` is called for any created `llama_context`.

This strict RAII approach is critical for preventing resource leaks.

### `tokens.{h,cpp}`

- **Purpose:** Provides centralized helper functions for token-related operations.
- **Functionality:**
    - `tokenize()`: A wrapper around `llama_tokenize` that takes a standard `std::string` and returns a `std::vector<llama_token>`.
    - `token_to_piece_str()`: A wrapper around `llama_token_to_piece` that safely converts a token ID back into a string piece for streaming to the client.

By centralizing these functions, we ensure that tokenization is handled consistently everywhere in the application.
