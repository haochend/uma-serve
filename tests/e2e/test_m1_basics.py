#!/usr/bin/env python3
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import pytest
import json
import struct

# Add root of project to path
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


# Import the shared JSON request helper and daemon fixture from conftest.py
from .conftest import umad_daemon, _send_json_request


def get_rss_kib(pid: int) -> int:
    try:
        out = subprocess.check_output(
            ["ps", "-o", "rss=", "-p", str(pid)], text=True
        ).strip()
        return int(out)
    except Exception:
        return -1


@pytest.mark.e2e
def test_basic_json_exchange(umad_daemon):
    sock_path, _ = umad_daemon
    request_id = "test_basic_json"
    request = {
      "id": request_id,
      "prompt": "Hello from basic JSON test",
      "stream": True,
      "max_tokens": 10,
    }
    responses = _send_json_request(sock_path, request)

    assert len(responses) > 1, "Expected multiple events"
    token_events = [r for r in responses if r.get("event") == "token"]
    assert len(token_events) > 0, "Did not receive any 'token' events"
    assert token_events[0]["id"] == request_id
    assert "text" in token_events[0]
    eos_events = [r for r in responses if r.get("event") == "eos"]
    assert len(eos_events) == 1, "Expected one 'eos' event"
    assert eos_events[0]["id"] == request_id
    assert eos_events[0]["reason"] in ["stop", "length"]


@pytest.mark.e2e
def test_multi_clients_json(umad_daemon):
    sock_path, _ = umad_daemon
    results = [None] * 4

    def worker(i):
        request_id = f"client_{i}"
        request = {
            "id": request_id,
            "prompt": f"Client {i} saying hello.",
            "stream": True,
            "max_tokens": 5,
        }
        responses = _send_json_request(sock_path, request)
        results[i] = responses

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(len(results))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    
    for i, responses in enumerate(results):
        assert responses is not None, f"Client {i} got no response"
        token_events = [r for r in responses if r.get("event") == "token"]
        assert len(token_events) > 0, f"Client {i} did not receive any 'token' events"
        assert token_events[0]["id"] == f"client_{i}"
        eos_events = [r for r in responses if r.get("event") == "eos"]
        assert len(eos_events) == 1, f"Client {i} did not receive 'eos' event"


@pytest.mark.e2e
def test_reuse_session_json(umad_daemon):
    sock_path, _ = umad_daemon
    request_id_prefix = "reuse_session"

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(90)
        s.connect(sock_path)
        
        for i, msg in enumerate(("First prompt", "Second prompt")):
            current_request_id = f"{request_id_prefix}_{i}"
            request = {
                "id": current_request_id,
                "prompt": msg,
                "stream": True,
                "max_tokens": 5,
            }
            payload = json.dumps(request).encode('utf-8')
            header = struct.pack('<I', len(payload))
            s.sendall(header + payload)

            responses = []
            while True:
                try:
                    header_data = s.recv(4)
                    if not header_data or len(header_data) < 4:
                        break
                    payload_len = struct.unpack('<I', header_data)[0]
                    payload_data = s.recv(payload_len)
                    event = json.loads(payload_data.decode('utf-8'))
                    responses.append(event)
                    if event.get("event") == "eos":
                        break
                except socket.timeout:
                    break

            token_events = [r for r in responses if r.get("event") == "token"]
            assert len(token_events) > 0, f"Session reuse {i}: No tokens received"
            assert token_events[0]["id"] == current_request_id
            eos_events = [r for r in responses if r.get("event") == "eos"]
            assert len(eos_events) == 1, f"Session reuse {i}: No EOS event received"
            assert eos_events[0]["id"] == current_request_id


@pytest.mark.e2e
def test_rss_stability_json(umad_daemon):
    sock_path, proc = umad_daemon
    request_id_prefix = "rss_test"

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(60)
        s.connect(sock_path)
        
        # Warm-up
        warmup_request = {
            "id": "warmup", "prompt": "warm up", "stream": True, "max_tokens": 5
        }
        payload = json.dumps(warmup_request).encode('utf-8')
        header = struct.pack('<I', len(payload))
        s.sendall(header + payload)
        # Read until EOS
        while True:
            header_data = s.recv(4)
            if not header_data: break
            payload_len = struct.unpack('<I', header_data)[0]
            payload_data = s.recv(payload_len)
            event = json.loads(payload_data.decode('utf-8'))
            if event.get("event") == "eos": break

        rss_samples = []
        for i in range(3):
            current_request_id = f"{request_id_prefix}_{i}"
            request = {
                "id": current_request_id, "prompt": f"measure {i}", "stream": True, "max_tokens": 5
            }
            payload = json.dumps(request).encode('utf-8')
            header = struct.pack('<I', len(payload))
            s.sendall(header + payload)

            # Read until EOS
            while True:
                header_data = s.recv(4)
                if not header_data: break
                payload_len = struct.unpack('<I', header_data)[0]
                payload_data = s.recv(payload_len)
                event = json.loads(payload_data.decode('utf-8'))
                if event.get("event") == "eos": break
            
            rss_kib = get_rss_kib(proc.pid)
            if rss_kib > 0:
                rss_samples.append(rss_kib)
            time.sleep(0.3)
        
        if len(rss_samples) >= 2:
            drift = max(rss_samples) - min(rss_samples)
            assert drift < 64 * 1024, f"RSS drift too high: {drift} KiB"


@pytest.mark.e2e
def test_oversize_json(umad_daemon):
    sock_path, _ = umad_daemon
    request_id = "test_oversize"
    # Create an oversized prompt within a JSON request
    long_prompt = "A" * (90000) # This will be well over the 65536 byte limit
    request = {
        "id": request_id,
        "prompt": long_prompt,
        "stream": True,
        "max_tokens": 5,
    }
    
    responses = _send_json_request(sock_path, request, timeout=30)
    
    assert len(responses) == 1, "Expected only one error event"
    error_event = responses[0]
    assert error_event.get("event") == "error"
    assert error_event.get("id") == request_id
    assert "prompt too large" in error_event.get("message", "").lower()
    assert error_event.get("code") == "E_LIMIT_001" # As per PRD.md

@pytest.mark.e2e
def test_invalid_utf8_json(umad_daemon):
    sock_path, _ = umad_daemon
    request_id = "test_invalid_utf8"
    # Create a JSON request with invalid UTF-8 in the prompt
    # The JSON payload itself must be valid UTF-8, but the 'prompt' value can contain invalid bytes if encoded directly
    # Python strings cannot hold invalid UTF-8, so we construct the JSON with the invalid byte directly
    
    # This string literally includes an invalid UTF-8 byte
    invalid_utf8_prompt_value = "Hello \xc0bad world".encode('latin-1').decode('utf-8', 'ignore') # This creates a valid python string by ignoring the bad byte.                                                                                                # The actual test should aim to send raw bytes containing invalid utf-8.
                                                                                                # For simplicity, we'll test for an error where the server attempts to parse the prompt.

    # Instead of injecting raw bytes, let's craft a JSON string with an explicitly invalid escape sequence or character.
    # The server's JSON parser might validate this, or the `util::is_valid_utf8` will catch it after extraction.
    
    # Simulating a situation where the raw prompt would be invalid UTF-8
    # We can't put raw \xc0 into a Python string literal that's then json.dumps'd without it being escaped.
    # The server side `util::is_valid_utf8` checks after extracting the prompt string.
    # So we want to make sure the extracted string contains invalid UTF-8
    # A simple way to simulate this is if the server's *internal* parsing of the string yields invalid UTF-8.
    
    # Let's try sending a valid JSON frame, but with content that, if interpreted as a prompt string, is invalid.
    # The server will decode the JSON, get the prompt string, and then *its* UTF-8 validator should catch it.
    
    # A simple way to achieve this is to manually construct the JSON string, and put the invalid UTF-8 in the prompt.
    # This bypasses json.dumps's utf-8 enforcement for strings.
    json_payload_str = '{"id":"' + request_id + '", "prompt": "Hello \\xc0bad world", "stream": true, "max_tokens": 5}'
    
    # Pack it as a frame
    payload = json_payload_str.encode('utf-8') # This payload is valid UTF-8 as a string literal
    header = struct.pack('<I', len(payload))

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(30)
        s.connect(sock_path)
        s.sendall(header + payload)

        responses = []
        while True:
            try:
                header_data = s.recv(4)
                if not header_data: break
                payload_len = struct.unpack('<I', header_data)[0]
                payload_data = s.recv(payload_len)
                event = json.loads(payload_data.decode('utf-8'))
                responses.append(event)
                if event.get("event") == "eos" or event.get("event") == "error": break
            except socket.timeout:
                break
    
    assert len(responses) == 1, "Expected only one error event"
    error_event = responses[0]
    assert error_event.get("event") == "error"
    assert error_event.get("id") == request_id
    assert "invalid utf-8" in error_event.get("message", "").lower()
    assert error_event.get("code") == "E_PROTO_001" # Or similar code for invalid prompt