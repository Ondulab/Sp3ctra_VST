# Sp3ctra VST

Sp3ctra is a **synesthetic instrument**: it turns a line-by-line image stream
(the Sp3ctra device's CIS sensor, image, video, camera) into sound, in real
time. The project is a **JUCE** audio plugin (VST3, AU, Standalone) with a
real-time processing core written in **C**.

## Architecture (2026)

Everything is built on **video-stream chains** (8 max) assembled in a module
rack:

- **Chains**: a chain = a source (SP3CTRA / IMAGE / VIDEO / CAMERA) followed by
  an ordered list of modules. Chains operate exclusively on the image stream;
  each chain carries its modules' settings and can be duplicated or saved as a
  `.sp3chain` preset.
- **Modules**: processors (Pitch, Mask), FX (Reverb, Echo, EQ), players
  (Sampler ×2, Score, Timbre, Sequencer), probes (Video Scroll) and **OUT
  modules** (`→ LUXSTRAL`, `→ LUXSYNTH`, `→ LUXWAVE`) — conditioned sends
  (Negative / DC / Gamma / Contrast / Range dB / Intensity, per chain).
- **3 global synths**: LuxStral (additive), LuxSynth (FFT-additive), LuxWave
  (wavetable). They are fed ONLY by the OUT modules; when several chains send
  to the same engine, the streams are mixed (weighted by each send's Intensity).
- **Routing**: the chain model is compiled into a lock-free `ChainPlan`, the
  single routing authority consumed by the real-time threads (UDP, internal
  source feeder, player, audio).

UI style guide: [`docs/CHARTE_GRAPHIQUE.md`](docs/CHARTE_GRAPHIQUE.md).

## Source tree

```
vst/
├── CMakeLists.txt           # JUCE build (VST3 / AU / Standalone)
└── source/
    ├── PluginProcessor.*    # APVTS, param banks, ChainPlan derivation
    ├── PluginEditor.*       # 4-zone layout + chain rack
    ├── ui/                  # ChainModel, rack, .sp3chain presets, param manifest
    ├── processing/          # C image pipeline: chain_plan, stages, synth staging,
    │                        #   pitch/mask/FX, LuxSynth spectral feed
    ├── synthesis/           # C engines: luxstral, luxsynth, luxwave
    ├── luxsampler/          # Image sampler (2 engines A/B) + FramePlayerThread
    ├── sampler/ image/ video/  # UI pages (sampler, image modules, video mix)
    ├── sources/             # Internal IMAGE/VIDEO/CAMERA sources (M9)
    ├── threading/           # udpThread, feeder tick, chain executor
    ├── communication/       # UDP device + DMX
    ├── midi/                # MIDI mapping engine (CC/Note → any param)
    └── config/              # g_sp3ctra_config + config headers
```

## Install without compiling

```bash
git clone git@github.com:Ondulab/Sp3ctra_VST.git
cd Sp3ctra_VST
./scripts/install_vst.sh     # installs the binaries from prebuilt/
```

See [`QUICKSTART.md`](QUICKSTART.md).

## Building

```bash
# Full build (VST3 + AU + Standalone) + archives in prebuilt/
./scripts/build_vst.sh clean

# Incremental build of the standalone only
cmake --build vst/build --target Sp3ctraVST_Standalone -j 8

# Launch the standalone
./scripts/run_standalone.sh
```

CMake troubleshooting: [`TROUBLESHOOTING_CMAKE.md`](TROUBLESHOOTING_CMAKE.md).
Distribution: [`DISTRIBUTION_GUIDE.md`](DISTRIBUTION_GUIDE.md).

## Sp3ctra device

The device (CIS sensor) streams its lines over **UDP** and exposes an **HTTP
REST** configuration API (`192.168.100.1`). The plugin integrates it through
`Sp3ctraDeviceClient` (the device is the source of truth for its own config).

## Known limitations

- Only one plugin instance per DAW project (shared global configuration).
- `.sp3chain` V1 presets: topology + settings of the manifest modules (the
  sampler's audio slots, the SCORE image, media paths, and the sequencer
  pattern are not embedded).

## License

Sp3ctra is free software distributed under the **GNU GPL v3 or later** (see
[LICENSE](LICENSE)). Copyright (C) 2024-2026 Ondulab / Patrick Reybaud.

The binary statically links **espeak-ng** (GPLv3, phonemization for the VOICE
module) — this is the component that sets the license of the combined work.
JUCE is used under its **AGPLv3** option, the VST3 SDK under its **GPLv3**
option. The full inventory of third-party components and the source-availability
obligations are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

A build without the TTS engine (and therefore without third-party GPL code
beyond the VST3 SDK) remains possible: `cmake -DSP3CTRA_ENABLE_TTS=OFF`.
