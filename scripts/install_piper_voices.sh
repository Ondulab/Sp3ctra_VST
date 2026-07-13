#!/bin/bash

################################################################################
# Sp3ctra VST - Piper voice installer (VOICE module)
#
# Downloads a starter set of Piper VITS voices (sherpa-onnx vits-piper-*
# bundles: <id>.onnx + tokens.txt + espeak-ng-data/) into the folder the
# VOICE module scans at runtime:
#   ~/Library/Application Support/Sp3ctra/piper_voices/
#
# Each bundle is ~60-65 MB; the full starter set is ~250 MB.
# Voice zoo: https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models
#
# Usage: bash scripts/install_piper_voices.sh [extra-bundle-name ...]
################################################################################

set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

BASE_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models"
VOICES_DIR="$HOME/Library/Application Support/Sp3ctra/piper_voices"

# Starter set: French + English (matches the tts_voices corpus languages).
VOICES=(
    "vits-piper-fr_FR-siwis-medium"     # fr, female — reference French voice
    "vits-piper-fr_FR-tom-medium"       # fr, male
    "vits-piper-en_US-lessac-medium"    # en-US — Piper's reference voice
    "vits-piper-en_GB-alba-medium"      # en-GB, female
)

# Extra bundles can be passed as arguments.
VOICES+=("$@")

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Sp3ctra — Piper voice installer${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "Target: ${GREEN}$VOICES_DIR${NC}"
echo ""

mkdir -p "$VOICES_DIR"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

INSTALLED=0
SKIPPED=0
FAILED=0

for name in "${VOICES[@]}"; do
    if [ -d "$VOICES_DIR/$name" ]; then
        echo -e "${GREEN}✓${NC} $name already installed"
        SKIPPED=$((SKIPPED + 1))
        continue
    fi

    echo -e "${YELLOW}⇩${NC} Downloading $name ..."
    if curl -fL --progress-bar -o "$TMP_DIR/$name.tar.bz2" "$BASE_URL/$name.tar.bz2"; then
        tar xjf "$TMP_DIR/$name.tar.bz2" -C "$VOICES_DIR"
        rm -f "$TMP_DIR/$name.tar.bz2"
        echo -e "${GREEN}✓${NC} $name installed"
        INSTALLED=$((INSTALLED + 1))
    else
        echo -e "${RED}✗${NC} $name download failed (check the name on the tts-models release page)"
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo -e "${BLUE}========================================${NC}"
echo -e "Installed: ${GREEN}$INSTALLED${NC}   Already present: ${GREEN}$SKIPPED${NC}   Failed: ${RED}$FAILED${NC}"
du -sh "$VOICES_DIR" 2>/dev/null | awk '{print "Total size: " $1}'
echo -e "${BLUE}========================================${NC}"

[ "$FAILED" -eq 0 ] || exit 1
