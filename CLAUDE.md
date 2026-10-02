# ClaudePluginHost

AU/VST3 plugins ("Claude Host FX" for effects, "Claude Host Instrument" for instruments) that each host one other plugin inside it and exposes **all** of that plugin's parameters over a loopback socket, so Claude can read and set any knob from Ableton Live. Live's own API (and AbletonMCP) only sees plugin parameters after the user clicks Configure; this wrapper avoids that.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo   # first time; fetches JUCE 8.0.15
cmake --build build --target ClaudeHostFX_All ClaudeHostInst_All # AU + VST3, copied into ~/Library/Audio/Plug-Ins
killall -9 AudioComponentRegistrar
auval -v aufx Chfx Clde; auval -v aumu Chin Clde                 # validate our AUs only; never scan every AU
```

Live must rescan plug-ins (Settings → Plug-Ins → Rescan) to pick up a new build. Live 12 runs native arm64, so the build is arm64 only.

## Tests

```sh
cmake --build build --target ClaudeHostTests
ctest --test-dir build -LE validation --output-on-failure     # fast: C++ unit + integration, Python client (~15 s)
cmake --build build --target ClaudeHostFX_All
ctest --test-dir build -L validation --output-on-failure      # auval + pluginval on the installed AU and VST3
PLUGINVAL_STRICTNESS=10 tests/validate_plugin.sh build        # strictest pluginval run
```

- `tests/ControlServerTests.cpp` — protocol framing (batched/split lines, split UTF-8, bad JSON, arrays, oversized input, shutdown).
- `tests/InstanceRegistryTests.cpp` — publish/remove, tag collisions, dead-process and junk entries.
- `tests/HostProcessorTests.cpp` — the real host with Apple's built-in AUs (AULowpass, AUDelay): loading, every command, text values with units, audio really filtered, bit-exact passthrough, state round-trip, tag re-roll on duplicate, the socket path end to end, closing with a request in flight, the editor.
- `tests/InstrumentHostTests.cpp` — the instrument host with DLSMusicDevice: silence without notes (even with junk in the buffer), notes sound, sample-accurate note timing, note-off release, unload mid-note, params, state, socket.
- `tests/FabFilterTests.cpp`, `tests/SerumTests.cpp` — hidden (`[fabfilter]`, `[serum]`): need the user's plugins. They pin down unlabelled codes measured from the audio; `[measure]` ones only print.
- `tests/RoutingTests.cpp`, `tests/InstrumentRoutingTests.cpp` — bus routing with fake plugins (`tests/FakePlugin.h`): sidechain on/off/mono, mono-only plugins, plugins refusing layouts, latency and tail, re-prepare after a layout change, multi-out mapping, MIDI timing. `HostProcessor::loadPluginInstance()` hosts an already-made plugin for this.
- `tests/test_host_ctl.py` — the Python client against a fake host.
- `tests/bridge/` — Claude Bridge: commands against fake Live objects, the socket server, `bridge_ctl.py` (ctest `python_bridge`).
- `tests/validate_plugin.sh` — auval and pluginval (downloads pluginval into `build/tools`).

CI: `.github/workflows/tests.yml` runs on both forges with `runs-on: macos-latest`. On **GitHub** (via the push mirror) it builds everything on a clean VM, runs the fast suite, then auval and pluginval. On **Gitea** it runs on the user's own Mac runner (host mode, `~/gitea-runner`), so it only builds and runs the tests: building the plugins there would install them over the copy Live uses. The `[fabfilter]` tests need the user's FabFilter plugins, so they only run by hand.

The test program sets `CLAUDE_HOST_REGISTRY_DIR` to a temp folder so it never touches instances running in Live. Run the suite before every commit.

## Layout

- `Source/HostProcessor.*` — the AudioProcessor. Hosts the inner plugin (AU preferred, then VST3), passes audio through it, saves/restores the inner plugin and its state, and handles control commands. All inner-plugin access happens on the message thread; the audio thread only ever `tryLock`s `innerLock`.
- `Source/ControlServer.*` — line-delimited JSON over TCP on 127.0.0.1, OS-assigned port. One request object per line, one reply per line. Client sockets set `SO_NOSIGPIPE`: without it, a client disconnecting mid-reply would SIGPIPE-kill the host process (Live).
- `Source/InstanceRegistry.*` — one JSON file per instance in `~/Library/Application Support/ClaudePluginHost/instances/` (pid, port, tag, track, plugin).
- `Source/ParameterText.*` — text to 0..1 values, checked against the plugin's own display: numbers with units (bisection over the display), words and menu entries (exact match, scanning the range when the plugin doesn't mark switches/menus as discrete, as Serum doesn't). "1/4"-style entries count as text, not numbers. Tested directly with fake parameters (`tests/ParameterTextTests.cpp`).
- `Source/HostEditor.*` — header bar plus the inner plugin's own editor embedded below.

The wrapper's only Live-visible parameter is **Instance Tag** (1–9999, unique among running instances). AbletonMCP can read it on the device, which maps a Live track/device to a registry entry and port.

## Commands

`info`, `list_plugins {query}`, `load {name, format?}`, `unload`, `params {filter}`, `get {param}`, `set {param, value 0..1 | text "2.5 kHz"}`, `set_many {changes: [set requests...]}` (applied in order, each reported, `allOk`), `programs`, `set_program {index}`.

Each host only lists plugins it can host: AU identifiers carry the type (`AudioUnit:Synths/…`, `AudioUnit:Effects/…`), so the instrument host offers AU instruments and the effect host hides them. VST3s can't be sorted without loading, so both hosts list them. `param` is a name (exact, or a unique substring) or an index. `set` replies with the value the plugin actually kept.

Client: `tools/host_ctl.py`. The ableton-live-12 skill symlinks it as `~/.claude/skills/ableton-live-12/scripts/host_ctl.py`.

Text values: see `Source/ParameterText.*`. Out-of-range or unknown text is refused and the parameter is left alone. VST3s are listed by file name (JUCE gives their path).

## Claude Bridge (`bridge/ClaudeBridge`)

A Live Remote Script (Python, runs inside Live) serving line-delimited JSON on `127.0.0.1:9879`. Install with `tools/install_bridge.sh` (copies into `~/Music/Ableton/User Library/Remote Scripts/ClaudeBridge`); the user enables it as a Control Surface. Client: `tools/bridge_ctl.py` (symlinked into the ableton-live-12 skill).

- `commands.py`: all logic, written against Live's API (LOM) but importing nothing from Live. It's tested with fake Live objects (`tests/bridge/fake_live.py`). Bump `VERSION` when changing it.
- `server.py`: the socket server, loopback only, SO_NOSIGPIPE, one thread per client.
- `bridge.py`: the Live side (ControlSurface). It runs each request on Live's main thread via `schedule_message`, handles `reload` (re-imports `commands.py`, so no restart needed), and calls `Commands.background_tick()` every ~2 s (housekeeping; currently it tries `name_hosts`, which only changes anything for renamable devices). Changes to `bridge.py` itself need a Live restart.
- Live API findings (12.2.7):
  - Continuous parameters **raise** on `value_items`.
  - Plugin devices have **no `input_routings`**, so their sidechain dropdown isn't scriptable. A track's output can instead go to `<track>` / `Sidechain-<device>` (`feed_sidechain`).
  - `Device.name` is settable for Live's own devices only. On a **plugin** device Live accepts the assignment silently and keeps the plugin's name, so always read back. The fakes mimic this. Hosts show "<plugin> - Claude Host #<tag>" as their program name and in their editor header instead.
  - Clip envelope events at or past the clip's end are dropped.
  - Unnamed routing channels have an empty `display_name`.
  - `bridge_ctl.py inspect` lists what Live exposes on a track or device.
- Mirror every new Live quirk in the fakes, so the tests catch it.

## Phases

1. Effect host (`ClaudeHostFX`). ← current. Verified in Live 12.2.7: loads FF Pro-Q 2 (AU), all 190 params listed, set by real units ("250 Hz", "-4 dB") with exact read-back; Live reads the Instance Tag. Save, quit and reopen restores the inner plugin, its settings and the tag (also across a rebuild). Still to test: FF Saturn.

Live caches a loaded plugin's code per process: after rebuilding, quit and reopen Live to pick up the new build.
2. Instrument host (`ClaudeHostInst`, AU type aumu `Chin`). Done. MIDI goes through to the inner synth; the synth's inputs are disabled and the buffer cleared before it renders. Tested headless with Apple's DLSMusicDevice (`ClaudeHostInstTests`) and the user's Serum 1 (hidden `[serum]` tests: 288 params, plays). Still to test: in Live. Serum 2 is unlicensed for now.
3. ← current. Done: effect host sidechain input (bus 1, off by default) passed to the inner plugin's sidechain; instrument host has 8 stereo outputs (main + 7 aux, off by default), inner output N → ours N; inner buses negotiated one at a time (ours, then stereo, mono, off) so one refusal doesn't lose the rest; processBlock routes bus by bus through scratch (mono↔stereo bridged, missing buses silent); latency/tail follow the inner plugin. Tested with fake plugins (`tests/FakePlugin.h`, `RoutingTests.cpp`, `InstrumentRoutingTests.cpp`) and real Pro-C 2 sidechain ducking (`[fabfilter]`: needs Side Chain Expert Mode 1 + Side Chain Input 1). Still to do: sidechain and multi-out in Live itself; state-save test on a copy of a real project.

Test only in a blank Live set, never the user's projects.

## Repos

The `gitea` remote is the user's self-hosted Gitea, `TheHalfrican/Claude-Plugin-Host` (private). Gitea push-mirrors every branch and commit to the **public** GitHub repo of the same name, so commit only what's fine to publish.

## License

AGPLv3 (see `LICENSE`), matching JUCE's open-source license.
