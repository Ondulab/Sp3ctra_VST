# Sp3ctra VST Distribution Guide

This guide explains how to distribute and install the Sp3ctra VST plugin via Git, **without needing to recompile**.

## 📦 For Developers: Preparing a Release

### 1. Build the project in Release mode

```bash
./scripts/build_vst.sh clean
```

The build script will automatically:
- ✅ Build VST3, AU and Standalone in Release mode
- ✅ Create ZIP archives in the `prebuilt/` folder
- ✅ Prepare the archives to be committed into Git

### 2. Check the binaries

```bash
ls -la prebuilt/VST3/
ls -la prebuilt/AU/
ls -la prebuilt/Standalone/
```

You should see:
- `prebuilt/VST3/Sp3ctra.vst3/`
- `prebuilt/AU/Sp3ctra.component/`
- `prebuilt/Standalone/Sp3ctra.app/`

### 3. Commit and push

```bash
git add prebuilt/
git commit -m "chore: update prebuilt binaries to version X.Y.Z"
git push
```

The binaries are now available in the Git repository for distribution.

---

## 🎵 For Users: Quick Install

### Method 1: Automatic Install (Recommended)

```bash
# Clone the repository
git clone git@github.com:Ondulab/Sp3ctra_VST.git
cd Sp3ctra_VST

# Install the VST
./scripts/install_vst.sh
```

This command automatically installs:
- **VST3** into `~/Library/Audio/Plug-Ins/VST3/`
- **AU** into `/Library/Audio/Plug-Ins/Components/` (requires sudo)

### Method 2: Manual Install

```bash
# VST3
cp -R prebuilt/VST3/Sp3ctra.vst3 ~/Library/Audio/Plug-Ins/VST3/

# Audio Unit (requires sudo)
sudo cp -R prebuilt/AU/Sp3ctra.component /Library/Audio/Plug-Ins/Components/

# Standalone (can be launched directly)
open prebuilt/Standalone/Sp3ctra.app
```

### Method 3: Selective Install

```bash
# Install VST3 only
./scripts/install_vst.sh vst3

# Install the Audio Unit only
./scripts/install_vst.sh au

# Show the Standalone location
./scripts/install_vst.sh standalone
```

---

## 🔄 Updating

To update to a new version:

```bash
cd Sp3ctra_VST
git pull
./scripts/install_vst.sh
```

The old plugins will be replaced automatically.

---

## ❓ FAQ

### Q: Do I need to install Xcode or any dependencies to use the VST?

**No!** The pre-compiled binaries in `prebuilt/` work directly on macOS. You don't need any development tools.

### Q: Which architecture is supported?

The binaries are built as a **Universal Binary**, supporting:
- Apple Silicon (ARM64) - M1/M2/M3
- Intel (x86_64)

### Q: What minimum macOS version is required?

**macOS 10.13 (High Sierra)** or later.

### Q: Can I use the plugin without cloning the whole repository?

Technically yes, but the repository also contains the necessary configuration files (`sp3ctra.ini`, `midi_mapping.ini`). Cloning the full repository is recommended.

### Q: How do I check that the plugin is properly installed?

```bash
# Check VST3
ls -la ~/Library/Audio/Plug-Ins/VST3/Sp3ctra.vst3

# Check AU
ls -la /Library/Audio/Plug-Ins/Components/Sp3ctra.component
```

Then restart your DAW and rescan the plugins. Look for "Sp3ctra" by "Ondulab".

---

## 🛠️ For Developers: Full Workflow

### Development

```bash
# Build in Debug mode with sanitizers
./scripts/build_vst.sh debug run

# Release build + local install
./scripts/build_vst.sh install
```

### Distribution

```bash
# Release build + copy into prebuilt/
./scripts/build_vst.sh clean

# Check the binaries
ls -la prebuilt/

# Commit and push
git add prebuilt/
git commit -m "chore: update prebuilt binaries"
git push
```

### Benefits of this approach

✅ **No recompilation**: Users can install directly  
✅ **Git history**: Every version is tracked in Git  
✅ **Simple deployment**: A single `git pull` to update  
✅ **CI/CD friendly**: Can be automated with GitHub Actions  
✅ **Optimized size**: Only the Release binaries are distributed  

---

## 📝 Technical Notes

### Why store the binaries in Git?

1. **Simplicity**: No need for a complex GitHub releases system
2. **Traceability**: Every code commit has its matching binary
3. **Accessibility**: A simple `git clone` is enough to get everything

### Why ZIP archives?

macOS bundles (.vst3, .component, .app) are actually **directories** containing symbolic links and complex structures. Git has limitations with these structures:

1. **Symbolic links**: Git stores them as text pointers (only a few KB)
2. **macOS metadata**: Can be lost during a clone
3. **Complex structure**: Bundles may not be transferred correctly

**Solution**: ZIP archives perfectly preserve:
- ✅ All symbolic links
- ✅ macOS metadata
- ✅ The complete bundle structure
- ✅ Compression = reduced Git size

### Repository size

The ZIP archives are compressed (a few MB). The `.gitignore` is configured to:
- ❌ Ignore temporary builds (`vst/build/`)
- ❌ Ignore extracted bundles (`prebuilt/VST3/`, `prebuilt/AU/`, etc.)
- ✅ Track only the ZIP archives (`prebuilt/*.zip`)

### Security

The binaries are built on a trusted machine and signed by the Ondulab team. Always verify the source of the Git repository before installing.

---

## 🚀 Quick Start

```bash
# Install in 3 commands
git clone git@github.com:Ondulab/Sp3ctra_VST.git
cd Sp3ctra_VST
./scripts/install_vst.sh
```

That's it! Restart your DAW and enjoy Sp3ctra 🎵

---

## ⚖️ License compliance (GPLv3) — mandatory for every release

Sp3ctra is distributed under **GPLv3-or-later** (espeak-ng, statically linked in
the VOICE module's TTS engine, imposes it on the combined work; JUCE is used
under AGPLv3, the VST3 SDK under GPLv3). See [LICENSE](LICENSE) and
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Checklist per binary release:

1. **Corresponding source**: the public repository at the release tag IS the
   corresponding source of the Sp3ctra code. Also attach the `sherpa-onnx
   v1.13.4` source archive (contains espeak-ng) to the release — do not rely on
   upstream's future availability alone.
2. **Files bundled with the binaries**: `LICENSE` and `THIRD-PARTY-NOTICES.md`
   must accompany every binary archive (the `prebuilt/` zips live in the
   repository, so they already travel with it — keep that true if the binaries
   are ever distributed outside the repository).
3. **No additional restrictions**: do not add any EULA/terms that would
   contradict the GPLv3 on the distributed binary.
4. **Embedded Piper voices**: the voices listed in the CMake option
   `SP3CTRA_EMBED_VOICES` are copied into each format's
   `Contents/Resources/piper_voices/` and are therefore REDISTRIBUTED (~79 MB
   per voice per format — watch the size of the `prebuilt/` zips). Only include
   voices with a clear license (defaults: siwis CC-BY 4.0, ljspeech public
   domain — see THIRD-PARTY-NOTICES.md; never lessac/Blizzard). Other voices
   remain external via `scripts/install_piper_voices.sh` + the configurable
   folder on the VOICE page.
5. **macOS Gatekeeper** (license-independent): for distribution without a
   quarantine warning, sign with Developer ID + notarize all three formats.
