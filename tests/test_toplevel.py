import time
from typing import Callable
from conftest import Compositor
from conftest import Daemon
from conftest import Selection
from conftest import cmp_entry
from conftest import wait_cond


def test_toplevel_detection(compositor: Compositor, daemon_runner: Callable):
    """Test that active toplevel is detected and works properly"""
    compositor.add_seat("test")

    config = """
    [daemon]
    dedup = "none"

    [[daemon.toplevels]]
    titles = ["^title a", "^title b"]
    app_ids = ["^a", "^b"]
    allowed_mime_types=["^allowed"]
    """

    daemon: Daemon = daemon_runner(compositor.display, config)

    # Test that allowed mime types are from configured toplevel
    compositor.set_toplevel("win1", "title a", "a")
    compositor.activate_toplevel("win1", True)

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"allowed": "1", "not_allowed": "2"})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_history", "start": 0, "n": 1})
    cmp_entry(msg.msg[0], 1, ["allowed"], False, None, None)

    # Test no match toplevel is not affected
    compositor.set_toplevel("win2", "title c", "c")
    compositor.activate_toplevel("win2", True)

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"allowed": "1", "not_allowed": "2"})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_history", "start": 0, "n": 1})
    cmp_entry(msg.msg[0], 2, ["allowed", "not_allowed"], False, None, None)

    # Test that title and app_id both must match
    compositor.set_toplevel("win1", "title a", "not_a")
    compositor.activate_toplevel("win1", True)

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"allowed": "1", "not_allowed": "2"})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_history", "start": 0, "n": 1})
    cmp_entry(msg.msg[0], 3, ["allowed", "not_allowed"], False, None, None)
