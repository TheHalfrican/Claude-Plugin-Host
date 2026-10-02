#!/usr/bin/env python3
"""Talk to the Claude Bridge Remote Script inside Live (127.0.0.1:9879).

Tracks are given by name ("Keys", "A-Reverb", "Master") or index; devices by
name or index ("mixer" for the track's mixer). Values are text as Live shows
them ("-6 dB", "25L", "C", "On", "Post FX") unless they're plain numbers.

Usage:
  bridge_ctl.py info | tracks
  bridge_ctl.py reload                                          # load updated bridge code (after install_bridge.sh)
  bridge_ctl.py track TRACK
  bridge_ctl.py set-track TRACK mute=on solo=off volume="-6 dB" pan=25L "send A=-12 dB"
  bridge_ctl.py routing TRACK
  bridge_ctl.py set-routing TRACK input|output TYPE [CHANNEL]     # e.g. output "Sends Only"
  bridge_ctl.py devices TRACK
  bridge_ctl.py params TRACK DEVICE [FILTER]                    # DEVICE "mixer" for volume/pan/sends
  bridge_ctl.py set-param TRACK DEVICE PARAM VALUE [--text]     # number = raw value; text/--text = as displayed
  bridge_ctl.py sidechain TRACK DEVICE SOURCE [TAP]             # e.g. "Kick (SC)" "Post FX"
  bridge_ctl.py feed-sidechain KEY_TRACK TARGET_TRACK [DEVICE]  # route the key into DEVICE's sidechain
  bridge_ctl.py name-hosts                                      # rename hosts after their plugin
  bridge_ctl.py rename-device TRACK DEVICE NAME
  bridge_ctl.py duplicate-track TRACK
  bridge_ctl.py delete-track TRACK                              # destructive: ask the user first
  bridge_ctl.py delete-device TRACK DEVICE                      # destructive: ask the user first
  bridge_ctl.py envelope TRACK DEVICE PARAM --clip N "0=-24 dB" "16=0 dB" [--shape step] [--res 0.25]
  bridge_ctl.py envelope-get TRACK DEVICE PARAM --clip N [--res 1]
  bridge_ctl.py envelope-clear TRACK DEVICE PARAM --clip N
  bridge_ctl.py inspect TRACK|song [DEVICE [FILTER]] [--chain N]  # developer aid: API attributes
  bridge_ctl.py move-device TRACK DEVICE TO_TRACK [POSITION]
  bridge_ctl.py raw '{"cmd": "tracks"}'
Use --arrangement-clip N instead of --clip N for clips in the Arrangement.
"""
import argparse
import json
import os
import socket
import sys

PORT = int(os.environ.get("CLAUDE_BRIDGE_PORT", "9879"))


def send(request):
    try:
        with socket.create_connection(("127.0.0.1", PORT), timeout=30) as s:
            s.sendall((json.dumps(request) + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = s.recv(1 << 16)
                if not chunk:
                    break
                buf += chunk
    except OSError as e:
        sys.exit("Can't reach Claude Bridge on 127.0.0.1:%d (%s). Is Live open with ClaudeBridge "
                 "selected as a Control Surface?" % (PORT, e))
    return json.loads(buf)


def ref(text):
    """Indexes as ints, everything else as names."""
    return int(text) if text.lstrip("-").isdigit() else text


def value(text):
    """Plain numbers raw, anything with units or words as display text."""
    try:
        return float(text)
    except ValueError:
        return text


def parse_on_off(text):
    t = text.strip().lower()
    if t in ("on", "true", "yes", "1"):
        return True
    if t in ("off", "false", "no", "0"):
        return False
    sys.exit("expected on/off, got %r" % text)


def build(a, rest):
    cmd = a.command
    need = {"track": 1, "set-track": 2, "routing": 1, "set-routing": 3, "devices": 1, "params": 2,
            "set-param": 4, "sidechain": 3, "feed-sidechain": 2, "rename-device": 3, "move-device": 3, "duplicate-track": 1, "delete-track": 1, "delete-device": 2,
            "envelope": 4, "envelope-get": 3, "envelope-clear": 3}.get(cmd, 0)
    if len(rest) < need:
        sys.exit("%s needs %d argument(s); see --help" % (cmd, need))

    if cmd in ("info", "tracks", "reload"):
        return {"cmd": cmd}
    if cmd in ("track", "routing", "devices", "duplicate-track", "delete-track"):
        return {"cmd": cmd.replace("-", "_"), "track": ref(rest[0])}
    if cmd == "set-track":
        req = {"cmd": "set_track", "track": ref(rest[0])}
        for item in rest[1:]:
            if "=" not in item:
                sys.exit("set-track takes key=value items; got %r" % item)
            k, v = [x.strip() for x in item.split("=", 1)]
            kl = k.lower()
            if kl in ("mute", "solo", "arm"):
                req[kl] = parse_on_off(v)
            elif kl in ("volume", "pan"):
                req[kl] = v                      # always as Live shows it: "-6 dB", "25L", "C"
            elif kl.startswith("send"):
                req.setdefault("sends", {})[k[4:].strip()] = v
            else:
                sys.exit("unknown track setting %r (mute, solo, arm, volume, pan, send X)" % k)
        return req
    if cmd == "set-routing":
        req = {"cmd": "set_routing", "track": ref(rest[0]), "direction": rest[1], "type": rest[2]}
        if len(rest) > 3:
            req["channel"] = rest[3]
        return req
    if cmd == "params":
        req = {"cmd": "params", "track": ref(rest[0]), "device": ref(rest[1])}
        if len(rest) > 2:
            req["filter"] = rest[2]
        return req
    if cmd == "set-param":
        v = rest[3] if a.text else value(rest[3])
        req = {"cmd": "set_param", "track": ref(rest[0]), "device": ref(rest[1]), "param": ref(rest[2])}
        req["value" if isinstance(v, float) else "text"] = v
        return req
    if cmd == "sidechain":
        req = {"cmd": "set_sidechain", "track": ref(rest[0]), "device": ref(rest[1]), "source": rest[2]}
        if len(rest) > 3:
            req["tap"] = rest[3]
        return req
    if cmd == "feed-sidechain":
        req = {"cmd": "feed_sidechain", "from": ref(rest[0]), "to": ref(rest[1])}
        if len(rest) > 2:
            req["device"] = rest[2]
        return req
    if cmd == "rename-device":
        return {"cmd": "rename_device", "track": ref(rest[0]), "device": ref(rest[1]), "name": rest[2]}
    if cmd == "name-hosts":
        return {"cmd": "name_hosts"}
    if cmd == "delete-device":
        return {"cmd": "delete_device", "track": ref(rest[0]), "device": ref(rest[1])}
    if cmd.startswith("envelope"):
        req = {"cmd": cmd.replace("-", "_"), "track": ref(rest[0]), "device": ref(rest[1]), "param": ref(rest[2])}
        if a.clip is not None:
            req["clip"] = a.clip
        elif a.arrangement_clip is not None:
            req["arrangement_clip"] = a.arrangement_clip
        else:
            sys.exit("give --clip N or --arrangement-clip N")
        if a.res is not None:
            req["resolution"] = a.res
        if cmd == "envelope":
            req["cmd"] = "envelope_set"
            points = []
            for item in rest[3:]:
                if "=" not in item:
                    sys.exit('envelope points are "BEAT=VALUE", e.g. "0=-24 dB"; got %r' % item)
                beat, v = item.split("=", 1)
                points.append([float(beat), value(v.strip())])
            req["points"] = points
            if a.shape:
                req["shape"] = a.shape
        return req
    if cmd == "inspect":
        req = {"cmd": "inspect", "track": ref(rest[0])}
        if len(rest) > 1:
            req["device"] = ref(rest[1])
        if len(rest) > 2:
            req["filter"] = rest[2]
        if a.chain is not None:
            req["chain"] = a.chain
        return req
    if cmd == "move-device":
        req = {"cmd": "move_device", "track": ref(rest[0]), "device": ref(rest[1]), "to_track": ref(rest[2])}
        if len(rest) > 3:
            req["position"] = int(rest[3])
        return req
    if cmd == "raw":
        return json.loads(rest[0])
    sys.exit("unknown command %r; see --help" % cmd)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--clip", type=int)
    ap.add_argument("--arrangement-clip", type=int)
    ap.add_argument("--shape", choices=["linear", "step"])
    ap.add_argument("--res", type=float)
    ap.add_argument("--chain", type=int, help="inspect: a rack's chain")
    ap.add_argument("--text", action="store_true", help="set-param: treat VALUE as display text even if it's a number")
    a = ap.parse_intermixed_args()  # options may sit between envelope points
    reply = send(build(a, a.args))
    print(json.dumps(reply, indent=1))
    sys.exit(0 if reply.get("ok") else 1)


if __name__ == "__main__":
    main()
