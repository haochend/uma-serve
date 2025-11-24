#!/usr/bin/env python3
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import pytest

# Add root of project to path
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))



@pytest.fixture(scope="module")
def umad_daemon():
    """
    Spins up the `umad` daemon for end-to-end tests.
    This is a shared fixture for all tests in the e2e directory.

    Yields:
        tuple: (socket_path, process_object)
    """
    model_path = os.environ.get("UMA_MODEL")
    if not model_path:
        pytest.fail("UMA_MODEL environment variable is not set.")
    if not os.path.exists(model_path):
        pytest.fail(f"Model file not found at: {model_path}")

    sock_path = os.path.join(tempfile.gettempdir(), f"uma.test.{os.getpid()}.sock")
    
    bin_path = os.path.join(ROOT, "build", "umad")
    if not os.path.exists(bin_path):
        # build if missing
        subprocess.run([os.path.join(ROOT, "build.sh")], check=True, cwd=ROOT)

    cmd = [
        bin_path,
        "--model", model_path,
        "--socket", sock_path,
        "--max-tokens", "32",
        "--n-ctx", "4096",
        "--threads", "6",
    ]
    
    env = os.environ.copy()
    # reduce log noise a bit
    env.setdefault("LLAMA_LOG_VERBOSITY", "2")
    env.setdefault("LLAMA_LOG_PREFIX", "1")
    env.setdefault("LLAMA_LOG_TIMESTAMPS", "0")
    env["UMA_DEBUG"] = "1"
    
    log_path = os.path.join(ROOT, "build", "umad_e2e_tests.log")
    logf = open(log_path, "w")

    # new process group so we can signal the whole group
    proc = subprocess.Popen(
        cmd,
        env=env,
        stdout=logf,
        stderr=subprocess.STDOUT,
        preexec_fn=os.setsid,
        cwd=ROOT,
    )

    try:
        # wait until socket is connectable
        deadline = time.time() + 120
        while time.time() < deadline:
            if os.path.exists(sock_path):
                try:
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                        s.settimeout(0.5)
                        s.connect(sock_path)
                        # connected; server ready
                        yield (sock_path, proc)
                        return
                except Exception:
                    pass
            if proc.poll() is not None:
                pytest.fail(f"daemon exited early; see {log_path}")
            time.sleep(0.2)
        pytest.fail("timed out waiting for UDS to be ready")

    finally:
        # Teardown
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
        logf.close()


@pytest.fixture(scope="module")
def umad_daemon_m3():
    """
    Spins up the `umad` daemon with M3-specific arguments for testing interleaving.
    """
    model_path = os.environ.get("UMA_MODEL")
    if not model_path:
        pytest.fail("UMA_MODEL environment variable is not set.")
    if not os.path.exists(model_path):
        pytest.fail(f"Model file not found at: {model_path}")

    sock_path = os.path.join(tempfile.gettempdir(), f"uma.m3.{os.getpid()}.sock")

    bin_path = os.path.join(ROOT, "build", "umad")
    if not os.path.exists(bin_path):
        subprocess.run([os.path.join(ROOT, "build.sh")], check=True, cwd=ROOT)

    cmd = [
        bin_path,
        "--model", model_path,
        "--socket", sock_path,
        "--n-ctx", "2048",
        "--threads", "6",
        "--max-tokens", "64",
    ]

    env = os.environ.copy()
    env.setdefault("LLAMA_LOG_VERBOSITY", "2")
    env["UMA_DEBUG"] = "1"
    log_path = os.path.join(ROOT, "build", "umad_test_m3.log")
    logf = open(log_path, "w")

    proc = subprocess.Popen(
        cmd,
        env=env,
        stdout=logf,
        stderr=subprocess.STDOUT,
        preexec_fn=os.setsid,
        cwd=ROOT,
    )

    try:
        # wait for socket
        deadline = time.time() + 120
        while time.time() < deadline:
            if os.path.exists(sock_path):
                try:
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                        s.settimeout(0.5)
                        s.connect(sock_path)
                        yield sock_path, proc
                        return
                except Exception:
                    pass
            if proc.poll() is not None:
                pytest.fail(f"daemon exited early; see {log_path}")
            time.sleep(0.2)
        pytest.fail("timed out waiting for UDS to be ready")

    finally:
        # Teardown
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
        logf.close()

import json
import struct

def _send_json_request(sock_path, request_data, timeout=20):
    """Sends a framed JSON request and returns a list of received JSON objects."""
    responses = []
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        s.connect(sock_path)
        
        payload = json.dumps(request_data).encode('utf-8')
        header = struct.pack('<I', len(payload))
        # Use blocking mode for sending to avoid hitting the per-operation timeout on large frames.
        s.settimeout(None)
        s.sendall(header + payload)
        # Restore timeout for receiving frames
        s.settimeout(timeout)
        
        while True:
            try:
                header_data = s.recv(4)
                if not header_data:
                    break
                
                if len(header_data) < 4:
                    # Could be a partial read, but for tests, we assume this is an error
                    # or the end of a stream from a buggy server.
                    break

                payload_len = struct.unpack('<I', header_data)[0]
                
                # Simple guard against absurdly large payloads
                if payload_len > 10 * 1024 * 1024:
                     break

                payload_data = b""
                while len(payload_data) < payload_len:
                    chunk = s.recv(payload_len - len(payload_data))
                    if not chunk:
                        break
                    payload_data += chunk
                
                if len(payload_data) == payload_len:
                    responses.append(json.loads(payload_data.decode('utf-8')))
                else:
                    # Incomplete payload read
                    break

            except (socket.timeout, ConnectionResetError, BrokenPipeError):
                break
    return responses

def _send_json_request_stream_timed(s: socket.socket, prompt: str, req_id: str, max_tokens: int = 32, timeout: int = 120):
    """
    Sends a framed JSON request on an already open socket and returns timing info.
    """
    first_at = None
    done_at = None
    total_events = 0

    request = {"id": req_id, "prompt": prompt, "stream": True, "max_tokens": max_tokens}
    payload = json.dumps(request).encode("utf-8")
    header = struct.pack('<I', len(payload))
    
    s.settimeout(None) # Use blocking send to avoid timeout on sendall itself
    s.sendall(header + payload)
    s.settimeout(timeout) # Restore timeout for receiving

    while True:
        try:
            hdr = s.recv(4)
            if not hdr or len(hdr) < 4:
                break
            n = struct.unpack('<I', hdr)[0]
            data = s.recv(n)
            if not data:
                break
            evt = json.loads(data.decode('utf-8'))
            total_events += 1
            if first_at is None and evt.get("event") == "token":
                first_at = time.time()
            if evt.get("event") in ("eos", "error"):
                done_at = time.time()
                break
        except (socket.timeout, ConnectionResetError, BrokenPipeError):
            break
    return first_at, done_at, total_events
