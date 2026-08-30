#!/usr/bin/env bash
# Guard the Sp3ctra Link wire contract: the VST copy of sp3ctra_link.h must be
# byte-identical to the firmware's Common/Inc/sp3ctra_link.h.
#   scripts/check_link_header.sh          # compare (exit 1 when they differ)
#   scripts/check_link_header.sh --sync   # copy the firmware header over the VST one
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VST="$ROOT/vst/source/communication/link/sp3ctra_link.h"
FW="${SP3CTRA_FW_ROOT:-$ROOT/../Sp3ctra_CIS_Firmware}/Common/Inc/sp3ctra_link.h"

if [ ! -f "$FW" ]; then
    echo "firmware header not found: $FW (set SP3CTRA_FW_ROOT)"; exit 2
fi
if [ "${1:-}" = "--sync" ]; then
    cp "$FW" "$VST"; echo "synced: $VST"; exit 0
fi
if cmp -s "$FW" "$VST"; then
    echo "sp3ctra_link.h in sync ($(shasum -a 256 "$VST" | cut -c1-16)...)"
else
    echo "sp3ctra_link.h DIFFERS between firmware and VST:"; diff -u "$FW" "$VST" | head -40; exit 1
fi
