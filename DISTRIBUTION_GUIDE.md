# Guide de Distribution Sp3ctra VST

Ce guide explique comment distribuer et installer le plugin Sp3ctra VST via Git, **sans avoir besoin de recompiler**.

## 📦 Pour les Développeurs : Préparer une Release

### 1. Compiler le projet en mode Release

```bash
./scripts/build_vst.sh clean
```

Le script de build va automatiquement :
- ✅ Compiler le VST3, AU et Standalone en mode Release
- ✅ Créer des archives ZIP dans le dossier `prebuilt/`
- ✅ Préparer les archives pour être commitées dans Git

### 2. Vérifier les binaires

```bash
ls -la prebuilt/VST3/
ls -la prebuilt/AU/
ls -la prebuilt/Standalone/
```

Vous devriez voir :
- `prebuilt/VST3/Sp3ctra.vst3/`
- `prebuilt/AU/Sp3ctra.component/`
- `prebuilt/Standalone/Sp3ctra.app/`

### 3. Commiter et pusher

```bash
git add prebuilt/
git commit -m "chore: update prebuilt binaries to version X.Y.Z"
git push
```

Les binaires sont maintenant disponibles dans le dépôt Git pour distribution.

---

## 🎵 Pour les Utilisateurs : Installation Rapide

### Méthode 1 : Installation Automatique (Recommandée)

```bash
# Cloner le dépôt
git clone git@github.com:Ondulab/Sp3ctra_VST.git
cd Sp3ctra_VST

# Installer le VST
./scripts/install_vst.sh
```

Cette commande installe automatiquement :
- **VST3** dans `~/Library/Audio/Plug-Ins/VST3/`
- **AU** dans `/Library/Audio/Plug-Ins/Components/` (nécessite sudo)

### Méthode 2 : Installation Manuelle

```bash
# VST3
cp -R prebuilt/VST3/Sp3ctra.vst3 ~/Library/Audio/Plug-Ins/VST3/

# Audio Unit (nécessite sudo)
sudo cp -R prebuilt/AU/Sp3ctra.component /Library/Audio/Plug-Ins/Components/

# Standalone (peut être lancé directement)
open prebuilt/Standalone/Sp3ctra.app
```

### Méthode 3 : Installation Sélective

```bash
# Installer seulement le VST3
./scripts/install_vst.sh vst3

# Installer seulement l'Audio Unit
./scripts/install_vst.sh au

# Afficher l'emplacement du Standalone
./scripts/install_vst.sh standalone
```

---

## 🔄 Mise à Jour

Pour mettre à jour vers une nouvelle version :

```bash
cd Sp3ctra_VST
git pull
./scripts/install_vst.sh
```

Les anciens plugins seront automatiquement remplacés.

---

## ❓ FAQ

### Q: Dois-je installer Xcode ou des dépendances pour utiliser le VST ?

**Non !** Les binaires pré-compilés dans `prebuilt/` fonctionnent directement sur macOS. Vous n'avez besoin d'aucun outil de développement.

### Q: Quelle architecture est supportée ?

Les binaires sont compilés en **Universal Binary**, supportant :
- Apple Silicon (ARM64) - M1/M2/M3
- Intel (x86_64)

### Q: Quelle version minimale de macOS est requise ?

**macOS 10.13 (High Sierra)** ou supérieur.

### Q: Puis-je utiliser le plugin sans cloner tout le dépôt ?

Techniquement oui, mais le dépôt contient aussi les fichiers de configuration nécessaires (`sp3ctra.ini`, `midi_mapping.ini`). Il est recommandé de cloner le dépôt complet.

### Q: Comment vérifier que le plugin est bien installé ?

```bash
# Vérifier VST3
ls -la ~/Library/Audio/Plug-Ins/VST3/Sp3ctra.vst3

# Vérifier AU
ls -la /Library/Audio/Plug-Ins/Components/Sp3ctra.component
```

Ensuite, relancez votre DAW et rescannez les plugins. Cherchez "Sp3ctra" par "Ondulab".

---

## 🛠️ Pour les Développeurs : Workflow Complet

### Développement

```bash
# Build en mode Debug avec sanitizers
./scripts/build_vst.sh debug run

# Build Release + Installation locale
./scripts/build_vst.sh install
```

### Distribution

```bash
# Build Release + Copie dans prebuilt/
./scripts/build_vst.sh clean

# Vérifier les binaires
ls -la prebuilt/

# Commit et push
git add prebuilt/
git commit -m "chore: update prebuilt binaries"
git push
```

### Avantages de cette approche

✅ **Pas de recompilation** : Les utilisateurs peuvent installer directement  
✅ **Historique Git** : Chaque version est trackée dans Git  
✅ **Déploiement simple** : Un simple `git pull` pour mettre à jour  
✅ **Compatible CI/CD** : Peut être automatisé avec GitHub Actions  
✅ **Taille optimisée** : Seuls les binaires Release sont distribués  

---

## 📝 Notes Techniques

### Pourquoi stocker les binaires dans Git ?

1. **Simplicité** : Pas besoin de système de releases GitHub complexe
2. **Traçabilité** : Chaque commit de code a son binaire correspondant
3. **Accessibilité** : Un simple `git clone` suffit pour tout avoir

### Pourquoi des archives ZIP ?

Les bundles macOS (.vst3, .component, .app) sont en réalité des **répertoires** contenant des liens symboliques et des structures complexes. Git a des limitations avec ces structures :

1. **Liens symboliques** : Git les stocke comme pointeurs texte (quelques Ko seulement)
2. **Métadonnées macOS** : Peuvent être perdues lors du clone
3. **Structure complexe** : Les bundles peuvent ne pas être transférés correctement

**Solution** : Les archives ZIP préservent parfaitement :
- ✅ Tous les liens symboliques
- ✅ Les métadonnées macOS
- ✅ La structure complète du bundle
- ✅ Compression = réduction de la taille Git

### Taille du dépôt

Les archives ZIP sont compressées (quelques MB). Le `.gitignore` est configuré pour :
- ❌ Ignorer les builds temporaires (`vst/build/`)
- ❌ Ignorer les bundles extraits (`prebuilt/VST3/`, `prebuilt/AU/`, etc.)
- ✅ Tracker uniquement les archives ZIP (`prebuilt/*.zip`)

### Sécurité

Les binaires sont compilés sur une machine de confiance et signés par l'équipe Ondulab. Vérifiez toujours la source du dépôt Git avant installation.

---

## 🚀 Quick Start

```bash
# Installation en 3 commandes
git clone git@github.com:Ondulab/Sp3ctra_VST.git
cd Sp3ctra_VST
./scripts/install_vst.sh
```

C'est tout ! Relancez votre DAW et profitez de Sp3ctra 🎵
