# 🚀 Quick Start - Sp3ctra VST

## Install Without Compiling (Users)

```bash
# 1. Clone the repository
git clone git@github.com:Ondulab/Sp3ctra_VST.git
cd Sp3ctra_VST

# 2. Install the plugins
./scripts/install_vst.sh

# 3. Restart your DAW and scan for plugins
# Look for "Sp3ctra" by "Ondulab"
```

**That's it!** No compilation required. ✨

---

## Building and Distribution (Developers)

### 1. Build in Release mode

```bash
./scripts/build_vst.sh clean
```

This command:
- ✅ Builds VST3, AU and Standalone
- ✅ Automatically creates ZIP archives in `prebuilt/`
- ✅ Prepares the archives for Git distribution

### 2. Commit the binaries

```bash
git add prebuilt/
git commit -m "chore: update prebuilt binaries v1.0.0"
git push
```

### 3. Users can now install

```bash
git pull
./scripts/install_vst.sh
```

---

## Advanced Options

### Selective install

```bash
./scripts/install_vst.sh vst3        # VST3 only
./scripts/install_vst.sh au          # AU only
./scripts/install_vst.sh standalone  # Standalone info
```

### Build and test immediately

```bash
./scripts/build_vst.sh run           # Build + launch standalone
./scripts/build_vst.sh install       # Build + install
./scripts/build_vst.sh debug run     # Debug + launch
```

---

## Structure

```
Sp3ctra_VST/
├── prebuilt/              # Pre-compiled binaries (tracked by Git)
│   ├── VST3/
│   │   └── Sp3ctra.vst3/
│   ├── AU/
│   │   └── Sp3ctra.component/
│   └── Standalone/
│       └── Sp3ctra.app/
├── scripts/
│   ├── build_vst.sh       # Building
│   └── install_vst.sh     # Installation
└── DISTRIBUTION_GUIDE.md  # Full guide
```

---

## Full Documentation

- **[DISTRIBUTION_GUIDE.md](DISTRIBUTION_GUIDE.md)** - Detailed distribution guide
- **[README.md](README.md)** - Main project documentation
- **[prebuilt/README.md](prebuilt/README.md)** - Info about the binaries

---

## Support

For any questions: https://github.com/Ondulab/Sp3ctra_VST/issues
