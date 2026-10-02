"""tools/bridge_ctl.py against a fake bridge."""
import json
import os
import socketserver
import subprocess
import sys
import threading
from pathlib import Path

import pytest

BRIDGE_CTL = Path(__file__).resolve().parents[2] / "tools" / "bridge_ctl.py"


@pytest.fixture
def fake():
    requests = []

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            for line in self.rfile:
                requests.append(json.loads(line))
                self.wfile.write(b'{"ok": true}\n')

    server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    server.requests = requests
    yield server
    server.shutdown()
    server.server_close()


def ctl(fake, *args):
    env = dict(os.environ, CLAUDE_BRIDGE_PORT=str(fake.server_address[1]))
    return subprocess.run([sys.executable, str(BRIDGE_CTL), *args], env=env, capture_output=True, text=True, timeout=30)


@pytest.mark.parametrize("args, expected", [
    (["tracks"], {"cmd": "tracks"}),
    (["track", "Keys"], {"cmd": "track", "track": "Keys"}),
    (["track", "2"], {"cmd": "track", "track": 2}),
    (["set-track", "Kick (SC)", "mute=on", "volume=-6 dB", "pan=25L", "send A=-12 dB"],
     {"cmd": "set_track", "track": "Kick (SC)", "mute": True, "volume": "-6 dB", "pan": "25L", "sends": {"A": "-12 dB"}}),
    (["set-routing", "Kick (SC)", "output", "Sends Only"],
     {"cmd": "set_routing", "track": "Kick (SC)", "direction": "output", "type": "Sends Only"}),
    (["sidechain", "Bass (Host)", "Claude Host FX", "Kick (SC)", "Post FX"],
     {"cmd": "set_sidechain", "track": "Bass (Host)", "device": "Claude Host FX", "source": "Kick (SC)", "tap": "Post FX"}),
    (["set-param", "Keys", "Auto Filter", "Frequency", "0.5"],
     {"cmd": "set_param", "track": "Keys", "device": "Auto Filter", "param": "Frequency", "value": 0.5}),
    (["set-param", "Keys", "Auto Filter", "Frequency", "1 kHz"],
     {"cmd": "set_param", "track": "Keys", "device": "Auto Filter", "param": "Frequency", "text": "1 kHz"}),
    (["set-param", "Keys", "Auto Filter", "Frequency", "500", "--text"],
     {"cmd": "set_param", "track": "Keys", "device": "Auto Filter", "param": "Frequency", "text": "500"}),
    (["delete-device", "Keys", "1"], {"cmd": "delete_device", "track": "Keys", "device": 1}),
    (["envelope", "Keys", "mixer", "volume", "--clip", "0", "0=-24 dB", "16=0 dB", "--res", "0.5"],
     {"cmd": "envelope_set", "track": "Keys", "device": "mixer", "param": "volume", "clip": 0,
      "resolution": 0.5, "points": [[0.0, "-24 dB"], [16.0, "0 dB"]]}),
    (["envelope-clear", "Keys", "mixer", "pan", "--arrangement-clip", "2"],
     {"cmd": "envelope_clear", "track": "Keys", "device": "mixer", "param": "pan", "arrangement_clip": 2}),
])
def test_commands_send_the_right_request(fake, args, expected):
    r = ctl(fake, *args)
    assert r.returncode == 0, r.stderr
    assert fake.requests == [expected]


@pytest.mark.parametrize("args", [
    ["set-track", "Keys", "loud=yes"], ["set-track", "Keys", "mute=maybe"], ["sidechain", "Bass"],
    ["envelope", "Keys", "mixer", "volume", "0=-6 dB"], ["envelope", "Keys", "mixer", "volume", "--clip", "0", "bad"],
    ["bogus"],
])
def test_bad_commands_fail_without_contacting_live(fake, args):
    assert ctl(fake, *args).returncode != 0
    assert fake.requests == []


def test_no_bridge_gives_a_helpful_error():
    env = dict(os.environ, CLAUDE_BRIDGE_PORT="1")
    r = subprocess.run([sys.executable, str(BRIDGE_CTL), "tracks"], env=env, capture_output=True, text=True, timeout=30)
    assert r.returncode != 0 and "Control Surface" in r.stderr
