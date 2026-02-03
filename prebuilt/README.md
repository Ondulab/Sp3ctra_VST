# Binaires Pré-Compilés Sp3ctra VST

Ce dossier contient les versions pré-compilées du plugin Sp3ctra pour macOS.

## Structure

```
prebuilt/
├── VST3/
│   └── Sp3ctra.vst3/      # Plugin VST3
├── AU/
│   └── Sp3ctra.component/  # Plugin Audio Unit
└── Standalone/
    └── Sp3ctra.app/        # Application standalone
```

## Installation Rapide

Pour installer le VST sans recompiler :

```bash
./scripts/install_vst.sh
```

Ce script copiera automatiquement les plugins dans les emplacements système appropriés.

## Emplacements d'Installation

- **VST3** : `~/Library/Audio/Plug-Ins/VST3/Sp3ctra.vst3`
- **AU** : `/Library/Audio/Plug-Ins/Components/Sp3ctra.component` (nécessite sudo)
- **Standalone** : Peut être lancé directement depuis `prebuilt/Standalone/Sp3ctra.app`

## Notes

- Les binaires sont compilés en mode **Release** avec optimisations `-O2`
- Architecture : **Apple Silicon (ARM64)** et **Intel (x86_64)** (Universal Binary)
- Version macOS minimale : **10.13**
- Ces binaires sont automatiquement générés par le script `scripts/build_vst.sh`

## Mise à Jour

Après avoir compilé une nouvelle version avec `./scripts/build_vst.sh`, les binaires dans ce dossier sont automatiquement mis à jour et peuvent être commités dans Git.
