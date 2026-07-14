# Third-party notices

Sp3ctra est distribué sous **GNU GPL v3 ou ultérieure** (voir [LICENSE](LICENSE)).
Copyright (C) 2024-2026 Ondulab / Patrick Reybaud.

Le binaire distribué (Standalone, VST3, AU) est une œuvre combinée qui inclut,
liés statiquement, les composants ci-dessous. La distribution de l'ensemble se
fait aux conditions de la GPLv3 (le composant le plus contraignant, espeak-ng,
l'impose ; toutes les autres licences listées y sont compatibles).

## Frameworks

| Composant | Licence | Rôle | Source |
|---|---|---|---|
| JUCE 8.0.4 | **AGPLv3** (option open-source retenue) | framework plugin/UI/audio | https://github.com/juce-framework/JUCE (tag 8.0.4) |
| VST3 SDK (embarqué dans JUCE) | **GPLv3** (option open-source de la double licence Steinberg) | format VST3 | https://github.com/steinbergmedia/vst3sdk |
| AudioUnit SDK (embarqué dans JUCE) | Apache-2.0 | format AU | https://github.com/apple/AudioUnitSDK |

JUCE embarque ses propres dépendances (HarfBuzz, zlib, FLAC, Ogg/Vorbis, etc.) ;
leurs notices sont dans l'arbre JUCE (`modules/*/` licences respectives).

## Moteur TTS du module VOICE (archive statique sherpa-onnx v1.13.4)

Récupérée au configure par CMake (SHA256 épinglé) depuis
https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.13.4 — le code source
exact correspondant est le tag `v1.13.4` de ce dépôt (qui vendorise ou épingle
chacune des bibliothèques ci-dessous).

| Composant | Licence | Rôle |
|---|---|---|
| sherpa-onnx (core, c-api, fst, fstfar, kaldifst) | Apache-2.0 | runtime TTS |
| onnxruntime | MIT | inférence des modèles VITS |
| **espeak-ng** | **GPLv3-or-later** | phonémisation des voix Piper — **c'est ce composant qui place l'œuvre combinée sous GPLv3** |
| piper-phonemize (+ libucd) | MIT | pont texte→phonèmes Piper |
| kaldi-native-fbank, kaldi-decoder | Apache-2.0 | DSP/decoding |
| ssentencepiece (simple-sentencepiece, k2-fsa) | Apache-2.0 | tokenisation |
| KissFFT (`libkissfft-float`) | BSD-3-Clause | FFT |

## Vendorisé dans l'arbre

| Composant | Licence | Emplacement |
|---|---|---|
| KissFFT | BSD-3-Clause | `vst/source/synthesis/luxsynth/kissfft/` |

## Voix Piper embarquées (distribuées dans les bundles)

Les bundles du plugin embarquent dans `Contents/Resources/piper_voices/` les
voix listées par l'option CMake `SP3CTRA_EMBED_VOICES`. Par défaut :

| Voix | Langue | Dataset / licence |
|---|---|---|
| `vits-piper-fr_FR-siwis-medium` | fr-FR | SIWIS — **CC-BY 4.0** (attribution : datashare.is.ed.ac.uk/handle/10283/2353) |
| `vits-piper-en_US-ljspeech-medium` | en-US | LJ Speech (keithito.com/LJ-Speech-Dataset) — **domaine public** |

Le `MODEL_CARD` de chaque voix embarquée est livré à côté du modèle dans les
Resources. ⚠ Avant d'ajouter une voix à `SP3CTRA_EMBED_VOICES`, vérifier son
`MODEL_CARD` : certaines voix ont des licences de dataset restrictives (ex.
`en_US-lessac` = licence Blizzard 2013 — ne PAS l'embarquer) ; `fr_FR-tom` est
AGPLv3 (acceptable dans cette distribution copyleft, à documenter ici si ajouté).

## Voix Piper externes (NON distribuées)

Les voix supplémentaires restent hors binaire : l'utilisateur les télécharge
(`scripts/install_piper_voices.sh`) dans son dossier externe (configurable
depuis la page VOICE). Leur licence (MODEL_CARD) relève de son propre usage.

## Source correspondante (obligation GPLv3 §6)

Chaque release binaire doit donner accès à la « Corresponding Source » :
- le tag git de ce dépôt correspondant à la release ;
- l'archive source `sherpa-onnx v1.13.4` (inclut espeak-ng) — en conserver une
  copie jointe à la release (ne pas dépendre uniquement de la disponibilité
  future de l'upstream).
