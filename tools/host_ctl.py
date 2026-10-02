#!/usr/bin/env python3
"""Talk to Claude Host plugins (ClaudePluginHost) running inside Live.

Each "Claude Host FX" instance hosts one real plugin and exposes ALL of its
parameters over a loopback socket, so no Configure step is needed. Instances
publish themselves in ~/Library/Application Support/ClaudePluginHost/instances/.

Pick an instance with --tag N (the "Instance Tag" Live shows on the device and
AbletonMCP's get_device_parameters can read), --track NAME (the Live track
name, or part of it), or nothing when only one instance is running.

Usage:
  host_ctl.py instances
  host_ctl.py [--tag N|--track NAME] info
  host_ctl.py [...] plugins [QUERY]            # plugins it can load
  host_ctl.py [...] load NAME [--format AudioUnit|VST3]
  host_ctl.py [...] unload
  host_ctl.py [...] params [FILTER]            # name, value 0..1, display text, choices
  host_ctl.py [...] get PARAM                  # PARAM = name (or unique part) or index
  host_ctl.py [...] set PARAM VALUE            # VALUE: 0..1 normalized
  host_ctl.py [...] set PARAM --text "2.5 kHz" # real units, as the plugin displays them
  host_ctl.py [...] set-many "Band 1 State=1" "Band 1 Frequency=250 Hz" ...
                                               # several text changes, in order, one round trip
  host_ctl.py [...] programs                   # factory presets the plugin exposes
  host_ctl.py [...] program INDEX
  host_ctl.py [...] raw '{"cmd": "info"}'
"""
import argparse, glob, json, os, socket, sys

REGISTRY = os.environ.get("CLAUDE_HOST_REGISTRY_DIR") or os.path.expanduser(
    "~/Library/Application Support/ClaudePluginHost/instances")


def alive(pid):
    try:
        os.kill(int(pid), 0)
        return True
    except PermissionError:
        return True
    except (OSError, ValueError):
        return False


def instances():
    found = []
    for path in glob.glob(os.path.join(REGISTRY, "*.json")):
        try:
            with open(path) as f:
                info = json.load(f)
        except (OSError, ValueError):
            continue
        if not alive(info.get("pid", 0)):
            try:
                os.remove(path)  # left behind by a crash
            except OSError:
                pass
            continue
        found.append(info)
    return sorted(found, key=lambda i: (i.get("track", ""), i.get("tag", 0)))


def track_name(info):
    """The Live track name. Live reports it as "<device name>/<track name>"."""
    track = info.get("track", "")
    product = info.get("product", "")
    if product and track.startswith(product + "/"):
        return track[len(product) + 1:]
    return track


def pick(args):
    found = instances()
    if args.tag is not None:
        found = [i for i in found if i.get("tag") == args.tag]
    if args.track:
        wanted = args.track.lower()
        exact = [i for i in found if track_name(i).lower() == wanted]
        found = exact or [i for i in found if wanted in track_name(i).lower()]
    if not found:
        sys.exit("No matching Claude Host instance. Is one loaded in Live? (run: host_ctl.py instances)")
    if len(found) > 1:
        rows = "\n".join(f"  tag {i['tag']}: {i.get('plugin') or 'empty'} on {track_name(i) or '?'}" for i in found)
        sys.exit(f"Several instances match; pass --tag:\n{rows}")
    return found[0]


def send(port, request):
    with socket.create_connection(("127.0.0.1", port), timeout=30) as s:
        s.sendall((json.dumps(request) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 16)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", type=int)
    ap.add_argument("--track")
    ap.add_argument("command")
    ap.add_argument("args", nargs="*")
    ap.add_argument("--text")
    ap.add_argument("--format")
    a = ap.parse_args()

    if a.command == "instances":
        print(json.dumps(instances(), indent=1))
        return

    cmd, rest = a.command, a.args
    if cmd == "info":
        req = {"cmd": "info"}
    elif cmd == "plugins":
        req = {"cmd": "list_plugins", "query": " ".join(rest)}
    elif cmd == "load":
        req = {"cmd": "load", "name": " ".join(rest)}
        if a.format:
            req["format"] = a.format
    elif cmd == "unload":
        req = {"cmd": "unload"}
    elif cmd == "params":
        req = {"cmd": "params", "filter": " ".join(rest)}
    elif cmd in ("get", "set"):
        if not rest:
            sys.exit(f"{cmd} needs a parameter name or index")
        param = int(rest[0]) if rest[0].isdigit() else rest[0]
        req = {"cmd": cmd, "param": param}
        if cmd == "set":
            if a.text is not None:
                req["text"] = a.text
            elif len(rest) > 1:
                req["value"] = float(rest[1])
            else:
                sys.exit("set needs VALUE (0..1) or --text")
    elif cmd == "set-many":
        changes = []
        for item in rest:
            if "=" not in item:
                sys.exit(f'set-many takes "PARAM=TEXT" items; got {item!r}')
            param, text = item.split("=", 1)
            changes.append({"param": param.strip(), "text": text.strip()})
        if not changes:
            sys.exit("set-many needs at least one PARAM=TEXT")
        req = {"cmd": "set_many", "changes": changes}
    elif cmd == "programs":
        req = {"cmd": "programs"}
    elif cmd == "program":
        req = {"cmd": "set_program", "index": int(rest[0])}
    elif cmd == "raw":
        req = json.loads(rest[0])
    else:
        sys.exit(f"unknown command {cmd}; see --help")

    target = pick(a)
    reply = send(target["port"], req)
    print(json.dumps(reply, indent=1))
    sys.exit(0 if reply.get("ok") and reply.get("allOk", True) else 1)


if __name__ == "__main__":
    main()
