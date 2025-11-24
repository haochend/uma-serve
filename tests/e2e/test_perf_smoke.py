import json
import socket
import struct
import time
import pytest

from .conftest import _send_json_request


def _metrics_once(sock_path: str, timeout: float = 10.0):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        s.connect(sock_path)
        payload = json.dumps({"type": "metrics"}).encode("utf-8")
        header = struct.pack('<I', len(payload))
        s.sendall(header + payload)
        hdr = s.recv(4)
        if not hdr or len(hdr) < 4:
            return None
        n = struct.unpack('<I', hdr)[0]
        data = s.recv(n)
        if not data:
            return None
        evt = json.loads(data.decode("utf-8"))
        if isinstance(evt, dict) and evt.get("event") == "metrics":
            return evt.get("metrics", {})
        # Some configs may return raw JSON on newline path; try direct parse
        if isinstance(evt, dict):
            return evt
        return None


@pytest.mark.e2e
def test_metrics_perf_smoke(umad_daemon):
    sock_path, _ = umad_daemon

    # Kick a short generation to initialize decode path
    req = {"id": "perf-smoke-1", "prompt": "hello", "stream": True, "max_tokens": 8}
    evts = _send_json_request(sock_path, req)
    assert any(e.get("event") == "eos" for e in evts), "expected EOS"

    m1 = _metrics_once(sock_path)
    assert m1 is not None, "metrics should be available"
    # Basic presence checks
    assert m1.get("batch_calls_total", 0) >= 1
    assert m1.get("decode_ns_total", 0) >= 0
    assert m1.get("decode_ms_ewma", 0) >= 0

    # Another short generation to bump counters
    req2 = {"id": "perf-smoke-2", "prompt": "world", "stream": True, "max_tokens": 8}
    _ = _send_json_request(sock_path, req2)
    # Allow a brief moment for metrics to update
    time.sleep(0.05)
    m2 = _metrics_once(sock_path)
    assert m2 is not None

    # Tokens and calls should be monotonic
    assert m2.get("tokens_generated_total", 0) >= m1.get("tokens_generated_total", 0)
    assert m2.get("batch_calls_total", 0) >= m1.get("batch_calls_total", 0)

