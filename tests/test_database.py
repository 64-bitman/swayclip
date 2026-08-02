import time
from typing import Callable
from conftest import Compositor
from conftest import Daemon
from conftest import Selection
from conftest import cmp_entry
from conftest import wait_cond


def test_set_clipboard(compositor: Compositor, daemon_runner: Callable):
    """Test that clipboard state can be modified"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    for n in range(1, 11):
        daemon.add_events(["entry_add"])
        compositor.copy("test", Selection.REGULAR, {"a": str(n)})
        daemon.recv_event("entry_add")

    # "update_time" should be updated
    msg = daemon.roundtrip({"type": "get_entries", "start": 5, "n": 1})
    update_time = msg.msg[0]["update_time"]

    daemon.add_events(["entry_state"])
    msg = daemon.roundtrip({"type": "set_clipboard", "id": 5})
    # Previous set entry should have "entry_state" event too. Its sent before
    # the success response, confusing but oh well.
    assert msg.msg["id"] == 10
    assert msg.msg["state"] == False
    pmsg = daemon.recv_event("entry_state")
    assert pmsg.msg["id"] == 5
    assert pmsg.msg["state"] == True
    msg = daemon.recv_msg()
    assert msg.msg == {"type": "success"}
    compositor.expect("test", Selection.REGULAR, {"a": "5"})

    msg = daemon.roundtrip({"type": "get_entries", "start": 5, "n": 1})
    assert update_time < msg.msg[0]["update_time"]

    msg = daemon.roundtrip({"type": "set_clipboard", "id": 10})
    assert msg.msg == {"type": "success"}
    compositor.expect("test", Selection.REGULAR, {"a": "10"})

    # Clear clipboard
    msg = daemon.roundtrip({"type": "set_clipboard", "id": -1})
    assert msg.msg == {"type": "success"}
    compositor.expect("test", Selection.REGULAR, None)

    msg = daemon.roundtrip({"type": "set_clipboard", "id": 1})
    assert msg.msg == {"type": "success"}
    compositor.expect("test", Selection.REGULAR, {"a": "1"})


def test_dedup(compositor: Compositor, daemon_runner: Callable):
    """Test that dedup config option works properly"""
    compositor.add_seat("test")

    config = """
    [daemon]
    dedup = "none"
    set_on_startup = false # Needed to prevent interference from daemon
    """

    daemon: Daemon = daemon_runner(compositor.display, config)

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")
    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    assert daemon.roundtrip({"type": "get_history_length"}).msg == {"size": 2}

    daemon.close()

    config = """
    [daemon]
    dedup = "prev"
    set_on_startup = false
    """

    daemon = daemon_runner(compositor.display, config)

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"x": "y"})
    daemon.recv_event("entry_add")
    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"x": "y"})
    daemon.recv_event("entry_add")

    assert daemon.roundtrip({"type": "get_history_length"}).msg == {"size": 5}

    daemon.close()

    config = """
    [daemon]
    dedup = "all"
    set_on_startup = false
    """

    daemon = daemon_runner(compositor.display, config)

    daemon.add_events(["entry_move"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_move")

    assert daemon.roundtrip({"type": "get_history_length"}).msg == {"size": 5}


def test_move(compositor: Compositor, daemon_runner: Callable):
    """
    Test that "dedup" option set to "all" results in new entries that are same
    content wise to any previous entry in clipboard history, are discarded and
    instead the existing entry is used/moved to the front
    """
    compositor.add_seat("test")

    config = """
    [daemon]
    dedup = "all"
    """

    daemon: Daemon = daemon_runner(compositor.display, config)

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    # "update_time" should be updated
    msg = daemon.roundtrip({"type": "get_entries", "start": 0, "n": 1})
    update_time = msg.msg[0]["update_time"]

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"x": "y"})
    daemon.recv_event("entry_add")

    daemon.add_events(["entry_move", "entry_update"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    assert daemon.recv_msg().msg["event"] == "entry_update"
    assert daemon.recv_msg().msg["event"] == "entry_move"
    daemon.remove_events(["entry_move", "entry_update"])

    msg = daemon.roundtrip({"type": "get_entries", "start": 0, "n": 1})
    assert update_time < msg.msg[0]["update_time"]

    msg = daemon.roundtrip({"type": "get_history_length"})
    assert msg.msg == {"size": 2}

    daemon.add_events(["entry_move"])
    compositor.copy("test", Selection.REGULAR, {"x": "y"})
    daemon.recv_event("entry_move")

    assert daemon.roundtrip({"type": "get_history_length"}).msg == {"size": 2}


def test_trim(compositor: Compositor, daemon_runner: Callable):
    """
    Test that entries are trimed according to "max_entries" option. Pinned
    entries are not taken into account
    """
    compositor.add_seat("test")

    config = """
    [daemon]
    max_entries = 2
    """

    daemon: Daemon = daemon_runner(compositor.display, config)

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    assert daemon.roundtrip({"type": "pin_entry", "id": 1, "pin": "yes"}).msg == {
        "type": "success"
    }

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"x": "y"})
    daemon.recv_event("entry_add")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"f": "g"})
    daemon.recv_event("entry_add")

    assert daemon.roundtrip({"type": "get_history_length"}).msg == {"size": 3}

    msg = daemon.roundtrip({"type": "get_entries", "start": 0, "n": 3})
    cmp_entry(msg.msg[2], 1, ["a"], True, None, None)
    cmp_entry(msg.msg[1], 2, ["x"], False, None, None)
    cmp_entry(msg.msg[0], 3, ["f"], False, None, None)

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"m": "n"})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({"type": "get_entries", "start": 0, "n": 3})
    cmp_entry(msg.msg[2], 1, ["a"], True, None, None)
    cmp_entry(msg.msg[1], 3, ["f"], False, None, None)
    cmp_entry(msg.msg[0], 4, ["m"], False, None, None)


def test_last_entry(compositor: Compositor, daemon_runner: Callable):
    """Test that last entry is restored on startup"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    daemon.close()

    daemon_runner(compositor.display, "")

    compositor.expect("test", Selection.REGULAR, {"a": "b"})


def test_pin_toggle(compositor: Compositor, daemon_runner: Callable):
    """Test that pin status can be toggled"""
    compositor.add_seat("test")

    daemon: Daemon = daemon_runner(compositor.display, "")

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {"a": "b"})
    daemon.recv_event("entry_add")

    assert daemon.roundtrip({"type": "pin_entry", "id": 1, "pin": "toggle"}).msg == {
        "type": "success"
    }

    msg = daemon.roundtrip({"type": "get_entries", "start": 0, "n": 1})
    assert msg.msg[0]["pinned"] == True

    assert daemon.roundtrip({"type": "pin_entry", "id": 1, "pin": "toggle"}).msg == {
        "type": "success"
    }

    msg = daemon.roundtrip({"type": "get_entries", "start": 0, "n": 1})
    assert msg.msg[0]["pinned"] == False
