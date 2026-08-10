import pytest
import socket
import random
import time
import json
import struct
from typing import Callable
from conftest import Compositor
from conftest import Daemon
from conftest import Selection
from conftest import cmp_entry
from conftest import wait_cond


def test_chunked(compositor: Compositor, daemon_runner: Callable):
    """Test IPC server correctly handles partial header reads"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    data: bytes = json.dumps({"type": "get_history_length"}).encode("utf-8")
    header: bytes = struct.pack("=BI", 0, len(data))

    daemon.sock.sendall(header[:3])
    time.sleep(0.5)
    daemon.sock.sendall(header[3:])
    daemon.sock.sendall(data)

    assert daemon.recv_msg().msg == {"size": 0}


def test_bad_input(compositor: Compositor, daemon_runner: Callable):
    """Test if daemon recovers from invalid JSON message"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "", False)

    data: bytes = '{"hey": 2, "}'.encode("utf-8")
    header: bytes = struct.pack("=BI", 0, len(data))

    daemon.sock.sendall(header + data)

    data = json.dumps({"type": "get_history_length"}).encode("utf-8")
    header = struct.pack("=BI", 0, len(data))

    daemon.sock.sendall(header + data)

    assert daemon.recv_msg().msg == {"size": 0}


def test_atomic_requests(compositor: Compositor, daemon_runner: Callable):
    """Test that multiple requests can be sent as a single JSON array"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "", False)

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"stuff": "x"})
    daemon.recv_event("entry_add")

    data: bytes = json.dumps(
        [
            {"type": "get_history_length"},
            {"type": "subscribe", "events": ["entry_add"]},
            {"type": "get_data", "id": 1, "mime_type": "stuff"},
            {"type": "unknown"},  # Error
        ]
    ).encode("utf-8")
    header: bytes = struct.pack("=BI", 0, len(data))

    daemon.sock.sendall(header + data)

    msg =  daemon.recv_msg()

    assert msg.msg == [
        {"size": 1},
        {"type": "success"},
        {"type": "success"},
        {"type": "error", "desc": "Invalid arguments"},
    ]
    assert msg.aux_data is None
