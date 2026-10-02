# ClaudePluginHost

An AU/VST3 plugin ("Claude Host FX") that hosts one other plugin inside it and exposes **all** of that plugin's parameters over a loopback socket, so Claude can read and set any knob from Ableton Live. Live's own API (and AbletonMCP) only sees plugin parameters after the user clicks Configure; this wrapper avoids that.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo   # first time; fetches JUCE 8.0.15
cmake --build build --target ClaudeHostFX_AU                     # also copies to ~/Library/Audio/Plug-Ins/Components
killall -9 AudioComponentRegistrar; auval -v aufx Chfx Clde      # refresh the AU cache and validate
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
- `tests/test_host_ctl.py` — the Python client against a fake host.
- `tests/validate_plugin.sh` — auval and pluginval (downloads pluginval into `build/tools`).

CI: `.github/workflows/tests.yml` runs on both forges with `runs-on: macos-latest`. On **GitHub** (via the push mirror) it builds everything on a clean VM, runs the fast suite, then auval and pluginval. On **Gitea** it runs on the user's own Mac runner (host mode, `~/gitea-runner`), so it only builds and runs the tests: building the plugins there would install them over the copy Live uses. The `[fabfilter]` tests need the user's FabFilter plugins, so they only run by hand.

The test program sets `CLAUDE_HOST_REGISTRY_DIR` to a temp folder so it never touches instances running in Live. Run the suite before every commit.

## Layout

- `Source/HostProcessor.*` — the AudioProcessor. Hosts the inner plugin (AU preferred, then VST3), passes audio through it, saves/restores the inner plugin and its state, and handles control commands. All inner-plugin access happens on the message thread; the audio thread only ever `tryLock`s `innerLock`.
- `Source/ControlServer.*` — line-delimited JSON over TCP on 127.0.0.1, OS-assigned port. One request object per line, one reply per line. Client sockets set `SO_NOSIGPIPE`: without it, a client disconnecting mid-reply would SIGPIPE-kill the host process (Live).
- `Source/InstanceRegistry.*` — one JSON file per instance in `~/Library/Application Support/ClaudePluginHost/instances/` (pid, port, tag, track, plugin).
- `Source/HostEditor.*` — header bar plus the inner plugin's own editor embedded below.

The wrapper's only Live-visible parameter is **Instance Tag** (1–9999, unique among running instances). AbletonMCP can read it on the device, which maps a Live track/device to a registry entry and port.

## Commands

`info`, `list_plugins {query}`, `load {name, format?}`, `unload`, `params {filter}`, `get {param}`, `set {param, value 0..1 | text "2.5 kHz"}`, `programs`, `set_program {index}`. `param` is a name (exact, or a unique substring) or an index. `set` replies with the value the plugin actually kept.

Client: `tools/host_ctl.py`. The ableton-live-12 skill symlinks it as `~/.claude/skills/ableton-live-12/scripts/host_ctl.py`.

Text values: the host checks what the plugin displays for the converted value and, if it doesn't match, bisects the range using the plugin's own display text (units: Hz/kHz, ms/s, dB, %; AU unit labels are used too). Words ("On", choice names) are only accepted if the plugin shows exactly that word. Out-of-range or unknown text is refused and the parameter is left alone.

## Phases

1. Effect host (`ClaudeHostFX`). ← current. Verified in Live 12.2.7: loads FF Pro-Q 2 (AU), all 190 params listed, set by real units ("250 Hz", "-4 dB") with exact read-back; Live reads the Instance Tag. Save, quit and reopen restores the inner plugin, its settings and the tag (also across a rebuild). Still to test: FF Saturn.

Live caches a loaded plugin's code per process: after rebuilding, quit and reopen Live to pick up the new build.
2. Instrument host (`claude_host_plugin(... TRUE)` in CMakeLists), MIDI through, tested with Serum 1 (Serum 2 is unlicensed for now).
3. Sidechain, multi-out, latency edge cases, state-save tests on real projects; wire into the skill.

Test only in a blank Live set, never the user's projects.

## Repos

The `gitea` remote is the user's self-hosted Gitea, `TheHalfrican/Claude-Plugin-Host` (private). Gitea push-mirrors every branch and commit to the **public** GitHub repo of the same name, so commit only what's fine to publish.

## License

AGPLv3 (see `LICENSE`), matching JUCE's open-source license.
