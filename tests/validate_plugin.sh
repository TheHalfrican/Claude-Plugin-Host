#!/usr/bin/env bash
# Validates the installed plugins with Apple's auval and Tracktion's pluginval.
#
#   tests/validate_plugin.sh [build-dir]      (default: ./build)
#   PLUGINVAL_STRICTNESS=10 tests/validate_plugin.sh
#
# Build and install first: cmake --build build --target ClaudeHostFX_All
set -euo pipefail

BUILD_DIR="${1:-build}"
STRICTNESS="${PLUGINVAL_STRICTNESS:-5}"
PLUGINVAL_VERSION="v1.0.4"
TOOLS_DIR="$BUILD_DIR/tools"
PLUGINVAL="$TOOLS_DIR/pluginval.app/Contents/MacOS/pluginval"

AU_PATH="$HOME/Library/Audio/Plug-Ins/Components/Claude Host FX.component"
VST3_PATH="$HOME/Library/Audio/Plug-Ins/VST3/Claude Host FX.vst3"

failures=0
step() { printf '\n=== %s\n' "$*"; }

for p in "$AU_PATH" "$VST3_PATH"; do
    [[ -e "$p" ]] || { echo "Missing $p (build target ClaudeHostFX_All first)"; exit 1; }
done

step "auval (Audio Unit)"
killall -9 AudioComponentRegistrar 2>/dev/null || true
if auval -v aufx Chfx Clde | tee "$BUILD_DIR/auval.log" | tail -3 | grep -q "AU VALIDATION SUCCEEDED"; then
    echo "auval: PASS"
else
    echo "auval: FAIL (see $BUILD_DIR/auval.log)"; failures=$((failures + 1))
fi

if [[ ! -x "$PLUGINVAL" ]]; then
    step "Downloading pluginval $PLUGINVAL_VERSION"
    mkdir -p "$TOOLS_DIR"
    curl -fsSL -o "$TOOLS_DIR/pluginval.zip" \
        "https://github.com/Tracktion/pluginval/releases/download/$PLUGINVAL_VERSION/pluginval_macOS.zip"
    unzip -qo "$TOOLS_DIR/pluginval.zip" -d "$TOOLS_DIR"
fi

for p in "$AU_PATH" "$VST3_PATH"; do
    name="$(basename "$p")"
    step "pluginval strictness $STRICTNESS: $name"
    log="$BUILD_DIR/pluginval-${name// /_}.log"
    if "$PLUGINVAL" --strictness-level "$STRICTNESS" --validate-in-process --timeout-ms 120000 \
           --validate "$p" > "$log" 2>&1; then
        echo "pluginval $name: PASS"
    else
        echo "pluginval $name: FAIL (see $log)"; tail -20 "$log"; failures=$((failures + 1))
    fi
done

step "Summary"
if (( failures == 0 )); then echo "All validation passed"; else echo "$failures validation step(s) failed"; fi
exit $(( failures > 0 ))
