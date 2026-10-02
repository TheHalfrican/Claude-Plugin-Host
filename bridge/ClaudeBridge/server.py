"""Line-delimited JSON over TCP, loopback only.

One request object per line in, one reply object per line out. Importable
outside Live, so it can be tested with a plain Python interpreter.
"""
import json
import socket
import threading

MAX_LINE_BYTES = 4 * 1024 * 1024


def _no_sigpipe(sock):
    # Writing to a socket the client closed raises SIGPIPE, which would kill
    # the whole process: inside Live, that's Live. macOS lets us opt out per
    # socket (the constant is 0x1022 when Python doesn't define it).
    opt = getattr(socket, "SO_NOSIGPIPE", 0x1022)
    try:
        sock.setsockopt(socket.SOL_SOCKET, opt, 1)
    except OSError:
        pass


def _error(message):
    return {"ok": False, "error": message}


class LineJsonServer(object):
    def __init__(self, address, handler, log=None):
        """handler(request_dict) -> reply_dict, called on a server thread."""
        self._address = address
        self._handler = handler
        self._log = log or (lambda message: None)
        self._listener = None
        self._threads = []
        self._running = False
        self.port = 0

    def start(self):
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(self._address)
        self._listener.listen(4)
        self._listener.settimeout(0.5)  # so stop() is noticed
        self.port = self._listener.getsockname()[1]
        self._running = True
        accept = threading.Thread(target=self._accept_loop, name="ClaudeBridge accept")
        accept.daemon = True
        accept.start()
        self._threads.append(accept)
        return self.port

    def stop(self):
        self._running = False
        try:
            self._listener.close()
        except Exception:
            pass
        for t in self._threads:
            t.join(2.0)

    def _accept_loop(self):
        while self._running:
            try:
                client, _ = self._listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            _no_sigpipe(client)
            t = threading.Thread(target=self._serve, args=(client,), name="ClaudeBridge client")
            t.daemon = True
            t.start()
            self._threads = [x for x in self._threads if x.is_alive()] + [t]

    def _serve(self, client):
        client.settimeout(0.5)
        pending = b""
        try:
            while self._running:
                try:
                    chunk = client.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    return
                pending += chunk
                if len(pending) > MAX_LINE_BYTES and b"\n" not in pending:
                    return  # a broken client; drop it
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    reply = self._reply_to(line)
                    client.sendall((json.dumps(reply) + "\n").encode("utf-8"))
        except OSError:
            return
        finally:
            try:
                client.close()
            except OSError:
                pass

    def _reply_to(self, line):
        try:
            request = json.loads(line.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return _error("request must be one JSON object per line")
        if not isinstance(request, dict):
            return _error("request must be one JSON object per line")
        try:
            return self._handler(request)
        except Exception as e:  # never let one request kill the connection
            self._log("ClaudeBridge handler error: %r" % (e,))
            return _error("internal error: %s" % (e,))
