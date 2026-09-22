#!/bin/sh
# Launch the local standalone with opt-in render/presentation timing.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
app="$root/build-mac/Sp3ctraVST_artefacts/Release/Standalone/Sp3ctra.app/Contents/MacOS/Sp3ctra"
if [ ! -x "$app" ]; then
    app="$root/build/Sp3ctraVST_artefacts/Release/Standalone/Sp3ctra.app/Contents/MacOS/Sp3ctra"
fi
if [ ! -x "$app" ]; then
    echo "Compile the Sp3ctraVST_Standalone target first." >&2
    exit 1
fi
log=$(mktemp "${TMPDIR:-/tmp}/sp3ctra-video-timing.XXXXXX")
echo "VIDEO MIX diagnostic log: $log"
echo "Reproduce the 3 Hz / 8 Hz automation, then quit Sp3ctra."
SP3CTRA_VIDEO_TIMING=1 "$app" 2>&1 | tee "$log"
