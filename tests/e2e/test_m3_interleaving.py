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

# Import the shared JSON request stream timed helper and daemon fixture from conftest.py
from .conftest import umad_daemon_m3, _send_json_request_stream_timed


@pytest.mark.e2e
def test_interleaving_json(umad_daemon_m3):
    sock_path, _ = umad_daemon_m3

    # Two clients: one long, one short
    p_long = "Write a long detailed story about UMA runtime and scheduling, at least 300 words."
    p_short = "Say 'ok' ten times separated by spaces."
    res = {}

    def run(name, prompt):
        # We need to open a new socket for each thread since _send_json_request_stream_timed expects an open socket
        # and we want parallel connections.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(120)
            s.connect(sock_path)
            res[name] = _send_json_request_stream_timed(s, prompt, req_id=name)

    t1 = threading.Thread(target=run, args=("long", p_long))
    t2 = threading.Thread(target=run, args=("short", p_short))

    # Stagger starts: short first to ensure it gets some tokens quickly.
    t2.start()
    time.sleep(0.05) # Small stagger to make the short request arrive slightly earlier
    t1.start()

    t1.join()
    t2.join()

    # Unpack results. If a TimeoutError occurred within a thread, the entry in `res` might be missing
    # so check for existence first to avoid KeyError
    long_res = res.get("long")
    short_res = res.get("short")

    assert long_res is not None, "Long client did not get any response/events."
    assert short_res is not None, "Short client did not get any response/events."

    (l_first, l_done, l_events) = long_res
    (s_first, s_done, s_events) = short_res

    assert l_first is not None, "Long-prompt client got no initial token event"
    assert s_first is not None, "Short-prompt client got no initial token event"
    
    # Interleaving expectation: short receives its first token before long completes its entire response
    assert s_first < l_done, "Short client did not receive output before long client finished (no interleaving?)"
    
    # Both produced output/events
    assert l_events > 0, "Long-prompt client produced no output events"
    assert s_events > 0, "Short-prompt client produced no output events"