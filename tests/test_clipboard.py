import time
import sys
from conftest import *

def test_receive(compositor, daemon, client):
    """Test if selection event is received properly"""
    dmon: Daemon = daemon(compositor, "")
    client1: Client = client(compositor, None)

    client1.copy(Selection.REGULAR, {'text/plain': 'hi'})

    assert client1.paste(Selection.REGULAR) == {'text/plain': 'hi'}

    resp: IpcMessage = dmon.roundtrip({
        'type': 'get-entries', 'start': 0, 'n': 1
        })

    cmp_entry(resp.msg[0], 1, ['text/plain'], False)

    resp = dmon.roundtrip({
        'type': 'get-data', 'id': 1, 'mime-type': 'text/plain'
        })

    assert resp.aux_data is not None
    assert resp.aux_data.decode('utf-8') == 'hi'

# Test that multiple clients are synced to the clipboard
def test_sync(compositor, daemon, client):
    dmon: Daemon = daemon(compositor, "")
    client1a: Client = client(compositor, None)
    client1b: Client = client(compositor, client1a.seat_name)
    client2a: Client = client(compositor, None)

    client1a.copy(Selection.REGULAR, {'1': 'one', '2': 'two'})

    assert client1b.paste(Selection.PRIMARY) == {'1': 'one', '2': 'two'}
