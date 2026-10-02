"""The Live side: a Control Surface that serves commands on 127.0.0.1:9879.

Live's API may only be used on Live's main thread, so each request is handed
to it with schedule_message and the server thread waits for the reply (the
same approach AbletonMCP uses).
"""
import importlib
import queue

from _Framework.ControlSurface import ControlSurface

from . import commands as commands_module
from .server import LineJsonServer

PORT = 9879
REPLY_TIMEOUT_SECONDS = 15.0
TICKS_BETWEEN_HOUSEKEEPING = 20   # ~2 s: rename hosts after their plugin


class ClaudeBridge(ControlSurface):
    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        self._commands = commands_module.Commands(lambda: self.song(), self.application())
        self._server = LineJsonServer(("127.0.0.1", PORT), self._handle, log=self.log_message)
        try:
            self._server.start()
            self.log_message("ClaudeBridge listening on 127.0.0.1:%d" % PORT)
            self.show_message("Claude Bridge ready")
        except OSError as e:
            self.log_message("ClaudeBridge couldn't listen on port %d: %s" % (PORT, e))
            self.show_message("Claude Bridge: port %d is busy" % PORT)

    def _handle(self, request):
        """Server thread: run the command on Live's main thread and wait."""
        replies = queue.Queue()

        def task():
            try:
                if request.get("cmd") == "reload":
                    replies.put(self._reload())
                    return
                replies.put(self._commands.run(request))
            except Exception as e:
                self.log_message("ClaudeBridge error: %r" % (e,))
                replies.put({"ok": False, "error": "internal error: %s" % (e,)})

        try:
            self.schedule_message(0, task)
        except AssertionError:
            task()  # already on the main thread

        try:
            return replies.get(timeout=REPLY_TIMEOUT_SECONDS)
        except queue.Empty:
            return {"ok": False, "error": "timed out waiting for Live's main thread"}

    def update_display(self):
        """Live calls this about ten times a second on its main thread."""
        ControlSurface.update_display(self)
        self._ticks = getattr(self, "_ticks", 0) + 1
        if self._ticks % TICKS_BETWEEN_HOUSEKEEPING == 0:
            tick = getattr(self._commands, "background_tick", None)
            if tick is not None:
                tick()

    def _reload(self):
        """Loads updated command code from disk without restarting Live
        (after tools/install_bridge.sh). The server and this class stay."""
        global commands_module
        commands_module = importlib.reload(commands_module)
        self._commands = commands_module.Commands(lambda: self.song(), self.application())
        self.log_message("ClaudeBridge reloaded commands %s" % commands_module.VERSION)
        return {"ok": True, "reloaded": commands_module.VERSION}

    def disconnect(self):
        self._server.stop()
        ControlSurface.disconnect(self)
