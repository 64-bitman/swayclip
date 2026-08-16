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
from conftest import MessageType


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


def test_hold_requests(compositor: Compositor, daemon_runner: Callable):
    """Tests if requests can be held so that they executed atomically"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "", False)

    daemon.add_events(["entry_add"])
    assert (
        daemon.roundtrip({"type": "hold_requests", "n": 3}).msg_type
        == MessageType.SUCCESS
    )

    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    msg = daemon.recv_msg()
    assert msg.msg["event"] == "entry_add"

    daemon.send_msg({"type": "get_history_length"})
    daemon.send_msg({"type": "get_history_length"})
    daemon.send_msg({"type": "get_history_length"})

    assert daemon.recv_msg().msg == {"size": 1}
    assert daemon.recv_msg().msg == {"size": 1}
    assert daemon.recv_msg().msg == {"size": 1}

    assert daemon.roundtrip({"type": "get_history_length"}).msg == {"size": 1}


def test_events_after(compositor: Compositor, daemon_runner: Callable):
    """Test that emitted events are always sent after a request"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "", False)

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    daemon.add_events(["entry_state"])
    assert (
        daemon.roundtrip({"type": "set_clipboard", "id": 1}).msg_type
        == MessageType.SUCCESS
    )
    daemon.recv_msg().msg["id"] = 1
    daemon.recv_msg().msg["id"] = 1
