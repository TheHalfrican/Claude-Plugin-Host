"""Commands for the Claude Bridge Remote Script.

Written against Live's API (the Live Object Model) but importing nothing from
Live, so everything here can be tested with fake objects outside Live.

Every command takes a request dict and returns a reply dict with "ok". Refs:
  track:  an index (regular tracks), or a name ("Keys", "A-Reverb", "Master");
          an exact name wins over a unique partial one.
  device: an index on that track, or a name; "mixer" means the track's mixer.
  param:  an index or a name. On "mixer": "volume", "pan", "send A" /
          "send <return name>" / "send 0".
  clip:   {"clip": session slot} or {"arrangement_clip": index}.
Values are either raw ("value", in the parameter's own range) or text ("text",
as Live displays it: "-6 dB", "25L", "C", "On", "Post FX").
"""
import glob
import json
import os
import re

VERSION = "0.1.7"


class CommandError(Exception):
    pass


# ----------------------------------------------------------------------------
# Looking things up by index or name

def _norm(s):
    return (s or "").strip().lower()


def _pick(items, key, what, name_of=lambda x: x.name):
    """items: list. key: int index or name. Exact name first, then a unique
    partial match."""
    if isinstance(key, bool) or key is None:
        raise CommandError("give the %s as a name or index" % what)
    if isinstance(key, int):
        if 0 <= key < len(items):
            return items[key]
        raise CommandError("%s index %d out of range (0..%d)" % (what, key, len(items) - 1))
    wanted = _norm(str(key))
    exact = [x for x in items if _norm(name_of(x)) == wanted]
    if len(exact) == 1:
        return exact[0]
    if len(exact) > 1:
        raise CommandError("%d %ss are named %r; use an index" % (len(exact), what, key))
    partial = [x for x in items if wanted in _norm(name_of(x))]
    if len(partial) == 1:
        return partial[0]
    if not partial:
        raise CommandError("no %s named %r (have: %s)" % (what, key, ", ".join(name_of(x) for x in items)))
    raise CommandError("%r matches several %ss: %s" % (key, what, ", ".join(name_of(x) for x in partial)))


def _display_name(x):
    name = getattr(x, "display_name", None)
    if name is None:
        return str(x)
    return name or "(default)"   # Live leaves some channels unnamed, e.g. Main's


# ----------------------------------------------------------------------------
# Text <-> parameter values, checked against Live's own display text

def parse_quantity(text):
    """'-6 dB' -> -6, '2.5 kHz' -> 2500, '250 ms' -> 250, '1.2 s' -> 1200,
    '25L' -> -25, '25R' -> 25, '4:1' -> 4. None if not a plain number."""
    t = (text or "").strip()
    m = re.match(r"^([+-]?\d+(?:\.\d+)?)\s*:\s*1$", t)
    if m:
        return float(m.group(1)), False
    m = re.match(r"^([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)\s*([A-Za-z%]*)$", t)
    if not m:
        return None
    value, unit = float(m.group(1)), m.group(2).lower()
    if unit.startswith("k"):
        value *= 1000.0
    elif unit in ("s", "sec", "secs", "second", "seconds"):
        value *= 1000.0
    elif unit == "l":
        value = -value
    return value, bool(unit)


def _choices(param):
    """A stepped parameter's choices. Live raises if you ask a continuous
    parameter for value_items, so only ask stepped ones."""
    if not getattr(param, "is_quantized", False):
        return []
    try:
        return [str(x) for x in (param.value_items or [])]
    except Exception:
        return []


def _shown(param, value):
    try:
        return str(param.str_for_value(value)).strip()
    except Exception:
        return ""


def value_for_text(param, text):
    """The raw value whose display reads `text`, else CommandError."""
    want = _norm(text)
    lo, hi = float(param.min), float(param.max)

    items = _choices(param)
    if items:
        for i, item in enumerate(items):
            if _norm(str(item)) == want:
                return lo + i
        raise CommandError("%s has no choice %r (choices: %s)" % (param.name, text, ", ".join(map(str, items))))

    def scan_for_word():
        steps = 1000
        run = []
        for i in range(steps + 1):
            v = lo + (hi - lo) * i / steps
            if _norm(_shown(param, v)) == want:
                run.append(v)
            elif run:
                break
        return run[len(run) // 2] if run else None

    wanted = parse_quantity(text)
    if wanted is None:
        v = scan_for_word()
        if v is None:
            raise CommandError("%s never shows %r" % (param.name, text))
        return v

    target, has_unit = wanted

    def shown_number(v):
        q = parse_quantity(_shown(param, v))
        return q[0] if q else None

    def shown_number_near(v, span):
        # Some positions show a word instead of a number (pan's "C"); look
        # just either side of them.
        for nudge in (0.0, 1e-3, -1e-3, 1e-2, -1e-2):
            x = min(max(v + span * nudge, float(param.min)), hi)
            n = shown_number(x)
            if n is not None:
                return n
        return None

    a_val, b_val = shown_number_near(lo, hi - lo), shown_number_near(hi, hi - lo)
    if a_val is None or b_val is None:
        # e.g. a volume showing "-inf dB" at the bottom: start where the
        # display becomes numeric.
        v = scan_for_word()
        if v is not None:
            return v
        a = lo
        for i in range(1, 1001):
            a = lo + (hi - lo) * i / 1000.0
            if shown_number(a) is not None:
                break
        a_val = shown_number(a)
        if a_val is None or b_val is None:
            raise CommandError("can't read %s's display as a number; give a raw value" % param.name)
        lo = a
    rising = b_val >= a_val
    low, high = min(a_val, b_val), max(a_val, b_val)
    if not (low - abs(low) * 1e-3 - 1e-9 <= target <= high + abs(high) * 1e-3 + 1e-9):
        v = scan_for_word()
        if v is not None:
            return v
        raise CommandError("%r is outside %s's range (%s to %s)" % (text, param.name, _shown(param, float(param.min)), _shown(param, hi)))

    span = hi - lo

    def first_where(pred):
        """First value (going up the range) where pred(display) holds."""
        a, b = lo, hi
        for _ in range(60):
            mid = (a + b) / 2.0
            n = shown_number_near(mid, span)
            if n is not None and pred(n):
                b = mid
            else:
                a = mid
        return b

    # Displays are rounded ("-6.0 dB" covers a stretch of values). Find the
    # stretch that shows the target and take its middle: the edges round to
    # the neighbouring reading.
    eps = max(abs(target) * 1e-9, 1e-12)
    if rising:
        run_start = first_where(lambda n: n >= target - eps)
        run_end = first_where(lambda n: n > target + eps)
    else:
        run_start = first_where(lambda n: n <= target + eps)
        run_end = first_where(lambda n: n < target - eps)
    middle = (run_start + run_end) / 2.0
    shown = shown_number_near(middle, span)
    if shown is not None and abs(shown - target) <= max(abs(target) * 0.01, 0.01):
        return middle
    return run_start  # the target falls between two readings: the nearest one up


def _value_from(param, request):
    if "text" in request:
        return value_for_text(param, str(request["text"]))
    if "value" in request:
        v = float(request["value"])
        if not (float(param.min) <= v <= float(param.max)):
            raise CommandError("value %g is outside %s's range %g..%g" % (v, param.name, param.min, param.max))
        return v
    raise CommandError('give "value" (raw) or "text" (as Live shows it)')


def describe_param(param, index=None):
    d = {"name": param.name, "value": param.value, "min": param.min, "max": param.max,
         "text": _shown(param, param.value), "quantized": bool(getattr(param, "is_quantized", False))}
    items = _choices(param)
    if items:
        d["choices"] = items
    if index is not None:
        d["index"] = index
    return d


# ----------------------------------------------------------------------------

class Commands(object):
    def __init__(self, get_song, application=None):
        self._song = get_song
        self._app = application

    def run(self, request):
        cmd = request.get("cmd", "")
        handler = getattr(self, "cmd_" + cmd, None) if re.match(r"^[a-z_]+$", cmd or "") else None
        if handler is None:
            return {"ok": False, "error": "unknown cmd %r (try: %s)" % (cmd, ", ".join(self.command_names()))}
        try:
            reply = handler(request) or {}
            reply["ok"] = True
            return reply
        except CommandError as e:
            return {"ok": False, "error": str(e)}

    @classmethod
    def command_names(cls):
        return sorted(n[4:] for n in dir(cls) if n.startswith("cmd_"))

    # ---- refs ---------------------------------------------------------------

    def _all_tracks(self):
        song = self._song()
        tracks = [(t, "track", i) for i, t in enumerate(song.tracks)]
        tracks += [(t, "return", i) for i, t in enumerate(song.return_tracks)]
        tracks.append((song.master_track, "master", 0))
        return tracks

    def _track(self, key):
        song = self._song()
        if isinstance(key, int) and not isinstance(key, bool):
            return _pick(list(song.tracks), key, "track"), "track", key
        if _norm(str(key)) in ("master", "main"):
            return song.master_track, "master", 0
        entries = self._all_tracks()
        t, kind, index = _pick(entries, key, "track", name_of=lambda e: e[0].name if e[1] != "master" else "Master")
        return t, kind, index

    def _device(self, track, key):
        devices = list(track.devices)
        try:
            return _pick(devices, key, "device")
        except CommandError as by_name:
            # Renamed devices can still be found by their type ("Claude Host FX").
            try:
                return _pick(devices, key, "device", name_of=lambda d: getattr(d, "class_display_name", "") or "")
            except CommandError:
                raise by_name

    def _param(self, track, device_key, param_key):
        if _norm(str(device_key)) == "mixer":
            return self._mixer_param(track, param_key)
        device = self._device(track, device_key)
        return _pick(list(device.parameters), param_key, "parameter")

    def _mixer_param(self, track, key):
        mixer = track.mixer_device
        k = _norm(str(key))
        if k == "volume":
            return mixer.volume
        if k in ("pan", "panning"):
            return mixer.panning
        if k.startswith("send"):
            sends = list(mixer.sends)
            which = k[4:].strip()
            if which.isdigit():
                return _pick(sends, int(which), "send")
            returns = list(self._song().return_tracks)
            for i, r in enumerate(returns[:len(sends)]):
                letter = chr(ord("a") + i)
                name = _norm(r.name)                     # e.g. "a-reverb"
                plain = name.split("-", 1)[1] if "-" in name else name
                if which in (letter, name, plain):
                    return sends[i]
            raise CommandError("no send %r (returns: %s)" % (which, ", ".join(r.name for r in returns)))
        raise CommandError('mixer parameters are "volume", "pan" and "send A" / "send <return name>"')

    def _clip(self, track, request):
        if "clip" in request:
            slots = list(track.clip_slots)
            slot = _pick(slots, int(request["clip"]), "clip slot")
            if not slot.has_clip:
                raise CommandError("clip slot %d on %s is empty" % (int(request["clip"]), track.name))
            return slot.clip
        if "arrangement_clip" in request:
            return _pick(list(track.arrangement_clips), int(request["arrangement_clip"]), "arrangement clip")
        raise CommandError('give "clip" (session slot) or "arrangement_clip" (index)')

    def background_tick(self):
        """Called by the Live side every couple of seconds: keeps hosts named
        after their plugin, including ones loaded by hand. Never raises."""
        try:
            self.cmd_name_hosts({})
        except Exception:
            pass

    # ---- reading ------------------------------------------------------------

    def cmd_info(self, request):
        version = None
        if self._app is not None:
            try:
                version = "%d.%d.%d" % (self._app.get_major_version(), self._app.get_minor_version(), self._app.get_bugfix_version())
            except Exception:
                pass
        song = self._song()
        return {"bridge": VERSION, "live": version, "tracks": len(song.tracks),
                "returns": len(song.return_tracks), "commands": self.command_names()}

    def _describe_track(self, t, kind, index):
        d = {"index": index, "kind": kind, "name": t.name if kind != "master" else "Master",
             "devices": [dv.name for dv in t.devices]}
        mixer = t.mixer_device
        d["volume"] = _shown(mixer.volume, mixer.volume.value)
        d["pan"] = _shown(mixer.panning, mixer.panning.value)
        if kind != "master":
            d["mute"] = bool(t.mute)
            d["solo"] = bool(t.solo)
            d["sends"] = [{"to": r.name, "text": _shown(s, s.value)} for s, r in zip(mixer.sends, self._song().return_tracks)]
        if kind == "track" and getattr(t, "can_be_armed", False):
            d["arm"] = bool(t.arm)
        for direction in ("input", "output"):
            rt = getattr(t, direction + "_routing_type", None)
            if rt is not None:
                d[direction] = _display_name(rt)
                ch = getattr(t, direction + "_routing_channel", None)
                if ch is not None:
                    d[direction + "_channel"] = _display_name(ch)
        return d

    def cmd_tracks(self, request):
        return {"tracks": [self._describe_track(*e) for e in self._all_tracks()]}

    def cmd_track(self, request):
        t, kind, index = self._track(request.get("track"))
        return {"track": self._describe_track(t, kind, index)}

    def cmd_routing(self, request):
        t, _, _ = self._track(request.get("track"))
        out = {}
        for direction in ("input", "output"):
            if getattr(t, direction + "_routing_type", None) is None:
                continue
            out[direction] = {
                "type": _display_name(getattr(t, direction + "_routing_type")),
                "channel": _display_name(getattr(t, direction + "_routing_channel")),
                "types": [_display_name(x) for x in getattr(t, "available_%s_routing_types" % direction)],
                "channels": [_display_name(x) for x in getattr(t, "available_%s_routing_channels" % direction)],
            }
        return out

    def cmd_devices(self, request):
        t, _, _ = self._track(request.get("track"))
        devices = []
        for i, dv in enumerate(t.devices):
            d = {"index": i, "name": dv.name, "class": getattr(dv, "class_name", ""),
                 "parameters": len(list(dv.parameters))}
            ios = list(getattr(dv, "input_routings", []) or [])
            if ios:
                d["sidechain"] = [{"source": _display_name(io.routing_type), "tap": _display_name(io.routing_channel)} for io in ios]
            devices.append(d)
        return {"track": t.name, "devices": devices}

    def cmd_params(self, request):
        t, _, _ = self._track(request.get("track"))
        if _norm(str(request.get("device"))) == "mixer":
            m = t.mixer_device
            params = [m.volume, m.panning] + list(m.sends)
        else:
            params = list(self._device(t, request.get("device")).parameters)
        f = _norm(request.get("filter", ""))
        return {"params": [describe_param(p, i) for i, p in enumerate(params) if f in _norm(p.name)]}

    def cmd_inspect(self, request):
        """Developer aid: the API attributes Live exposes on a track or device
        (optionally only those containing `filter`), with simple values."""
        t, _, _ = self._track(request.get("track"))
        target = self._device(t, request.get("device")) if request.get("device") is not None else t
        f = _norm(request.get("filter", ""))
        attrs = {}
        for name in dir(target):
            if name.startswith("_") or (f and f not in name.lower()):
                continue
            try:
                v = getattr(target, name)
            except Exception as e:
                attrs[name] = "<error: %s>" % e
                continue
            if callable(v):
                attrs[name] = "<method>"
            elif isinstance(v, (bool, int, float, str)) or v is None:
                attrs[name] = v
            else:
                try:
                    attrs[name] = "<%s, %d items>" % (type(v).__name__, len(v))
                except Exception:
                    attrs[name] = "<%s>" % type(v).__name__
        return {"of": getattr(target, "name", "?"), "attributes": attrs}

    # ---- changing -----------------------------------------------------------

    def cmd_set_param(self, request):
        t, _, _ = self._track(request.get("track"))
        p = self._param(t, request.get("device"), request.get("param"))
        p.value = _value_from(p, request)
        return {"param": describe_param(p)}

    def cmd_set_track(self, request):
        """mute/solo/arm (bool), volume/pan (text or raw), sends {name: text}."""
        t, kind, _ = self._track(request.get("track"))
        changed = {}
        for flag in ("mute", "solo", "arm"):
            if flag in request:
                if kind == "master" or (flag == "arm" and not getattr(t, "can_be_armed", False)):
                    raise CommandError("%s can't be %s" % (t.name, {"mute": "muted", "solo": "soloed", "arm": "armed"}[flag]))
                setattr(t, flag, bool(request[flag]))
                changed[flag] = bool(getattr(t, flag))
        for key, param_name in (("volume", "volume"), ("pan", "pan")):
            if key in request:
                p = self._mixer_param(t, param_name)
                v = request[key]
                p.value = value_for_text(p, v) if isinstance(v, str) else _value_from(p, {"value": v})
                changed[key] = _shown(p, p.value)
        for send_key, v in (request.get("sends") or {}).items():
            p = self._mixer_param(t, "send " + str(send_key))
            p.value = value_for_text(p, v) if isinstance(v, str) else _value_from(p, {"value": v})
            changed["send " + str(send_key)] = _shown(p, p.value)
        if not changed:
            raise CommandError("nothing to change: give mute, solo, arm, volume, pan or sends")
        return {"track": t.name, "changed": changed}

    def cmd_set_routing(self, request):
        """direction: "input" or "output"; type: e.g. "Sends Only", "Drums"; channel optional."""
        t, _, _ = self._track(request.get("track"))
        direction = _norm(request.get("direction"))
        if direction not in ("input", "output"):
            raise CommandError('direction must be "input" or "output"')
        types = list(getattr(t, "available_%s_routing_types" % direction))
        setattr(t, direction + "_routing_type", _pick(types, request.get("type"), "routing", _display_name))
        if request.get("channel") is not None:
            channels = list(getattr(t, "available_%s_routing_channels" % direction))
            setattr(t, direction + "_routing_channel", _pick(channels, request.get("channel"), "channel", _display_name))
        return self.cmd_routing({"track": request.get("track")})

    def cmd_set_sidechain(self, request):
        """source: the key track ("Kick (SC)"), tap: e.g. "Post FX"; io: which input (0)."""
        t, _, _ = self._track(request.get("track"))
        dv = self._device(t, request.get("device"))
        ios = list(getattr(dv, "input_routings", []) or [])
        if not ios:
            raise CommandError("%s has no sidechain input Live can route" % dv.name)
        io = _pick(ios, int(request.get("io", 0)), "sidechain input", name_of=lambda x: "")
        io.routing_type = _pick(list(io.available_routing_types), request.get("source"), "sidechain source", _display_name)
        if request.get("tap") is not None:
            io.routing_channel = _pick(list(io.available_routing_channels), request.get("tap"), "tap point", _display_name)
        return {"device": dv.name, "source": _display_name(io.routing_type), "tap": _display_name(io.routing_channel),
                "sources": [_display_name(x) for x in io.available_routing_types],
                "taps": [_display_name(x) for x in io.available_routing_channels]}

    def cmd_feed_sidechain(self, request):
        """Send a key track straight into a device's sidechain: from (the key,
        e.g. "Kick (SC)"), to (the track holding the device), device (name).

        Live's API can't set a plugin device's own sidechain dropdown, but it
        can route a track's output to "<track> / Sidechain-<device>". The key
        then goes only to that sidechain, so it's also out of the mix. One key
        track feeds one device this way; for several, use one key track each
        or set the device dropdowns by hand."""
        source, kind, _ = self._track(request.get("from"))
        if kind != "track":
            raise CommandError("only a regular track can feed a sidechain")
        target, _, _ = self._track(request.get("to"))
        types = list(source.available_output_routing_types)
        source.output_routing_type = _pick(types, target.name, "routing", _display_name)
        channels = list(source.available_output_routing_channels)
        wanted = _norm(request.get("device", ""))
        sidechains = [c for c in channels if _norm(_display_name(c)).startswith("sidechain")]
        matches = [c for c in sidechains if wanted and wanted in _norm(_display_name(c))] or (sidechains if not wanted else [])
        if len(matches) != 1:
            source.output_routing_type = _pick(types, "Main", "routing", _display_name)  # put it back
            if not sidechains:
                raise CommandError("%s has no device with a sidechain Live can feed" % target.name)
            raise CommandError("pick a device: %s" % ", ".join(_display_name(c) for c in sidechains))
        source.output_routing_channel = matches[0]
        return {"from": source.name, "to": target.name, "channel": _display_name(matches[0])}

    def cmd_rename_device(self, request):
        t, _, _ = self._track(request.get("track"))
        dv = self._device(t, request.get("device"))
        name = str(request.get("name", "")).strip()
        if not name:
            raise CommandError('give the new "name"')
        try:
            dv.name = name
        except Exception as e:
            raise CommandError("Live won't rename %s: %s" % (dv.name, e))
        return {"device": dv.name}

    def cmd_name_hosts(self, request):
        """Renames every Claude Host device after the plugin it hosts, e.g.
        "FF Pro-Q 2 (Claude Host)", using each host's Instance Tag and the
        hosts' instance registry. Empty hosts get their plain type name."""
        registry = request.get("registry") or os.path.expanduser(
            "~/Library/Application Support/ClaudePluginHost/instances")
        hosted = {}
        for path in glob.glob(os.path.join(registry, "*.json")):
            try:
                with open(path) as f:
                    entry = json.load(f)
                hosted[int(entry["tag"])] = entry.get("plugin") or ""
            except Exception:
                continue

        renamed = []
        for t, kind, _ in self._all_tracks():
            for dv in t.devices:
                kind_name = getattr(dv, "class_display_name", "") or ""
                if not kind_name.startswith("Claude Host"):
                    continue
                tags = [p for p in dv.parameters if p.name == "Instance Tag"]
                if not tags:
                    continue
                tag = int(round(float(tags[0].value) * 9999))
                plugin = hosted.get(tag)
                if plugin is None:
                    continue  # not in the registry (yet): leave it alone
                wanted = "%s (Claude Host)" % plugin if plugin else kind_name
                if dv.name != wanted:
                    try:
                        dv.name = wanted
                    except Exception as e:
                        raise CommandError("Live won't rename devices: %s" % e)
                renamed.append({"track": t.name, "tag": tag, "name": wanted})
        return {"hosts": renamed}

    def cmd_duplicate_track(self, request):
        t, kind, index = self._track(request.get("track"))
        if kind != "track":
            raise CommandError("only regular tracks can be duplicated")
        self._song().duplicate_track(index)
        return {"new_index": index + 1, "name": self._song().tracks[index + 1].name}

    def cmd_delete_track(self, request):
        t, kind, index = self._track(request.get("track"))
        if kind == "master":
            raise CommandError("the master track can't be deleted")
        name = t.name
        if kind == "return":
            self._song().delete_return_track(index)
        else:
            self._song().delete_track(index)
        return {"deleted": name}

    def cmd_delete_device(self, request):
        t, _, _ = self._track(request.get("track"))
        devices = list(t.devices)
        dv = self._device(t, request.get("device"))
        name = dv.name
        t.delete_device(devices.index(dv))
        return {"deleted": name, "devices": [d.name for d in t.devices]}

    # ---- clip automation ----------------------------------------------------

    def _envelope_target(self, request):
        t, _, _ = self._track(request.get("track"))
        clip = self._clip(t, request)
        p = self._param(t, request.get("device"), request.get("param"))
        return clip, p

    def cmd_envelope_set(self, request):
        """points: [[beat, value_or_text], ...]; shape: "linear" (default) or
        "step"; resolution: beats per step for ramps (default 1/16 note).
        Replaces whatever the envelope had between the first and last point."""
        clip, p = self._envelope_target(request)
        raw = request.get("points") or []
        if len(raw) < 1:
            raise CommandError('give "points": [[beat, value], ...]')
        points = []
        for item in raw:
            beat, v = float(item[0]), item[1]
            points.append((beat, value_for_text(p, v) if isinstance(v, str) else _value_from(p, {"value": v})))
        points.sort(key=lambda x: x[0])
        shape = _norm(request.get("shape", "linear"))
        res = float(request.get("resolution", 0.25))
        if res <= 0:
            raise CommandError("resolution must be positive")

        # Live drops events at or past the clip's end, so a ramp ending on the
        # last beat never got there (a throw to -12 dB stopped at -15.5).
        # Pull a final point at the end back to the clip's last step.
        length = float(getattr(clip, "length", 0.0) or 0.0)
        if length > 0 and points[-1][0] > length - res:
            last_beat = max(length - res, points[-2][0] if len(points) > 1 else 0.0)
            points[-1] = (last_beat, points[-1][1])

        env = clip.automation_envelope(p)
        if env is None:
            env = clip.create_automation_envelope(p)
        if env is None:
            raise CommandError("Live won't create an envelope for %s here" % p.name)

        start, end = points[0][0], points[-1][0]
        env.delete_events_in_range(start, end + res)
        steps = 0
        for (t0, v0), (t1, v1) in zip(points, points[1:]):
            if shape == "step" or t1 <= t0:
                env.insert_step(t0, t1 - t0, v0)
                steps += 1
                continue
            t = t0
            while t < t1 - 1e-9:
                dur = min(res, t1 - t)
                env.insert_step(t, dur, v0 + (v1 - v0) * (t - t0) / (t1 - t0))
                t += dur
                steps += 1
        env.insert_step(points[-1][0], res, points[-1][1])
        return {"param": p.name, "from": start, "to": end, "steps": steps + 1}

    def cmd_envelope_get(self, request):
        clip, p = self._envelope_target(request)
        env = clip.automation_envelope(p)
        if env is None:
            return {"param": p.name, "envelope": None}
        start = float(request.get("from", 0.0))
        end = float(request.get("to", clip.length))
        res = float(request.get("resolution", 1.0))
        samples, t = [], start
        while t <= end + 1e-9:
            v = env.value_at_time(t)
            samples.append([t, v, _shown(p, v)])
            t += res
        return {"param": p.name, "samples": samples}

    def cmd_envelope_clear(self, request):
        clip, p = self._envelope_target(request)
        clip.clear_envelope(p)
        return {"param": p.name, "cleared": True}
