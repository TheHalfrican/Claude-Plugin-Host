#!/usr/bin/env bash
# Copies the Claude Bridge Remote Script into Live's User Library.
# Then, in Live: Settings > Link, Tempo & MIDI > Control Surface > ClaudeBridge.
# After updating an installed bridge, restart Live to load the new code.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
dest="${LIVE_REMOTE_SCRIPTS:-$HOME/Music/Ableton/User Library/Remote Scripts}/ClaudeBridge"

mkdir -p "$dest"
rsync -a --delete --exclude "__pycache__" "$here/bridge/ClaudeBridge/" "$dest/"
echo "Installed Claude Bridge to: $dest"
