#!/usr/bin/env python3
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import pytest
import json
import struct

# Add root of project to path
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _send_json_request(sock_path, request_data, timeout=20):
    """Sends a framed JSON request and returns a list of received JSON objects."""
    responses = []
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        s.connect(sock_path)
        
        payload = json.dumps(request_data).encode('utf-8')
        header = struct.pack('<I', len(payload))
        s.sendall(header + payload)
        
        while True:
            try:
                header_data = s.recv(4)
                if not header_data:
                    break
                
                if len(header_data) < 4:
                    break

                payload_len = struct.unpack('<I', header_data)[0]
                
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
                    break

            except (socket.timeout, ConnectionResetError, BrokenPipeError):
                break
    return responses


@pytest.mark.e2e
def test_json_protocol_exchange(umad_daemon):
    """
    Tests a basic request/response flow using the framed JSON protocol.
    This is the primary test for M4 functionality.
    """
    sock_path, _ = umad_daemon

    request = {
      "id": "test_m4_json",
      "prompt": "Hello world",
      "stream": True,
      "max_tokens": 5,
    }

    responses = _send_json_request(sock_path, request)

    assert len(responses) > 1, f"Expected multiple events, but got {len(responses)}"

    token_events = [r for r in responses if r.get("event") == "token"]
    assert len(token_events) > 0, "Did not receive any 'token' events"
    first_token = token_events[0]
    assert first_token["id"] == "test_m4_json"
    assert "text" in first_token
    assert "token_id" in first_token

    eos_events = [r for r in responses if r.get("event") == "eos"]
    assert len(eos_events) == 1, "Expected exactly one 'eos' event"
    eos_event = eos_events[0]
    assert eos_event["id"] == "test_m4_json"
    assert eos_event["reason"] in ["stop", "length", "error"], f"Unexpected EOS reason: {eos_event['reason']}"


@pytest.mark.e2e
@pytest.mark.xfail(reason="Cancellation not implemented yet")
def test_json_protocol_cancellation(umad_daemon):
    """
    Tests that an in-flight request can be cancelled.
    This is a TDD test and is expected to fail until cancellation is implemented.
    """
    sock_path, _ = umad_daemon
    request_id = "test_cancellation"

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(20)
        s.connect(sock_path)
        
        request = {
          "id": request_id,
          "prompt": "Tell me a very long story about a robot that explores the deep sea.",
          "max_tokens": 200,
        }
        payload = json.dumps(request).encode('utf-8')
        header = struct.pack('<I', len(payload))
        s.sendall(header + payload)

        received_events = []
        for _ in range(2):
            header_data = s.recv(4)
            if not header_data or len(header_data) < 4:
                pytest.fail("Server closed connection prematurely while waiting for tokens.")
            
            payload_len = struct.unpack('<I', header_data)[0]
            payload_data = s.recv(payload_len)
            event = json.loads(payload_data.decode('utf-8'))
            assert event.get("event") == "token"
            received_events.append(event)
        
        assert len(received_events) == 2, "Did not receive initial tokens."

        cancel_request = { "event": "cancel", "id": request_id }
        payload = json.dumps(cancel_request).encode('utf-8')
        header = struct.pack('<I', len(payload))
        s.sendall(header + payload)

        while True:
            try:
                header_data = s.recv(4)
                if not header_data or len(header_data) < 4:
                    break
                payload_len = struct.unpack('<I', header_data)[0]
                payload_data = s.recv(payload_len)
                event = json.loads(payload_data.decode('utf-8'))
                received_events.append(event)
                if event.get("event") == "eos":
                    break
            except socket.timeout:
                pytest.fail("Test timed out waiting for EOS event after cancellation.")
                break
    
    eos_events = [r for r in received_events if r.get("event") == "eos"]
    assert len(eos_events) == 1, "Expected exactly one EOS event after cancellation."
    
    eos_event = eos_events[0]
    assert eos_event.get("reason") == "cancelled", "EOS reason should be 'cancelled'."
    
    token_events = [r for r in received_events if r.get("event") == "token"]
    assert len(token_events) < request["max_tokens"], "Generation was not stopped and continued to completion."


@pytest.mark.e2e
def test_json_protocol_invalid_request(umad_daemon):
    """Send a JSON frame missing 'prompt' and expect an error event."""
    sock_path, _ = umad_daemon
    request = {"id": "bad1"}  # missing prompt
    responses = _send_json_request(sock_path, request)
    assert len(responses) == 1, "Expected a single error response and close"
    ev = responses[0]
    assert ev.get("event") == "error"
    assert ev.get("code") in ("E_PROTO_BAD_REQUEST", "E_PROTO_INVALID_JSON")


@pytest.mark.e2e
def test_json_protocol_oversize_header(umad_daemon):
    """Send an oversize length-prefixed frame and expect an error and close."""
    sock_path, _ = umad_daemon
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(10)
        s.connect(sock_path)
        # craft a frame with length > 1 MiB
        payload_len = 2 * 1024 * 1024
        header = struct.pack('<I', payload_len)
        s.sendall(header)
        # attempt to read an error frame
        try:
            hdr = s.recv(4)
            if not hdr or len(hdr) < 4:
                pytest.fail("Server did not send any response to oversize frame header")
            n = struct.unpack('<I', hdr)[0]
            data = s.recv(n)
            ev = json.loads(data.decode('utf-8'))
            assert ev.get("event") == "error"
            assert ev.get("code") in ("E_PROTO_FRAME_TOO_LARGE", "E_PROTO_INVALID_LEN")
        except socket.timeout:
            pytest.fail("Timed out waiting for oversize error event")
