# Sp3ctra VST

Sp3ctra est un **instrument synesthésique** : il transforme un flux d'image
ligne-à-ligne (capteur CIS du device Sp3ctra, image, vidéo, caméra) en son,
en temps réel. Le projet est un plugin audio **JUCE** (VST3, AU, Standalone)
avec un cœur de traitement temps réel en **C**.

## Architecture (2026)

Tout est construit sur des **chaînes de flux vidéo** (8 max) assemblées dans
un rack de modules :

- **Chaînes** : une chaîne = une source (SP3CTRA / IMAGE / VIDEO / CAMERA) suivie
  d'une liste ordonnée de modules. Les chaînes manipulent exclusivement le flux
  image ; chaque chaîne porte les réglages de ses modules et peut être
  dupliquée ou sauvée en preset `.sp3chain`.
- **Modules** : processeurs (Pitch, Mask), FX (Reverb, Echo, EQ), joueurs
  (Sampler ×2, Score, Timbre, Sequencer), sondes (Video Scroll) et **modules
  OUT** (`→ LUXSTRAL`, `→ LUXSYNTH`, `→ LUXWAVE`) — des sends conditionnés
  (Negative / DC / Gamma / Contrast / Range dB / Intensity par chaîne).
- **3 synthèses globales** : LuxStral (additive), LuxSynth (FFT-additive),
  LuxWave (wavetable). Elles ne sont nourries QUE par les modules OUT ; quand
  plusieurs chaînes envoient vers le même moteur, les flux sont mixés
  (pondérés par l'Intensity de chaque send).
- **Routage** : le modèle de chaînes est compilé en un `ChainPlan` lock-free,
  seule autorité de routage consommée par les threads temps réel (UDP,
  feeder de sources internes, player, audio).

La feuille de route et l'état détaillé de cette architecture :
[`docs/PLAN_P3_CHAIN_SETTINGS.md`](docs/PLAN_P3_CHAIN_SETTINGS.md).
Charte graphique de l'UI : [`docs/CHARTE_GRAPHIQUE.md`](docs/CHARTE_GRAPHIQUE.md).

## Arborescence

```
vst/
├── CMakeLists.txt           # Build JUCE (VST3 / AU / Standalone)
└── source/
    ├── PluginProcessor.*    # APVTS, banques de params, dérivation du ChainPlan
    ├── PluginEditor.*       # Layout 4 zones + rack de chaînes
    ├── ui/                  # ChainModel, rack, presets .sp3chain, manifest params
    ├── processing/          # Pipeline image C : chain_plan, stages, staging synth,
    │                        #   pitch/mask/FX, feed spectral LuxSynth
    ├── synthesis/           # Moteurs C : luxstral, luxsynth, luxwave
    ├── luxsampler/          # Sampler d'images (2 moteurs A/B) + FramePlayerThread
    ├── sampler/ image/ video/  # Pages UI (sampler, modules image, video mix)
    ├── sources/             # Sources internes IMAGE/VIDEO/CAMERA (M9)
    ├── threading/           # udpThread, feeder tick, exécuteur de chaînes
    ├── communication/       # UDP device + DMX
    ├── midi/                # MIDI mapping engine (CC/Note → tout param)
    └── config/              # g_sp3ctra_config + headers de config
```

## Installation sans compilation

```bash
git clone git@github.com:Ondulab/Sp3ctra_VST.git
cd Sp3ctra_VST
./scripts/install_vst.sh     # installe les binaires de prebuilt/
```

Voir [`QUICKSTART.md`](QUICKSTART.md).

## Compilation

```bash
# Build complet (VST3 + AU + Standalone) + archives dans prebuilt/
./scripts/build_vst.sh clean

# Build incrémental du standalone seul
cmake --build vst/build --target Sp3ctraVST_Standalone -j 8

# Lancer le standalone
./scripts/run_standalone.sh
```

Dépannage CMake : [`TROUBLESHOOTING_CMAKE.md`](TROUBLESHOOTING_CMAKE.md).
Distribution : [`DISTRIBUTION_GUIDE.md`](DISTRIBUTION_GUIDE.md).

## Device Sp3ctra

Le device (capteur CIS) streame ses lignes en **UDP** et expose une API de
configuration **HTTP REST** (`192.168.100.1`). Le plugin l'intègre via
`Sp3ctraDeviceClient` (le device est la source de vérité de sa config).

## Limitations connues

- Une seule instance du plugin par projet DAW (configuration globale partagée).
- Presets `.sp3chain` V1 : topologie + réglages des modules du manifest
  (les slots audio du sampler, l'image SCORE, les chemins media et le pattern
  du séquenceur ne sont pas embarqués).
