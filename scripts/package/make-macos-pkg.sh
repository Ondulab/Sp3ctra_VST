#!/bin/bash
# Build the macOS .pkg installer: one package with three checkboxes
# (Standalone → /Applications, VST3 + AU → /Library/Audio/Plug-Ins).
#
# Usage: make-macos-pkg.sh <artefacts-dir> <version> <output.pkg>
#   artefacts-dir : .../Sp3ctraVST_artefacts/Release (contains Standalone/ VST3/ AU/)
#
# The pkg is NOT codesigned/notarized (same status as the zips): first launch
# of the installer needs right-click > Open on machines other than the build
# host. Signing is a separate release concern.
set -euo pipefail

art="$1"; ver="$2"; out="$3"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# pkgbuild enables bundle relocation by default (would follow a moved copy
# instead of installing to the declared location) — force it off.
build_component() { # <name> <bundle-path> <install-location> <pkg-id>
  local name="$1" bundle="$2" loc="$3" id="$4"
  local root="$work/root-$name"
  mkdir -p "$root"
  cp -R "$bundle" "$root/"
  pkgbuild --analyze --root "$root" "$work/$name.plist" >/dev/null
  python3 - "$work/$name.plist" <<'PY'
import plistlib, sys
path = sys.argv[1]
with open(path, 'rb') as f:
    items = plistlib.load(f)
def pin(bundles):
    for b in bundles:
        b['BundleIsRelocatable'] = False
        pin(b.get('ChildBundles', []))
pin(items)
with open(path, 'wb') as f:
    plistlib.dump(items, f)
PY
  pkgbuild --root "$root" --component-plist "$work/$name.plist" \
    --identifier "$id" --version "$ver" \
    --install-location "$loc" "$work/$name.pkg" >/dev/null
}

build_component standalone "$art/Standalone/Sp3ctra.app" \
  /Applications com.ondulab.sp3ctra.pkg.standalone
build_component vst3 "$art/VST3/Sp3ctra.vst3" \
  /Library/Audio/Plug-Ins/VST3 com.ondulab.sp3ctra.pkg.vst3
build_component au "$art/AU/Sp3ctra.component" \
  /Library/Audio/Plug-Ins/Components com.ondulab.sp3ctra.pkg.au

cp "$repo/LICENSE" "$work/license.txt"

cat > "$work/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>Sp3ctra $ver</title>
    <license file="license.txt"/>
    <options customize="always" rootVolumeOnly="true" hostArchitectures="arm64"/>
    <domains enable_localSystem="true"/>
    <choices-outline>
        <line choice="standalone"/>
        <line choice="vst3"/>
        <line choice="au"/>
    </choices-outline>
    <choice id="standalone" title="Standalone application"
            description="Sp3ctra.app, installed in /Applications">
        <pkg-ref id="com.ondulab.sp3ctra.pkg.standalone"/>
    </choice>
    <choice id="vst3" title="VST3 plug-in"
            description="Sp3ctra.vst3, installed in /Library/Audio/Plug-Ins/VST3">
        <pkg-ref id="com.ondulab.sp3ctra.pkg.vst3"/>
    </choice>
    <choice id="au" title="Audio Unit plug-in"
            description="Sp3ctra.component, installed in /Library/Audio/Plug-Ins/Components">
        <pkg-ref id="com.ondulab.sp3ctra.pkg.au"/>
    </choice>
    <pkg-ref id="com.ondulab.sp3ctra.pkg.standalone" version="$ver">standalone.pkg</pkg-ref>
    <pkg-ref id="com.ondulab.sp3ctra.pkg.vst3" version="$ver">vst3.pkg</pkg-ref>
    <pkg-ref id="com.ondulab.sp3ctra.pkg.au" version="$ver">au.pkg</pkg-ref>
</installer-gui-script>
XML

productbuild --distribution "$work/distribution.xml" \
  --package-path "$work" --resources "$work" "$out"
