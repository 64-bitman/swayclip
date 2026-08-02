import time
from collections import Counter
import sys
import mmap
import socket
from yapftests.main_test import captured_output
from typing import NamedTuple, Generator, Callable, Any, SupportsFloat
from enum import StrEnum, Enum
import subprocess as sb
import struct
import os
import pytest
import shutil
import re
import pathlib
import json


class Selection(StrEnum):
    REGULAR = "regular"
    PRIMARY = "primary"


class Compositor:
    proc: sb.Popen
    display: str

    def __init__(self, proc: sb.Popen, display: str):
        self.proc = proc
        self.display = display

    def _write_msg(self, msg) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def _read_msg(self):
        assert self.proc.stdout is not None
        return json.loads(self.proc.stdout.readline())

    def add_seat(self, name: str) -> None:
        """Add a new seat to the compositor"""
        self._write_msg({"cmd": "add_seat", "name": name})
        assert self._read_msg() == "OK"

    def del_seat(self, seat: str) -> None:
        """Remove seat from compositor"""
        self._write_msg({"cmd": "del_seat", "name": seat})
        assert self._read_msg() == "OK"

    def copy(
        self, seat: str, sel: Selection, mime_types: dict[str, str] | None
    ) -> None:
        """Copy "mime_types" to clipboard or if None, clear the clipboard"""
        if mime_types is None:
            self._write_msg({"cmd": "clear", "seat": seat, "sel": str(sel)})
        else:
            self._write_msg(
                {"cmd": "set", "seat": seat, "sel": str(sel), "mime_types": mime_types}
            )
        assert self._read_msg() == "OK"

    def pastex(self, seat: str, sel: Selection) -> dict[str, str] | None:
        """Paste current clipboard contents, or None if clipboard is cleared"""
        self._write_msg(
            {
                "cmd": "get",
                "seat": seat,
                "sel": str(sel),
            }
        )

        ret: dict[str, str] | None = self._read_msg()

        return ret

    def expect(self, seat: str, sel: Selection, expect: dict[str, str] | None) -> None:
        """Expect clipboard contents to be "expected" """

        def func():
            assert self.pastex(seat, sel) == expect

        wait_cond(func)

    def close(self):
        self.proc.terminate()
        self.proc.wait()


@pytest.fixture
def compositor(tmp_path_factory) -> Generator:
    """Run a Wayland compositor"""
    tmp_path = tmp_path_factory.mktemp("compositor")

    path: str | None = os.getenv("TEST_SERVER")

    assert path is not None

    proc: sb.Popen = sb.Popen(
        [path, "-d", tmp_path / "test"],
        stdout=sb.PIPE,
        stdin=sb.PIPE,
        text=True,
        bufsize=1,
        env={**os.environ, "XDG_RUNTIME_DIR": str(tmp_path)},
    )

    assert proc.stdout is not None

    assert proc.stdout.readline() == "Ready\n"

    # Must use absolute path to display, because we cannot rely on
    # $XDG_RUNTIME_DIR
    comp = Compositor(proc, tmp_path / "test")

    yield comp

    proc.terminate()
    proc.wait()


class MessageType(Enum):
    CALL = 0
    EVENT = 1


class IpcMessage(NamedTuple):
    """
    Represents an IPC message, which has a message type and payload. If the IPC
    message has an SCM_RIGHTS fd, then it will be mapped to 'aux_data".
    """

    msg_type: MessageType
    msg: Any
    aux_data: bytes | None


class Daemon:
    """
    Represents the swayclip daemon, can communicate with IPC.
    """

    proc: sb.Popen

    sock: socket.socket

    config_path: pathlib.Path
    db_path: pathlib.Path
    ipc_path: pathlib.Path

    # Current events to listen for
    events: list[str]

    def __init__(self, display: str, config: str, dir: pathlib.Path):
        path: str | None = os.getenv("TEST_DAEMON")

        assert path is not None

        self.config_path = dir / "config.toml"
        with open(self.config_path, "w") as f:
            f.writelines(config)

        self.db_path = dir / "history.sqlite3"
        self.ipc_path = dir / "ipc.socket"

        self.proc = sb.Popen(
            [path, "-r", "-d", "-c", str(self.config_path), "-s", str(self.db_path)],
            stdout=sb.PIPE,
            text=True,
            bufsize=1,
            env={
                **os.environ,
                "SWAYCLIP_SOCK": str(self.ipc_path),
                "WAYLAND_DISPLAY": str(display),
            },
        )

        # Wait for initial ready message
        assert self.proc.stdout is not None
        assert self.proc.stdout.readline() == "Ready\n"

        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(str(self.ipc_path))
        self.events = []

    def _subscribe(self):
        assert self.roundtrip({"type": "subscribe", "events": self.events}).msg == {
            "type": "success"
        }

    def add_events(self, events: list[str]):
        self.events.extend(events)
        self._subscribe()

    def remove_events(self, events: list[str]):
        self.events = [e for e in self.events if e not in events]
        self._subscribe()

    def send_msg(self, msg: dict[str, Any], scm_fd: int | None = None) -> None:
        """
        Send message to compositor with "scm_fd" as ancillary data if not None
        """
        data: bytes = json.dumps(msg).encode("utf-8")
        header: bytes = struct.pack("=BI", 0, len(data))
        packet: bytes = header + data

        if scm_fd is not None:
            ancdata: list[tuple] = [
                (socket.SOL_SOCKET, socket.SCM_RIGHTS, struct.pack("i", scm_fd))
            ]
            self.sock.sendmsg([packet], ancdata)
        else:
            self.sock.sendall(packet)

    def _recv_exact(self, n: int, want_fd: bool = False) -> tuple[bytes, int | None]:
        """
        Receive exactly "n" bytes from socket. If "want_fd" is True, then also
        return the SCM_RIGHTS fd if any, otherwise None.
        """
        buf: bytearray = bytearray()
        scm_fd: int | None = None

        while len(buf) < n:
            if want_fd and scm_fd is None:
                ancbuf_size = socket.CMSG_SPACE(struct.calcsize("i"))
                chunk, ancdata, _, _addr = self.sock.recvmsg(n - len(buf), ancbuf_size)
                for level, type_, cmsg_data in ancdata:
                    if level == socket.SOL_SOCKET and type_ == socket.SCM_RIGHTS:
                        scm_fd = struct.unpack("i", cmsg_data[: struct.calcsize("i")])[
                            0
                        ]
            else:
                chunk = self.sock.recv(n - len(buf))

            assert chunk is not None
            buf.extend(chunk)

        return bytes(buf), scm_fd

    def recv_msg(self) -> IpcMessage:
        """
        Receive a message from the daemon
        """
        header, fd = self._recv_exact(5, want_fd=True)
        msg_type, size = struct.unpack("=BI", header)

        # Only keep asking for the body if we haven't already picked up
        # the fd while reading the header.
        body, fd2 = self._recv_exact(size, want_fd=(fd is None))

        if fd is None:
            fd = fd2

        msg: dict[str, Any] = json.loads(body.decode("utf-8"))

        aux_data: bytes | None = None
        if fd is not None:
            try:
                st = os.fstat(fd)
                if st.st_size > 0:
                    with mmap.mmap(fd, st.st_size, prot=mmap.PROT_READ) as mm:
                        aux_data = mm.read(st.st_size)
            finally:
                os.close(fd)

        return IpcMessage(msg_type=MessageType(msg_type), msg=msg, aux_data=aux_data)

    def roundtrip(self, msg: dict[str, Any], scm_fd: int | None = None) -> IpcMessage:
        """Do a roundtrip (send message -> receive response)"""
        self.send_msg(msg, scm_fd)
        return self.recv_msg()

    def recv_event(self, event: str) -> IpcMessage:
        """Receive an event and remove it from the events list"""
        msg = self.recv_msg()
        self.remove_events([event])
        assert "event" in msg.msg
        assert msg.msg["event"] == event
        return msg

    def close(self):
        self.sock.close()
        self.proc.terminate()
        self.proc.wait()


@pytest.fixture
def daemon_runner(tmp_path) -> Generator:
    """Run swayclip daemon"""
    created: list[Daemon] = []

    def run_daemon(display: str, config: str) -> Daemon:
        dmon: Daemon = Daemon(display, config, tmp_path)

        created.append(dmon)
        return dmon

    yield run_daemon

    for dmon in created:
        dmon.close()


def cmp_entry(
    entry: dict[str, Any],
    id: int,
    mime_types: list[str],
    pinned: bool,
    content_type: str | None,
    content_mime: str | None,
) -> None:
    """
    Assert if "entry" has the given values, ignoring creation time and update
    time
    """
    assert entry["id"] == id
    assert Counter(entry["mime_types"]) == Counter(mime_types)
    assert entry["pinned"] == pinned
    if content_type is not None:
        assert entry["content_type"] == content_type
    if content_mime is not None:
        assert entry["content_mime_type"] == content_mime


def wait_cond(func: Callable, timeout: float = 1):
    start = time.time()

    while True:
        try:
            func()
            break  # Success
        except:
            if time.time() - start > timeout:
                raise
            time.sleep(0.05)
