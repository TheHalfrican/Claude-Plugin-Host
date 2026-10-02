"""Minimal stand-ins for Live's API objects, shaped like the real ones (same
attribute and method names), for testing bridge commands outside Live."""
import math


class Param(object):
    def __init__(self, name, lo, hi, value, display=None, quantized=False, items=None):
        self.name, self.min, self.max, self.value = name, lo, hi, value
        self.is_quantized = quantized
        self._items = items or []
        self._display = display or (lambda v: "%.2f" % v)

    @property
    def value_items(self):
        # Like Live: asking a continuous parameter for its choices raises.
        if not self.is_quantized:
            raise RuntimeError("Only quantized parameters have value items")
        return self._items

    def str_for_value(self, v):
        return self._display(v)


def volume_display(v):
    # Live-like: 0.85 is 0 dB, 0 is -inf.
    if v <= 0:
        return "-inf dB"
    return "%.1f dB" % (40.0 * math.log10(v / 0.85))


def pan_display(v):
    if abs(v) < 1e-6:
        return "C"
    return "%d%s" % (round(abs(v) * 50), "L" if v < 0 else "R")


class Mixer(object):
    def __init__(self, num_sends):
        self.volume = Param("Track Volume", 0.0, 1.0, 0.85, volume_display)
        self.panning = Param("Track Panning", -1.0, 1.0, 0.0, pan_display)
        self.sends = [Param("Send " + chr(65 + i), 0.0, 1.0, 0.0, volume_display) for i in range(num_sends)]


class Routing(object):
    def __init__(self, name):
        self.display_name = name


class DeviceIO(object):
    def __init__(self, sources, taps):
        self.available_routing_types = [Routing(s) for s in sources]
        self.available_routing_channels = [Routing(t) for t in taps]
        self.routing_type = self.available_routing_types[0]
        self.routing_channel = self.available_routing_channels[0]


class Device(object):
    def __init__(self, name, params=(), class_name="PluginDevice", sidechain=None):
        self._name, self.class_name = name, class_name
        self.class_display_name = name  # the device type; stays put when renamed
        self.parameters = list(params)
        self.input_routings = [sidechain] if sidechain else []

    # Like Live: Live's own devices can be renamed by a script; plugin
    # devices silently keep their plugin's name.
    @property
    def name(self):
        return self._name

    @name.setter
    def name(self, value):
        if self.class_name not in ("PluginDevice", "AuPluginDevice"):
            self._name = value


class Envelope(object):
    def __init__(self):
        self.events = []  # (time, duration, value)

    length = 16.0  # like Live, nothing at or past the clip's end is kept

    def insert_step(self, time, duration, value):
        if time >= self.length - 1e-9:
            return
        self.events = [e for e in self.events if not (time <= e[0] < time + duration)]
        self.events.append((time, duration, value))
        self.events.sort()

    def delete_events_in_range(self, start, end):
        self.events = [e for e in self.events if not (start <= e[0] < end)]

    def value_at_time(self, t):
        current = None
        for time, _, value in self.events:
            if time <= t + 1e-9:
                current = value
        return current if current is not None else 0.0


class Clip(object):
    def __init__(self, length=16.0):
        self.length = length
        self.envelopes = {}

    def automation_envelope(self, param):
        return self.envelopes.get(id(param))

    def create_automation_envelope(self, param):
        return self.envelopes.setdefault(id(param), Envelope())

    def clear_envelope(self, param):
        self.envelopes.pop(id(param), None)


class ClipSlot(object):
    def __init__(self, clip=None):
        self.clip = clip

    @property
    def has_clip(self):
        return self.clip is not None


class Track(object):
    def __init__(self, name, num_sends=2, devices=(), midi=True, clips=None, routing_types=None):
        self.name = name
        self.mute = self.solo = self.arm = False
        self.can_be_armed = True
        self.devices = list(devices)
        self.mixer_device = Mixer(num_sends)
        self.clip_slots = [ClipSlot(c) for c in (clips or [None] * 8)]
        self.arrangement_clips = []
        self.available_input_routing_types = [Routing(x) for x in (routing_types or ["All Ins", "Ext. In"])]
        self.available_output_routing_types = [Routing(x) for x in ["Main", "Sends Only", "A-Reverb"]]
        self.available_input_routing_channels = [Routing("All Channels")]
        self.input_routing_type = self.available_input_routing_types[0]
        self._output_type = self.available_output_routing_types[0]
        self.input_routing_channel = self.available_input_routing_channels[0]
        self.output_routing_channel = Routing("")
        self.song = None

    # Like Live: the output channels depend on the output type. A track as
    # the target offers its devices' sidechain inputs.
    @property
    def output_routing_type(self):
        return self._output_type

    @output_routing_type.setter
    def output_routing_type(self, routing):
        self._output_type = routing
        self.output_routing_channel = self.available_output_routing_channels[0]

    @property
    def available_output_routing_channels(self):
        target = next((t for t in (self.song.tracks if self.song else []) if t.name == self._output_type.display_name), None)
        if target is None:
            return [Routing("")]
        return [Routing("Sidechain-" + d.name) for d in target.devices if d.input_routings] or [Routing("Track In")]

    def delete_device(self, index):
        del self.devices[index]


class Song(object):
    def __init__(self, tracks, returns, master):
        self.tracks, self.return_tracks, self.master_track = tracks, returns, master
        for t in tracks:
            t.song = self
            t.available_output_routing_types = [Routing(x) for x in ["Main", "Sends Only"]] + [Routing(o.name) for o in tracks if o is not t]

    def duplicate_track(self, index):
        original = self.tracks[index]
        copy = Track(original.name, devices=list(original.devices))
        self.tracks.insert(index + 1, copy)

    def delete_track(self, index):
        del self.tracks[index]

    def move_device(self, device, target, position):
        # Like Live: only tracks and chains are valid targets.
        if not hasattr(target, "devices") or isinstance(target, Device):
            raise RuntimeError("No valid target track or chain.")
        for t in self.tracks + self.return_tracks + [self.master_track]:
            if device in t.devices:
                t.devices.remove(device)
        target.devices.insert(min(position, len(target.devices)), device)

    def delete_return_track(self, index):
        del self.return_tracks[index]


class App(object):
    def get_major_version(self): return 12
    def get_minor_version(self): return 2
    def get_bugfix_version(self): return 7


def make_set():
    """A small set like the user's test set."""
    pro_c = Device("Claude Host FX", [Param("Instance Tag", 0.0, 1.0, 0.1)],
                   sidechain=DeviceIO(["No Input", "Drums", "Kick (SC)", "Keys"], ["Pre FX", "Post FX", "Post Mixer"]))
    on_off = Param("Device On", 0.0, 1.0, 1.0, lambda v: "On" if v >= 0.5 else "Off", quantized=True, items=["Off", "On"])
    cutoff = Param("Frequency", 0.0, 1.0, 0.5, lambda v: "%.0f Hz" % (20 * (1000 ** v)))
    auto_filter = Device("Auto Filter", [on_off, cutoff], class_name="AutoFilter2")

    tracks = [
        Track("Bass (Serum)"),
        Track("Bass (Host)", devices=[Device("Claude Host Instrument"), pro_c]),
        Track("Keys", devices=[Device("Electric"), auto_filter], clips=[Clip()] + [None] * 7),
        Track("Drums", devices=[Device("Boom Bap Kit")]),
        Track("Kick (SC)", devices=[Device("Boom Bap Kit")]),
    ]
    returns = [Track("A-Reverb"), Track("B-Delay")]
    master = Track("Main", num_sends=0)
    return Song(tracks, returns, master)
