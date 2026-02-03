# Analyse du problème de sample rate (96kHz - 50% buffer miss)

## 🔍 État actuel

### Comportement observé
- À **48kHz**: Fonctionne correctement ✅ (~0% buffer miss)
- À **96kHz**: Exactement **50% de buffer miss** ❌

### Cause identifiée

Le problème vient de l'architecture **synchrone 1:1** du mode VST:

```c
// Mode VST: Synchronisation producteur/consommateur forcée
luxstral_wait_for_buffer_consumed();  // BLOQUE jusqu'à consommation
```

Cette synchronisation garantit qu'un buffer est généré APRÈS que le précédent soit consommé. Quand la synthèse prend plus de temps que le budget audio:

**À 48kHz/128 samples:**
- Budget: **2667 µs**
- Synthèse: ~2200 µs
- Budget > Synthèse → **OK** (synthèse finit AVANT le prochain callback)

**À 96kHz/128 samples:**
- Budget: **1333 µs** 
- Synthèse: ~2200 µs
- Budget < Synthèse → **50% MISS GARANTI**

### Pourquoi exactement 50%?

Le pattern est:
1. Callback 1: buffer prêt → OK + signal consumed
2. Synthèse commence (2.2ms)
3. Callback 2 (1.3ms): synthèse pas finie → **MISS**
4. Synthèse finit → buffer prêt
5. Callback 3: buffer prêt → OK + signal consumed
6. etc.

= 1 OK, 1 MISS, 1 OK, 1 MISS = **50% exact**

## ⚠️ Tentative de fix (REVERTÉE)

Une tentative de supprimer la synchronisation bloquante a été faite, mais elle a causé des problèmes à 48kHz car le thread de synthèse ne recevait plus les signaux de synchronisation correctement.

Le code a été **remis à l'état original** pour garantir le fonctionnement à 48kHz.

## 🔧 Solutions possibles (non implémentées)

### Option 1: Limiter le sample rate max à 48kHz
- Simple et fiable
- Pas de perte de qualité audible

### Option 2: Augmenter le buffer size recommandé à 96kHz
- À 96kHz, utiliser 256+ samples au lieu de 128
- Budget 256@96kHz = 2667µs ≈ Budget 128@48kHz

### Option 3: Optimiser la synthèse (long terme)
- Profiler et optimiser les hot spots
- Réduire le temps de synthèse sous 1333µs
- Nécessite un travail significatif

### Option 4: Architecture triple buffering
- Découpler complètement producteur/consommateur
- Permet au thread de synthèse de "prendre de l'avance"
- Plus complexe à implémenter

## 📝 Recommandations utilisateur

Pour l'instant, **à 96kHz, augmentez le buffer size** dans les préférences audio du DAW:

| Sample Rate | Buffer Size recommandé |
|-------------|------------------------|
| 44.1 kHz    | 128+ samples           |
| 48 kHz      | 128+ samples           |
| 88.2 kHz    | 256+ samples           |
| 96 kHz      | **256+ samples**       |

## 📌 Notes techniques

### Fichier clé
`src/threading/multithreading.c` - Fonction `audioProcessingThread()`

### Synchronisation VST actuelle
```c
#ifdef VST_MODE
    luxstral_wait_for_buffer_consumed();  // Wait for processBlock() to signal
    if (!context->audio_thread_running) break;
#endif
```

### Standalone (fonctionne à toutes les fréquences)
```c
#ifndef VST_MODE
    usleep(100); // 0.1ms polling - no blocking
#endif
```

La version standalone utilise un polling non-bloquant et n'a pas ce problème. La différence est que le mode standalone a un contrôle direct sur le driver audio via RtAudio, tandis que le mode VST doit s'adapter au rythme du DAW.
