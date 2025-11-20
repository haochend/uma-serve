#!/usr/bin/env python3
import os
import socket
import struct
import threading
import time
import json
import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Import shared fixtures and helpers from conftest
from .conftest import umad_daemon, _send_json_request_stream_timed


@pytest.mark.e2e
def test_concurrent_json_interleaving(umad_daemon):
    """
    Tests that the scheduler correctly interleaves two concurrent JSON requests.
    """
    sock_path, _ = umad_daemon
    p_long = "Write a long detailed story about UMA runtime and scheduling, at least 300 words."
    p_short = "Say 'ok' ten times separated by spaces."

    res = {}
    
    def run_json_client(name, prompt, max_tokens=100):
        # Each thread needs its own socket connection
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(120)
            s.connect(sock_path)
            res[name] = _send_json_request_stream_timed(s, prompt, req_id=name, max_tokens=max_tokens)

    # Use a higher max_tokens for the long prompt
    t1 = threading.Thread(target=run_json_client, args=("long", p_long, 300))
    t2 = threading.Thread(target=run_json_client, args=("short", p_short, 32))

    # Stagger starts slightly
    t1.start()
    time.sleep(0.1)
    t2.start()

    t1.join()
    t2.join()

    # Check that both clients got responses
    assert "long" in res, "Long JSON client did not return a result."
    assert "short" in res, "Short JSON client did not return a result."

    long_first, long_done, long_events = res['long']
    short_first, short_done, short_events = res['short']
    
    assert long_first is not None and long_done is not None, "Long JSON client failed to receive a full response."
    assert short_first is not None and short_done is not None, "Short JSON client failed to receive a full response."

    assert long_events > 1 and short_events > 1, "Both clients should have received multiple events."

    # The key interleaving assertion: the short request should get a token before the long one finishes.
    assert short_first < long_done, "JSON short must see tokens before JSON long completes"