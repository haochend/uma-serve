#!/usr/bin/env python3
import argparse
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def start_daemon(model, sock_path, n_ctx=2048, threads=6, max_tokens=64, max_merge=2):
    bin_path = os.path.join(ROOT, "build", "umad")
    if not os.path.exists(bin_path):
        subprocess.run([os.path.join(ROOT, "build.sh")], check=True)
    cmd = [bin_path, "--model", model, "--socket", sock_path,
           "--n-ctx", str(n_ctx), "--threads", str(threads),
           "--max-tokens", str(max_tokens), "--max-merge", str(max_merge)]
    env = os.environ.copy()
    env.setdefault("LLAMA_LOG_VERBOSITY", "2")
    logf = open(os.path.join(ROOT, "build", "umad_m3.log"), "w")
    proc = subprocess.Popen(cmd, env=env, stdout=logf, stderr=subprocess.STDOUT, preexec_fn=os.setsid, cwd=ROOT)
    # wait for socket
    deadline = time.time() + 120
    while time.time() < deadline:
        if os.path.exists(sock_path):
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                    s.settimeout(0.5)
                    s.connect(sock_path)
                    return proc
            except Exception:
                pass
        if proc.poll() is not None:
            raise RuntimeError("daemon exited; see build/umad_m3.log")
        time.sleep(0.2)
    raise TimeoutError("UDS not ready")


def client_timed(sock_path, prompt):
    first_at = None
    done_at = None
    total = 0
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(120)
        s.connect(sock_path)
        s.sendall((prompt + "\n").encode("utf-8"))
        while True:
            data = s.recv(4096)
            if not data:
                break
            total += len(data)
            if first_at is None:
                first_at = time.time()
            if data.endswith(b"\n"):
                break
        done_at = time.time()
    return first_at, done_at, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=False, default=os.environ.get("UMA_MODEL"))
    ap.add_argument("--socket", default=None)
    ap.add_argument("--max-merge", type=int, default=2)
    ap.add_argument("--max-tokens", type=int, default=64)
    args = ap.parse_args()
    if not args.model:
        print("--model or UMA_MODEL required", file=sys.stderr)
        return 2
    sock = args.socket or os.path.join(tempfile.gettempdir(), f"uma.m3.{os.getpid()}.sock")

    proc = None
    try:
        proc = start_daemon(args.model, sock, max_tokens=args.max_tokens, max_merge=args.max_merge)
        # Two clients: one long, one short
        p_long = "Write a long detailed story about UMA runtime and scheduling, at least 300 words."
        p_short = "Say 'ok' ten times separated by spaces."
        res = {}

        def run(name, prompt):
            res[name] = client_timed(sock, prompt)

        t1 = threading.Thread(target=run, args=("long", p_long))
        t2 = threading.Thread(target=run, args=("short", p_short))
        t1.start(); t2.start(); t1.join(); t2.join()

        (l_first, l_done, l_bytes) = res["long"]
        (s_first, s_done, s_bytes) = res["short"]
        assert l_first and s_first
        # Interleaving expectation: short receives something before long completes
        assert s_first < l_done, "short client did not receive output before long finished (no interleaving?)"
        # Both produced outputs
        assert l_bytes > 0 and s_bytes > 0
        print("M3 interleaving smoke: PASS")
        return 0
    except Exception as e:
        print(f"M3 test failed: {e}", file=sys.stderr)
        return 1
    finally:
        if proc and proc.poll() is None:
            try: os.killpg(proc.pid, signal.SIGINT)
            except Exception: pass
            try: proc.wait(timeout=30)
            except Exception: proc.kill()
        try:
            if os.path.exists(sock): os.unlink(sock)
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main())

