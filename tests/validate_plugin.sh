#!/usr/bin/env bash
# Validates the installed plugins with Apple's auval and Tracktion's pluginval.
#
#   tests/validate_plugin.sh [build-dir]      (default: ./build)
#   PLUGINVAL_STRICTNESS=10 tests/validate_plugin.sh
#
# Build and install first:
#   cmake --build build --target ClaudeHostFX_All ClaudeHostInst_All
set -euo pipefail

BUILD_DIR="${1:-build}"
STRICTNESS="${PLUGINVAL_STRICTNESS:-5}"
PLUGINVAL_VERSION="v1.0.4"
TOOLS_DIR="$BUILD_DIR/tools"
PLUGINVAL="$TOOLS_DIR/pluginval.app/Contents/MacOS/pluginval"

COMPONENTS="$HOME/Library/Audio/Plug-Ins/Components"
VST3S="$HOME/Library/Audio/Plug-Ins/VST3"
PLUGINS=(
    "$COMPONENTS/Claude Host FX.component"
    "$VST3S/Claude Host FX.vst3"
    "$COMPONENTS/Claude Host Instrument.component"
    "$VST3S/Claude Host Instrument.vst3"
)
# auval type, subtype, manufacturer for each AU
AU_CODES=("aufx Chfx Clde" "aumu Chin Clde")

failures=0
step() { printf '\n=== %s\n' "$*"; }

for p in "${PLUGINS[@]}"; do
    [[ -e "$p" ]] || { echo "Missing $p (build ClaudeHostFX_All and ClaudeHostInst_All first)"; exit 1; }
done

# Only our own AUs, by code. Never scan every AU: macOS then checks each
# installed component and pops up dialogs for unsigned third-party ones.
killall -9 AudioComponentRegistrar 2>/dev/null || true
for codes in "${AU_CODES[@]}"; do
    step "auval $codes"
    log="$BUILD_DIR/auval-${codes// /_}.log"
    if auval -v $codes > "$log" 2>&1 && tail -3 "$log" | grep -q "AU VALIDATION SUCCEEDED"; then
        echo "auval $codes: PASS"
    else
        echo "auval $codes: FAIL (see $log)"; failures=$((failures + 1))
    fi
done

if [[ ! -x "$PLUGINVAL" ]]; then
    step "Downloading pluginval $PLUGINVAL_VERSION"
    mkdir -p "$TOOLS_DIR"
    curl -fsSL -o "$TOOLS_DIR/pluginval.zip" \
        "https://github.com/Tracktion/pluginval/releases/download/$PLUGINVAL_VERSION/pluginval_macOS.zip"
    unzip -qo "$TOOLS_DIR/pluginval.zip" -d "$TOOLS_DIR"
fi

for p in "${PLUGINS[@]}"; do
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
