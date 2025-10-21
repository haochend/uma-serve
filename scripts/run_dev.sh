#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# Defaults
MODEL="${UMA_MODEL:-}"
N_CTX="${UMA_N_CTX:-}"
THREADS="${UMA_THREADS:-}"
SOCK="${UMA_SOCK:-}"

usage() {
  cat <<EOF
run_dev.sh - build and run umad with sensible dev env

Usage:
  scripts/run_dev.sh -m /path/model.gguf [--n-ctx N] [--threads N] [--socket /tmp/uma.sock]

Env (fallbacks):
  UMA_MODEL, UMA_N_CTX, UMA_THREADS, UMA_SOCK

Logging (override as desired):
  LLAMA_LOG_VERBOSITY (default 2=INFO), LLAMA_LOG_PREFIX=1, LLAMA_LOG_TIMESTAMPS=1

Metal resources (macOS):
  GGML_METAL_PATH_RESOURCES (auto-set to llama.cpp metal dir if unset)
EOF
}

# Parse args
ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -m|--model)
      MODEL="$2"; shift 2 ;;
    --n-ctx)
      N_CTX="$2"; shift 2 ;;
    --threads)
      THREADS="$2"; shift 2 ;;
    --socket|--sock)
      SOCK="$2"; shift 2 ;;
    -h|--help)
      usage; exit 0 ;;
    --)
      shift; break ;;
    *)
      echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ -z "${MODEL:-}" ]]; then
  echo "Error: model path is required (-m or UMA_MODEL)" >&2
  usage
  exit 2
fi

# Build
"$ROOT/build.sh"

# Logging defaults (can be overridden by user env)
export LLAMA_LOG_VERBOSITY="${LLAMA_LOG_VERBOSITY:-2}"
export LLAMA_LOG_PREFIX="${LLAMA_LOG_PREFIX:-1}"
export LLAMA_LOG_TIMESTAMPS="${LLAMA_LOG_TIMESTAMPS:-1}"

# Metal resources hint if not provided
if [[ "${GGML_METAL_PATH_RESOURCES:-}" == "" ]]; then
  METAL_RES="$ROOT/external/llama.cpp/ggml/src/ggml-metal"
  if [[ -d "$METAL_RES" ]]; then
    export GGML_METAL_PATH_RESOURCES="$METAL_RES"
  fi
fi

CMD=("$ROOT/build/umad" --model "$MODEL")
[[ -n "${N_CTX}"    ]] && CMD+=(--n-ctx    "$N_CTX")
[[ -n "${THREADS}"  ]] && CMD+=(--threads  "$THREADS")
[[ -n "${SOCK}"     ]] && CMD+=(--socket   "$SOCK")

echo "Running: ${CMD[*]}"
exec "${CMD[@]}"

