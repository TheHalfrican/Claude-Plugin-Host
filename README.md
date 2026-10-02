# Claude Plugin Host

Tools that let an AI assistant (Claude) work hands-on in Ableton Live:

- **Claude Host FX** and **Claude Host Instrument**: two small AU/VST3 plugins that each host one other plugin inside them and expose **every one of its parameters** over a local socket, so any knob can be read and set by name and in real units.
- **Claude Bridge**: a Live Remote Script for what Live's own scripting API allows but common tools don't expose: mixer, routing, feeding sidechains, duplicating/deleting tracks and devices, and clip automation.


```
$ host_ctl.py --track Keys set-many "Band 2 State=1" "Band 2 Shape=0" "Band 2 Frequency=400 Hz" "Band 2 Gain=-2 dB"
$ host_ctl.py --track "Bass (Host)" set "Fil Cutoff" --text "600 Hz"
```

## Why the host plugins

Live's own API (and anything built on it, such as Max for Live devices or AbletonMCP) can only see a third-party plugin's parameters after the user clicks **Configure** and touches each control by hand. For big plugins — Serum, FabFilter Pro-Q 2, Kontakt — that means a script sees nothing but "Device On".

A plugin that *hosts* the real plugin doesn't have that limit: it talks to the plugin directly. To Live it's an ordinary device, saved with the set like any other.

## What it does

- **Effect host** (`Claude Host FX`) and **instrument host** (`Claude Host Instrument`, MIDI passed through). Each only offers plugins of its own kind.
- **Every parameter by name**, with the plugin's own display text and choices, plus factory programs where the plugin exposes them.
- **Real units**: `"2.5 kHz"`, `"-4 dB"`, `"250 ms"`, `"4:1"`, `"on"`, `"MG Low 12"`, `"1/4"`. Each value is checked against what the plugin displays (JUCE's text conversion is unreliable for many plugins); out-of-range or unknown text is refused and the parameter left alone. Every reply reports the value the plugin actually kept.
- **Sidechain** on the effect host, passed to the hosted plugin's sidechain (e.g. Pro-C 2 ducking to a kick).
- **8 stereo outputs** on the instrument host for multi-out instruments (Kontakt, Battery…).
- **Per-bus routing** that copes with mono/stereo mismatches and plugins that refuse a layout; latency and tail length follow the hosted plugin.
- **State** saved with the Live set: the hosted plugin and all its settings come back on reopen.
- **Safe to run inside a DAW**: loopback-only socket, all plugin calls on the message thread, the audio thread never blocks, no SIGPIPE on disconnecting clients.

## Requirements

- macOS on Apple Silicon (arm64), Xcode command-line tools, CMake 3.22+, Ninja (optional)
- Ableton Live 12 (any host that loads AU or VST3 should work; Live is what it's tested in)
- Python 3 for the client and its tests (`pytest`)

JUCE 8 and Catch2 are fetched by CMake on first configure.

## Build and install

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target ClaudeHostFX_All ClaudeHostInst_All
```

Both plugins are copied to `~/Library/Audio/Plug-Ins/Components` (AU) and `~/Library/Audio/Plug-Ins/VST3`. In Live, rescan plug-ins; they appear under Plug-Ins → AUv2 → Claude. After rebuilding a plugin Live has already loaded, quit and reopen Live — it keeps the old code until then.

## Use

1. Put **Claude Host FX** at the end of a track's chain (or **Claude Host Instrument** as a MIDI track's instrument).
2. Load a plugin into it, either by typing its name in the host's header or from the client:

```sh
tools/host_ctl.py instances                          # running hosts: tag, track, hosted plugin
tools/host_ctl.py --track Bass load "FF Pro-Q 2"     # AU preferred over VST3
tools/host_ctl.py --track Bass params "Band 1"       # name, value 0..1, display text, choices
tools/host_ctl.py --track Bass get "Band 1 Gain"
tools/host_ctl.py --track Bass set "Band 1 Gain" --text "-3 dB"
tools/host_ctl.py --track Bass set "Band 1 Gain" 0.42          # normalized 0..1
tools/host_ctl.py --track Bass set-many "A=1" "B=250 Hz"       # in order, one round trip
tools/host_ctl.py --track Bass programs                         # factory presets, if any
```

Pick an instance with `--track NAME` (the Live track name) or `--tag N`. Each host has one Live-visible parameter, **Instance Tag** (shown in the host's header), which Live's API can read; that's how a script maps a Live device to its socket.

### Protocol

Line-delimited JSON over TCP on `127.0.0.1`, on a port the OS assigns. Each running instance publishes `{pid, port, tag, track, plugin}` in `~/Library/Application Support/ClaudePluginHost/instances/`.

| Command | Fields | Does |
|---|---|---|
| `info` | | Host, tag, port, track, hosted plugin |
| `list_plugins` | `query` | Plugins this host can load |
| `load` / `unload` | `name`, `format` | Host a plugin (AU preferred) / remove it |
| `params` | `filter` | Every parameter, or those whose name contains `filter` |
| `get` | `param` (name or index) | One parameter |
| `set` | `param`, `value` (0..1) or `text` | Set it; replies with what the plugin kept |
| `set_many` | `changes`: list of `set` requests | Applied in order; each reported; `allOk` |
| `programs` / `set_program` | `index` | Factory programs |

Connections stay open, so a client can send many requests on one connection; `tools/host_ctl.py` is a complete, small example.

## Claude Bridge (Remote Script)

```sh
tools/install_bridge.sh      # copies bridge/ClaudeBridge into Live's User Library
```

In Live: **Settings → Link, Tempo & MIDI → Control Surface → ClaudeBridge** (Input/Output: None). It listens on `127.0.0.1:9879`, using the same line-delimited JSON protocol as the hosts:

```sh
tools/bridge_ctl.py tracks
tools/bridge_ctl.py set-track "Melody" volume="+1.5 dB" "send A=-20 dB"
tools/bridge_ctl.py set-routing "Kick (SC)" output "Sends Only"
tools/bridge_ctl.py feed-sidechain "Kick (SC)" "Bass"        # route a key into a device's sidechain
tools/bridge_ctl.py envelope "Melody" mixer "send B" --clip 0 "12=-inf dB" "16=-12 dB"
tools/bridge_ctl.py name-hosts                                # "FF Pro-Q 2 (Claude Host)"
tools/bridge_ctl.py --help                                    # everything else
```

Values are given as Live displays them ("-6 dB", "25L", "C", "Off") and land exactly on that reading. The bridge also renames Claude Host devices after the plugin they hold, every couple of seconds. After changing bridge code, `tools/install_bridge.sh && tools/bridge_ctl.py reload` loads it without restarting Live (except for changes to `bridge.py` itself).

What Live's API doesn't allow, and so neither does the bridge: setting a plugin device's own sidechain dropdown (`feed-sidechain` routes the key track's output instead), writing the Arrangement's track automation lanes (clip envelopes work in both Session and Arrangement), and loading devices onto the master or return tracks.

## Tests

```sh
cmake --build build --target ClaudeHostTests ClaudeHostInstTests
ctest --test-dir build -LE validation --output-on-failure   # ~15 s
ctest --test-dir build -L validation --output-on-failure    # auval + pluginval on the installed plugins
```

- **Unit tests**: the control protocol (split/batched lines, split UTF-8, bad input, shutdown), the instance registry, and text-to-value conversion against fake parameters that behave like real plugins'.
- **Integration tests**, no DAW needed: the real host code hosting Apple's built-in Audio Units (AULowpass, AUDelay, DLSMusicDevice) — every command, audio actually filtered, bit-exact passthrough, sample-accurate MIDI, state round-trip — plus fake plugins for sidechain, multi-out, mono/stereo and latency routing.
- **Client tests** (pytest): `host_ctl.py` against a fake host.
- **Bridge tests** (pytest): every bridge command against fake Live objects that behave like Live's API (including its quirks, such as continuous parameters refusing `value_items` and events at a clip's end being dropped), its socket server, and `bridge_ctl.py`.
- **Validation**: Apple's `auval` and Tracktion's [pluginval](https://github.com/Tracktion/pluginval) (strictness 10 passes) on both formats of both plugins.
- **Plugin-specific tests**, hidden by default because they need the plugins installed: `ClaudeHostTests "[fabfilter]"`, `ClaudeHostInstTests "[serum]"`. They measure, from the audio, what plugins' unlabelled codes mean (e.g. Pro-Q 2's band shapes and slopes) and check real sidechain ducking with Pro-C 2.

CI runs the fast suite and validation on GitHub Actions (macOS, Apple Silicon) on every push.

## Status and limitations

- Tested in Ableton Live 12.2 on macOS, Apple Silicon, with FabFilter Pro-Q 2 / Pro-C 2 / Saturn and Xfer Serum, including sidechain ducking in Live and save/reopen. Other plugins and hosts should work but haven't been tried. Multi-output instruments are tested with fake plugins but not yet with a real one in Live.
- Claude Bridge is tested in Live 12.2.7: mixer, routing, mute/solo, feeding a sidechain, clip automation, renaming hosts.
- Live's automation lanes see only the host's own parameter (Instance Tag), not the hosted plugin's.
- Plugins whose UI only exposes settings as numbers (Pro-Q 2's band shapes, Saturn's styles) need those numbers mapped once; the hidden tests show how to measure them.
- The hosted plugin's own preset browser still has to be used by hand when the plugin doesn't expose programs (Serum's AU doesn't).

## License

[AGPLv3](LICENSE), matching JUCE's open-source license. JUCE is © Raw Material Software; see its own license terms.
