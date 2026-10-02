# ClaudePluginHost

An AU/VST3 plugin ("Claude Host FX") that hosts one other plugin inside it and exposes **all** of that plugin's parameters over a loopback socket, so Claude can read and set any knob from Ableton Live. Live's own API (and AbletonMCP) only sees plugin parameters after the user clicks Configure; this wrapper avoids that.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo   # first time; fetches JUCE 8.0.15
cmake --build build --target ClaudeHostFX_AU                     # also copies to ~/Library/Audio/Plug-Ins/Components
killall -9 AudioComponentRegistrar; auval -v aufx Chfx Clde      # refresh the AU cache and validate
```

Live must rescan plug-ins (Settings → Plug-Ins → Rescan) to pick up a new build. Live 12 runs native arm64, so the build is arm64 only.

## Layout

- `Source/HostProcessor.*` — the AudioProcessor. Hosts the inner plugin (AU preferred, then VST3), passes audio through it, saves/restores the inner plugin and its state, and handles control commands. All inner-plugin access happens on the message thread; the audio thread only ever `tryLock`s `innerLock`.
- `Source/ControlServer.*` — line-delimited JSON over TCP on 127.0.0.1, OS-assigned port. One request object per line, one reply per line.
- `Source/InstanceRegistry.*` — one JSON file per instance in `~/Library/Application Support/ClaudePluginHost/instances/` (pid, port, tag, track, plugin).
- `Source/HostEditor.*` — header bar plus the inner plugin's own editor embedded below.

The wrapper's only Live-visible parameter is **Instance Tag** (1–9999, unique among running instances). AbletonMCP can read it on the device, which maps a Live track/device to a registry entry and port.

## Commands

`info`, `list_plugins {query}`, `load {name, format?}`, `unload`, `params {filter}`, `get {param}`, `set {param, value 0..1 | text "2.5 kHz"}`, `programs`, `set_program {index}`. `param` is a name (exact, or a unique substring) or an index. `set` replies with the value the plugin actually kept.

Client: `~/.claude/skills/ableton-live-12/scripts/host_ctl.py` (part of the ableton-live-12 skill).

## Phases

1. Effect host (`ClaudeHostFX`). ← current. Verified in Live 12.2.7: loads FF Pro-Q 2 (AU), all 190 params listed, set by real units ("250 Hz", "-4 dB") with exact read-back; Live reads the Instance Tag. Still to test: saving and reopening a set, FF Saturn.
2. Instrument host (`claude_host_plugin(... TRUE)` in CMakeLists), MIDI through, tested with Serum 1 (Serum 2 is unlicensed for now).
3. Sidechain, multi-out, latency edge cases, state-save tests on real projects; wire into the skill.

Test only in a blank Live set, never the user's projects.

## Repos

The `gitea` remote is the user's self-hosted Gitea, `TheHalfrican/Claude-Plugin-Host` (private). Gitea push-mirrors every branch and commit to the **public** GitHub repo of the same name, so commit only what's fine to publish.

## License

AGPLv3 (see `LICENSE`), matching JUCE's open-source license.
