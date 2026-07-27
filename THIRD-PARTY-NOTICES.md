# Third-party notices

Sp3ctra is distributed under the **GNU GPL v3 or later** (see [LICENSE](LICENSE)).
Copyright (C) 2024-2026 Ondulab / Patrick Reybaud.

The distributed binary (Standalone, VST3, AU) is a combined work that includes,
statically linked, the components below. The whole is distributed under the
terms of the GPLv3 (the most restrictive component, espeak-ng, imposes it; all
the other licenses listed are compatible with it).

## Frameworks

| Component | License | Role | Source |
|---|---|---|---|
| JUCE 8.0.4 | **AGPLv3** (open-source option chosen) | plugin/UI/audio framework | https://github.com/juce-framework/JUCE (tag 8.0.4) |
| VST3 SDK (bundled in JUCE) | **GPLv3** (open-source option of Steinberg's dual license) | VST3 format | https://github.com/steinbergmedia/vst3sdk |
| AudioUnit SDK (bundled in JUCE) | Apache-2.0 | AU format | https://github.com/apple/AudioUnitSDK |

JUCE bundles its own dependencies (HarfBuzz, zlib, FLAC, Ogg/Vorbis, etc.);
their notices are in the JUCE tree (`modules/*/` respective licenses).

## VOICE module TTS engine (sherpa-onnx v1.13.4 static archive)

Fetched at configure time by CMake (pinned SHA256) from
https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.13.4 — the exact
corresponding source code is the `v1.13.4` tag of that repository (which vendors
or pins each of the libraries below).

| Component | License | Role |
|---|---|---|
| sherpa-onnx (core, c-api, fst, fstfar, kaldifst) | Apache-2.0 | TTS runtime |
| onnxruntime | MIT | VITS model inference |
| **espeak-ng** | **GPLv3-or-later** | phonemization of the Piper voices — **this is the component that places the combined work under GPLv3** |
| piper-phonemize (+ libucd) | MIT | Piper text→phoneme bridge |
| kaldi-native-fbank, kaldi-decoder | Apache-2.0 | DSP/decoding |
| ssentencepiece (simple-sentencepiece, k2-fsa) | Apache-2.0 | tokenization |
| KissFFT (`libkissfft-float`) | BSD-3-Clause | FFT |

## Vendored in the tree

| Component | License | Location |
|---|---|---|
| KissFFT | BSD-3-Clause | `vst/source/synthesis/luxsynth/kissfft/` |

## Embedded Piper voices (distributed in the bundles)

The plugin bundles embed, in `Contents/Resources/piper_voices/`, the voices
listed by the CMake option `SP3CTRA_EMBED_VOICES`. By default:

| Voice | Language | Dataset / license |
|---|---|---|
| `vits-piper-fr_FR-siwis-medium` | fr-FR | SIWIS — **CC-BY 4.0** (attribution: datashare.is.ed.ac.uk/handle/10283/2353) |
| `vits-piper-en_US-ljspeech-medium` | en-US | LJ Speech (keithito.com/LJ-Speech-Dataset) — **public domain** |

The `MODEL_CARD` of each embedded voice is shipped alongside the model in the
Resources. ⚠ Before adding a voice to `SP3CTRA_EMBED_VOICES`, check its
`MODEL_CARD`: some voices have restrictive dataset licenses (e.g.
`en_US-lessac` = Blizzard 2013 license — do NOT embed it); `fr_FR-tom` is
AGPLv3 (acceptable in this copyleft distribution, to be documented here if
added).

## External Piper voices (NOT distributed)

Additional voices stay outside the binary: the user downloads them
(`scripts/install_piper_voices.sh`) into their external folder (configurable
from the VOICE page). Their license (MODEL_CARD) governs their own use.

## Corresponding source (GPLv3 §6 obligation)

Every binary release must provide access to the "Corresponding Source":
- the git tag of this repository matching the release;
- the `sherpa-onnx v1.13.4` source archive (includes espeak-ng) — keep a copy
  attached to the release (do not rely solely on upstream's future
  availability).
