# Documentation Technique : LuxSynth & LuxWave

> Extraction et analyse du code legacy en vue de l'intégration VST3/JUCE  
> Version : 1.0 — Avril 2026

---

## Table des matières

1. [Vue d'ensemble](#1-vue-densemble)
2. [Infrastructure partagée](#2-infrastructure-partagée)
   - 2.1 [ADSR Envelope (`synth_common.h`)](#21-adsr-envelope)
   - 2.2 [LFO (`synth_common.h`)](#22-lfo-low-frequency-oscillator)
   - 2.3 [Voice Manager (`voice_manager.h/.c`)](#23-voice-manager)
3. [LuxSynth](#3-luxsynth)
   - 3.1 [Concept & principe de synthèse](#31-concept--principe-de-synthèse)
   - 3.2 [Architecture interne](#32-architecture-interne)
   - 3.3 [Pipeline audio (processBlock)](#33-pipeline-audio-processblock)
   - 3.4 [Gestion de la polyphonie](#34-gestion-de-la-polyphonie)
   - 3.5 [Paramètres exposés](#35-paramètres-exposés)
   - 3.6 [Valeurs par défaut et limites](#36-valeurs-par-défaut-et-limites)
   - 3.7 [Flux de données — Image → Audio](#37-flux-de-données--image--audio)
4. [LuxWave](#4-luxwave)
   - 4.1 [Concept & principe de synthèse](#41-concept--principe-de-synthèse)
   - 4.2 [Architecture interne](#42-architecture-interne)
   - 4.3 [Pipeline audio (processBlock)](#43-pipeline-audio-processblock)
   - 4.4 [Modes de scan](#44-modes-de-scan)
   - 4.5 [Filtre passe-bas](#45-filtre-passe-bas)
   - 4.6 [Gestion de la polyphonie](#46-gestion-de-la-polyphonie)
   - 4.7 [Paramètres exposés](#47-paramètres-exposés)
   - 4.8 [Valeurs par défaut et limites](#48-valeurs-par-défaut-et-limites)
   - 4.9 [Flux de données — Image → Audio](#49-flux-de-données--image--audio)
5. [Comparaison LuxSynth vs LuxWave](#5-comparaison-luxsynth-vs-luxwave)
6. [Contraintes RT et thread-safety](#6-contraintes-rt-et-thread-safety)
7. [Guide d'intégration VST3/JUCE](#7-guide-dintégration-vst3juce)

---

## 1. Vue d'ensemble

Sp3ctra embarque deux moteurs de synthèse audio basés sur l'image :

| Moteur       | Principe                                              | Domaine source                       | Nature du son                             |
|--------------|-------------------------------------------------------|--------------------------------------|-------------------------------------------|
| **LuxSynth** | Synthèse additive spectrale via FFT                   | Spectre fréquentiel de l'image       | Harmonique riche, contrôlable par couleur |
| **LuxWave**  | Lecture de forme d'onde optique (wavetable dynamique) | Niveaux de luminance pixel par pixel | Timbral, direct, organique                |

Les deux moteurs partagent :
- La même infrastructure ADSR (volume + filtre)
- Le même LFO sinusoïdal (vibrato)
- Le même gestionnaire de voix (`voice_manager`)
- Le même accordage MIDI standard (A4 = 440 Hz, 12-TET)
- La même architecture double-buffer producteur/consommateur

---

## 2. Infrastructure partagée

### 2.1 ADSR Envelope

**Fichier :** `legacy_old/synthesis/common/synth_common.h`

#### Structure `AdsrEnvelope`

```c
typedef struct {
    AdsrState state;               // État courant
    float attack_time_samples;     // Durée d'attaque en samples
    float decay_time_samples;      // Durée de décroissance en samples
    float sustain_level;           // Niveau de sustain (0.0–1.0)
    float release_time_samples;    // Durée de relâchement en samples
    float current_output;          // Sortie courante (0.0–1.0)
    long long current_samples;     // Compteur interne
    float attack_increment;        // Incrément par sample (attaque)
    float decay_decrement;         // Décrément par sample (décroissance)
    float release_decrement;       // Décrément par sample (relâchement)
    float attack_s;                // Paramètre temps original (s)
    float decay_s;                 // Paramètre temps original (s)
    float release_s;               // Paramètre temps original (s)
} AdsrEnvelope;
```

#### États (`AdsrState`)

```
IDLE → ATTACK → DECAY → SUSTAIN → RELEASE → IDLE
```

- **IDLE** : voix silencieuse, aucun calcul
- **ATTACK** : montée linéaire de 0 à 1 en `attack_time_samples`
- **DECAY** : descente linéaire de 1 à `sustain_level` en `decay_time_samples`
- **SUSTAIN** : valeur constante = `sustain_level` (tant que la note est tenue)
- **RELEASE** : descente linéaire depuis la valeur courante vers 0 en `release_time_samples`

#### Fonctionnement RT-safe

Tous les temps sont pré-calculés en **samples** à l'initialisation ou lors d'un changement de paramètre. La boucle audio ne fait que des additions/soustractions scalaires → **O(1) par sample**, sans allocation.

---

### 2.2 LFO (Low Frequency Oscillator)

**Fichier :** `legacy_old/synthesis/common/synth_common.h`

#### Structure `LfoState`

```c
typedef struct {
    float phase;             // Phase courante [0, 2π]
    float phase_increment;   // Incrément de phase par sample = 2π × rate_hz / sample_rate
    float current_output;    // Sortie courante [-1.0, +1.0]
    float rate_hz;           // Fréquence du LFO (Hz)
    float depth_semitones;   // Profondeur de modulation (demi-tons)
} LfoState;
```

#### Calcul de la modulation de fréquence

```
freq_modifiée = freq_base × 2^(lfo_output × depth_semitones / 12)
```

- Waveform : **sinusoïde pure**
- Portée : vibrato (modulation de pitch)
- Partagé globalement entre toutes les voix (une seule instance LFO par moteur)
- LuxWave utilise une approximation rapide `1 + 0.693147 × x` pour `|x| < 1`, `powf(2, x/12)` au-delà

---

### 2.3 Voice Manager

**Fichiers :** `legacy_old/synthesis/common/voice_manager.h/.c`

Module générique et agnostique du moteur. Utilisé identiquement par LuxSynth et LuxWave via des **callbacks**.

#### Structure `VoiceMetadata`

```c
typedef struct {
    int midi_note;                 // Note MIDI (-1 = inactive, 0-127 = active)
    unsigned long long trigger_order; // Ordre de déclenchement (pour LRU)
    AdsrState adsr_state;          // État ADSR courant
    float adsr_output;             // Niveau d'enveloppe courant
} VoiceMetadata;
```

#### API

| Fonction                       | Rôle                                                  |
|--------------------------------|-------------------------------------------------------|
| `voice_manager_allocate()`     | Alloue une voix selon un système à 3 priorités        |
| `voice_manager_release()`      | Libère une voix avec gestion de la période de grâce   |
| `voice_manager_cleanup_idle()` | Nettoie les voix IDLE (prévention des notes bloquées) |

#### Algorithme d'allocation (3 priorités)

1. **Priorité 1** : Trouver une voix à l'état IDLE (aucune interruption)
2. **Priorité 2** : Voler la voix en RELEASE ayant le niveau d'enveloppe le plus bas (la plus silencieuse)
3. **Priorité 3** : Voler la voix ACTIVE ayant le plus ancien `trigger_order` (LRU — Least Recently Used)

#### Algorithme de relâchement (Note Off avec période de grâce)

1. Chercher la voix ACTIVE la plus ancienne avec cette note
2. Chercher une voix en RELEASE avec cette note (Note Off tardif)
3. Chercher une voix IDLE avec cette note (Note Off très tardif — période de grâce)

> **Note importante :** Le `midi_note` n'est **pas** effacé immédiatement après un Note Off. Il est conservé pour permettre la gestion des Note Off tardifs (race condition MIDI), et écrasé uniquement lors du prochain `Note On` sur cette voix.

---

## 3. LuxSynth

### 3.1 Concept & principe de synthèse

**LuxSynth** est un moteur de **synthèse additive spectrale** piloté par image.

Le principe fondamental :

> Une ligne d'image CIS (scanner linéaire) est analysée par **FFT (Fast Fourier Transform)** afin d'extraire son spectre de fréquences. Les magnitudes spectrales résultantes deviennent les **amplitudes** des partiels d'un oscillateur additif polyphonique. La **couleur** des pixels module la **nature harmonique** des partiels (harmonique, semi-harmonique ou inharmonique).

En termes musicaux, LuxSynth est une sorte de **vocoder inversé** ou de **resynthèse additive spectrale** :
- L'image encode un timbre
- Le MIDI encode la hauteur
- Le résultat est un son additivement synthétisé dont le timbre évolue en temps réel avec l'image

**Analogie :** Proche d'un synthétiseur à spectre variable (type Kawai K5000 ou Kyma), où l'utilisateur dessine un spectre — sauf qu'ici le spectre est calculé automatiquement à partir d'une image physique.

---

### 3.2 Architecture interne

```
┌─────────────────────────────────────────────────────────┐
│                     THREAD UDP                          │
│  Image CIS → Preprocessing → FFT → magnitudes[]        │
│             → left_gains[], right_gains[]               │
│             → harmonicity[], detune_cents[]             │
│             → inharmonic_ratios[]                       │
└──────────────────────────┬──────────────────────────────┘
                           │ DoubleBuffer (lock-free)
┌──────────────────────────▼──────────────────────────────┐
│                  THREAD LUXSYNTH (RT prio 80)           │
│  read_preprocessed_fft_magnitudes()                     │
│  synth_luxsynthMode_process()                           │
│    → LFO → per-voice loop → per-oscillator loop        │
│    → spectral panning → ADSR volume + filtre            │
│    → double buffer L/R                                  │
└──────────────────────────┬──────────────────────────────┘
                           │ polyphonic_audio_buffers[]
┌──────────────────────────▼──────────────────────────────┐
│                  AUDIO CALLBACK (RT)                    │
│  Consomme les buffers L/R → sortie audio               │
└─────────────────────────────────────────────────────────┘
```

**Séparation des responsabilités :**
- Le **calcul de la FFT** est fait dans le thread UDP (hors RT), en pré-traitement
- Le **thread LuxSynth** (priorité RT 80) lit les magnitudes pré-calculées et génère l'audio
- Le **callback audio** consomme les buffers pré-remplis (zéro calcul lourd en RT)

---

### 3.3 Pipeline audio (processBlock)

Fonction : `synth_luxsynthMode_process(float *left, float *right, unsigned int buffer_size)`

#### Boucle par sample

```
Pour chaque sample :
  1. lfo_output = lfo_process(&global_vibrato_lfo)
  
  Pour chaque voix [0..g_num_poly_voices-1] :
    2. volume_adsr_val = adsr_get_output(&voice.volume_adsr)
    3. filter_adsr_val = adsr_get_output(&voice.filter_adsr)
    4. Mise à jour état IDLE si enveloppe terminée
    5. Skip si volume < 0.00001 et état IDLE
    
    6. modulated_cutoff = base_cutoff + filter_adsr × env_depth
       → clamp [20 Hz, sr/2]
    
    7. freq_mod_factor = 2^(lfo_output × depth_semitones / 12)
    8. actual_freq = voice.fundamental_frequency × freq_mod_factor
    
    9. Calcul max_harmonics adaptatif (CPU) :
       - freq > high_freq_limit : max_harmonics / 2
       - freq > high_freq_limit/2 : max_harmonics
    
    Pour chaque oscillateur [0..max_harmonics-1] :
      10. Calcul harmonic_multiple selon harmonicity[osc_idx] :
          - h > 0.7 (couleurs chaudes) → série harmonique + légère désaccordage
          - h ∈ [0.3, 0.7] (couleurs neutres) → semi-harmonique (effet piano)
          - h < 0.3 (couleurs froides) → ratio inharmonique (effet cloche/perc.)
      
      11. osc_freq = actual_freq × harmonic_multiple
      12. Nyquist check : break si osc_freq ≥ sr/2
      
      13. amplitude = global_smoothed_magnitudes[osc_idx]
      14. Skip si amplitude < min_audible (mais mise à jour de la phase)
      
      15. phase_increment = 2π × osc_freq / sr
      16. amplitude_gamma = amplitude^poly_amplitude_gamma
      
      17. Filtre spectral (passe-bas 1er ordre) :
          attenuation = 1 / sqrt(1 + (osc_freq/cutoff)^2)
      
      18. final_amplitude = amplitude_gamma × attenuation
      19. Si final_amplitude > min_audible :
          osc_sample = final_amplitude × sin(phase)
          voice_left  += osc_sample × stereo_left_gains[osc_idx]
          voice_right += osc_sample × stereo_right_gains[osc_idx]
      
      20. phase += phase_increment (avec wrap 2π)
    
    21. voice_left  × volume_adsr × velocity
    22. voice_right × volume_adsr × velocity
    
    master_left  += voice_left
    master_right += voice_right
  
  23. master_left  × poly_master_volume, clip [-1, 1]
  24. master_right × poly_master_volume, clip [-1, 1]
  25. out_left[i] = master_left
  26. out_right[i] = master_right
```

---

### 3.4 Gestion de la polyphonie

- **Polyphonie :** jusqu'à **32 voix** (compile-time `MAX_POLY_VOICES`), configurable à l'exécution via `g_num_poly_voices` (défaut : 8)
- **Oscillateurs par voix :** jusqu'à **256** (compile-time `MAX_MAPPED_OSCILLATORS`), configurable via `g_max_mapped_oscillators` (défaut : 128)
- Voice stealing : via `voice_manager` (3 priorités)
- ADSR indépendante par voix (volume + filtre)

#### Structure `SynthVoice`

```c
typedef struct {
    OscillatorState oscillators[MAX_MAPPED_OSCILLATORS]; // Phase de chaque partiel
    volatile float fundamental_frequency;  // Fréquence fondamentale (Hz)
    AdsrState voice_state;                 // État global de la voix
    int midi_note_number;                  // Note MIDI jouée
    AdsrEnvelope volume_adsr;              // Enveloppe de volume
    AdsrEnvelope filter_adsr;             // Enveloppe de filtre
    float last_velocity;                   // Vélocité normalisée [0, 1]
    unsigned long long last_triggered_order; // Ordre de déclenchement (LRU)
    uint64_t release_start_timestamp_us;   // Timestamp de début de release
    int pending_note_off;                  // Flag Note Off en attente
} SynthVoice;
```

---

### 3.5 Paramètres exposés

#### Volume ADSR

| Paramètre                        | Type  | Plage      | Défaut config |
|----------------------------------|-------|------------|---------------|
| `poly_volume_adsr_attack_s`      | float | ≥ 0 s      | depuis `.ini` |
| `poly_volume_adsr_decay_s`       | float | ≥ 0 s      | depuis `.ini` |
| `poly_volume_adsr_sustain_level` | float | [0.0, 1.0] | depuis `.ini` |
| `poly_volume_adsr_release_s`     | float | ≥ 0 s      | depuis `.ini` |

#### Filtre ADSR

| Paramètre                        | Type  | Plage      | Note |
|----------------------------------|-------|------------|------|
| `poly_filter_adsr_attack_s`      | float | ≥ 0 s      | -    |
| `poly_filter_adsr_decay_s`       | float | ≥ 0 s      | -    |
| `poly_filter_adsr_sustain_level` | float | [0.0, 1.0] | -    |
| `poly_filter_adsr_release_s`     | float | ≥ 0 s      | -    |

#### Filtre spectral

| Paramètre                  | Type  | Plage         | Note                                    |
|----------------------------|-------|---------------|-----------------------------------------|
| `poly_filter_cutoff_hz`    | float | [20 Hz, sr/2] | Fréquence de coupure de base            |
| `poly_filter_env_depth_hz` | float | ±∞ Hz         | Modulation par ADSR (peut être négatif) |

Le filtre spectral dans LuxSynth est implémenté **par partiel**, non sur le signal mixé :
```
attenuation[k] = 1 / sqrt(1 + (freq_k / cutoff_modulée)^2)
```
C'est un filtre passe-bas 1er ordre (Butterworth -6 dB/oct) appliqué harmonique par harmonique.

#### LFO (Vibrato)

| Paramètre                  | Type  | Plage        | Défaut        |
|----------------------------|-------|--------------|---------------|
| `poly_lfo_rate_hz`         | float | ≥ 0 Hz       | depuis `.ini` |
| `poly_lfo_depth_semitones` | float | ±∞ demi-tons | depuis `.ini` |

#### Paramètres de synthèse

| Paramètre                          | Type  | Description                                                   |
|------------------------------------|-------|---------------------------------------------------------------|
| `poly_master_volume`               | float | Volume maître global                                          |
| `poly_amplitude_gamma`             | float | Correction gamma sur les amplitudes (ex: 0.5 = racine carrée) |
| `poly_min_audible_amplitude`       | float | Seuil d'amplitude minimale (optimisation CPU)                 |
| `poly_high_freq_harmonic_limit_hz` | float | Seuil de réduction des harmoniques hautes fréquences          |

#### Paramètres structurels

| Paramètre              | Type | Plage    | Défaut |
|------------------------|------|----------|--------|
| `poly_num_voices`      | int  | [1, 32]  | 8      |
| `poly_max_oscillators` | int  | [1, 256] | 128    |

---

### 3.6 Valeurs par défaut et limites

```c
#define MAX_POLY_VOICES          32
#define MAX_MAPPED_OSCILLATORS   256
#define DEFAULT_FUNDAMENTAL_FREQUENCY  440.0f   // A4

// Defaults config (depuis synth_luxsynth.c)
g_num_poly_voices        = 8;    // configurable
g_max_mapped_oscillators = 128;  // configurable

// Stereo gains initiaux
global_stereo_left_gains[i]  = 0.707f;  // centre (-3 dB)
global_stereo_right_gains[i] = 0.707f;  // centre (-3 dB)
```

---

### 3.7 Flux de données — Image → Audio

```
Ligne image CIS (pixels bruts)
    │
    ▼ [Thread UDP - pré-traitement hors RT]
FFT (KissFFT real FFT)
    │
    ├── magnitudes[0..255]         → amplitudes des partiels
    ├── left_gains[0..255]         → panoramique spectral gauche
    ├── right_gains[0..255]        → panoramique spectral droit
    ├── harmonicity[0..255]        → caractère harmonique (de la température couleur)
    ├── detune_cents[0..255]       → désaccordage en cents
    └── inharmonic_ratios[0..255]  → ratios fréquentiels inharmoniques
    │
    ▼ [DoubleBuffer thread-safe]
global_smoothed_magnitudes[]
global_stereo_*_gains[]
global_harmonicity[], global_detune_cents[], global_inharmonic_ratios[]
    │
    ▼ [Thread LuxSynth RT]
Pour chaque voix active :
  Pour chaque partiel k :
    freq_k = fondamentale × harmonic_multiple(k, harmonicity[k])
    amp_k  = magnitudes[k]^gamma × filtre_spectral(freq_k, cutoff_modulée)
    sample += amp_k × sin(phase_k) × {left/right}_gain[k]
  sample × ADSR_volume × vélocité
    │
    ▼
Buffer stéréo L/R (polyphonic_audio_buffers)
    │
    ▼ [Audio Callback RT]
Sortie audio
```

---

## 4. LuxWave

### 4.1 Concept & principe de synthèse

**LuxWave** est un moteur de **wavetable dynamique optique** — ou plus précisément de **transduction spatiale→temporelle**.

Le principe fondamental :

> Les valeurs de luminance des pixels d'une ligne d'image CIS sont lues **directement comme des samples audio**. La ligne entière constitue une période complète d'une forme d'onde. La vitesse de lecture de cette forme d'onde est déterminée par la fréquence MIDI. La direction et le mode de balayage définissent la nature timbrale du son.

En termes musicaux, LuxWave est un **wavetable synthesizer** où :
- La **wavetable** est la ligne d'image (mise à jour en temps réel)
- La **hauteur** est contrôlée par MIDI (vitesse de lecture)
- Le **timbre** est directement la forme d'onde issue de la lumière réfléchie par le papier

**Analogie :** Proche d'un PPG Wave ou d'un Waldorf Blofeld (lecture cyclique d'une forme d'onde), mais la forme d'onde est fournie par le scanner et change avec chaque ligne scannée.

---

### 4.2 Architecture interne

```
┌─────────────────────────────────────────────────────────┐
│                  THREAD IMAGE                           │
│  Scanner CIS → ligne de pixels (uint8_t[pixel_count])  │
└──────────────────────────┬──────────────────────────────┘
                           │ pointeur image_line (RT-safe)
┌──────────────────────────▼──────────────────────────────┐
│                THREAD LUXWAVE (RT prio 80)              │
│  synth_luxwave_thread_func()                            │
│    → synth_luxwave_process()                           │
│    → LFO → per-voice loop → scan image → filtre        │
│    → mono buffer (L=R)                                 │
└──────────────────────────┬──────────────────────────────┘
                           │ photowave_audio_buffers[]
┌──────────────────────────▼──────────────────────────────┐
│                  AUDIO CALLBACK (RT)                    │
│  Consomme le buffer mono → sortie L et R identiques    │
└─────────────────────────────────────────────────────────┘
```

**Remarque :** LuxWave génère un signal **mono** (L = R). Contrairement à LuxSynth qui génère du stéréo via le panoramique spectral.

---

### 4.3 Pipeline audio (processBlock)

Fonction : `synth_luxwave_process(LuxWaveState *state, float *left, float *right, int num_frames)`

#### Boucle par sample

```
Pré-calcul :
  sample_rate_inv = 1 / sample_rate
  amplitude = config.amplitude
  scan_mode = config.scan_mode
  phase_mult = (DUAL) ? 2.0 : 1.0

Pour chaque sample i :
  1. Calcul du LFO (vibrato) :
     lfo_output = sin(lfo.phase)
     lfo.phase += phase_increment
     
     Si depth > 0.001 :
       Si |semitone_shift| < 1 : lfo_freq_ratio ≈ 1 + 0.693147 × shift/12  [approx rapide]
       Sinon : lfo_freq_ratio = 2^(shift/12)
  
  Pour chaque voix v [0..NUM_LUXWAVE_VOICES-1] :
    2. vol_adsr = adsr_get_output(&voice.volume_adsr)
    3. filt_adsr = adsr_get_output(&voice.filter_adsr)
    
    4. Si vol_adsr < MIN_AUDIBLE_AMPLITUDE et état IDLE → skip, voice.active = false
    
    5. modulated_freq = voice.frequency × lfo_freq_ratio
    
    6. modulated_cutoff = lowpass.base_cutoff + filt_adsr × lowpass.filter_env_depth
       → clamp [20 Hz, sr/2]
    
    7. Lecture de la forme d'onde (linéaire) :
       raw_sample = sample_waveform_linear(image_line, pixel_count, voice.phase, scan_mode)
    
    8. Filtre passe-bas 1 pôle (Euler forward) :
       rc = 1 / (2π × modulated_cutoff)
       α  = dt / (rc + dt)        [α recalculé par sample]
       filtered = α × raw + (1-α) × prev_output
    
    9. velocity_scale = voice.velocity / 127.0
       final_sample = filtered × vol_adsr × velocity_scale
    
    master_sum += final_sample
    
    10. phase_incr = modulated_freq × sample_rate_inv × phase_mult
        voice.phase += phase_incr
        Si voice.phase ≥ 1.0 : voice.phase -= 1.0   [wrap dans [0, 1]]
  
  11. master_sum × amplitude
      → clip [-1, 1]
  12. output_left[i] = output_right[i] = master_sum
```

---

### 4.4 Modes de scan

La position de lecture dans la forme d'onde (image_line) est déterminée par `phase ∈ [0, 1]` selon le mode :

| Mode             | Enum                         | Description                             | Formule position                                                               |
|------------------|------------------------------|-----------------------------------------|--------------------------------------------------------------------------------|
| Left→Right       | `LUXWAVE_SCAN_LEFT_TO_RIGHT` | Lecture standard gauche→droite          | `pos = phase × (pixel_count - 1)`                                              |
| Right→Left       | `LUXWAVE_SCAN_RIGHT_TO_LEFT` | Lecture inversée droite→gauche          | `pos = (1 - phase) × (pixel_count - 1)`                                        |
| Dual / Ping-Pong | `LUXWAVE_SCAN_DUAL`          | Aller-retour (double période effective) | Si phase < 0.5 : `pos = (phase×2) × (N-1)` sinon `pos = ((1-phase)×2) × (N-1)` |

**Mode Dual :** Le `phase_mult = 2.0` est appliqué sur l'incrément de phase → à fréquence MIDI identique, la vitesse de balayage est doublée mais la forme d'onde fait un aller-retour complet, ce qui produit des harmoniques supplémentaires (symétrie de la forme d'onde).

#### Contrôle MIDI CC

Le mode de scan peut être contrôlé via **CC1 (Modulation Wheel)** :
- CC1 ∈ [0, 42] → Left→Right
- CC1 ∈ [43, 84] → Right→Left  
- CC1 ∈ [85, 127] → Dual

---

### 4.5 Filtre passe-bas

LuxWave implémente un **filtre RC 1 pôle (one-pole IIR)** par voix, avec modulation de coupure par l'ADSR filtre.

#### Structure `LuxWaveLowpassFilter`

```c
typedef struct {
    float base_cutoff_hz;       // Fréquence de coupure de base (sans modulation)
    float filter_env_depth;     // Profondeur de modulation ADSR (Hz, peut être négatif)
    float prev_output;          // Sample précédent (état du filtre)
    float alpha;                // Coefficient de lissage (recalculé dynamiquement)
} LuxWaveLowpassFilter;
```

#### Formule

```
rc    = 1 / (2π × cutoff_modulée)
dt    = 1 / sample_rate
α     = dt / (rc + dt)
y[n]  = α × x[n] + (1 - α) × y[n-1]
```

**Caractéristiques :**
- Pente : -6 dB/octave (1er ordre)
- α recalculé par sample (permettant la modulation dynamique de cutoff)
- État du filtre (`prev_output`) réinitialisé à 0 à chaque Note On
- Valeurs par défaut : cutoff = 12 000 Hz, env_depth = -6 000 Hz

---

### 4.6 Gestion de la polyphonie

- **Polyphonie :** **8 voix** fixe (`NUM_LUXWAVE_VOICES = 8`)
- Voice stealing : via `voice_manager` (même algorithme que LuxSynth)
- ADSR indépendante par voix (volume + filtre)
- Phase réinitialisée à 0 à chaque Note On

#### Structure `LuxWaveVoice`

```c
typedef struct {
    float phase;                    // Position dans la wavetable [0, 1]
    float frequency;                // Fréquence de lecture (Hz)
    uint8_t midi_note;              // Note MIDI (0-127)
    uint8_t velocity;               // Vélocité MIDI (0-127)
    bool active;                    // Voix active ou non
    unsigned long long trigger_order; // Ordre LRU
    AdsrEnvelope volume_adsr;       // Enveloppe de volume
    AdsrEnvelope filter_adsr;       // Enveloppe de filtre
    LuxWaveLowpassFilter lowpass;   // Filtre passe-bas (avec état)
} LuxWaveVoice;
```

#### Calcul de fréquence (MIDI → Hz)

```c
frequency = 440.0 × 2^((note - 69) / 12)
```
Clampée dans `[f_min, f_max]` où :
- `f_min = sample_rate / pixel_count` (ex: 48000/3456 ≈ 13.9 Hz à 400 DPI)
- `f_max = 12 000 Hz` (constante)

La fréquence minimale est déterminée par la résolution du scanner (DPI) : plus le scanner est résolu (plus de pixels), plus `f_min` est basse.

---

### 4.7 Paramètres exposés

#### Volume ADSR

| Paramètre      | Type  | Plage      | Défaut |
|----------------|-------|------------|--------|
| Volume Attack  | float | ≥ 0 s      | 0.01 s |
| Volume Decay   | float | ≥ 0 s      | 0.1 s  |
| Volume Sustain | float | [0.0, 1.0] | 0.8    |
| Volume Release | float | ≥ 0 s      | 0.2 s  |

#### Filtre ADSR

| Paramètre      | Type  | Plage      | Défaut |
|----------------|-------|------------|--------|
| Filter Attack  | float | ≥ 0 s      | 0.02 s |
| Filter Decay   | float | ≥ 0 s      | 0.2 s  |
| Filter Sustain | float | [0.0, 1.0] | 0.3    |
| Filter Release | float | ≥ 0 s      | 0.3 s  |

#### Filtre

| Paramètre        | Type  | Plage         | Défaut    | Config key                      |
|------------------|-------|---------------|-----------|---------------------------------|
| Filter Cutoff    | float | [20 Hz, sr/2] | 12 000 Hz | `photowave_filter_cutoff_hz`    |
| Filter Env Depth | float | ±∞ Hz         | -6 000 Hz | `photowave_filter_env_depth_hz` |

#### LFO

| Paramètre | Type  | Plage     | Défaut  | Config key                      |
|-----------|-------|-----------|---------|---------------------------------|
| LFO Rate  | float | ≥ 0 Hz    | 5.0 Hz  | `photowave_lfo_rate_hz`         |
| LFO Depth | float | demi-tons | 0.25 st | `photowave_lfo_depth_semitones` |

#### Synthèse / Scan

| Paramètre   | Type  | Valeurs              | Défaut     | Config key              |
|-------------|-------|----------------------|------------|-------------------------|
| Scan Mode   | enum  | 0=L→R, 1=R→L, 2=Dual | 0 (L→R)    | `photowave_scan_mode`   |
| Interp Mode | enum  | 0=Linear, 1=Cubic    | 0 (Linear) | `photowave_interp_mode` |
| Amplitude   | float | [0.0, 1.0]           | 0.5        | `photowave_amplitude`   |

> **Note :** Le mode cubique est défini dans l'enum mais n'est plus implémenté dans le code de traitement (supprimé pour des raisons de performance). Seule l'interpolation linéaire est effective.

#### MIDI CC mappings

| CC                | Fonction    | Plage → Valeur                         |
|-------------------|-------------|----------------------------------------|
| CC1 (Modulation)  | Scan mode   | 0–42 → L→R, 43–84 → R→L, 85–127 → Dual |
| CC7 (Volume)      | Amplitude   | 0–127 → 0.0–1.0                        |
| CC74 (Brightness) | Interp mode | (supprimé)                             |

---

### 4.8 Valeurs par défaut et limites

```c
#define LUXWAVE_MAX_PIXELS        4096
#define LUXWAVE_MIN_FREQUENCY     10.0f
#define LUXWAVE_MAX_FREQUENCY     12000.0f
#define LUXWAVE_DEFAULT_AMPLITUDE 0.5f
#define NUM_LUXWAVE_VOICES        8
#define MIN_AUDIBLE_AMPLITUDE     0.001f

// Fréquence min dynamique
f_min = sample_rate / pixel_count
// ex: 48000 / 3456 = 13.89 Hz  (400 DPI)
// ex: 48000 / 1728 = 27.78 Hz  (200 DPI)
```

---

### 4.9 Flux de données — Image → Audio

```
Ligne image CIS (uint8_t[pixel_count])
   │   Luminance 0–255 par pixel
   │
   ▼ [RT-safe, pointeur simple, mis à jour en dehors du callback]
sample_waveform_linear(image_line, pixel_count, phase, scan_mode)
   │
   │   DC blocking :
   │   mean = Σ pixels / N
   │   sample = (pixel - mean) / 127.5     → centrage sur 0 (suppression DC)
   │   Interpolation linéaire entre pixel[i] et pixel[i+1]
   │
   ▼
raw_sample ∈ [-1.0, +1.0]
   │
   ▼ [Filtre passe-bas 1 pôle par voix]
   y[n] = α × raw + (1-α) × y[n-1]    (α modulé par ADSR filtre)
   │
   ▼
filtered_sample
   │
   ▼ [Enveloppe volume + LFO]
   × ADSR_volume × velocity / 127
   │
   ▼ [Accumulation voices]
   master_sum += voix
   │
   ▼ [Amplitude maître + clip]
   × amplitude, clip [-1, 1]
   │
   ▼
output_left[i] = output_right[i] = master_sum
```

**Point clé — DC Blocking :** LuxWave calcule la **moyenne des pixels** de chaque ligne à chaque lecture et recentre les samples autour de zéro. Ceci est crucial pour éviter la saturation des effets de réverbération (notamment Zita) qui amplifient exponentiellement les composantes DC.

---

## 5. Comparaison LuxSynth vs LuxWave

| Critère                   | LuxSynth                                           | LuxWave                                   |
|---------------------------|----------------------------------------------------|-------------------------------------------|
| **Paradigme**             | Synthèse additive spectrale (FFT)                  | Wavetable dynamique optique               |
| **Domaine source**        | Spectre fréquentiel de l'image                     | Niveaux de luminance pixel par pixel      |
| **Voix**                  | 8 (configurable jusqu'à 32)                        | 8 (fixe)                                  |
| **Oscillateurs par voix** | Jusqu'à 256 partiels additifs                      | 1 oscillateur wavetable                   |
| **Calcul lourd**          | FFT (hors RT, thread UDP)                          | Aucun (lecture directe de pixels)         |
| **Nature sonore**         | Harmonique / spectrale / évolutive                 | Timbrale directe / organique              |
| **Timbre**                | Dérivé du spectre FFT                              | Dérivé de la forme d'onde pixel           |
| **Couleur → son**         | Harmonicity (chaud=harmonique, froid=inharmonique) | Non (luminance brute uniquement)          |
| **Stéréo**                | Oui — panoramique spectral par partiel             | Non — mono (L=R)                          |
| **Filtre**                | Spectral (par partiel, -6 dB/oct)                  | IIR 1 pôle global par voix (-6 dB/oct)    |
| **DC Blocking**           | Non                                                | Oui (suppression de la moyenne)           |
| **CPU**                   | Élevé (N voix × M oscillateurs × sin)              | Faible (8 voix × lecture + filtre simple) |
| **Config section**        | `[poly]` dans `sp3ctra.ini`                        | `[photowave]` dans `sp3ctra.ini`          |
| **Thread priority**       | RT 80                                              | RT 80                                     |
| **Buffer**                | Double buffer stéréo L+R                           | Double buffer mono                        |

---

## 6. Contraintes RT et thread-safety

### Règles appliquées dans les deux moteurs

| Contrainte             | LuxSynth                     | LuxWave                                              |
|------------------------|------------------------------|------------------------------------------------------|
| Pas d'allocation en RT | ✅ (buffers alloués à l'init) | ✅ (sauf les 2 temp buffers du thread, hors callback) |
| Pas de mutex en RT     | ✅                            | ✅                                                    |
| Pas de logging en RT   | ✅                            | ✅                                                    |
| Pas d'exceptions       | ✅ (C pur)                    | ✅ (C pur)                                            |
| Exécution bornée       | ✅ (break sur Nyquist)        | ✅ (boucle fixe)                                      |

### Synchronisation producteur/consommateur

Les deux moteurs utilisent le même pattern de **double buffer lock-free** :

```
Producteur (thread synth) :
  1. Lit l'index courant sous mutex léger
  2. Attend (backoff exponentiel via nanosleep) que ready == 0
  3. Écrit dans le buffer
  4. Pose ready = 1 (atomic store RELEASE)
  5. Bascule l'index

Consommateur (audio callback) :
  1. Lit ready (atomic load ACQUIRE)
  2. Si ready == 1 : copie le buffer, pose ready = 0
  3. Sinon : utilise silence / buffer précédent
```

**Backoff exponentiel** (identique dans les deux moteurs) :
```
Itération 0-4   : sleep 5 µs
Itération 5-19  : sleep 20 µs
Itération 20-99 : sleep 50 µs
Itération 100+  : sleep 100 µs
Timeout à 500 itérations (~50 ms) → warning + dégradation gracieuse
```

### Mise à jour des paramètres ADSR en temps réel

Lorsqu'un paramètre ADSR est modifié pendant qu'une voix est active, la fonction `adsr_update_settings_and_recalculate_rates()` recalcule les taux **en tenant compte de la position courante** dans l'enveloppe pour éviter les sauts brutaux :

- En phase DECAY : recalcule `decay_decrement` à partir du niveau courant et du temps restant
- En phase RELEASE : recalcule `release_decrement` à partir du niveau courant et du temps restant

---

## 7. Guide d'intégration VST3/JUCE

### Architecture recommandée

```
PluginProcessor (juce::AudioProcessor)
│
├── [Mode LuxSynth]
│   ├── DSP Core : synth_luxsynth.c (C pur, aucun type JUCE)
│   ├── Adapter : convertit juce::MidiBuffer → synth_luxsynth_note_on/off()
│   ├── Adapter : convertit juce::AudioBuffer → buffers C
│   └── Params JUCE → synth_luxsynth_set_*()
│
└── [Mode LuxWave]
    ├── DSP Core : synth_luxwave.c (C pur, aucun type JUCE)
    ├── Adapter : convertit juce::MidiBuffer → synth_luxwave_note_on/off()
    ├── Adapter : convertit juce::AudioBuffer → buffers C
    └── Params JUCE → synth_luxwave_set_*()
```

### Paramètres JUCE à créer

#### Communs aux deux moteurs

```cpp
// Volume ADSR (4 params)
addParameter(volumeAttack  = new juce::AudioParameterFloat("VOL_ATK",  "Vol Attack",  0.0f, 10.0f, 0.01f));
addParameter(volumeDecay   = new juce::AudioParameterFloat("VOL_DEC",  "Vol Decay",   0.0f, 10.0f, 0.1f));
addParameter(volumeSustain = new juce::AudioParameterFloat("VOL_SUS",  "Vol Sustain", 0.0f, 1.0f,  0.8f));
addParameter(volumeRelease = new juce::AudioParameterFloat("VOL_REL",  "Vol Release", 0.0f, 10.0f, 0.2f));

// Filter ADSR (4 params)
addParameter(filterAttack  = new juce::AudioParameterFloat("FLT_ATK",  "Flt Attack",  0.0f, 10.0f, 0.02f));
addParameter(filterDecay   = new juce::AudioParameterFloat("FLT_DEC",  "Flt Decay",   0.0f, 10.0f, 0.2f));
addParameter(filterSustain = new juce::AudioParameterFloat("FLT_SUS",  "Flt Sustain", 0.0f, 1.0f,  0.3f));
addParameter(filterRelease = new juce::AudioParameterFloat("FLT_REL",  "Flt Release", 0.0f, 10.0f, 0.3f));

// Filtre
addParameter(filterCutoff   = new juce::AudioParameterFloat("FLT_CUT", "Filter Cutoff",    20.0f, 20000.0f, 12000.0f));
addParameter(filterEnvDepth = new juce::AudioParameterFloat("FLT_ENV", "Filter Env Depth", -12000.0f, 12000.0f, -6000.0f));

// LFO
addParameter(lfoRate  = new juce::AudioParameterFloat("LFO_RATE",  "Vibrato Rate",  0.0f, 20.0f, 5.0f));
addParameter(lfoDepth = new juce::AudioParameterFloat("LFO_DEPTH", "Vibrato Depth", 0.0f, 4.0f,  0.25f));
```

#### Spécifiques LuxSynth

```cpp
addParameter(numVoices    = new juce::AudioParameterInt("POLY_VOICES", "Voices",         1, 32,  8));
addParameter(numOsc       = new juce::AudioParameterInt("POLY_OSC",    "Oscillators",     1, 256, 128));
addParameter(masterVolume = new juce::AudioParameterFloat("POLY_VOL",  "Master Volume",   0.0f, 2.0f, 1.0f));
addParameter(ampGamma     = new juce::AudioParameterFloat("POLY_GAMMA","Amp Gamma",       0.1f, 2.0f, 1.0f));
```

#### Spécifiques LuxWave

```cpp
addParameter(scanMode  = new juce::AudioParameterChoice("LW_SCAN",  "Scan Mode", {"L→R", "R→L", "Dual"}, 0));
addParameter(amplitude = new juce::AudioParameterFloat ("LW_AMP",   "Amplitude", 0.0f, 1.0f, 0.5f));
```

### Appel dans `prepareToPlay()`

```cpp
void PluginProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Charger la config depuis .ini ou valeurs par défaut
    // LuxSynth
    synth_luxsynthMode_init();
    
    // LuxWave
    synth_luxwave_mode_init();  // Appelle synth_luxwave_load_global_params() puis synth_luxwave_init()
}
```

### Appel dans `processBlock()`

```cpp
void PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // 1. Traiter les événements MIDI
    for (const auto metadata : midiMessages) {
        const auto msg = metadata.getMessage();
        if (msg.isNoteOn())
            synth_luxsynth_note_on(msg.getNoteNumber(), msg.getVelocity());
        else if (msg.isNoteOff())
            synth_luxsynth_note_off(msg.getNoteNumber());
    }
    
    // 2. Appliquer les changements de paramètres (depuis le thread de message, pas le RT)
    //    → Utiliser des flags atomiques ou un lock-free queue pour passer les changements
    //    → Exemple : si paramètre changed → synth_luxsynth_set_filter_cutoff(newValue)
    
    // 3. Appeler le process (RT-safe)
    synth_luxsynthMode_process(
        buffer.getWritePointer(0),  // Left
        buffer.getWritePointer(1),  // Right
        buffer.getNumSamples()
    );
}
```

### Points d'attention pour l'intégration

1. **Suppression du double-buffer producteur/consommateur** : L'architecture legacy utilise un thread dédié qui pré-remplit des buffers audio. Dans JUCE VST3, le `processBlock` est appelé directement en RT → les fonctions `*_process()` seront appelées directement dans `processBlock()`, sans thread séparé ni double buffer.

2. **FFT dans LuxSynth** : La FFT doit être déplacée vers `prepareToPlay()` / thread non-RT ou calculée de manière incrémentale. Ne jamais calculer une FFT de taille arbitraire dans `processBlock()`.

3. **Passage des paramètres au thread RT** : Utiliser `std::atomic<float>` ou un lock-free ring buffer pour passer les changements de paramètres depuis le thread GUI/message vers `processBlock()`.

4. **Initialisation du filtre LuxWave** : La fonction `synth_luxwave_load_global_params()` doit être appelée **avant** `synth_luxwave_init()` pour que les voix soient correctement initialisées.

5. **Pixel count dynamique** : LuxWave dépend du nombre de pixels du scanner (DPI-dépendant). Ce paramètre doit être configuré dans `prepareToPlay()` et lors de changements de configuration du scanner.

6. **Mode LuxWave mono** : Le moteur produit `output_left == output_right`. Si du stéréo est requis, ajouter une étape de spatialisation après la synthèse.

---

*Document généré à partir de l'analyse du code legacy — `legacy_old/synthesis/luxsynth/` et `legacy_old/synthesis/luxwave/`*
