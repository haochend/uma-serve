# Utilities

**Component:** `src/util`

This directory contains small, self-contained, header-only utilities that are used throughout the UMA Serve application.

## Purpose

The `util` components provide common, reusable functionality that is not specific to any single part of the server. Keeping these utilities here avoids code duplication and dependency cycles.

## Key Components

### `logging.h`

- **Purpose:** A simple, lightweight, header-only logging facility.
- **Functionality:**
    - Provides a set of macros for different log levels: `UMA_LOG_DEBUG`, `UMA_LOG_INFO`, `UMA_LOG_WARN`, `UMA_LOG_ERROR`.
    - `DEBUG`-level logs are only compiled in if the `UMA_DEBUG` macro is defined, ensuring zero performance overhead in release builds.
    - The log level can be configured at runtime via the `UMA_LOG_LEVEL` environment variable (e.g., `UMA_LOG_LEVEL=debug`).

### `utf8.h`

- **Purpose:** A simple UTF-8 validation function.
- **Functionality:**
    - Provides the `is_valid_utf8()` function.
    - This function is used by the `SessionManager` to validate incoming prompt data before it is passed to the `llama.cpp` backend, preventing potential errors or security issues from malformed input.
