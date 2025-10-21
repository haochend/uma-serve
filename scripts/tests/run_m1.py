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


def start_daemon(model, sock_path, n_ctx=None, threads=None):
    bin_path = os.path.join(ROOT, "build", "umad")
    if not os.path.exists(bin_path):
        # build if missing
        subprocess.run([os.path.join(ROOT, "build.sh")], check=True)
    cmd = [bin_path, "--model", model, "--socket", sock_path]
    if n_ctx:
        cmd += ["--n-ctx", str(n_ctx)]
    if threads:
        cmd += ["--threads", str(threads)]
    env = os.environ.copy()
    # reduce log noise a bit
    env.setdefault("LLAMA_LOG_VERBOSITY", "2")
    env.setdefault("LLAMA_LOG_PREFIX", "1")
    env.setdefault("LLAMA_LOG_TIMESTAMPS", "0")
    logf = open(os.path.join(ROOT, "build", "umad_test.log"), "w")
    # new process group so we can signal the whole group
    proc = subprocess.Popen(
        cmd,
        env=env,
        stdout=logf,
        stderr=subprocess.STDOUT,
        preexec_fn=os.setsid,
        cwd=ROOT,
    )
    # wait until socket is connectable
    deadline = time.time() + 120
    while time.time() < deadline:
        if os.path.exists(sock_path):
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                    s.settimeout(0.5)
                    s.connect(sock_path)
                    # connected; server ready
                    return proc
            except Exception:
                pass
        if proc.poll() is not None:
            raise RuntimeError("daemon exited early; see build/umad_test.log")
        time.sleep(0.2)
    raise TimeoutError("timed out waiting for UDS to be ready")


def uds_request(sock_path, prompt, timeout=60, raw_bytes: bytes | None = None):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        s.connect(sock_path)
        if raw_bytes is not None:
            s.sendall(raw_bytes)
        else:
            s.sendall((prompt + "\n").encode("utf-8"))
        chunks = []
        start = time.time()
        while True:
            try:
                data = s.recv(4096)
            except socket.timeout:
                break
            if not data:
                break
            chunks.append(data)
            if b"\n" in data:
                break
            # safety: avoid running forever
            if time.time() - start > timeout:
                break
        return b"".join(chunks)


def test_basic(sock_path):
    out = uds_request(sock_path, "Hello from tests", timeout=90)
    assert len(out) > 0, "no output received"
    assert out.endswith(b"\n"), "stream should end with newline"


def test_multi_clients(sock_path):
    # 4 concurrent clients as per M2 acceptance
    results = [None, None, None, None]

    def worker(i):
        results[i] = uds_request(sock_path, f"Client {i}", timeout=90)

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(len(results))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for i, out in enumerate(results):
        assert out is not None and len(out) > 0, f"client {i} got no output"


def test_reuse_session(sock_path):
    # Single connection, two prompts
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(90)
        s.connect(sock_path)
        for msg in ("First prompt", "Second prompt"):
            s.sendall((msg + "\n").encode("utf-8"))
            chunks = []
            while True:
                data = s.recv(4096)
                if not data:
                    break
                chunks.append(data)
                if b"\n" in data:
                    break
            out = b"".join(chunks)
            assert len(out) > 0 and out.endswith(b"\n"), "session reuse response invalid"


def get_rss_kib(pid: int) -> int:
    try:
        out = subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], text=True).strip()
        return int(out)
    except Exception:
        return -1


def test_rss_stability(sock_path, pid: int):
    # Measure RSS across repeated prompts on a single session (after warm-up)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(60)
        s.connect(sock_path)
        # warm-up
        s.sendall(b"warm up\n")
        s.recv(4096)
        rss_samples = []
        for i in range(3):
            s.sendall(f"measure {i}\n".encode("utf-8"))
            # read until newline
            chunks = []
            while True:
                data = s.recv(4096)
                if not data:
                    break
                chunks.append(data)
                if b"\n" in data:
                    break
            rss_kib = get_rss_kib(pid)
            if rss_kib > 0:
                rss_samples.append(rss_kib)
            time.sleep(0.3)
        if len(rss_samples) >= 2:
            drift = max(rss_samples) - min(rss_samples)
            # allow modest drift (e.g., 64 MiB) due to allocator/Metal behavior
            assert drift < 64 * 1024, f"RSS drift too high: {drift} KiB"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=False, default=os.environ.get("UMA_MODEL"))
    ap.add_argument("--socket", default=None)
    ap.add_argument("--n-ctx", type=int, default=4096)
    ap.add_argument("--threads", type=int, default=6)
    args = ap.parse_args()

    if not args.model:
        print("--model or UMA_MODEL is required", file=sys.stderr)
        return 2

    sock_path = args.socket or os.path.join(tempfile.gettempdir(), f"uma.test.{os.getpid()}.sock")

    proc = None
    try:
        proc = start_daemon(args.model, sock_path, n_ctx=args.n_ctx, threads=args.threads)
        test_basic(sock_path)
        test_multi_clients(sock_path)
        test_reuse_session(sock_path)
        test_rss_stability(sock_path, proc.pid)
        test_oversize(sock_path)
        test_invalid_utf8(sock_path)
        print("M1/M2 smoke tests: PASS")
        return 0
    except Exception as e:
        print(f"Tests failed: {e}", file=sys.stderr)
        return 1
    finally:
        if proc and proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGINT)
            except Exception:
                pass
            try:
                proc.wait(timeout=30)
            except Exception:
                proc.kill()
        try:
            if os.path.exists(sock_path):
                os.unlink(sock_path)
        except Exception:
            pass


def test_oversize(sock_path):
    big = b"A" * 9000 + b"\n"
    out = uds_request(sock_path, "", raw_bytes=big, timeout=30)
    assert b"error: prompt too large" in out, "expected oversize error"


def test_invalid_utf8(sock_path):
    bad = b"\xC0bad\n"
    out = uds_request(sock_path, "", raw_bytes=bad, timeout=30)
    assert b"error: invalid utf-8" in out, "expected utf-8 error"
if __name__ == "__main__":
    sys.exit(main())
