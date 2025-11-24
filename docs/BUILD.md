# Building UMA Serve (Metal-only)

This project is configured to build against llama.cpp with a Metal-only ggml backend on macOS. We explicitly disable BLAS and Accelerate to avoid registering extra backends, which cause significantly larger reserved compute buffers on Metal and can hurt performance and memory footprint.

## Prerequisites

- macOS with Xcode command line tools (for Metal frameworks)
- CMake 3.13+

## Quick Start

```
./build.sh           # Debug build
./build.sh release   # Release build
```

Artifacts are placed under `./build/`.

## Why Metal-only?

When ggml registers multiple backends (e.g., Metal + BLAS/Accelerate), it reserves compute buffers for the worst-case execution path across backends and graph splits. On M4 Max we observed the Metal compute buffer grow from ~304 MiB (llama.cpp server) to ~1.8 GiB when extra backends were enabled. For UMA Serve, keeping the build Metal-only avoids that overhead and matches llama.cpp server behavior on Apple Silicon.

This repository enforces the following CMake options:

- `GGML_METAL=ON`
- `GGML_BLAS=OFF`
- `GGML_ACCELERATE=OFF`

`CMakeLists.txt` includes guards that will fail the configure step if BLAS or Accelerate are turned on.

## Clean Reconfigure

If you change build options or update the llama.cpp submodule, start from a clean build directory:

```
rm -rf build
./build.sh release
```

## Verifying Backend Configuration

Run `umad` with `UMA_LOG_LEVEL=debug` to print llama.cpp system info on startup. Look for lines indicating Metal support and the absence of BLAS/Accelerate backends.

```
UMA_LOG_LEVEL=debug ./build/umad --model /path/to/model.gguf --socket /tmp/uma.sock
```

## Submodule Version

To maintain consistent buffer sizing and performance, keep the `external/llama.cpp` submodule aligned with the commit used by the llama.cpp server you benchmark against. If you update the submodule, re-verify compute buffer sizes and throughput.

