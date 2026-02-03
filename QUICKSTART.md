# 🚀 Installation Rapide - Sp3ctra VST

## Installation Sans Compilation (Utilisateurs)

```bash
# 1. Cloner le dépôt
git clone git@github.com:Ondulab/Sp3ctra_VST.git
cd Sp3ctra_VST

# 2. Installer les plugins
./scripts/install_vst.sh

# 3. Relancer votre DAW et scanner les plugins
# Cherchez "Sp3ctra" par "Ondulab"
```

**C'est tout !** Aucune compilation nécessaire. ✨

---

## Compilation et Distribution (Développeurs)

### 1. Compiler en mode Release

```bash
./scripts/build_vst.sh clean
```

Cette commande :
- ✅ Compile VST3, AU et Standalone
- ✅ Copie automatiquement les binaires dans `prebuilt/`
- ✅ Prépare pour distribution Git

### 2. Commiter les binaires

```bash
git add prebuilt/
git commit -m "chore: update prebuilt binaries v1.0.0"
git push
```

### 3. Les utilisateurs peuvent maintenant installer

```bash
git pull
./scripts/install_vst.sh
```

---

## Options Avancées

### Installation sélective

```bash
./scripts/install_vst.sh vst3        # VST3 uniquement
./scripts/install_vst.sh au          # AU uniquement
./scripts/install_vst.sh standalone  # Info Standalone
```

### Build et test immédiat

```bash
./scripts/build_vst.sh run           # Build + Lance standalone
./scripts/build_vst.sh install       # Build + Installe
./scripts/build_vst.sh debug run     # Debug + Lance
```

---

## Structure

```
Sp3ctra_VST/
├── prebuilt/              # Binaires pré-compilés (trackés par Git)
│   ├── VST3/
│   │   └── Sp3ctra.vst3/
│   ├── AU/
│   │   └── Sp3ctra.component/
│   └── Standalone/
│       └── Sp3ctra.app/
├── scripts/
│   ├── build_vst.sh       # Compilation
│   └── install_vst.sh     # Installation
└── DISTRIBUTION_GUIDE.md  # Guide complet
```

---

## Documentation Complète

- **[DISTRIBUTION_GUIDE.md](DISTRIBUTION_GUIDE.md)** - Guide détaillé de distribution
- **[README.md](README.md)** - Documentation principale du projet
- **[prebuilt/README.md](prebuilt/README.md)** - Info sur les binaires

---

## Support

Pour toute question : https://github.com/Ondulab/Sp3ctra_VST/issues
