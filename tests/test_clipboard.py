import time
from typing import Callable
from conftest import Compositor
from conftest import Daemon
from conftest import Selection
from conftest import cmp_entry
from conftest import wait_cond


def test_receive(compositor: Compositor, daemon_runner: Callable):
    """Test that contents of selection event is correctly received"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"text/plain": "hi", "a": "b"})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_history", "start": 0, "n": 1})
    cmp_entry(msg.msg[0], 1, ["text/plain", "a"], False, "text", "text/plain")

    msg = daemon.roundtrip({"type": "get_data", "id": 1, "mime_type": "text/plain"})

    assert msg.aux_data is not None
    assert msg.aux_data.decode("utf-8") == "hi"

    msg = daemon.roundtrip({"type": "get_data", "id": 1, "mime_type": "a"})

    assert msg.aux_data is not None
    assert msg.aux_data.decode("utf-8") == "b"


def test_delete(compositor: Compositor, daemon_runner: Callable):
    """Test that entries can be deleted"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"x": "y"})
    daemon.recv_event("entry_add")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    daemon.add_events(["entry_delete"])
    assert daemon.roundtrip({"type": "delete_entry", "id": 2}).msg == {
        "type": "success"
    }
    daemon.recv_event("entry_delete")

    # Clipboard should be cleared
    compositor.expect("test", Selection.REGULAR, None)

    assert daemon.roundtrip({"type": "get_history_length"}).msg == {"size": 1}


def test_sync(compositor: Compositor, daemon_runner: Callable):
    """Test that multiple seats are synced to the clipboard"""
    compositor.add_seat("test1")
    compositor.add_seat("test2")

    daemon: Daemon = daemon_runner(compositor.display, "")

    daemon.add_events(["entry_add"])
    compositor.copy("test1", Selection.REGULAR, {"text/plain": "hi"})
    daemon.recv_event("entry_add")

    compositor.expect("test1", Selection.PRIMARY, {"text/plain": "hi"})
    compositor.expect("test2", Selection.REGULAR, {"text/plain": "hi"})
    compositor.expect("test2", Selection.PRIMARY, {"text/plain": "hi"})


def test_receive_existing(compositor: Compositor, daemon_runner: Callable):
    """
    Test if initial selection event is received by daemon (that does not set the
    selection itself because database is empty)
    """
    compositor.add_seat("test")
    compositor.copy("test", Selection.REGULAR, {"text/plain": "hi"})

    daemon: Daemon = daemon_runner(compositor.display, "")

    # Wayland events may be received after daemon request is sent and processed,
    # so must check multiple times.
    def func():
        msg = daemon.roundtrip({"type": "get_history", "start": 0, "n": 1})
        cmp_entry(msg.msg[0], 1, ["text/plain"], False, "text", "text/plain")
    wait_cond(func)


def test_massive_data(compositor: Compositor, daemon_runner: Callable):
    """Test that receiving and sending a lot of data works properly"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    # See testserver.c for "massive" special case.
    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"test": "massive"})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_data", "id": 1, "mime_type": "test"})

    assert msg.aux_data is not None
    assert msg.aux_data == b"a" * 2000000

    compositor.copy("test", Selection.REGULAR, None)
    compositor.expect("test", Selection.REGULAR, {"test": "a" * 2000000})


def test_restore(compositor: Compositor, daemon_runner: Callable):
    """Test that selection is restored when it is cleared"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    # Clear clipboard
    compositor.copy("test", Selection.REGULAR, None)

    compositor.expect("test", Selection.REGULAR, None)
    compositor.expect("test", Selection.PRIMARY, {"a": "b"})

    compositor.expect("test", Selection.REGULAR, {"a": "b"})


def test_seats(compositor: Compositor, daemon_runner: Callable):
    """Test seat deletion and addition"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    compositor.add_seat("new")

    compositor.expect("new", Selection.REGULAR, {"a": "b"})

    compositor.del_seat("new")

    compositor.expect("test", Selection.REGULAR, {"a": "b"})


def test_blocked_mime_types(compositor: Compositor, daemon_runner: Callable):
    """Test that blocked mime types prevent entry from being saved"""
    compositor.add_seat("test2")

    config = """
    [daemon.mime_types]
    blocked = ['blocked']
    """

    daemon: Daemon = daemon_runner(compositor.display, config)

    compositor.copy("test2", Selection.REGULAR, {"allowed": "1", "blocked": "2"})

    daemon.add_events(["entry_add"])
    compositor.copy("test2", Selection.REGULAR, {"allowed": "1"})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_history_length"})

    assert msg.msg == {"size": 1}


def test_allowed_mime_types(compositor: Compositor, daemon_runner: Callable):
    """Test that only allowed mime types are saved"""
    compositor.add_seat("test")

    config = """ \
    [daemon.mime_types]
    allowed = ['allowed[0-9]']
    """

    daemon: Daemon = daemon_runner(compositor.display, config)

    daemon.add_events(["entry_add"])
    compositor.copy(
        "test",
        Selection.REGULAR,
        {"allowed1": "1", "not-allowed": "2", "allowed2": "3"},
    )
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_history", "start": 0, "n": 1})

    cmp_entry(msg.msg[0], 1, ["allowed1", "allowed2"], False, None, None)


def test_seat_sel_config(compositor: Compositor, daemon_runner: Callable):
    """Test that disabled seats and selections are ignored"""
    compositor.add_seat("seat0")
    compositor.add_seat("seat1")

    config = """
    [daemon]
    regular = false
    primary = true
    [daemon.seats."seat0"]
    regular = true
    # Should default to true for primary

    [daemon.seats."seat1"]
    regular = false
    primary = false

    [daemon.seats."seat2"]
    regular = true
    primary = true
    """

    daemon: Daemon = daemon_runner(compositor.display, config)

    daemon.add_events(["entry_add"])
    compositor.copy("seat0", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    compositor.expect("seat0", Selection.REGULAR, {"a": "b"})
    compositor.expect("seat0", Selection.PRIMARY, {"a": "b"})

    compositor.expect("seat1", Selection.REGULAR, None)
    compositor.expect("seat1", Selection.PRIMARY, None)

    compositor.add_seat("seat2")

    compositor.expect("seat2", Selection.REGULAR, {"a": "b"})
    compositor.expect("seat2", Selection.PRIMARY, {"a": "b"})


def test_content_type(compositor: Compositor, daemon_runner: Callable):
    """Test that content type is correctly inferred"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"text/html": "html"})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_history", "start": 0, "n": 1})
    cmp_entry(msg.msg[0], 1, ["text/html"], False, "text", "text/html")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"image/png": "image"})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_history", "start": 0, "n": 1})
    cmp_entry(msg.msg[0], 2, ["image/png"], False, "image", "image/png")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"stuff": "x"})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_history", "start": 0, "n": 1})
    cmp_entry(msg.msg[0], 3, ["stuff"], False, "binary", "stuff")


def test_no_mime_types(compositor: Compositor, daemon_runner: Callable):
    """Test that selection events with no mime types are ignored"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    compositor.copy("test", Selection.REGULAR, {})

    daemon.add_events(["entry_add"])
    compositor.copy("seat0", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    assert daemon.roundtrip({"type": "get_history_length"}).msg == {"size": 1}
