"""Tests for tools/host_ctl.py, run against a fake host (no Live, no plugin)."""
import json
import os
import socketserver
import subprocess
import sys
import threading
from pathlib import Path

import pytest

HOST_CTL = Path(__file__).resolve().parent.parent / "tools" / "host_ctl.py"
DEAD_PID = 99999999


class FakeHost:
    """Line-delimited JSON server that records requests and replies {"ok": true}."""

    def __init__(self, reply=None):
        self.requests = []
        self.reply = reply or {"ok": True}
        fake = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                for line in self.rfile:
                    fake.requests.append(json.loads(line))
                    self.wfile.write((json.dumps(fake.reply) + "\n").encode())

        self.server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


@pytest.fixture
def registry(tmp_path):
    return tmp_path


@pytest.fixture
def fake():
    host = FakeHost()
    yield host
    host.close()


def register(registry, name, port, tag, track="Track", plugin="", pid=None):
    entry = {"pid": pid or os.getpid(), "port": port, "tag": tag, "track": track, "plugin": plugin}
    (registry / f"{name}.json").write_text(json.dumps(entry))
    return entry


def host_ctl(registry, *args):
    env = dict(os.environ, CLAUDE_HOST_REGISTRY_DIR=str(registry))
    return subprocess.run([sys.executable, str(HOST_CTL), *args], env=env,
                          capture_output=True, text=True, timeout=30)


# --- instance discovery -------------------------------------------------------

def test_instances_lists_live_entries_and_prunes_dead_ones(registry):
    register(registry, "live", 1111, 1, track="Bass")
    register(registry, "dead", 2222, 2, pid=DEAD_PID)

    result = host_ctl(registry, "instances")
    assert result.returncode == 0
    listed = json.loads(result.stdout)
    assert [i["tag"] for i in listed] == [1]
    assert not (registry / "dead.json").exists()


def test_instances_skips_unreadable_files(registry):
    (registry / "broken.json").write_text("{ nope")
    register(registry, "ok", 1111, 7)
    assert [i["tag"] for i in json.loads(host_ctl(registry, "instances").stdout)] == [7]


def test_no_instances_gives_a_helpful_error(registry):
    result = host_ctl(registry, "info")
    assert result.returncode != 0
    assert "No matching Claude Host instance" in result.stderr


def test_single_instance_is_picked_automatically(registry, fake):
    register(registry, "a", fake.port, 5)
    assert host_ctl(registry, "info").returncode == 0
    assert fake.requests == [{"cmd": "info"}]


def test_several_instances_require_a_choice(registry, fake):
    register(registry, "a", fake.port, 5, track="Bass")
    register(registry, "b", fake.port, 6, track="Keys")
    result = host_ctl(registry, "info")
    assert result.returncode != 0
    assert "tag 5" in result.stderr and "tag 6" in result.stderr


def test_tag_and_track_select_an_instance(registry):
    bass, keys = FakeHost(), FakeHost()
    try:
        register(registry, "a", bass.port, 5, track="Claude Host FX/Bass")
        register(registry, "b", keys.port, 6, track="Claude Host FX/Keys")

        assert host_ctl(registry, "--tag", "6", "info").returncode == 0
        assert host_ctl(registry, "--track", "bass", "info").returncode == 0
        assert len(keys.requests) == 1 and len(bass.requests) == 1
    finally:
        bass.close()
        keys.close()


# --- command translation ------------------------------------------------------

@pytest.mark.parametrize("args, expected", [
    (["info"], {"cmd": "info"}),
    (["plugins"], {"cmd": "list_plugins", "query": ""}),
    (["plugins", "pro", "q"], {"cmd": "list_plugins", "query": "pro q"}),
    (["load", "FF", "Pro-Q", "2"], {"cmd": "load", "name": "FF Pro-Q 2"}),
    (["load", "Serum", "--format", "VST3"], {"cmd": "load", "name": "Serum", "format": "VST3"}),
    (["unload"], {"cmd": "unload"}),
    (["params", "Band 1"], {"cmd": "params", "filter": "Band 1"}),
    (["get", "Band 1 Gain"], {"cmd": "get", "param": "Band 1 Gain"}),
    (["get", "12"], {"cmd": "get", "param": 12}),
    (["set", "Band 1 Gain", "0.25"], {"cmd": "set", "param": "Band 1 Gain", "value": 0.25}),
    (["set", "Band 1 Frequency", "--text", "250 Hz"], {"cmd": "set", "param": "Band 1 Frequency", "text": "250 Hz"}),
    (["programs"], {"cmd": "programs"}),
    (["program", "3"], {"cmd": "set_program", "index": 3}),
    (["raw", '{"cmd": "info", "x": 1}'], {"cmd": "info", "x": 1}),
])
def test_commands_send_the_right_request(registry, fake, args, expected):
    register(registry, "a", fake.port, 5)
    result = host_ctl(registry, *args)
    assert result.returncode == 0, result.stderr
    assert fake.requests == [expected]


@pytest.mark.parametrize("args", [["set", "Gain"], ["get"], ["bogus"]])
def test_incomplete_commands_fail_without_contacting_the_host(registry, fake, args):
    register(registry, "a", fake.port, 5)
    assert host_ctl(registry, *args).returncode != 0
    assert fake.requests == []


def test_host_errors_give_a_nonzero_exit(registry):
    failing = FakeHost(reply={"ok": False, "error": "no parameter named \"x\""})
    try:
        register(registry, "a", failing.port, 5)
        result = host_ctl(registry, "get", "x")
        assert result.returncode == 1
        assert json.loads(result.stdout)["error"] == 'no parameter named "x"'
    finally:
        failing.close()
