# Binaires Pré-Compilés Sp3ctra VST

Ce dossier contient les versions pré-compilées du plugin Sp3ctra pour macOS, **sous forme d'archives ZIP**.

## Structure

```
prebuilt/
├── Sp3ctra-VST3.zip        # Archive du plugin VST3
├── Sp3ctra-AU.zip          # Archive du plugin Audio Unit
└── Sp3ctra-Standalone.zip  # Archive de l'application standalone
```

> **Note** : Les archives ZIP résolvent les problèmes de liens symboliques avec Git et garantissent un transfert complet des binaires.

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
