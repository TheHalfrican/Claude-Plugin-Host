"""The bridge's socket server, with a plain handler (no Live needed)."""
import json
import socket
import time

import pytest

from ClaudeBridge.server import LineJsonServer


@pytest.fixture
def server():
    calls = []

    def handler(request):
        calls.append(request)
        if request.get("cmd") == "boom":
            raise RuntimeError("handler blew up")
        return {"ok": True, "echo": request}

    s = LineJsonServer(("127.0.0.1", 0), handler)
    s.start()
    s.calls = calls
    yield s
    s.stop()


class Conn(object):
    def __init__(self, server):
        self.sock = socket.create_connection(("127.0.0.1", server.port), timeout=5)
        self.reader = self.sock.makefile("rb")

    def sendall(self, data):
        self.sock.sendall(data)


def connect(server):
    return Conn(server)


def ask(c, raw):
    c.sendall(raw if isinstance(raw, bytes) else raw.encode())
    return json.loads(c.reader.readline())


def test_one_request_per_line(server):
    c = connect(server)
    assert ask(c, '{"cmd":"info"}\n')["echo"] == {"cmd": "info"}


def test_several_requests_in_one_write_answer_in_order(server):
    c = connect(server)
    c.sendall(b'{"cmd":"a"}\n{"cmd":"b"}\n')
    assert json.loads(c.reader.readline())["echo"]["cmd"] == "a"
    assert json.loads(c.reader.readline())["echo"]["cmd"] == "b"


def test_a_request_split_across_writes(server):
    c = connect(server)
    c.sendall(b'{"cmd":')
    time.sleep(0.05)
    assert ask(c, b'"split"}\n')["echo"]["cmd"] == "split"


def test_bad_input_is_refused_and_the_connection_survives(server):
    c = connect(server)
    assert ask(c, "not json\n")["ok"] is False
    assert ask(c, "[1, 2]\n")["ok"] is False
    assert ask(c, '{"cmd":"boom"}\n')["error"].startswith("internal error")
    assert ask(c, '{"cmd":"after"}\n')["ok"] is True


def test_blank_lines_are_ignored(server):
    c = connect(server)
    assert ask(c, '\n\n{"cmd":"x"}\n')["echo"]["cmd"] == "x"
    assert len(server.calls) == 1


def test_listens_on_loopback_only(server):
    assert server._listener.getsockname()[0] == "127.0.0.1"


def test_stop_is_prompt_with_a_client_connected(server):
    c = connect(server)
    start = time.time()
    server.stop()
    assert time.time() - start < 3
