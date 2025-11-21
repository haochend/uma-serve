# UMA-CLI

**Component:** `src/cli`
**Status:** Available (MVP)

This directory is intended for the `uma-cli` executable, a command-line interface for interacting with the `umad` server. It serves as a reference client implementation and a useful tool for testing and scripting.

## Purpose

The `uma-cli` tool provides a user-friendly way to send prompts to the `umad` daemon and receive streamed responses directly in the terminal. It communicates over the Unix Domain Socket using the standard framed JSON protocol.

## Planned Usage

The CLI is invoked with flags to control the prompt, generation parameters, and server connection.

```sh
uma-cli [OPTIONS] --prompt "Your prompt text here"
```

### Key Command-Line Arguments

| Flag                  | Type    | Default         | Description                                                               |
| --------------------- | ------- | --------------- | ------------------------------------------------------------------------- |
| `--prompt <string>`   | string  | (none)          | **Required.** The prompt text to send to the model.                       |
| `--socket <path>`     | string  | `/tmp/uma.sock` | Path to the `umad` Unix Domain Socket.                                    |
| `--id <string>`       | string  | (uuid)          | A unique ID for the request. Defaults to a generated UUID.                |
| `--max-tokens <n>`    | int     | `2048`          | Maximum number of tokens to generate.                                     |
| `--temp <float>`      | float   | `0.8`           | Generation temperature. `0.0` means greedy decoding.                      |
| `--top-p <float>`     | float   | `0.95`          | Nucleus sampling (top-p) probability.                                     |
| `--no-stream`         | bool    | `false`         | If set, request non-streaming. Server may still stream in current MVP. |
| `--metrics`           | bool    | `false`         | Request a one-shot metrics snapshot and print it as JSON. |

## Output Format

By default, `uma-cli` will stream the `text` field from each JSON `token` event directly to `stdout` as it receives it, providing a real-time view of the generation. The final output will be terminated by a newline.

If a non-recoverable error occurs, a message will be printed to `stderr`.

Notes
- The client prints token text as it arrives and a newline on EOS. Errors are printed to stderr and return exit code 2.
- `--metrics` prints a single JSON line (wrapped by the server) and exits.

## Exit Codes

The tool will use standard exit codes to indicate its status:

- **0:** Success. The request was completed and an `eos` event was received without an error.
- **1:** General error (e.g., invalid arguments).
- **2:** Server error. An `error` event was received from the `umad` daemon.
- **3:** Protocol or connection error (e.g., cannot connect to socket, invalid framing).
