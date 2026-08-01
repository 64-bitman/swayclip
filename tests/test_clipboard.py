import time
from typing import Callable
from conftest import Compositor
from conftest import Daemon
from conftest import Selection
from conftest import cmp_entry
from conftest import wait_cond


def test_receive(compositor: Compositor, daemon_runner: Callable):
    """Test that contents of selection event is correctly received"""
    compositor.add_seat('test')
    daemon: Daemon = daemon_runner(compositor.display, '')

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {'text/plain': 'hi', 'a': 'b'})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({'type': 'get_entries', 'start': 0, 'n': 1})

    cmp_entry(msg.msg[0], 1, ['text/plain', 'a'], False, 'text', 'text/plain')

    msg = daemon.roundtrip({'type': 'get_data', 'id': 1, 'mime_type': \
            'text/plain'})

    assert msg.aux_data is not None
    assert msg.aux_data.decode('utf-8') == 'hi'

    msg = daemon.roundtrip({'type': 'get_data', 'id': 1, 'mime_type': 'a'})

    assert msg.aux_data is not None
    assert msg.aux_data.decode('utf-8') == 'b'


def test_sync(compositor: Compositor, daemon_runner: Callable):
    """Test that multiple seats are synced to the clipboard"""
    compositor.add_seat('test1')
    compositor.add_seat('test2')

    daemon: Daemon = daemon_runner(compositor.display, '')

    daemon.add_events(["entry_add"])
    compositor.copy("test1", Selection.REGULAR, {'text/plain': 'hi'})
    daemon.recv_event("entry_add")

    assert compositor.paste("test1", Selection.PRIMARY) \
            == {'text/plain': 'hi'}
    assert compositor.paste("test2", Selection.REGULAR) \
            == {'text/plain': 'hi'}
    assert compositor.paste("test2", Selection.PRIMARY) \
            == {'text/plain': 'hi'}


def test_receive_existing(compositor: Compositor, daemon_runner: Callable):
    """
    Test if initial selection event is received by daemon (that does not set the
    selection itself because database is empty)
    """
    compositor.add_seat('test')
    compositor.copy("test", Selection.REGULAR, {'text/plain': 'hi'})

    daemon: Daemon = daemon_runner(compositor.display, '')

    # Wayland events may be received after daemon request is sent and processed,
    # so must check multiple times.
    def func():
        msg = daemon.roundtrip({'type': 'get_entries', 'start': 0, 'n': 1})
        cmp_entry(msg.msg[0], 1, ['text/plain'], False, 'text', 'text/plain')
    wait_cond(func)

def test_receive_massive(compositor: Compositor, daemon_runner: Callable):
    """Test that receiving a lot of data works properly"""
    compositor.add_seat('test')

    daemon: Daemon = daemon_runner(compositor.display, '')

    # See testserver.c for "massive" special case.
    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {'test': 'massive'})
    daemon.recv_event("entry_add")

    msg = daemon.roundtrip({'type': 'get_data', 'id': 1, 'mime_type': 'test'})

    assert msg.aux_data is not None
    assert msg.aux_data == b'a' * 2000000


def test_restore(compositor: Compositor, daemon_runner: Callable):
    """Test that selection is restored when it is cleared"""
    compositor.add_seat('test')

    daemon: Daemon = daemon_runner(compositor.display, '')

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {'a': 'b'})
    daemon.recv_event("entry_add")

    # Clear clipboard
    compositor.copy("test", Selection.REGULAR, None)

    assert compositor.paste("test", Selection.REGULAR) == None
    assert compositor.paste("test", Selection.PRIMARY) == {'a': 'b'}

    # Not sure if there is a better way...
    def func():
        assert compositor.paste("test", Selection.REGULAR) == {'a': 'b'}
    wait_cond(func)


def test_seats(compositor: Compositor, daemon_runner: Callable):
    """Test seat deletion and addition"""
    compositor.add_seat('test')

    daemon: Daemon = daemon_runner(compositor.display, '')

    daemon.add_events(["entry_add"])
    compositor.copy("test", Selection.REGULAR, {'a': 'b'})
    daemon.recv_event("entry_add")

    compositor.add_seat('new')

    def func():
        assert compositor.paste('new', Selection.REGULAR) == {'a': 'b'}
    wait_cond(func)

    compositor.del_seat('new')

    def func():
        assert compositor.paste('test', Selection.REGULAR) == {'a': 'b'}
    wait_cond(func)


