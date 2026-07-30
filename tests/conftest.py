import mmap
import socket
from yapftests.main_test import captured_output
from typing import NamedTuple, Generator, Callable, Any
from enum import StrEnum
import subprocess as sb
import struct
import os
import pytest
import shutil
import re
import pathlib
import json

@pytest.fixture(scope="session")
def compositor(tmp_path_factory) -> Generator:
    """Run a Wayland compositor and yield the display it connected to"""
    tmp_path = tmp_path_factory.mktemp("compositor")

    path: str | None = shutil.which('sway')

    assert path is not None

    config = tmp_path / "config"

    # Create empty config
    with open(config, "w") as f:
        f.write("\n")
        pass

    proc: sb.Popen = sb.Popen(
            [path, '-d', '-c', config],
            stderr=sb.PIPE,
            text=True,
            bufsize=1,
            env={
                **os.environ,
                'WLR_BACKENDS': 'headless',
                'XDG_RUNTIME_DIR': str(tmp_path)
                })

    assert proc.stderr is not None

    display: str | None = None

    # Match sway debug output
    for line in proc.stderr:
        match: re.Match | None = re.search(r"on wayland display '(.+)'", line)

        if match is not None:
            display = match.group(1)
            break

    assert display is not None

    # Must use absolute path to display, because we cannot rely on
    # $XDG_RUNTIME_DIR
    display = tmp_path / display

    yield display

    proc.terminate()
    proc.wait()

class IpcMessage(NamedTuple):
    """
    Represents an IPC message, which has a message type and payload. If the IPC
    message has an SCM_RIGHTS fd, then it will be mapped to 'aux_data".
    """
    msg_type: int
    msg: Any
    aux_data: bytes | None


class Daemon():
    """
    Represents the swayclip daemon, can communicate with IPC.
    """
    proc: sb.Popen

    sock: socket.socket

    config_path: pathlib.Path
    db_path: pathlib.Path
    ipc_path: pathlib.Path

    def __init__(self, display: str, config: str, dir: pathlib.Path):
        path: str | None = os.getenv("TEST_DAEMON")

        assert path is not None

        self.config_path = dir / "config.toml"
        with open(self.config_path, "w") as f:
            f.writelines(config)

        self.db_path = dir / "history.sqlite3"
        self.ipc_path = dir / "ipc.socket"

        self.proc = sb.Popen(
                [path, '-r', '-d',
                 '-c', str(self.config_path),
                 '-s', str(self.db_path)],
                stdout=sb.PIPE,
                text=True,
                bufsize=1,
                env={
                    **os.environ,
                    'SWAYCLIP_SOCK': str(self.ipc_path),
                    'WAYLAND_DISPLAY': display,
                    })

        # Wait for initial ready message
        assert self.proc.stdout is not None

        assert self.proc.stdout.readline() == "Ready\n"

        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(str(self.ipc_path))

    def send_msg(self, msg: dict[str, str], scm_fd: int | None = None) -> None:
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

    def _recv_exact(self, n: int, want_fd: bool = False) -> tuple[bytes, int |
                                                                  None]:
        """
        Receive exactly "n" bytes from socket. If "want_fd" is True, then also
        return the SCM_RIGHTS fd if any, otherwise None.
        """
        buf: bytearray = bytearray()
        scm_fd: int | None = None

        while len(buf) < n:
            if want_fd and scm_fd is None:
                ancbuf_size = socket.CMSG_SPACE(struct.calcsize("i"))
                chunk, ancdata, _, _addr = self.sock.recvmsg(
                    n - len(buf), ancbuf_size
                )
                for level, type_, cmsg_data in ancdata:
                    if level == socket.SOL_SOCKET and type_ == socket.SCM_RIGHTS:
                        scm_fd = struct.unpack(
                            "i", cmsg_data[: struct.calcsize("i")]
                        )[0]
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
        (msg_type, size) = struct.unpack("=BI", header)

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

        return IpcMessage(
            msg_type=msg_type, msg=msg, aux_data=aux_data
        )

    def roundtrip(
            self, msg: dict[str, Any],
            scm_fd: int | None = None) -> IpcMessage:
        """Do a roundtrip (send message -> receive response)"""
        self.send_msg(msg, scm_fd)
        return self.recv_msg()

    def close(self):
        self.sock.close()
        self.proc.terminate()
        self.proc.wait()


@pytest.fixture
def daemon(tmp_path) -> Generator:
    created: list[Daemon] = []

    def run_daemon(display: str, config: str) -> Daemon:
        dmon: Daemon = Daemon(display, config, tmp_path)

        created.append(dmon)
        return dmon

    yield run_daemon

    for dmon in created:
        dmon.close()

class Selection(StrEnum):
    REGULAR = "regular"
    PRIMARY = "primary"

class Expect(StrEnum):
    SET = "set"
    CLEARED = "cleared"

class Client():
    """
    Represents a Wayland client that uses the clipboard.
    """
    proc: sb.Popen
    seat_name: str

    def __init__(self, proc: sb.Popen):
        self.proc = proc

        # Get name of seat
        self.seat_name = self._read_msg()

    def _write_msg(self, msg) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def _read_msg(self):
        assert self.proc.stdout is not None
        return json.loads(self.proc.stdout.readline().rstrip('\n'))

    def copy(self, sel: Selection, mime_types: dict[str, str] | None) -> None:
        """Copy "mime_types" to clipboard or if None, clear the clipboard"""
        if mime_types is None:
            self._write_msg({'action': 'clear', 'sel': str(sel)})
        else:
            self._write_msg({
                'action': 'set',
                'sel': str(sel),
                'mime_types': mime_types
                })

    def paste(self, sel: Selection, expect: bool = True) -> dict[str, str] | None:
        """
        Paste current clipboard contents, or None if clipboard is cleared
        """
        self._write_msg({
            'action': 'get',
            'sel': str(sel),
            'expect': expect
            })

        ret: dict[str, str] | str = self._read_msg()

        if isinstance(ret, str):
            return None
        else:
            return ret

    def close(self):
        self.proc.terminate()
        self.proc.wait()


@pytest.fixture
def client() -> Generator:
    path: str | None = os.getenv("TEST_CLIENT")

    assert path is not None

    created: list[Client] = []

    def create_client(display: str, seat: str | None) -> Client:
        """If "seat" is None, then a transient seat will be created and used"""
        args: list[str] = [path]

        if seat is not None:
            args.extend(['-s', seat])

        proc = sb.Popen(
                args,
                stdout=sb.PIPE,
                stdin=sb.PIPE,
                text=True,
                bufsize=1,
                env={
                    **os.environ,
                    'WAYLAND_DISPLAY': display
                    })

        client: Client = Client(proc)

        created.append(client)
        return client
    
    yield create_client

    for client in created:
        client.close()


def cmp_entry(
        entry: dict[str, Any],
        id: int,
        mime_types: list[str],
        pinned: bool) -> bool:
    """
    Compare if "entry" has the given attributes, ignoring creation time and
    update time
    """
    return entry['id'] == id and entry['mime-types'] == mime_types and entry['pinned'] == pinned
