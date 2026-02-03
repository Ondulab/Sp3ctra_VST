# Dépannage CMake - Sp3ctra VST

Ce guide résout les problèmes de compilation CMake courants, notamment le blocage sur "detecting CXX compile features".

## Problème : CMake bloque sur "Detecting CXX compile features"

### Symptômes
- La compilation s'arrête pendant la phase de configuration CMake
- Message affiché : `-- Detecting CXX compile features`
- Le processus ne progresse plus pendant plusieurs minutes

### Causes probables

1. **Compilateur C++ manquant ou inaccessible**
2. **Xcode Command Line Tools non installés (macOS)**
3. **Licence Xcode non acceptée (macOS)**
4. **Conflit de chemins du compilateur**
5. **Cache CMake corrompu**

---

## Solutions (macOS)

### Solution 1 : Installer les dépendances automatiquement

Exécutez le script d'installation fourni :

```bash
bash scripts/install_dependencies.sh
```

Ce script va :
- ✅ Vérifier et installer Xcode Command Line Tools
- ✅ Installer/mettre à jour CMake
- ✅ Vérifier que le compilateur est accessible
- ✅ Installer les dépendances audio (optionnel)

---

### Solution 2 : Installation manuelle Xcode Command Line Tools

Si le script échoue, installez manuellement :

```bash
# Installer Xcode Command Line Tools
xcode-select --install
```

Une fenêtre de dialogue apparaîtra. Suivez les instructions d'installation.

**Après l'installation, vérifiez :**

```bash
# Vérifier l'installation
xcode-select -p
# Devrait afficher : /Library/Developer/CommandLineTools

# Vérifier le compilateur
clang++ --version
# Devrait afficher la version de clang
```

---

### Solution 3 : Accepter la licence Xcode

Si Xcode est installé mais la licence n'est pas acceptée :

```bash
sudo xcodebuild -license accept
```

---

### Solution 4 : Réinitialiser Xcode Command Line Tools

Si le compilateur n'est toujours pas détecté :

```bash
# Réinitialiser les outils
sudo xcode-select --reset

# Puis réinstaller
xcode-select --install
```

---

### Solution 5 : Nettoyer le cache CMake

Si CMake a déjà tenté de configurer le projet :

```bash
# Supprimer le cache CMake
cd vst
rm -rf build/
mkdir build
cd build

# Reconfigurer
cmake ..
```

---

### Solution 6 : Forcer le compilateur

Si CMake ne détecte pas automatiquement le compilateur :

```bash
cd vst/build

# Spécifier explicitement le compilateur
cmake .. -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++

# Ou avec le chemin complet Xcode
cmake .. \
  -DCMAKE_C_COMPILER=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang++
```

---

## Solutions (Linux - Debian/Ubuntu)

### Solution 1 : Installer les dépendances automatiquement

```bash
bash scripts/install_dependencies.sh
```

---

### Solution 2 : Installation manuelle

```bash
# Mettre à jour les paquets
sudo apt update

# Installer build-essential (inclut gcc/g++)
sudo apt install -y build-essential

# Installer CMake
sudo apt install -y cmake

# Vérifier le compilateur
g++ --version

# Vérifier CMake
cmake --version
```

---

### Solution 3 : Installer une version plus récente de CMake

Si votre version de CMake est trop ancienne (< 3.15) :

```bash
# Désinstaller l'ancienne version
sudo apt remove cmake

# Ajouter le dépôt Kitware (CMake officiel)
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | \
  gpg --dearmor - | \
  sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null

# Ajouter le dépôt à sources.list
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ focal main' | \
  sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null

# Installer CMake récent
sudo apt update
sudo apt install -y cmake

# Vérifier la version
cmake --version
```

---

## Diagnostic complet

### Vérifier l'environnement de compilation

Exécutez ces commandes pour diagnostiquer votre environnement :

```bash
# 1. Vérifier le système d'exploitation
uname -a

# 2. Vérifier CMake
cmake --version

# 3. Vérifier le compilateur C++
which clang++    # macOS
which g++        # Linux
clang++ --version   # macOS
g++ --version       # Linux

# 4. Vérifier Xcode (macOS uniquement)
xcode-select -p

# 5. Tester une compilation simple
echo 'int main() { return 0; }' > test.cpp
clang++ test.cpp -o test   # macOS
g++ test.cpp -o test       # Linux
./test && echo "Compilation OK" || echo "Compilation FAILED"
rm test.cpp test

# 6. Vérifier les variables d'environnement
echo $PATH
echo $CXX
echo $CC
```

---

## Procédure complète de dépannage

### Étape 1 : Nettoyer complètement

```bash
# Supprimer tous les fichiers de build
cd /Users/zhonx/Documents/Workspaces/Workspace_Ondulab/Sp3ctra_VST
rm -rf vst/build/
rm -rf build/
```

### Étape 2 : Installer les dépendances

```bash
# Exécuter le script d'installation
bash scripts/install_dependencies.sh
```

### Étape 3 : Vérifier l'installation

Le script affichera un rapport de vérification. Assurez-vous que :
- ✅ CMake version >= 3.15
- ✅ Compilateur C++ détecté (clang++ ou g++)
- ✅ Git installé

### Étape 4 : Compiler le projet

```bash
# Option A : Utiliser le script de build
bash scripts/build_vst.sh

# Option B : Build manuel
cd vst
mkdir -p build && cd build
cmake ..
cmake --build . --config Release
```

---

## Messages d'erreur courants

### "No CMAKE_CXX_COMPILER could be found"

**Solution :** Le compilateur n'est pas installé ou pas dans le PATH.

```bash
# macOS
xcode-select --install

# Linux
sudo apt install build-essential
```

---

### "CMake Error: your CXX compiler is not able to compile a simple test program"

**Causes possibles :**
1. Licence Xcode non acceptée (macOS)
2. Bibliothèques système manquantes
3. Compilateur corrompu

**Solutions :**

```bash
# macOS : Accepter la licence
sudo xcodebuild -license accept

# macOS : Réinstaller Command Line Tools
sudo rm -rf /Library/Developer/CommandLineTools
xcode-select --install

# Linux : Réinstaller build-essential
sudo apt install --reinstall build-essential
```

---

### Timeout sur "Detecting CXX compile features"

**Causes :**
- Problème de réseau (si CMake essaie de télécharger des dépendances)
- Compilateur non fonctionnel
- Processus bloqué

**Solutions :**

```bash
# 1. Tuer les processus CMake bloqués
killall cmake

# 2. Nettoyer le cache
rm -rf vst/build/

# 3. Vérifier que le compilateur fonctionne
clang++ --version  # Doit afficher une version sans erreur

# 4. Relancer avec mode verbose
cd vst/build
cmake .. --debug-output
```

---

## Compilation réussie

Lorsque CMake se configure correctement, vous verrez :

```
-- The CXX compiler identification is AppleClang X.X.X  (ou GNU X.X.X sur Linux)
-- Detecting CXX compiler ABI info
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: /usr/bin/clang++ - works
-- Detecting CXX compile features
-- Detecting CXX compile features - done
```

Ensuite, JUCE sera téléchargé automatiquement :

```
-- Fetching JUCE...
-- JUCE framework downloaded successfully
```

---

## Support supplémentaire

Si le problème persiste après avoir suivi ce guide :

1. **Collectez les informations de diagnostic** :
   ```bash
   cmake --version > diagnostic.txt
   clang++ --version >> diagnostic.txt  # ou g++ --version
   xcode-select -p >> diagnostic.txt    # macOS uniquement
   uname -a >> diagnostic.txt
   ```

2. **Vérifiez les logs CMake** :
   ```bash
   cd vst/build
   cat CMakeFiles/CMakeError.log
   cat CMakeFiles/CMakeOutput.log
   ```

3. **Partagez ces informations** avec votre équipe de développement

---

## Résumé des commandes rapides

### Diagnostic rapide (macOS)

```bash
# Vérification complète en une commande
bash scripts/install_dependencies.sh
```

### Diagnostic rapide (Linux)

```bash
# Vérification complète en une commande
bash scripts/install_dependencies.sh
```

### Nettoyage et recompilation

```bash
# Nettoyer
rm -rf vst/build/

# Recompiler
bash scripts/build_vst.sh
```

---

**Note :** Le framework JUCE est téléchargé automatiquement par CMake via `FetchContent`. Aucune installation manuelle de JUCE n'est nécessaire.
