# Optimisation de la compilation parallèle

## Résumé

Tous les scripts de build ont été optimisés pour utiliser **tous les cœurs CPU disponibles** lors de la compilation.

## Modifications effectuées

### ✅ `scripts/build_vst.sh`
**Statut:** Déjà optimisé (aucune modification nécessaire)

```bash
cmake --build . --config $BUILD_CONFIG -j$(sysctl -n hw.ncpu)
```

- Utilise `-j$(sysctl -n hw.ncpu)` pour détecter et utiliser tous les cœurs sur macOS
- Fonctionne correctement depuis le début

### ✅ `scripts/build/build.sh`
**Statut:** Modifié pour ajouter le support macOS

**Avant:**
```bash
# Determine parallel jobs (Linux only)
if [ "$UNAME_S" = "Linux" ]; then
    JOBS=$(nproc 2>/dev/null || echo 2)
    echo -e "${CYAN}Using $JOBS parallel jobs${NC}"
    MAKE_JOBS="-j$JOBS"
else
    MAKE_JOBS=""  # ❌ Pas de parallélisation sur macOS !
fi
```

**Après:**
```bash
# Determine parallel jobs
if [ "$UNAME_S" = "Linux" ]; then
    JOBS=$(nproc 2>/dev/null || echo 2)
    echo -e "${CYAN}Using $JOBS parallel jobs${NC}"
    MAKE_JOBS="-j$JOBS"
elif [ "$UNAME_S" = "Darwin" ]; then
    JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 2)
    echo -e "${CYAN}Using $JOBS parallel jobs${NC}"
    MAKE_JOBS="-j$JOBS"
else
    MAKE_JOBS=""
fi
```

## Commandes utilisées

### macOS
```bash
sysctl -n hw.ncpu
```
Retourne le nombre total de cœurs CPU (physiques + logiques).

### Linux
```bash
nproc
```
Retourne le nombre de processeurs disponibles.

## Impact sur les performances

### Avant l'optimisation
- **macOS:** Compilation séquentielle (1 seul cœur) ⏱️ Lent
- **Linux:** Compilation parallèle (tous les cœurs) ⚡ Rapide

### Après l'optimisation
- **macOS:** Compilation parallèle (tous les cœurs) ⚡ Rapide
- **Linux:** Compilation parallèle (tous les cœurs) ⚡ Rapide

### Gain estimé
Sur une machine avec 8 cœurs :
- **Temps de compilation:** Réduit de ~70-80% 🚀
- **Exemple:** 3 minutes → ~45 secondes

## Utilisation

### Build VST (CMake)
```bash
./scripts/build_vst.sh
```
Utilise automatiquement tous les cœurs disponibles.

### Build standalone (Make)
```bash
./scripts/build/build.sh
```
Utilise automatiquement tous les cœurs disponibles sur macOS et Linux.

## Option pour limiter les cœurs (optionnel)

Si vous souhaitez limiter le nombre de cœurs utilisés (par exemple, pour éviter de surcharger votre système pendant le développement), vous pouvez modifier manuellement la valeur :

### Pour CMake (build_vst.sh)
```bash
# Au lieu de -j$(sysctl -n hw.ncpu), utilisez un nombre fixe
cmake --build . --config $BUILD_CONFIG -j4  # Limite à 4 cœurs
```

### Pour Make (build/build.sh)
```bash
# Au lieu de -j$JOBS, utilisez un nombre fixe
make -j4 $BUILD_TARGET  # Limite à 4 cœurs
```

## Vérification

Pour vérifier combien de cœurs sont utilisés lors de la compilation :

### macOS
```bash
# Afficher le nombre de cœurs
sysctl -n hw.ncpu

# Pendant la compilation, surveiller l'activité CPU
top -l 1 | grep "CPU usage"
```

### Linux
```bash
# Afficher le nombre de cœurs
nproc

# Pendant la compilation, surveiller l'activité CPU
htop
```

## Notes techniques

### Conformité aux règles du projet
✅ Conforme à `.clinerules/custom_instructions.md`
- Pas de modifications du code source C/C++
- Optimisation du processus de build uniquement
- Amélioration de l'efficacité du développement

### Sécurité
- Fallback sur 2 cœurs si la détection échoue
- Aucun risque de surcharge système (le système d'exploitation gère automatiquement la priorité)

### Portabilité
- ✅ macOS: `sysctl -n hw.ncpu`
- ✅ Linux: `nproc`
- ✅ Autres systèmes: Fallback sur compilation séquentielle

## Références

- [GNU Make Manual - Parallel Execution](https://www.gnu.org/software/make/manual/html_node/Parallel.html)
- [CMake --build documentation](https://cmake.org/cmake/help/latest/manual/cmake.1.html#build-tool-mode)
- [sysctl man page (macOS)](https://www.unix.com/man-page/osx/8/sysctl/)
- [nproc man page (Linux)](https://man7.org/linux/man-pages/man1/nproc.1.html)

---

**Date:** 2026-03-02  
**Auteur:** Assistant Cline  
**Version:** 1.0
