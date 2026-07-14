#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>   // SCORE source-audio preview
#include <iterator>                                  // std::size (insert bank tables)
#include "sources/MediaSourceEngines.h"              // M9 — IMAGE/VIDEO/CAMERA engines
#include "sources/MediaSourceService.h"              // M9 — source service thread
#include "sampler/SamplerMidiTargets.h"              // MIDI-Learn virtual targets (sampler play params)
#include "tts/PiperTts.h"                            // VOICE — offline TTS (startup smoke test)

// C headers still used directly by this file
extern "C" {
    #include "core/context.h"
    #include "config/config_loader.h"
    #include "utils/logger.h"
    #include "utils/rt_profiler.h"
    #include "synthesis/luxstral/synth_luxstral_algorithms.h" // update_gap_limiter_coefficients()
    #include "synthesis/luxstral/vst_adapters.h"              // luxstral_are_audio_buffers_ready(), buffers
    #include "synthesis/luxstral/wave_generation.h"           // request_frequency_reinit() hot-reload
    #include "processing/lux_pitch.h"                         // LuxPitch engine (g_lux_pitch_proc)
    #include "processing/lux_mask.h"                          // LuxMask engine (g_lux_mask_proc)
    #include "processing/lux_reverb.h"                        // LuxReverb FX (g_lux_reverb_proc)
    #include "processing/lux_echo.h"                          // LuxEcho FX (g_lux_echo_proc)
    #include "processing/lux_eq.h"                            // LuxEq FX (g_lux_eq_proc)
    #include "processing/video_scroll.h"                      // VideoScroll capture-ring pool
    #include "processing/image_chain.h"                       // Insert chain executor (order + taps)
    #include "processing/chain_plan.h"                         // M6 Phase 2 — RT chain descriptor
    #include "processing/synth_staging.h"                      // deferred staging resets (M3)
}
#include "ui/ChainPresetIO.h"                                  // J4 — .sp3chain presets
extern "C" {
    #include "audio/buffers/audio_image_buffers.h"             // selection tap (contextual zone 1)
    #include "synthesis/luxsynth/luxsynth_vst_adapter.h"      // luxsynth_push_midi_event(), buffers, engine
    #include "synthesis/luxwave/luxwave_vst_adapter.h"        // luxwave_push_midi_event(), g_luxwave_engine

}
// Note: synth_luxstral_threading.h / synth_luxstral_runtime.h / AudioProcessingThread.h
// are now included transitively via Sp3ctraSharedCore.h and handled by Sp3ctraSharedCore.

// Global RT Profiler accessible from C threads (audioProcessingThread)
// This must be declared here (not in header) to avoid multiple definition errors
RTProfiler g_vst_rt_profiler = {};

//==============================================================================
// Pooled-insert bank suffixes — single source of truth for the per-instance
// param banks (luxpitch{N}_* / luxmask{N}_* / luxreverb{N}_* / luxecho{N}_*).
// Consumed by: listener registration, the per-chain settings memory
// (snapshot / restore / reset-to-defaults) and the legacy-session migration
// (legacy id = type prefix + suffix, bank id = prefix + N + "_" + suffix).
// MUST match the ids created in createParameterLayout().
namespace
{
    // (J1: the per-type suffix tables and InsertBankDesc moved to the single
    // manifest — ui/ModuleParamManifest.h. Call sites use moduleParamManifest()
    // directly.)
    bool isPooledInsertType(ModuleType t)
    {
        return t == ModuleType::Pitch  || t == ModuleType::Mask
            || t == ModuleType::Reverb || t == ModuleType::Echo
            || t == ModuleType::Equalizer;
    }
}

//==============================================================================
// SCORE transport: derive the engine LoopMode from the two loop params.
// Reverse always loops (the engine has no one-shot reverse), mirroring the
// SCORE page pictograms: reverse → INVERSE, else loop → LOOP, else NONE.
static LoopMode scoreLoopModeFromParams(juce::AudioProcessorValueTreeState& apvts)
{
    if (apvts.getRawParameterValue("scoreReverse")->load() > 0.5f) return LoopMode::INVERSE;
    if (apvts.getRawParameterValue("scoreLoop")->load()    > 0.5f) return LoopMode::LOOP;
    return LoopMode::NONE;
}

//==============================================================================
// Create parameter layout (called once during construction)
juce::AudioProcessorValueTreeState::ParameterLayout Sp3ctraAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Attribute helpers: infrastructure params hidden from DAW automation lanes.
    // They remain fully functional (saved in presets, accessible via APVTS).
    const auto kHiddenInt    = juce::AudioParameterIntAttributes   {}.withAutomatable(false);
    const auto kHiddenFloat  = juce::AudioParameterFloatAttributes {}.withAutomatable(false);
    const auto kHiddenBool   = juce::AudioParameterBoolAttributes  {}.withAutomatable(false);
    const auto kHiddenChoice = juce::AudioParameterChoiceAttributes{}.withAutomatable(false);

    // ── Infrastructure — UDP ──────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_UDP_PORT, 1}, "UDP Port",
        1024, 65535, Sp3ctraConstants::DEFAULT_UDP_PORT, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_UDP_BYTE1, 1}, "UDP Byte 1", 0, 255, 192, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_UDP_BYTE2, 1}, "UDP Byte 2", 0, 255, 168, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_UDP_BYTE3, 1}, "UDP Byte 3", 0, 255, 100, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_UDP_BYTE4, 1}, "UDP Byte 4", 0, 255, 10, kHiddenInt));

    // ── Infrastructure — Device HTTP host (config.html) ──────────────────────
    // Persistent transport param: where Sp3ctraDeviceClient reaches the device's
    // embedded web server. Default 192.168.100.1 (the config page host).
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_DEVICE_IP_BYTE1, 1}, "Device IP 1", 0, 255, 192, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_DEVICE_IP_BYTE2, 1}, "Device IP 2", 0, 255, 168, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_DEVICE_IP_BYTE3, 1}, "Device IP 3", 0, 255, 100, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_DEVICE_IP_BYTE4, 1}, "Device IP 4", 0, 255, 1, kHiddenInt));

    // ── Infrastructure — Sensor / Log / Visualizer ───────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PARAM_SENSOR_DPI, 1}, "Sensor DPI",
        juce::StringArray{"200 DPI", "400 DPI"}, 1, kHiddenChoice));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PARAM_LOG_LEVEL, 1}, "Log Level",
        juce::StringArray{"Error", "Warning", "Info", "Debug"},
        Sp3ctraConstants::DEFAULT_LOG_LEVEL, kHiddenChoice));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PARAM_VISUALIZER_MODE, 1}, "Visualizer Mode",
        juce::StringArray{"Image", "Waveform", "Inverted Waveform"}, 2, kHiddenChoice));

    // ── Infrastructure — Musical scale (Settings window) ─────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralTuning", 1}, "Tuning (A4)",
        juce::NormalisableRange<float>(415.0f, 466.0f, 0.1f), 440.0f,
        kHiddenFloat.withLabel("Hz")));

    juce::StringArray noteNames;
    const char* noteLetters[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    for (int octave = 1; octave <= 6; ++octave)
        for (int note = 0; note < 12; ++note)
            noteNames.add(juce::String(noteLetters[note]) + juce::String(octave));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxstralRootNote", 1}, "Root Note",
        noteNames, 12, kHiddenChoice));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"luxstralNumOctaves", 1}, "Octaves", 1, 10, 8, kHiddenInt));

    // ── Gameplay — Envelope (ms) ─────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralAttackMs", 1}, "Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 0.5f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralReleaseMs", 1}, "Release",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 0.5f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // (Purge 2026-07-12: the legacy pre-split conditioning params —
    // luxstralInvertIntensity/GammaEnable/GammaValue/ContrastMin,
    // luxstral/luxsynth Inversion/AcRemoval/Source, luxsynthGammaValue,
    // luxpitch/luxmask Source, luxstralFidelityMode/RangeDb and the
    // VolumeWeighting/SummationResponse exponents — are DELETED. The per-OUT
    // banks are the only conditioning authority; the synthSplitVersion
    // migration still reads the OLD ids from the raw session XML, which does
    // not require the params to exist.)

    // ── Gameplay — Stereo enable (PLAY-page badge toggle) ────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralStereoEnable", 1}, "Stereo Enable",
        true));

    // ── Gameplay — Stereo temperature ────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralStereoTempAmp", 1}, "Stereo Temp.",
        juce::NormalisableRange<float>(0.0f, 5.0f, 0.01f), 2.5f));

    // ── Gameplay — Noise gate ────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralNoiseGateThreshold", 1}, "Noise Gate",
        juce::NormalisableRange<float>(0.0f, 0.1f, 0.001f), 0.005f));

    // ── Infrastructure — Worker Threads (Settings window only) ────────────────
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"luxstralNumWorkers", 1}, "Workers", 1, 16, 8, kHiddenInt));

    // ── Infrastructure — Physiological filter ────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralPhysiologicalFilter", 1}, "Equal-Loudness",
        false, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralPhysiologicalDepth", 1}, "Equal-Loudness Depth",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f, kHiddenFloat));

    // ── Gameplay — Volume Controls ────────────────────────────────────────────
    // Master output gain applied after all synthesis consumers.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"masterVolume", 1}, "Volume",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    // Per-synth volume: independent gain for LuxStral and LuxSynth consumers.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralVolume", 1}, "LuxStral Vol.",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthVolume", 1}, "LuxSynth Vol.",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    // ── Gameplay — Device On ─────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"deviceEnabled", 1}, "Device On", true));

    // ── Setup — Soft limiter (LuxStral A) ────────────────────────────────────
    // These IDs were referenced by LuxStralSetupPanel but never created — the
    // sliders were silently inert (JUCE skips attachments on unknown IDs).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralSoftLimitThreshold", 1}, "Soft Limit Thr.",
        juce::NormalisableRange<float>(0.1f, 1.0f, 0.01f), 0.8f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralSoftLimitKnee", 1}, "Soft Limit Knee",
        juce::NormalisableRange<float>(0.01f, 1.0f, 0.01f), 0.2f, kHiddenFloat));

    // (Purge 2026-07-12: luxstralFidelityMode/RangeDb deleted — the inverse-dB
    // decode law is ALWAYS ON with its dB window PER OUT SEND, from the
    // luxstralOut{N}_rangeDb bank; the migration seeds the banks from the old
    // shared knob read out of the raw session XML.)

    // ── Gameplay — Phase management: physical onset modes ───────────────────
    // When a note attacks (strong volume jump crossing the gate from silence),
    // its oscillator's phase is set by the mode's physical initial conditions:
    //   Strike = struck string (velocity → ±sine)  Pluck = plucked (±cosine)
    //   Bell   = fixed per-note impact hash (repeatable, never combs)
    //   Breath = fresh random per attack (reed/flute turbulence)
    // Free = legacy free-running phases. Silent oscillators idle on random
    // phases → a mode change never inherits the previous organization.
    // Order must match LUXSTRAL_PHASE_MODE_* (config_loader.h).
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxstralPhaseMode", 1}, "Phase Mode",
        juce::StringArray{"Free", "Strike", "Pluck", "Bell", "Breath"}, 0));

    // Onset sensitivity — RELATIVE to the material: the engine tracks a
    // rolling max of note volumes and gates at fraction × ref, so selecting
    // a mode always works without tuning an absolute threshold. 1 = catch
    // even soft onsets (3 % of recent peak), 0 = only the hardest attacks.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralPhaseSensitivity", 1}, "Phase Sensitivity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.7f));

    // Strike/Pluck: position along the string (sign-alternation period of the
    // partials' phases; 0 = whole band aligned). Bell: impact point (16
    // distinct, each a stable phase fingerprint). Breath: unused.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralPhasePosition", 1}, "Phase Position",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));

    // Phase drift — companion of Phase Reset: each onset also redraws a random
    // micro-detune of ±cents for that note. Melts the flanger-like comb of
    // phase-aligned onsets into ensemble texture while keeping the coherent
    // attack. 0 = off (grid-exact pitch, full flanger character).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralPhaseDriftCents", 1}, "Phase Drift",
        juce::NormalisableRange<float>(0.0f, 3.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ct")));

    // ── Synth-split P1 — per-OUT conditioning banks (pool slots 0..7) ────────
    // One bank per OUT-module instance: the OUT conditions its chain's flux
    // before sending it to the global engine. Gamma has NO enable toggle
    // (1.0 = off); Intensity is the pre-engine mix weight of the send.
    // Defaults mirror the legacy conditioning defaults so a fresh session is
    // bit-identical. (The legacy pre-split globals were deleted from the
    // layout on 2026-07-12 — the migration reads their OLD ids from the raw
    // session XML, see setStateInformation.)
    for (int s = 0; s < 8; ++s)
    {
        const juce::String n(s);

        // LuxStral OUT
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lsOutParam(s, "negative"), 1},
            "LS OUT" + n + " Negative", true));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lsOutParam(s, "dcBlocking"), 1},
            "LS OUT" + n + " DC Blocking", true));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lsOutParam(s, "gamma"), 1},
            "LS OUT" + n + " Gamma",
            juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lsOutParam(s, "contrastMin"), 1},
            "LS OUT" + n + " Contrast Min",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.21f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lsOutParam(s, "rangeDb"), 1},
            "LS OUT" + n + " Range",
            juce::NormalisableRange<float>(20.0f, 80.0f, 0.5f), 50.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("dB")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lsOutParam(s, "intensity"), 1},
            "LS OUT" + n + " Intensity",
            juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 1.0f));
        // Per-send power (rack LED) — the ENGINE enable stays deviceEnabled
        // (AUDIO MIX strip LED). A disabled send contributes silence to the mix.
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lsOutParam(s, "enabled"), 1},
            "LS OUT" + n + " On", true));

        // LuxSynth OUT
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lxOutParam(s, "negative"), 1},
            "LX OUT" + n + " Negative", true));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lxOutParam(s, "dcBlocking"), 1},
            "LX OUT" + n + " DC Blocking", true));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lxOutParam(s, "gamma"), 1},
            "LX OUT" + n + " Gamma",
            juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lxOutParam(s, "intensity"), 1},
            "LX OUT" + n + " Intensity",
            juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lxOutParam(s, "enabled"), 1},
            "LX OUT" + n + " On", true));

        // LuxWave OUT — autonomous conditioning (no longer inherits LuxSynth's)
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lwOutParam(s, "negative"), 1},
            "LW OUT" + n + " Negative", true));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lwOutParam(s, "dcBlocking"), 1},
            "LW OUT" + n + " DC Blocking", true));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lwOutParam(s, "gamma"), 1},
            "LW OUT" + n + " Gamma",
            juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lwOutParam(s, "intensity"), 1},
            "LW OUT" + n + " Intensity",
            juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lwOutParam(s, "enabled"), 1},
            "LW OUT" + n + " On", true));
    }

    // ── Gameplay — StrokeForge enable ────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sfEnabled", 1}, "SF Active", false));

    // ── Infrastructure — SF internal parameters ───────────────────────────────
    // sfBlobBaseThreshold / sfBlobMinWidth / sfBlobMergeGap removed:
    // these are now exclusively controlled by spctrBlob* (IMAGE LUXSTRAL tab).
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sfBlobContrastAdaptive", 1}, "SF Contrast Adaptive",
        true, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfBlobContrastSensitivity", 1}, "SF Contrast Sensitivity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f, kHiddenFloat));

    // ── Infrastructure — SF advanced ─────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"sfMaxHarmonics", 1}, "SF Max Harmonics",
        1, 16, 8, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfHarmonicAmpFloor", 1}, "SF Harmonic Floor",
        juce::NormalisableRange<float>(0.001f, 0.1f, 0.001f), 0.01f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfVolumeCenterSigma", 1}, "SF Volume Sigma",
        juce::NormalisableRange<float>(0.1f, 2.0f, 0.01f), 0.4f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sfPhaseCoherence", 1}, "SF Phase Coherence",
        true, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfPhaseSmoothAlpha", 1}, "SF Phase Alpha",
        juce::NormalisableRange<float>(0.01f, 0.5f, 0.01f), 0.05f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfMorphWidthScale", 1}, "SF Morph Scale",
        juce::NormalisableRange<float>(2.0f, 500.0f, 1.0f), 400.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"sfWavetableMinWidth", 1}, "SF WT Min Width",
        1, 200, 50, kHiddenInt));

    // ── Gameplay — Focus Sigma / Spectral Threshold ───────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfBlobFocusSigma", 1}, "Focus Sigma",
        juce::NormalisableRange<float>(0.5f, 100.0f, 0.5f, 0.4f), 20.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("notes")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfSpectralWidthThreshold", 1}, "Spectral Thr.",
        juce::NormalisableRange<float>(0.0f, 3456.0f, 1.0f), 200.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("notes")));

    // ── Gameplay — Focus Only ─────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sfFocusOnly", 1}, "Focus Only", false));

    // ── Image Pipeline ────────────────────────────────────────────────────────
    // imageFreezeMode: 0 = PLAY (live frames flow)
    //                  1 = HOLD (freeze last captured frame)
    //                  2 = WHITE (force all pixels → 255, silences synthesis)
    // Stored as int [0..2].  Backend effect is in CisVisualizerComponent::updateCisData().
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"imageFreezeMode", 1}, "Freeze Mode", 0, 2, 0));

    // Live stream opacity [0..1] — darken-blend weight applied to the live CIS frame.
    // Blend mode: min(live_pixel, sampler_pixel) per channel (ImageChops.darker equivalent).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imageLiveOpacity", 1}, "Live Opacity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f, kHiddenFloat));

    // Sampler stream opacity [0..1] — driven by imageMixBalance crossfader.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imageSamplerOpacity", 1}, "Sampler Opacity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f, kHiddenFloat));

    // Mix balance crossfader: 0.0=full Sampler, 0.5=equal, 1.0=full Live.
    // Drives imageLiveOpacity and imageSamplerOpacity via parameterChanged.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imageMixBalance", 1}, "Mix Balance",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f, kHiddenFloat));

    // (Purge 2026-07-12: the per-path routing/conditioning params —
    // luxstral/luxsynth Source/Inversion/AcRemoval, luxsynthGammaValue — are
    // deleted: the ChainPlan routes, the per-OUT banks condition.)

    // ── LuxSynth blob detection — independent of StrokeForge/LuxStral ────────
    // These parameters are owned exclusively by the SYNTH_BLOB visualizer
    // (CisVisualizerComponent::detectSynthBlobs). They have NO effect on the
    // LuxStral audio synthesis path (which uses the sf* parameters).
    // Amplitude threshold: normalised CIS brightness [0..1].
    // Wide range [0.001..1.0] to accommodate both high-contrast and low-contrast sources.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"lxBlobThreshold", 1}, "LX Blob Thr.",
        juce::NormalisableRange<float>(0.001f, 1.0f, 0.001f, 0.35f), 0.05f,
        juce::AudioParameterFloatAttributes{}.withLabel("")));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"lxBlobMinWidth", 1}, "LX Blob Min W",
        1, 200, 10));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"lxBlobMergeGap", 1}, "LX Merge Gap",
        0, 100, 3));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"lxBlobColorSplit", 1}, "LX Color Split",
        juce::NormalisableRange<float>(0.01f, 1.0f, 0.01f), 0.20f,
        juce::AudioParameterFloatAttributes{}.withLabel("")));

    // ── SPCTR (LuxStral) blob detection — identical ranges to lxBlob* ────────
    // These params drive both the IMAGE LUXSTRAL visualizer (detectSpctrBlobs)
    // and the StrokeForge audio synthesis engine (via g_sp3ctra_config).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"spctrBlobThreshold", 1}, "SPCTR Blob Thr.",
        juce::NormalisableRange<float>(0.001f, 1.0f, 0.001f, 0.35f), 0.05f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"spctrBlobMinWidth", 1}, "SPCTR Blob Min W",
        1, 200, 10));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"spctrBlobMergeGap", 1}, "SPCTR Merge Gap",
        0, 100, 3));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"spctrBlobColorSplit", 1}, "SPCTR Color Split",
        juce::NormalisableRange<float>(0.01f, 1.0f, 0.01f), 0.20f));

    // ── LuxSynth FFT quality / synthesis-data parameters ─────────────────────
    // lxFftBins: number of harmonics extracted from the spatial FFT.
    // Each bin maps to one oscillator in the LuxSynth additive synthesis engine.
    // 32 = fast / low quality, 256 = slow / high quality (default 128).
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"lxFftBins", 1}, "LX FFT Bins",
        juce::StringArray{"32", "64", "128", "256"}, 2));

    // lxFftSmoothing: temporal smoothing for FFT magnitudes [0..1].
    // 0 = very fast / reactive (alpha_attack≈0.80, alpha_release≈0.50).
    // 1 = very slow / smooth   (alpha_attack≈0.05, alpha_release≈0.02).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"lxFftSmoothing", 1}, "LX FFT Smoothing",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f));

    // ── LuxSynth Engine Parameters ────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxsynthEnabled", 1}, "LuxSynth Active", false));

    // Volume ADSR
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthAttackMs", 1}, "LS Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 10.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthDecayMs", 1}, "LS Decay",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthSustainLevel", 1}, "LS Sustain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.7f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthReleaseMs", 1}, "LS Release",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 200.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // Filter ADSR
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterAttackMs", 1}, "LS Flt Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 20.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterDecayMs", 1}, "LS Flt Decay",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 150.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterSustain", 1}, "LS Flt Sustain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterReleaseMs", 1}, "LS Flt Release",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 300.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterCutoff", 1}, "LS Flt Cutoff",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterEnvDepth", 1}, "LS Flt Depth",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));

    // ADSR segment curvature ([-1,1], 0 = linear) — volume + filter envelopes
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthAttackCurve", 1}, "LS Attack Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthDecayCurve", 1}, "LS Decay Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthReleaseCurve", 1}, "LS Release Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterAttackCurve", 1}, "LS Flt Attack Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterDecayCurve", 1}, "LS Flt Decay Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterReleaseCurve", 1}, "LS Flt Release Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));

    // Spectral (per-OUT bank gamma is the only gamma — the engine-side
    // "luxsynthGamma" param was never read and is gone, purge 2026-07-12)
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"luxsynthNumOscillators", 1}, "LS Oscillators",
        1, 128, 64));

    // LFO
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthLfoRate", 1}, "LS LFO Rate",
        juce::NormalisableRange<float>(0.0f, 20.0f, 0.01f), 5.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthLfoDepth", 1}, "LS LFO Depth",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.1f,
        juce::AudioParameterFloatAttributes{}.withLabel("st")));

    // LuxSynth MIDI Channel (Channel 1-16)
    {
        juce::StringArray lxMidiChNames;
        for (int i = 1; i <= 16; ++i)
            lxMidiChNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxsynthMidiChannel", 1}, "LuxSynth MIDI Channel",
            lxMidiChNames, 0, kHiddenChoice));  // default = Channel 1 (index 0)
    }

    // LuxSynth Octave Offset (-2 .. +2)
    {
        juce::StringArray octaveNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxsynthOctaveOffset", 1}, "LuxSynth Octave Offset",
            octaveNames, 2, kHiddenChoice));  // default index 2 = 0
    }

    // ── LuxWave Engine Parameters ─────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxwaveEnabled", 1}, "LuxWave Active", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveVolume", 1}, "LuxWave Vol.",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    // LuxWave Volume ADSR
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveAttackMs", 1}, "LW Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 10.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveDecayMs", 1}, "LW Decay",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveSustainLevel", 1}, "LW Sustain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.7f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveReleaseMs", 1}, "LW Release",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 200.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    // ADSR segment curvature ([-1,1], 0 = linear)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveAttackCurve", 1}, "LW Attack Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveDecayCurve", 1}, "LW Decay Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveReleaseCurve", 1}, "LW Release Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));

    // LuxWave Filter ADSR
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveFilterCutoff", 1}, "LW Flt Cutoff",
        juce::NormalisableRange<float>(100.0f, 20000.0f, 1.0f, 0.3f), 8000.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveFilterEnvDepth", 1}, "LW Flt Depth",
        juce::NormalisableRange<float>(0.0f, 20000.0f, 1.0f, 0.3f), 4000.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));

    // LuxWave LFO
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveLfoRate", 1}, "LW LFO Rate",
        juce::NormalisableRange<float>(0.0f, 20.0f, 0.01f), 5.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveLfoDepth", 1}, "LW LFO Depth",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.1f,
        juce::AudioParameterFloatAttributes{}.withLabel("st")));

    // LuxWave Scan Mode
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxwaveScanMode", 1}, "LW Scan Mode",
        juce::StringArray{"Left→Right", "Right→Left", "Dual"}, 0));

    // LuxWave Amplitude
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveAmplitude", 1}, "LW Amplitude",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));

    // LuxWave MIDI Channel (Channel 1-16)
    {
        juce::StringArray lwMidiChNames;
        for (int i = 1; i <= 16; ++i)
            lwMidiChNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxwaveMidiChannel", 1}, "LuxWave MIDI Channel",
            lwMidiChNames, 0, kHiddenChoice));
    }

    // LuxWave Octave Offset (-2 .. +2)
    {
        juce::StringArray lwOctNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxwaveOctaveOffset", 1}, "LuxWave Octave Offset",
            lwOctNames, 2, kHiddenChoice));
    }

    // (P4-M5: "chainInsertOrder" deleted — per-chain insert order comes from
    // each chain's own recipe; the global order died with the modulated bus.)

    // ── PITCH / MASK — per-instance automatable banks (×8) ───────────────────
    // Each pooled insert instance owns a state-pool slot 0..7 (modulePoolSlots_,
    // keyed by the ModuleInstance UUID) that keys both its RT pool instance
    // (lux_pitch_instance / lux_mask_instance) and this APVTS bank — the same
    // pattern as the VideoScroll banks below. Same ranges/defaults as the
    // former per-type params; the zone-3 pages rebind luxpitch{N}_* /
    // luxmask{N}_* when a block is selected.
    {
        juce::StringArray midiChNames;
        for (int i = 1; i <= 16; ++i)
            midiChNames.add("Channel " + juce::String(i));
        const juce::StringArray octNames { "-2", "-1", " 0", "+1", "+2" };
        juce::StringArray noteNames;   // C1..B6 (72 items), default A3 = index 33
        {
            const char* noteLetters[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            for (int oct = 1; oct <= 6; ++oct)
                for (int n = 0; n < 12; ++n)
                    noteNames.add(juce::String(noteLetters[n]) + juce::String(oct));
        }

        for (int n = 0; n < 8; ++n)
        {
            const juce::String tag = "P" + juce::String(n) + " ";
            auto id = [n](const char* sfx) { return juce::ParameterID{lpParam(n, sfx), 1}; };

            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("Enabled"), tag + "Enabled", false));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("Polyphony"), tag + "Polyphony", false));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("BackgroundMode"), tag + "Background",
                juce::StringArray{"Black", "White"}, 0));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("CouplingMode"), tag + "Coupling",
                juce::StringArray{"LuxStral", "Free"}, 0, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("FreePixelsPerST"), tag + "px/semitone",
                juce::NormalisableRange<float>(1.0f, 200.0f, 0.5f), 36.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("PitchBendRange"), tag + "PB Range",
                juce::NormalisableRange<float>(0.0f, 24.0f, 0.5f), 2.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("st")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("AttackMs"), tag + "Attack",
                juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 10.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("DecayMs"), tag + "Decay",
                juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 50.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("SustainLevel"), tag + "Sustain",
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("ReleaseMs"), tag + "Release",
                juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 100.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            // Per-segment curvature [-1,1] (0 = linear). Set visually by bending
            // each envelope segment; MIDI-mappable like every other param.
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("AttackCurve"), tag + "Attack Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("DecayCurve"), tag + "Decay Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("ReleaseCurve"), tag + "Release Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.5f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("GlideMs"), tag + "Glide",
                juce::NormalisableRange<float>(0.0f, 5000.0f, 1.0f, 0.3f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("LfoRate"), tag + "LFO Rate",
                juce::NormalisableRange<float>(0.0f, 20.0f, 0.01f), 5.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("LfoDepth"), tag + "LFO Depth",
                juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("st")));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("VelocityCoupling"), tag + "Velocity", false));
            // MIDI infrastructure (SETUP face)
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("MidiChannel"), tag + "MIDI Channel", midiChNames, 0, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("OctaveOffset"), tag + "Octave Offset", octNames, 2, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("ReferenceNote"), tag + "Reference Note",
                noteNames, 33, kHiddenChoice));  // A3 = index 33
        }

        // ── MASK banks — spatial bandpass filter driven by the ADSR ──────────
        // Always a bandpass centred on the played note (keyboard tracking). The
        // ADSR output is the openness (0 = closed to nothing, 1 = full width).
        //   Width : band width at full open, % of image.
        //   Offset: band-centre offset from the note, % of image.
        //   Slope : edge steepness (1 = sharp, 0 = soft).
        for (int n = 0; n < 8; ++n)
        {
            const juce::String tag = "M" + juce::String(n) + " ";
            auto id = [n](const char* sfx) { return juce::ParameterID{lmParam(n, sfx), 1}; };

            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("Enabled"), tag + "Enabled", false));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("Polyphony"), tag + "Polyphony", false));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("BackgroundMode"), tag + "Background",
                juce::StringArray{"Black", "White"}, 0));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("CouplingMode"), tag + "Coupling",
                juce::StringArray{"LuxStral", "Free"}, 0, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("FreePixelsPerST"), tag + "px/semitone",
                juce::NormalisableRange<float>(1.0f, 200.0f, 0.5f), 36.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("PitchBendRange"), tag + "PB Range",
                juce::NormalisableRange<float>(0.0f, 24.0f, 0.5f), 2.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("st")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("FilterWidth"), tag + "Filter Width",
                juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 30.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("%")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("FilterOffset"), tag + "Filter Offset",
                juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("%")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("FilterSlope"), tag + "Filter Slope",
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));
            // ADSR (drives the filter cutoff/openness)
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("AttackMs"), tag + "Attack",
                juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 20.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("DecayMs"), tag + "Decay",
                juce::NormalisableRange<float>(0.5f, 10000.0f, 0.1f, 0.3f), 120.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("SustainLevel"), tag + "Sustain",
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("ReleaseMs"), tag + "Release",
                juce::NormalisableRange<float>(0.5f, 10000.0f, 0.1f, 0.3f), 200.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("AttackCurve"), tag + "Attack Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("DecayCurve"), tag + "Decay Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("ReleaseCurve"), tag + "Release Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.5f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("GlideMs"), tag + "Glide",
                juce::NormalisableRange<float>(0.0f, 5000.0f, 1.0f, 0.3f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            // Position LFO only (vibrato) — the width LFO never sounded musical.
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("LfoPosRate"), tag + "LFO Pos Rate",
                juce::NormalisableRange<float>(0.0f, 30.0f, 0.01f), 5.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("LfoPosDepth"), tag + "LFO Pos Depth",
                juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("st")));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("VelocityCoupling"), tag + "Velocity", false));
            // MIDI infrastructure (SETUP face)
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("MidiChannel"), tag + "MIDI Channel", midiChNames, 0, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("OctaveOffset"), tag + "Octave Offset", octNames, 2, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("ReferenceNote"), tag + "Reference Note",
                noteNames, 33, kHiddenChoice));  // A3 = index 33
        }
    }

    // ── REVERB / ECHO FX — per-instance automatable banks (×8) ───────────────
    for (int n = 0; n < 8; ++n)
    {
        const juce::String tag = "RV" + juce::String(n) + " ";
        auto id = [n](const char* sfx) { return juce::ParameterID{rvParam(n, sfx), 1}; };

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id("Enabled"), tag + "Enabled", false));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Decay"), tag + "Decay",
            juce::NormalisableRange<float>(0.1f, 20.0f, 0.01f, 0.4f), 3.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("s")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Diffusion"), tag + "Diffusion",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 30.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("%")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Mix"), tag + "Mix",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 60.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("%")));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id("BackgroundMode"), tag + "Background",
            juce::StringArray{"Auto", "Black", "White"}, 0));
    }
    for (int n = 0; n < 8; ++n)
    {
        const juce::String tag = "EC" + juce::String(n) + " ";
        auto id = [n](const char* sfx) { return juce::ParameterID{ecParam(n, sfx), 1}; };

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id("Enabled"), tag + "Enabled", false));
        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id("Delay"), tag + "Delay",
            1, LUX_ECHO_MAX_DELAY, 48,
            juce::AudioParameterIntAttributes{}.withLabel("lines")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Feedback"), tag + "Feedback",
            juce::NormalisableRange<float>(0.0f, 95.0f, 0.1f), 35.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("%")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Mix"), tag + "Mix",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 60.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("%")));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id("BackgroundMode"), tag + "Background",
            juce::StringArray{"Auto", "Black", "White"}, 0));
    }
    for (int n = 0; n < 8; ++n)
    {
        const juce::String tag = "EQ" + juce::String(n) + " ";
        auto id = [n](const char* sfx) { return juce::ParameterID{eqParam(n, sfx), 1}; };

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id("Enabled"), tag + "Enabled", false));
        // 9 gain nodes on the octave boundaries of the pixel/frequency axis
        // (LUX_EQ_NUM_BANDS) — 0 dB default = flat curve = pass-through.
        for (int b = 0; b < LUX_EQ_NUM_BANDS; ++b)
        {
            const auto sfx = "Band" + juce::String(b);
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{eqParam(n, sfx.toRawUTF8()), 1},
                tag + sfx,
                juce::NormalisableRange<float>(-LUX_EQ_GAIN_DB_MAX,
                                               LUX_EQ_GAIN_DB_MAX, 0.1f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("dB")));
        }
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id("BackgroundMode"), tag + "Background",
            juce::StringArray{"Auto", "Black", "White"}, 0));
    }

    // Fade-in duration [ms] — applied when restarting the live stream after Stop.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imageFadeInMs", 1}, "Fade-In",
        juce::NormalisableRange<float>(0.0f, 2000.0f, 10.0f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // ── Sampler stream preprocessing (mirroring live params independently) ────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"samplerGamma", 1}, "Sampler Gamma",
        juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"samplerContrastMin", 1}, "Sampler Contrast Min",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f));
    // samplerFreezeMode: 0=PLAY, 1=HOLD (freeze last sampler frame), 2=STOP (silence)
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"samplerFreezeMode", 1}, "Sampler Freeze Mode",
        0, 2, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"samplerFadeInMs", 1}, "Sampler Fade-In",
        juce::NormalisableRange<float>(0.0f, 2000.0f, 10.0f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // rawFreezeMode: 0=PLAY, 1=HOLD (freeze last raw frame), 2=STOP (white)
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"rawFreezeMode", 1}, "RAW Freeze Mode",
        0, 2, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"rawFadeInMs", 1}, "RAW Fade-In",
        juce::NormalisableRange<float>(0.0f, 2000.0f, 10.0f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // ── LuxSampler ──────────────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxSamplerEnabled", 1}, "LuxSampler Enabled",
        false));

    {
        juce::StringArray midiChannelNames;
        for (int i = 1; i <= 16; ++i)
            midiChannelNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerMidiChannel", 1}, "LuxSampler MIDI Channel",
            midiChannelNames, 0, kHiddenChoice));  // default = Channel 1 (index 0)
    }

    {
        juce::StringArray octaveNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerOctaveOffset", 1}, "LuxSampler Octave Offset",
            octaveNames, 2, kHiddenChoice));  // default index 2 = 0
    }

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxSamplerMaxDuration", 1}, "LuxSampler Max Duration",
        juce::NormalisableRange<float>(1.0f, 60.0f, 0.1f), 10.0f, kHiddenFloat));

    // REC / PLAY transport-button mode (engine A). Toggle = bistable click
    // (press flips record / play on↔off). Momentary = press-and-hold (start on
    // press, stop on release) — applies to BOTH the UI buttons and MIDI-mapped
    // keys. Default Toggle preserves the historical click behaviour.
    {
        juce::StringArray modeNames { "Toggle", "Momentary" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerRecMode", 1}, "LuxSampler REC Mode",
            modeNames, 0, kHiddenChoice));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerPlayMode", 1}, "LuxSampler PLAY Mode",
            modeNames, 0, kHiddenChoice));
    }

    // Number of banks shown in the SAMPLER page (1..6). A choice param (index
    // 0..5 → 1..6 banks) so the SETUP combo binds mechanically; the engine
    // keeps its NUM_SLOTS internal slots for session compatibility.
    {
        juce::StringArray bankCounts { "1", "2", "3", "4", "5", "6" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerNumBanks", 1}, "LuxSampler Banks",
            bankCounts, 3, kHiddenChoice));  // default = 4 banks
    }

    // ── LuxSampler engine B — its own play-param bank ("Part B") ─────────────
    // Same suffixes as A's legacy ids so the UI rebinds mechanically
    // (fsEngineParam). MIDI channel defaults to 2, preserving the value that
    // used to be hard-coded in the constructor sync; sessions saved before this
    // bank pick up the defaults (B used to mirror A anyway). REC/PLAY/SAVE are
    // now mapped through the unified MIDI-Learn engine (right-click the buttons),
    // so the old bespoke REC/PLAY/SAVE bind params were removed for both engines.
    {
        juce::StringArray bMidiChNames;
        for (int i = 1; i <= 16; ++i)
            bMidiChNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerBMidiChannel", 1},
            "LuxSampler B MIDI Channel", bMidiChNames, 1, kHiddenChoice));

        juce::StringArray bOctaveNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerBOctaveOffset", 1},
            "LuxSampler B Octave Offset", bOctaveNames, 2, kHiddenChoice));

        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"luxSamplerBMaxDuration", 1},
            "LuxSampler B Max Duration",
            juce::NormalisableRange<float>(1.0f, 60.0f, 0.1f), 10.0f, kHiddenFloat));

        // REC / PLAY transport-button mode (engine B) — mirrors engine A.
        juce::StringArray bModeNames { "Toggle", "Momentary" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerBRecMode", 1},
            "LuxSampler B REC Mode", bModeNames, 0, kHiddenChoice));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerBPlayMode", 1},
            "LuxSampler B PLAY Mode", bModeNames, 0, kHiddenChoice));

        // Number of banks (engine B) — mirrors luxSamplerNumBanks.
        juce::StringArray bBankCounts { "1", "2", "3", "4", "5", "6" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerBNumBanks", 1}, "LuxSampler B Banks",
            bBankCounts, 3, kHiddenChoice));  // default = 4 banks
    }

    // Image export on Save Session: bool toggle + format choice (PNG / JPEG)
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxSamplerExportImages", 1},
        "LuxSampler Export Images On Save",
        false, kHiddenBool));

    {
        juce::StringArray formats { "PNG", "JPEG" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerExportFormat", 1},
            "LuxSampler Export Format",
            formats, 0, kHiddenChoice));
    }

    // ── FrameSequencer parameters ─────────────────────────────────────────────

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"seqEnabled",  1}, "Sequencer Enabled", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"seqBpm",      1}, "Seq BPM",
        juce::NormalisableRange<float>(40.0f, 240.0f, 0.5f), 120.0f));
    // 2..16: the sequencer grid displays at most 16 cells (8×2) and a single
    // step is not a sequence — the whole span is MIDI/automation-addressable.
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"seqNumSteps", 1}, "Seq Steps", 2, 16, 16));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"seqLoop",     1}, "Seq Loop",    true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"seqDawSync",  1}, "Seq DAW Sync", true));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"seqBeatsPerStep", 1}, "Seq Beats/Step", 1, 8, 1));
    // Transport as an automatable param (0=Stop, 1=Play, 2=Hold) so the DAW can
    // drive / MIDI-map the PLAY-HOLD-STOP buttons. parameterChanged() maps it to
    // FrameSequencer::uiStop/uiPlay(uiResume)/uiHold (all RT-safe atomics).
    // Forced back to Stop on session restore — never auto-run on open.
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"seqTransport", 1}, "Seq Transport",
        juce::StringArray{"Stop", "Play", "Hold"}, 0));

    // ── SCORE playback transport (LuxSampler score slot) ─────────────────────
    // The SCORE generator itself stays out of the APVTS (offline settings), but
    // its playback transport is a live performance control: expose it so the
    // DAW can automate / MIDI-map it. parameterChanged() relays to LuxSampler.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"scorePlaying", 1}, "Score Play", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"scoreLoop", 1}, "Score Loop", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"scoreReverse", 1}, "Score Reverse", false));
    {
        // Same feel as the SCORE page knob: 0.1×–6× with 1× at the centre.
        juce::NormalisableRange<float> spd(0.1f, 6.0f, 0.01f);
        spd.setSkewForCentre(1.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"scoreSpeed", 1}, "Score Speed", spd, 1.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("x")));
    }

    // ── M9: IMAGE / VIDEO / CAMERA source modules ────────────────────────────
    // The line position IS the musical control of these sources — automatable,
    // like the SCORE transport above. Loop modes mirror the sampler's:
    // Once / Loop / Reverse / Ping-Pong.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imgSrcPos", 1}, "Image Src Line",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.5f));
    {
        // Full-image scan time (one traversal top→bottom), log feel around 5 s.
        juce::NormalisableRange<float> dur(0.1f, 120.0f, 0.01f);
        dur.setSkewForCentre(5.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"imgSrcDuration", 1}, "Image Src Scan Time", dur, 5.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("s")));
    }
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"imgSrcLoop", 1}, "Image Src Loop",
        juce::StringArray{"Once", "Loop", "Reverse", "Ping-Pong"}, 1));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"imgSrcPlay", 1}, "Image Src Play", false));
    // ACTIVE: off = the source feeds NOTHING (its chain streams blank paper);
    // media/params are kept, on resumes instantly. Automatable/MIDI-learnable.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"imgSrcEnabled", 1}, "Image Src Active", true));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"vidSrcLine", 1}, "Video Src Line",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.5f));
    {
        juce::NormalisableRange<float> spd(0.1f, 4.0f, 0.01f);
        spd.setSkewForCentre(1.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"vidSrcSpeed", 1}, "Video Src Speed", spd, 1.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("x")));
    }
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"vidSrcLoop", 1}, "Video Src Loop",
        juce::StringArray{"Once", "Loop", "Reverse", "Ping-Pong"}, 1));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"vidSrcPlay", 1}, "Video Src Play", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"vidSrcEnabled", 1}, "Video Src Active", true));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"camSrcLine", 1}, "Camera Src Line",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"camSrcEnabled", 1}, "Camera Src Active", true));

    // ── Video Scroll — live controls (hidden from DAW automation) ─────────────
    // (Purge 2026-07-12: the dead global singletons videoScrollEnabled/
    // Direction/Exposure/BlendMode/Bpm/MidiSync are gone — the per-instance
    // videoScroll{N}_* banks carry the module params.)

    // Transport: when true the waterfall is frozen in place (Play/Pause); the
    // renderer drains its capture ring but performs no scroll/stamp.  "Stop"
    // sets this true AND clears the image via requestVideoScrollClear().
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoScrollPaused", 1}, "Video Scroll Paused",
        true, kHiddenBool));   // start paused — the user presses Play to run

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollMode", 1}, "Video Scroll Mode",
        juce::StringArray{
            "0 deg", "90 deg", "180 deg", "270 deg"
        }, 0, kHiddenChoice));

    // Bipolar scroll speed (legacy birth-line model): negative = reverse,
    // 0 = frozen, positive = forward.  Magnitude maps exponentially in the
    // renderer (px = sign(s) * (2^(3*|s|) - 1)), giving ±7 px/frame at the ends.
    // This single control replaces the old Speed slider + Direction dropdown.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollSpeed", 1}, "Video Scroll Speed",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f),
        0.33f, kHiddenFloat));

    // ── Video Scroll — display configuration (Settings window) ────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollZoom", 1}, "Video Scroll Zoom",
        juce::NormalisableRange<float>(0.5f, 4.0f, 0.05f),
        1.0f, kHiddenFloat.withLabel("x")));

    // Birth-line position: -1 = far edge (top), 0 = centre (symmetric
    // bidirectional scroll), +1 = near edge (bottom → classic upward waterfall).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollLinePos", 1}, "Video Line Position",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f),
        1.0f, kHiddenFloat));

    // Thickness of each freshly-drawn scanline: 0 = single pixel,
    // 1 = full viewport ("barcode" mode).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollLineThickness", 1}, "Video Line Thickness",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f),
        0.0f, kHiddenFloat));

    // Progressive aging with distance from the birth line: the farther a
    // scanline has scrolled, the more it desaturates and dims.  Applied at
    // display time in paint() (NOT baked into the history buffer), so it is
    // instant and never affects the scroll.  0 = none, 1 = strong.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollFade", 1}, "Video Fade",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f),
        0.0f, kHiddenFloat));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoInvertColor", 1}, "Video Invert Color",
        false, kHiddenBool));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoColorMode", 1}, "Video Color Mode (RGB)",
        false, kHiddenBool));

    // ── Video Scroll — window geometry (saved across sessions) ────────────────
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"videoWindowWidth", 1}, "Video Window Width",
        320, 2560, 800, kHiddenInt));

    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"videoWindowHeight", 1}, "Video Window Height",
        240, 1440, 600, kHiddenInt));

    // ── MIDI follow — when a mapped MIDI controller moves a parameter (played
    // notes excluded), jump the editor to that module's page. Toggled in the
    // gear-wheel System Settings. Non-automatable (a UI preference). ──────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"midiFollowParam", 1}, "MIDI Follow Selection",
        true, kHiddenBool));

    // ── Video: live performance params ────────────────────────────────────────
    // Source = which synth engine's input image we visualize — the engine
    // taps published by the chain executors (AUDIO_IMAGE_ENGINE_TAP_*), so
    // the waterfall always matches what the audio engine actually sees.
    //   0 = LuxStral          (engine tap A)
    //   1 = LuxSynth/LuxWave  (Path-B tap — LuxWave shares it)
    //   2 = AllSynth          (50/50 blend of the two streams above)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollSource", 1}, "Video Scroll Source",
        juce::StringArray{"LuxStral", "LuxSynth/LuxWave", "AllSynth"}, 0));

    // Non-linear temporal compression: applied at display time in paint() as a
    // progressive vertical squish with distance from the birth line — content
    // is full-scale at the source and packed ever tighter as it ages (the rows
    // it swallows are averaged, giving a soft blur).  1 = linear (no squish),
    // higher = more history squeezed on screen.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollMaxDuration", 1}, "Video Compression (time squish)",
        juce::NormalisableRange<float>(1.0f, 64.0f, 1.0f), 1.0f, kHiddenFloat));

    // ── SP3CTRA source — acquisition speed (frame-advance brake) ──────────────
    // Brakes how often the live CIS line advances the active frame (audio +
    // visual).  See AcquisitionGate.h.  Off = full-rate (no behaviour change).
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"acqGateMode", 1}, "Acquisition Gate",
        juce::StringArray{"Off", "Internal (LFO)", "DAW Sync"}, 0));
    // Internal-mode period in ms (skewed for finer control at short periods).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"acqGateRateMs", 1}, "Acquisition Rate",
        juce::NormalisableRange<float>(1.0f, 5000.0f, 0.1f, 0.3f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    // DAW-sync musical division (period per advance).  Index → beats in the
    // kSyncDivBeats table consumed in processBlock.
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"acqGateSyncDiv", 1}, "Acquisition Division",
        juce::StringArray{"1/1", "1/2", "1/4", "1/8", "1/16", "1/32"}, 2));
    // Common refresh-rate multiplier/divider (stretches the period for very slow
    // updates).  Index → factor in the kRefreshFactor table consumed in processBlock.
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"acqGateMultDiv", 1}, "Acquisition Mult/Div",
        juce::StringArray{"/32", "/16", "/8", "/4", "/2", "x1", "x2", "x4"}, 5));

    // ── VIDEO SCROLL output modules — per-instance automatable banks (×8) ──────
    // Each VideoScroll module instance owns a slot 0..7 (ModuleInstance.slot) that
    // keys both its RT capture ring (video_scroll_instance) and this APVTS bank.
    // Same ranges/defaults as the legacy global controls; the contextual zone-3
    // panel binds videoScroll{N}_* and the right-band mixer binds videoMix{N}_*.
    for (int n = 0; n < 8; ++n)
    {
        const juce::String p  = "videoScroll" + juce::String(n) + "_";
        const juce::String mx = "videoMix"    + juce::String(n) + "_";
        const juce::String tag = "VS" + juce::String(n) + " ";

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{p + "mode", 1}, tag + "Mode",
            juce::StringArray{"0 deg", "90 deg", "180 deg", "270 deg"}, 0));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "speed", 1}, tag + "Speed",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.33f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "linePos", 1}, tag + "Line Pos",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "thickness", 1}, tag + "Thickness",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "zoom", 1}, tag + "Zoom",
            juce::NormalisableRange<float>(0.5f, 4.0f, 0.05f), 1.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("x")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "fade", 1}, tag + "Fade",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "compress", 1}, tag + "Compression",
            juce::NormalisableRange<float>(1.0f, 64.0f, 1.0f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{p + "invert", 1}, tag + "Invert Color", false));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{p + "colorMode", 1}, tag + "Color (RGB)", false));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{p + "paused", 1}, tag + "Paused", false));
        // Per-instance output enable (the rack block's LED toggles this). Default
        // ON so a freshly placed VideoScroll produces output immediately; the mixer
        // skips this output while off.
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{p + "enabled", 1}, tag + "Enabled", true));

        // Right-band mixer voice for this output.
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{mx + "level", 1}, "Mix " + tag + "Level",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{mx + "blend", 1}, "Mix " + tag + "Blend",
            juce::StringArray{"Mix", "Add", "Screen"}, 0));
    }

    return { params.begin(), params.end() };
}

//==============================================================================
Sp3ctraAudioProcessor::Sp3ctraAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
       apvts(*this, nullptr, "Parameters", createParameterLayout())
#else
     : apvts(*this, nullptr, "Parameters", createParameterLayout())
#endif
{
    log_info("VST", "=============================================================");
    log_info("VST", "Sp3ctraAudioProcessor: Constructor - Initializing VST plugin");
    log_info("VST", "  Using APVTS (AudioProcessorValueTreeState) for parameters");
    log_info("VST", "=============================================================");

    // VOICE module TTS diagnostic — no-op unless SP3CTRA_TTS_SMOKE is set.
    PiperTts::runSmokeTestIfRequested();


    // Cache parameter pointers for fast access
    udpPortParam = apvts.getRawParameterValue(PARAM_UDP_PORT);
    udpByte1Param = apvts.getRawParameterValue(PARAM_UDP_BYTE1);
    udpByte2Param = apvts.getRawParameterValue(PARAM_UDP_BYTE2);
    udpByte3Param = apvts.getRawParameterValue(PARAM_UDP_BYTE3);
    udpByte4Param = apvts.getRawParameterValue(PARAM_UDP_BYTE4);
    sensorDpiParam = apvts.getRawParameterValue(PARAM_SENSOR_DPI);
    logLevelParam = apvts.getRawParameterValue(PARAM_LOG_LEVEL);
    deviceEnabledParam  = apvts.getRawParameterValue(PARAM_DEVICE_ENABLED);
    visualizerModeParam = apvts.getRawParameterValue(PARAM_VISUALIZER_MODE);
    masterVolumeParam   = apvts.getRawParameterValue("masterVolume");

    // Route the MIDI-mapping engine's NON-APVTS targets (sampler play params /
    // action buttons) back to this processor. Set before any state restore so
    // restored virtual mappings resolve.
    midiMap_.setVirtualSink(this);
    // Warm up the sampler-target skewed ranges here (message thread) so their
    // first use never triggers a lazy static init on the audio thread.
    (void) SamplerMidiTargets::speedRange();
    (void) SamplerMidiTargets::powerRange();
    // Seed the MIDI EQ-band pending latches to the "no pending" sentinel (-1);
    // atomic<float> default-inits to 0.0f which is a valid gain value.
    for (auto& engine : smpEqPending)
        for (auto& slot : engine)
            for (auto& band : slot)
                band.store(-1.0f, std::memory_order_relaxed);

    // Cache raw-parameter pointers read by processBlock (audio thread) —
    // getRawParameterValue("literal") allocates a juce::String per call.
    for (int e = 0; e < 2; ++e)   // sampler engines A (0) and B (1)
        luxSamplerMidiChannelParam[e] = apvts.getRawParameterValue(fsEngineParam(e, "MidiChannel"));
    for (int s = 0; s < ChainModel::kMaxChains; ++s)
    {
        luxpitchMidiChannelParam [s] = apvts.getRawParameterValue(lpParam(s, "MidiChannel"));
        luxpitchOctaveOffsetParam[s] = apvts.getRawParameterValue(lpParam(s, "OctaveOffset"));
        luxmaskMidiChannelParam  [s] = apvts.getRawParameterValue(lmParam(s, "MidiChannel"));
        luxmaskOctaveOffsetParam [s] = apvts.getRawParameterValue(lmParam(s, "OctaveOffset"));
    }
    luxsynthEnabledParam        = apvts.getRawParameterValue("luxsynthEnabled");
    luxsynthMidiChannelParam    = apvts.getRawParameterValue("luxsynthMidiChannel");
    luxsynthOctaveOffsetParam   = apvts.getRawParameterValue("luxsynthOctaveOffset");
    luxsynthVolumeParam         = apvts.getRawParameterValue("luxsynthVolume");
    luxwaveEnabledParam         = apvts.getRawParameterValue("luxwaveEnabled");
    luxwaveMidiChannelParam     = apvts.getRawParameterValue("luxwaveMidiChannel");
    luxwaveOctaveOffsetParam    = apvts.getRawParameterValue("luxwaveOctaveOffset");
    luxwaveVolumeParam          = apvts.getRawParameterValue("luxwaveVolume");
    luxwaveAttackMsParam        = apvts.getRawParameterValue("luxwaveAttackMs");
    luxwaveDecayMsParam         = apvts.getRawParameterValue("luxwaveDecayMs");
    luxwaveSustainLevelParam    = apvts.getRawParameterValue("luxwaveSustainLevel");
    luxwaveReleaseMsParam       = apvts.getRawParameterValue("luxwaveReleaseMs");
    luxwaveAttackCurveParam     = apvts.getRawParameterValue("luxwaveAttackCurve");
    luxwaveDecayCurveParam      = apvts.getRawParameterValue("luxwaveDecayCurve");
    luxwaveReleaseCurveParam    = apvts.getRawParameterValue("luxwaveReleaseCurve");
    luxwaveFilterCutoffParam    = apvts.getRawParameterValue("luxwaveFilterCutoff");
    luxwaveFilterEnvDepthParam  = apvts.getRawParameterValue("luxwaveFilterEnvDepth");
    luxwaveLfoRateParam         = apvts.getRawParameterValue("luxwaveLfoRate");
    luxwaveLfoDepthParam        = apvts.getRawParameterValue("luxwaveLfoDepth");
    luxwaveScanModeParam        = apvts.getRawParameterValue("luxwaveScanMode");
    luxwaveAmplitudeParam       = apvts.getRawParameterValue("luxwaveAmplitude");
    acqGateModeParam            = apvts.getRawParameterValue("acqGateMode");
    acqGateRateMsParam          = apvts.getRawParameterValue("acqGateRateMs");
    acqGateSyncDivParam         = apvts.getRawParameterValue("acqGateSyncDiv");
    acqGateMultDivParam         = apvts.getRawParameterValue("acqGateMultDiv");
    luxstralVolumeParam         = apvts.getRawParameterValue("luxstralVolume");

    // CC1 mod-wheel targets driven from processBlock (setValueNotifyingHost)
    for (int s = 0; s < ChainModel::kMaxChains; ++s)
    {
        luxpitchLfoDepthParam  [s] = apvts.getParameter(lpParam(s, "LfoDepth"));
        luxmaskLfoPosDepthParam[s] = apvts.getParameter(lmParam(s, "LfoPosDepth"));
    }

    // Register as listener for parameter changes
    apvts.addParameterListener(PARAM_UDP_PORT, this);
    apvts.addParameterListener(PARAM_UDP_BYTE1, this);
    apvts.addParameterListener(PARAM_UDP_BYTE2, this);
    apvts.addParameterListener(PARAM_UDP_BYTE3, this);
    apvts.addParameterListener(PARAM_UDP_BYTE4, this);
    apvts.addParameterListener(PARAM_SENSOR_DPI, this);
    apvts.addParameterListener(PARAM_LOG_LEVEL, this);
    
    // Register LuxStral parameter listeners
    apvts.addParameterListener("luxstralTuning", this);
    apvts.addParameterListener("luxstralRootNote", this);
    apvts.addParameterListener("luxstralNumOctaves", this);
    apvts.addParameterListener("luxstralAttackMs", this);
    apvts.addParameterListener("luxstralReleaseMs", this);
    apvts.addParameterListener("luxstralStereoEnable", this);
    apvts.addParameterListener("luxstralStereoTempAmp", this);
    apvts.addParameterListener("luxstralNoiseGateThreshold", this);
    apvts.addParameterListener("luxstralNumWorkers", this);
    apvts.addParameterListener("luxstralPhysiologicalFilter", this);
    apvts.addParameterListener("luxstralPhysiologicalDepth", this);
    apvts.addParameterListener("luxstralSoftLimitThreshold", this);
    apvts.addParameterListener("luxstralSoftLimitKnee", this);
    // M4 — core-side LuxSynth engine feed (lx_fft_* config mirror)
    apvts.addParameterListener("lxFftBins", this);
    apvts.addParameterListener("lxFftSmoothing", this);
    // Phase management (mode + gate + position + drift)
    apvts.addParameterListener("luxstralPhaseMode", this);
    apvts.addParameterListener("luxstralPhaseSensitivity", this);
    apvts.addParameterListener("luxstralPhasePosition", this);
    apvts.addParameterListener("luxstralPhaseDriftCents", this);
    
    // Register StrokeForge parameter listeners
    apvts.addParameterListener("sfEnabled", this);
    apvts.addParameterListener("sfBlobContrastAdaptive", this);
    apvts.addParameterListener("sfBlobContrastSensitivity", this);
    apvts.addParameterListener("sfMaxHarmonics", this);
    apvts.addParameterListener("sfHarmonicAmpFloor", this);
    apvts.addParameterListener("sfVolumeCenterSigma", this);
    apvts.addParameterListener("sfPhaseCoherence", this);
    apvts.addParameterListener("sfPhaseSmoothAlpha", this);
    apvts.addParameterListener("sfMorphWidthScale", this);
    apvts.addParameterListener("sfWavetableMinWidth", this);
    apvts.addParameterListener("sfBlobFocusSigma", this);
    apvts.addParameterListener("sfSpectralWidthThreshold", this);
    apvts.addParameterListener("sfFocusOnly", this);

    // LuxSynth blob detection (independent of StrokeForge)
    apvts.addParameterListener("lxBlobThreshold",  this);
    apvts.addParameterListener("lxBlobMinWidth",   this);
    apvts.addParameterListener("lxBlobMergeGap",   this);
    apvts.addParameterListener("lxBlobColorSplit", this);

    // SPCTR blob detection — IMAGE LUXSTRAL tab (drives both visualizer + StrokeForge audio)
    apvts.addParameterListener("spctrBlobThreshold",  this);
    apvts.addParameterListener("spctrBlobMinWidth",   this);
    apvts.addParameterListener("spctrBlobMergeGap",   this);
    apvts.addParameterListener("spctrBlobColorSplit", this);

    // Image pipeline parameters (live transport + opacity + fade-in)
    apvts.addParameterListener("imageFreezeMode",      this);
    apvts.addParameterListener("imageLiveOpacity",     this);
    apvts.addParameterListener("imageSamplerOpacity",  this);
    apvts.addParameterListener("imageMixBalance",      this);
    apvts.addParameterListener("imageFadeInMs",        this);
    // Sampler-specific preprocessing parameters
    apvts.addParameterListener("samplerGamma",         this);
    apvts.addParameterListener("samplerContrastMin",   this);
    apvts.addParameterListener("samplerFreezeMode",    this);
    apvts.addParameterListener("samplerFadeInMs",      this);
    apvts.addParameterListener("rawFreezeMode",        this);
    apvts.addParameterListener("rawFadeInMs",          this);

    // Pitch/Mask/Reverb/Echo/EQ per-instance banks + per-OUT conditioning
    // banks (J1: iterate the SINGLE manifest): every bank param of every slot
    // funnels into the same applyConfigurationToCore() sync. RULE learned in
    // P2b: creating APVTS params is NOT enough — without addParameterListener
    // a knob move never re-syncs g_sp3ctra_config ("no effect").
    // "luxstralOut…" rides the startsWith("luxstral") branch of
    // applyParameterChange; the others land on the generic fallback — all end
    // in applyConfigurationToCore(false). VideoScroll and the Sampler keep
    // their own dedicated listening mechanisms (excluded here, iso-behaviour).
    for (const auto& bank : kModuleParamManifest)
    {
        if (bank.type == ModuleType::VideoScroll
            || bank.type == ModuleType::Sampler)
            continue;
        for (int s = 0; s < bank.numSlots; ++s)
            for (int i = 0; i < bank.numSuffixes; ++i)
                apvts.addParameterListener(bank.paramId(s, bank.suffixes[i]),
                                           this);
    }
    // Create the sampler engines: A (slot 0) + B (slot 1). Per-engine enable is
    // driven by module presence in deriveChainRouting(); both share the single
    // modulated playback channel (one plays at a time — see the arbiter).
    luxSampler  = std::make_unique<LuxSampler>(0);
    luxSamplerB = std::make_unique<LuxSampler>(1);

    // SCORE generation defaults (shared by the PLAY page and the SETUP panel).
    score_settings_defaults(&scoreSettings_);
    scoreSettings_.writingSpeed = 2.5;   // page maps to a sensible default duration

    // Create FrameSequencer and wire it to the ordered sampler engines (A, B).
    frameSequencer = std::make_unique<FrameSequencer>();
    {
        LuxSampler* engines[] = { luxSampler.get(), luxSamplerB.get() };
        frameSequencer->setSamplers(engines, 2);
    }

    // ── M9: IMAGE / VIDEO / CAMERA source engines + service thread ──────────
    imageSource_  = std::make_unique<ImageSourceEngine>();
    videoSource_  = std::make_unique<VideoSourceEngine>();
    cameraSource_ = std::make_unique<CameraSourceEngine>();
    mediaService_ = std::make_unique<MediaSourceService>(*imageSource_,
                                                         *videoSource_,
                                                         *cameraSource_);
    // ONCE traversals snap the automatable play param back off when they end.
    imageSource_->onPlaybackFinished = [this]
    {
        if (auto* p = apvts.getParameter(PARAM_IMGSRC_PLAY))
            p->setValueNotifyingHost(0.0f);
    };
    videoSource_->onPlaybackFinished = [this]
    {
        if (auto* p = apvts.getParameter(PARAM_VIDSRC_PLAY))
            p->setValueNotifyingHost(0.0f);
    };
    // Initial param sync (media/presence arrive later: state restore + model).
    imageSource_->setPosition (apvts.getRawParameterValue(PARAM_IMGSRC_POS)->load());
    imageSource_->setDurationS(apvts.getRawParameterValue(PARAM_IMGSRC_DUR)->load());
    imageSource_->setLoopMode ((int) apvts.getRawParameterValue(PARAM_IMGSRC_LOOP)->load());
    imageSource_->setEnabled  (apvts.getRawParameterValue(PARAM_IMGSRC_ENABLED)->load() > 0.5f);
    videoSource_->setLineFrac (apvts.getRawParameterValue(PARAM_VIDSRC_LINE)->load());
    videoSource_->setSpeed    (apvts.getRawParameterValue(PARAM_VIDSRC_SPEED)->load());
    videoSource_->setLoopMode ((int) apvts.getRawParameterValue(PARAM_VIDSRC_LOOP)->load());
    videoSource_->setEnabled  (apvts.getRawParameterValue(PARAM_VIDSRC_ENABLED)->load() > 0.5f);
    cameraSource_->setLineFrac(apvts.getRawParameterValue(PARAM_CAMSRC_LINE)->load());
    cameraSource_->setEnabled (apvts.getRawParameterValue(PARAM_CAMSRC_ENABLED)->load() > 0.5f);

    // Register LuxSampler parameter listeners
    apvts.addParameterListener(PARAM_FS_ENABLED,    this);
    apvts.addParameterListener(PARAM_FS_MIDI_CH,    this);
    apvts.addParameterListener(PARAM_FS_OCT_OFFSET, this);
    apvts.addParameterListener(PARAM_FS_MAX_DUR,    this);
    // Engine B bank (same play params, own values)
    apvts.addParameterListener(fsEngineParam(1, "MidiChannel"),  this);
    apvts.addParameterListener(fsEngineParam(1, "OctaveOffset"), this);
    apvts.addParameterListener(fsEngineParam(1, "MaxDuration"),  this);

    apvts.addParameterListener(PARAM_SEQ_ENABLED,  this);
    apvts.addParameterListener(PARAM_SEQ_BPM,      this);
    apvts.addParameterListener(PARAM_SEQ_NSTEPS,   this);
    apvts.addParameterListener(PARAM_SEQ_LOOP,     this);
    apvts.addParameterListener(PARAM_SEQ_DAW_SYNC, this);
    apvts.addParameterListener(PARAM_SEQ_BPS,      this);
    apvts.addParameterListener(PARAM_SEQ_TRANSPORT, this);

    // SCORE playback transport (relayed to LuxSampler in parameterChanged)
    apvts.addParameterListener(PARAM_SCORE_PLAYING, this);
    apvts.addParameterListener(PARAM_SCORE_LOOP,    this);
    apvts.addParameterListener(PARAM_SCORE_REVERSE, this);
    apvts.addParameterListener(PARAM_SCORE_SPEED,   this);

    // M9 — IMAGE / VIDEO / CAMERA source params → engines
    apvts.addParameterListener(PARAM_IMGSRC_POS,     this);
    apvts.addParameterListener(PARAM_IMGSRC_DUR,     this);
    apvts.addParameterListener(PARAM_IMGSRC_LOOP,    this);
    apvts.addParameterListener(PARAM_IMGSRC_PLAY,    this);
    apvts.addParameterListener(PARAM_IMGSRC_ENABLED, this);
    apvts.addParameterListener(PARAM_VIDSRC_LINE,    this);
    apvts.addParameterListener(PARAM_VIDSRC_SPEED,   this);
    apvts.addParameterListener(PARAM_VIDSRC_LOOP,    this);
    apvts.addParameterListener(PARAM_VIDSRC_PLAY,    this);
    apvts.addParameterListener(PARAM_VIDSRC_ENABLED, this);
    apvts.addParameterListener(PARAM_CAMSRC_LINE,    this);
    apvts.addParameterListener(PARAM_CAMSRC_ENABLED, this);

    // Sync LuxSampler config with initial APVTS values
    luxSampler->setEnabled(*apvts.getRawParameterValue(PARAM_FS_ENABLED) > 0.5f);
    luxSampler->setMidiChannel(
        static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_MIDI_CH)) + 1);
    luxSampler->setOctaveOffset(
        static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_OCT_OFFSET)) - 2);
    luxSampler->setMaxDuration(*apvts.getRawParameterValue(PARAM_FS_MAX_DUR));

    // SCORE transport params → engine (speed 1×, loop on by default).
    luxSampler->setScoreSpeed(*apvts.getRawParameterValue(PARAM_SCORE_SPEED));
    luxSampler->setScoreLoopMode(scoreLoopModeFromParams(apvts));

    // Engine B: its own APVTS bank (luxSamplerB*) — MIDI channel defaults to 2
    // so direct MIDI doesn't double-trigger out of the box. Per-engine enable
    // is set authoritatively by deriveChainRouting().
    if (luxSamplerB)
    {
        luxSamplerB->setMidiChannel(
            static_cast<int>(*apvts.getRawParameterValue(fsEngineParam(1, "MidiChannel"))) + 1);
        luxSamplerB->setOctaveOffset(
            static_cast<int>(*apvts.getRawParameterValue(fsEngineParam(1, "OctaveOffset"))) - 2);
        luxSamplerB->setMaxDuration(*apvts.getRawParameterValue(fsEngineParam(1, "MaxDuration")));
    }

    // ── Acquire the process-wide shared core ─────────────────────────────────
    // If this is the FIRST plugin instance in this DAW process → creates the
    // singleton (UDP socket, image pipeline, synthesis engine not yet started).
    // If a SECOND instance is being created → returns the existing singleton.
    // The shared_ptr keeps the singleton alive for this instance's lifetime.
    sharedCore = Sp3ctraSharedCore::acquire();
    
    // 🔧 LAZY INITIALIZATION: Do NOT start the shared pipeline here.
    // Host ordering DIFFERS per wrapper — both must work:
    //   DAW:        ctor → setStateInformation() → prepareToPlay()
    //   Standalone: ctor → prepareToPlay() → setStateInformation() → editor
    // We defer startWithConfig() to prepareToPlay() so we have the correct
    // sample rate and buffer size when initializing LuxStral; coreNeedsInit
    // makes setStateInformation hot-reload instead when it runs second.
    
    // Initialize the LuxPitch / LuxMask processing instances.
    // Since the single-snapshot refactor (M2) there is ONE simulation per
    // insert: the synthesis-thread instance.  Visualizers read the published
    // the contextual selection tap instead of
    // re-simulating.
    // M6 Phase 2 — init the whole per-chain instance pool (slot 0 == legacy).
    lux_pitch_init_all();
    lux_mask_init_all();
    lux_reverb_init_all();
    lux_echo_init_all();
    lux_eq_init_all();
    video_scroll_init_all();   // init 8 VideoScroll capture rings (RT pool) before the synth thread starts

    // Just update g_sp3ctra_config with current APVTS defaults (no socket/buffer creation)
    applyConfigurationToCore(false);

    // M6 Phase 2 — build the default chain topology and derive routing. A saved
    // session reloads it later in setStateInformation().
    loadChainModelFromState();

    // Deferred-dispatch table for parameterChanged() (see dispatcher): one
    // dirty flag per parameter, drained by the 30 ms message-thread timer.
    for (auto* p : getParameters())
        if (auto* pw = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
            if (paramIndexById_.find(pw->paramID) == paramIndexById_.end())
            {
                paramIndexById_[pw->paramID] = deferredParamIds_.size();
                deferredParamIds_.add(pw->paramID);
            }
    paramDirty_ = std::make_unique<std::atomic<bool>[]>(
        (size_t) juce::jmax(1, deferredParamIds_.size()));
    for (int i = 0; i < deferredParamIds_.size(); ++i)
        paramDirty_[(size_t) i].store(false, std::memory_order_relaxed);
    {
        const auto it = paramIndexById_.find(PARAM_SCORE_PLAYING);
        scorePlayingParamIdx_ = (it != paramIndexById_.end()) ? it->second : -1;
    }
    startTimer(30);

    log_info("VST", "Sp3ctraAudioProcessor: Constructor complete (deferred init)");
    log_info("VST", "  - Shared core acquired (ref-count now %ld)",
             sharedCore.use_count());
    log_info("VST", "  - Pipeline start deferred to prepareToPlay()");
}

void Sp3ctraAudioProcessor::getMusicalFrequencyRange(double& lowHz, double& highHz) const noexcept
{
    // Mirror the LuxStral range driven by Tuning + Root Note + Octaves, kept in
    // sync in g_sp3ctra_config whenever those musical params change.
    extern sp3ctra_config_t g_sp3ctra_config;
    double lo = (double) g_sp3ctra_config.low_frequency;
    double hi = (double) g_sp3ctra_config.high_frequency;
    // Defensive fallback if the config has not been synced yet.
    if (!(lo > 0.0) || !(hi > lo))
    {
        lo = 65.41;       // C2
        hi = 16744.04;    // ~8 octaves above C2
    }
    lowHz  = lo;
    highHz = hi;
}

void Sp3ctraAudioProcessor::getScoreFrequencyRange(double& lowHz, double& highHz) const noexcept
{
    if (!scoreFreq_.manual)
    {
        getMusicalFrequencyRange(lowHz, highHz);
        return;
    }

    // Manual override — same formula LuxStral uses (root MIDI = 24 + index).
    const int    rootMidi = 24 + scoreFreq_.rootIndex;
    double low  = scoreFreq_.tuning * pow(2.0, (double)(rootMidi - 69) / 12.0);
    double high = low * pow(2.0, (double) scoreFreq_.octaves);
    if (high > 20000.0) high = 20000.0;
    if (!(low > 0.0) || !(high > low)) { low = 65.41; high = 16744.04; }
    lowHz  = low;
    highHz = high;
}

Sp3ctraAudioProcessor::~Sp3ctraAudioProcessor()
{
    log_info("VST", "=============================================================");
    log_info("VST", "Sp3ctraAudioProcessor: Destructor - Shutting down");
    log_info("VST", "=============================================================");

    // ── M9: media source service FIRST (uses Context/buffers owned by sharedCore) ──
    if (mediaService_)
    {
        mediaService_->stopThread(2000);
        mediaService_.reset();
    }
    if (cameraSource_) cameraSource_->closeDevice();   // release the capture device
    imageSource_.reset();
    videoSource_.reset();
    cameraSource_.reset();

    // ── LuxSampler (uses AudioImageBuffers / DoubleBuffer owned by sharedCore) ──
    // Must stop before releasing sharedCore to avoid use-after-free.
    if (luxSamplerB)
    {
        luxSamplerB->stopPlayerThread();
        luxSamplerB.reset();
    }
    if (luxSampler)
    {
        log_info("VST", "Stopping LuxSampler player thread...");
        luxSampler->stopPlayerThread();
        luxSampler.reset();
        log_info("VST", "LuxSampler stopped");
    }

    // ── Release the shared core ──────────────────────────────────────────────
    // Decrements the ref-count. If this is the last plugin instance in the DAW
    // process (ref-count → 0), Sp3ctraSharedCore::~Sp3ctraSharedCore() fires and:
    //   1. Stops AudioProcessingThread
    //   2. Calls synth_luxstral_cleanup()
    //   3. Stops UdpReceiverThread
    //   4. Calls sp3ctraCore->shutdown() (frees buffers)
    //
    // If another instance is still alive (ref-count ≥ 1), the singleton keeps
    // running — the other instance continues producing audio normally.
    if (sharedCore)
    {
        log_info("VST", "Releasing shared core (remaining ref-count will be %ld)...",
                 static_cast<long>(sharedCore.use_count()) - 1);
        sharedCore.reset();
    }

    log_info("VST", "=============================================================");
    log_info("VST", "Sp3ctraAudioProcessor: Destructor complete");
    log_info("VST", "=============================================================");
}

//==============================================================================
const juce::String Sp3ctraAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool Sp3ctraAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool Sp3ctraAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool Sp3ctraAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double Sp3ctraAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int Sp3ctraAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int Sp3ctraAudioProcessor::getCurrentProgram()
{
    return 0;
}

void Sp3ctraAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String Sp3ctraAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void Sp3ctraAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void Sp3ctraAudioProcessor::setVisualizerSuspendedSafely (bool suspend)
{
    auto apply = [] (Sp3ctraAudioProcessor& p, bool s)
    {
        if (auto* editor = dynamic_cast<Sp3ctraAudioProcessorEditor*>(p.getActiveEditor()))
        {
            if (s) editor->suspendVisualizer();
            else   editor->resumeVisualizer();
        }
    };

    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread())
    {
        apply(*this, suspend);
        return;
    }

    // Off the message thread (host-dependent prepareToPlay): the editor may be
    // mid-destruction on the message thread — marshal instead of touching it.
    juce::WeakReference<Sp3ctraAudioProcessor> weakThis(this);
    juce::MessageManager::callAsync([weakThis, suspend, apply]
    {
        if (auto* p = weakThis.get())
            apply(*p, suspend);
    });
}

void Sp3ctraAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // 🛡️ PROTECTION: Suspend visualizer to prevent Metal/CoreGraphics race
    setVisualizerSuspendedSafely(true);

    log_info("VST", "=============================================================");
    log_info("VST", "prepareToPlay - SR=%.1f Hz, BS=%d samples", sampleRate, samplesPerBlock);

    // ── Update global audio parameters (needed by startWithConfig) ───────────
    extern sp3ctra_config_t g_sp3ctra_config;
    int oldSampleRate = g_sp3ctra_config.sampling_frequency;
    g_sp3ctra_config.sampling_frequency = static_cast<int>(sampleRate);
    // audio_buffer_size drives how many samples the synthesis engine WRITES into
    // the shared output buffers. Once the pipeline runs, it must only change
    // together with a successful buffer reallocation (ensureAudioBufferSize()
    // below) — updating it unconditionally made the engine overflow buffers
    // still allocated for the previous host block size.
    if (!sharedCore || !sharedCore->isReady())
        g_sp3ctra_config.audio_buffer_size = samplesPerBlock;
    g_sp3ctra_config.semitone_per_octave = 12;
    g_sp3ctra_config.comma_per_semitone  = 36;

    // ── Performance budget warning ───────────────────────────────────────────
    double bufferDurationUs = (samplesPerBlock / sampleRate) * 1000000.0;
    const double SYNTHESIS_TIME_ESTIMATE_US = 2200.0;
    double loadRatio = SYNTHESIS_TIME_ESTIMATE_US / bufferDurationUs;
    if (loadRatio > 1.0)
    {
        log_warning("VST", "⚠️  PERFORMANCE WARNING: SR too high for buffer size!");
        log_warning("VST", "    SR=%.0f Hz, BS=%d, budget=%.0f µs, estimate=%.0f µs",
                    sampleRate, samplesPerBlock, bufferDurationUs, SYNTHESIS_TIME_ESTIMATE_US);
    }
    else
    {
        log_info("VST", "✅ Headroom: %.0f µs budget, ~%.0f µs synthesis (%.1f%% load)",
                 bufferDurationUs, SYNTHESIS_TIME_ESTIMATE_US, loadRatio * 100.0);
    }

    // ── RT Profiler ──────────────────────────────────────────────────────────
    rt_profiler_init(&g_vst_rt_profiler, static_cast<int>(sampleRate), samplesPerBlock);
    rt_profiler_set_enabled(&g_vst_rt_profiler, 1);
    log_info("VST", "RT Profiler active (interval=%d frames)", RT_PROFILER_REPORT_INTERVAL_FRAMES);

    // ── Recalculate Nyquist-clamped frequencies if SR changed ────────────────
    if (oldSampleRate != static_cast<int>(sampleRate))
    {
        log_info("VST", "SR changed %d → %d Hz — recalculating frequencies",
                 oldSampleRate, static_cast<int>(sampleRate));
        applyConfigurationToCore(false);
    }

    // ── Reset consumer tracking (prevent stale buffer re-output at startup) ──
    lastConsumedReadIdx = -1;
    lastConsumedReadIdxLuxSynth = -1;

    // ── Start the shared pipeline (idempotent: no-op if already running) ─────
    if (coreNeedsInit)
    {
        log_info("VST", "Starting shared pipeline (first call or new plugin)...");

        // Ensure g_sp3ctra_config is fully populated before startWithConfig()
        applyConfigurationToCore(false);

        Sp3ctraCore::ActiveConfig udpCfg;
        udpCfg.udpPort           = static_cast<int>(udpPortParam->load());
        udpCfg.udpAddress        = getUdpAddressString().toStdString();
        udpCfg.multicastInterface = "";
        udpCfg.logLevel          = static_cast<int>(logLevelParam->load());

        if (!sharedCore->startWithConfig(udpCfg,
                                          g_sp3ctra_config.pixels_per_note,
                                          sampleRate, samplesPerBlock))
        {
            log_error("VST", "prepareToPlay — sharedCore->startWithConfig() FAILED");
            setVisualizerSuspendedSafely(false);
            return;
        }

        lastInitPixelsPerNote = g_sp3ctra_config.pixels_per_note;
        coreNeedsInit = false;

        log_info("VST", "Shared pipeline started — UDP on %s:%d",
                 getUdpAddressString().toRawUTF8(),
                 static_cast<int>(udpPortParam->load()));
    }
    else
    {
        // Second instance, or subsequent prepareToPlay on the same instance.
        // The shared pipeline (UDP + synthesis thread) is already running.
        // This instance simply reads from the same luxstral_buffers_L/R globals.
        log_info("VST", "Shared pipeline already running — connecting as additional consumer");

        // Host buffer size may have changed since the buffers were allocated —
        // resize them (stops/restarts the synthesis thread). When refused
        // (multi-instance), the old size stays authoritative and processBlock
        // clamps its reads to luxstral_get_audio_buffer_size().
        if (!sharedCore->ensureAudioBufferSize(samplesPerBlock))
            log_warning("VST", "prepareToPlay — shared output buffers stay at %d "
                               "samples (host asked %d); reads are clamped",
                        luxstral_get_audio_buffer_size(), samplesPerBlock);
    }

    // ── LuxSampler player threads (per-instance, non-RT) ───────────────────
    if (sharedCore && sharedCore->getCore())
    {
        auto* aib = sharedCore->getCore()->getAudioImageBuffers();
        auto* dbf = sharedCore->getCore()->getDoubleBuffer();
        if (luxSampler)  luxSampler->startPlayerThread(aib, dbf);
        if (luxSamplerB) luxSamplerB->startPlayerThread(aib, dbf);

        // M9 — media source service (ticks the IMAGE/VIDEO/CAMERA engines and
        // pumps the chains when the device is not streaming).
        if (mediaService_)
        {
            mediaService_->setContext(sharedCore->getCore()->getContext());
            if (! mediaService_->isThreadRunning())
                mediaService_->startThread();
        }
    }

    log_info("VST", "=============================================================");

    setVisualizerSuspendedSafely(false);
}

void Sp3ctraAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool Sp3ctraAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

//==============================================================================
// SCORE source-audio preview
//==============================================================================
void Sp3ctraAudioProcessor::startScorePreview(const juce::File& wav,
                                              double startSec, double lengthSec)
{
    stopScorePreview();
    if (! wav.existsAsFile()) return;

    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(wav));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return;

    const double      fileRate    = reader->sampleRate;
    const int         srcCh       = (int) juce::jmax((juce::uint32) 1, reader->numChannels);
    const juce::int64 startSample = juce::jlimit((juce::int64) 0, reader->lengthInSamples,
                                                 (juce::int64) (startSec * fileRate));
    juce::int64 numFile = reader->lengthInSamples - startSample;
    if (lengthSec > 0.0)
        numFile = juce::jmin(numFile, (juce::int64) (lengthSec * fileRate));
    if (numFile <= 0) return;

    const int srcLen = (int) numFile;
    juce::AudioBuffer<float> src(juce::jmin(srcCh, 2), srcLen);
    if (! reader->read(&src, 0, srcLen, startSample, true, true))
        return;

    // Resample (linear) to the host rate so the preview plays at correct pitch.
    const double hostRate = (getSampleRate() > 0.0) ? getSampleRate() : fileRate;
    const double ratio    = hostRate / fileRate;
    const int    outLen   = juce::jmax(1, (int) std::floor((double) srcLen * ratio));
    const int    sc       = src.getNumChannels();

    juce::AudioBuffer<float> out(2, outLen);
    for (int i = 0; i < outLen; ++i)
    {
        const double sp = (double) i / ratio;
        const int    i0 = juce::jmin((int) sp, srcLen - 1);
        const int    i1 = juce::jmin(i0 + 1, srcLen - 1);
        const float  fr = (float) (sp - (double) i0);
        for (int c = 0; c < 2; ++c)
        {
            const int   cc = juce::jmin(c, sc - 1);
            const float a  = src.getSample(cc, i0);
            const float b  = src.getSample(cc, i1);
            out.setSample(c, i, a + (b - a) * fr);
        }
    }

    {
        juce::SpinLock::ScopedLockType sl(scorePreviewLock_);
        scorePreviewBuf_  = std::move(out);
        scorePreviewPos_  = 0;
        scorePreviewRate_ = hostRate;
    }
    scorePreviewPosAtomic_.store(0, std::memory_order_release);
    scorePreviewPlaying_.store(true, std::memory_order_release);
}

void Sp3ctraAudioProcessor::pauseScorePreview() noexcept
{
    scorePreviewPlaying_.store(false, std::memory_order_release);
}

bool Sp3ctraAudioProcessor::resumeScorePreview() noexcept
{
    juce::SpinLock::ScopedLockType sl(scorePreviewLock_);
    if (scorePreviewBuf_.getNumSamples() <= 0) return false;
    if (scorePreviewPos_ >= scorePreviewBuf_.getNumSamples())
        scorePreviewPos_ = 0;                 // finished → restart from the top
    scorePreviewPosAtomic_.store(scorePreviewPos_, std::memory_order_release);
    scorePreviewPlaying_.store(true, std::memory_order_release);
    return true;
}

void Sp3ctraAudioProcessor::stopScorePreview() noexcept
{
    scorePreviewPlaying_.store(false, std::memory_order_release);
    {
        juce::SpinLock::ScopedLockType sl(scorePreviewLock_);
        scorePreviewPos_ = 0;
    }
    scorePreviewPosAtomic_.store(0, std::memory_order_release);
}

double Sp3ctraAudioProcessor::getScorePreviewPositionSec() const noexcept
{
    const double r = (scorePreviewRate_ > 0.0) ? scorePreviewRate_ : 48000.0;
    return (double) scorePreviewPosAtomic_.load(std::memory_order_acquire) / r;
}

void Sp3ctraAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    rt_profiler_callback_start(&g_vst_rt_profiler);
    
    juce::ScopedNoDenormals noDenormals;
    auto totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    
    // Clear output buffer first
    buffer.clear();

    // (The LuxSampler note-triggered play path was removed 2026-07-13 — banks
    //  are no longer note-addressed. Per-bank PLAY/REC lives in the unified
    //  MIDI-Learn targets handled by midiMap_ below.)

    // ── Generic MIDI mappings: CC/Note → any mapped play parameter ─────────
    // RT-safe (lock-free slot table, no alloc). APVTS params land through the
    // same setValueNotifyingHost path the host's automation uses; NON-APVTS
    // sampler targets (play params + REC/PLAY/SAVE action pulses) route through
    // this processor's IVirtualMidiSink implementation.
    midiMap_.processMidi(midiMessages);


    // ── All Notes Off (panic): release every held/stuck note across engines ───
    // Triggered by the header panic button. RT-safe. LuxSynth/LuxWave receive a
    // note-off for every note via the same lock-free ring the audio thread already
    // feeds (consumed below). lux_pitch/lux_mask get a musical release for LIVE
    // instances, plus a hard reset for disabled/orphaned ones (see below).
    if (panicRequested.exchange(false, std::memory_order_acq_rel))
    {
        for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)   // every per-chain instance
        {
            LuxPitchState* pit = lux_pitch_instance(i);
            LuxMaskState*  msk = lux_mask_instance(i);

            // Clear the held-voice atomics. For a LIVE instance this is a musical
            // release: process_frame() sees the `active` falling edge and runs the
            // RELEASE envelope to zero on the image thread.
            lux_pitch_all_notes_off(pit);
            lux_mask_all_notes_off(msk);

            // ...but a DISABLED / orphaned instance (bypassed, or dragged off every
            // chain) has config.enabled == 0, so process_frame() early-returns and
            // never advances the envelope — the runtime voice state stays frozen at
            // its held level and the note is stuck forever (lit key, shift that
            // resurfaces if the slot is reused). all_notes_off alone cannot fix this.
            // Force the runtime state to IDLE here. Writing voices[] from the audio
            // thread is safe precisely BECAUSE the instance is not enabled: the
            // image/synth thread does not pull a disabled pool, so there is no
            // concurrent writer (same rationale as lux_*_reset for orphaned slots).
            if (!pit->config.enabled) lux_pitch_reset(pit);
            if (!msk->config.enabled) lux_mask_reset(msk);
        }
        if (g_luxsynth_engine.initialized)
            for (int n = 0; n < 128; ++n)
                luxsynth_push_midi_event(0x80, (uint8_t)n, 0);
        if (g_luxwave_engine.initialized)
            for (int n = 0; n < 128; ++n)
                luxwave_push_midi_event(0x80, (uint8_t)n, 0);
    }

    // ── LuxPitch MIDI (RT-safe) — fanned out to every active per-chain instance,
    // each filtering on ITS OWN bank's MIDI channel / octave offset ──
    {
        const uint32_t pitchBits = chainPitchMask_.load(std::memory_order_relaxed);
        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();
            for (uint32_t bits = pitchBits, i = 0; bits != 0; bits >>= 1, ++i)
            {
                if ((bits & 1u) == 0) continue;
                auto* chP  = luxpitchMidiChannelParam [i];
                auto* octP = luxpitchOctaveOffsetParam[i];
                if (chP == nullptr || octP == nullptr) continue;
                if (msg.getChannel() != static_cast<int>(chP->load()) + 1) continue;

                if (msg.isControllerOfType(1)) // CC1 mod wheel → this bank's LFO Depth
                {
                    // The wheel and the on-screen "LFO Depth" slider are a
                    // single control (this instance's bank param).
                    if (auto* p = luxpitchLfoDepthParam[i])
                        p->setValueNotifyingHost((float)msg.getControllerValue() / 127.0f);
                    continue;
                }

                const int shifted = msg.getNoteNumber()
                                  + (static_cast<int>(octP->load()) - 2) * 12;
                LuxPitchState* st = lux_pitch_instance((int) i);
                if (msg.isNoteOn())
                    lux_pitch_note_on(st, shifted, msg.getFloatVelocity());
                else if (msg.isNoteOff())
                    lux_pitch_note_off(st, shifted);
                else if (msg.isPitchWheel())
                    lux_pitch_set_pitch_bend(st, (msg.getPitchWheelValue() - 8192) / 8192.0f);
                else if (msg.isSustainPedalOn() || msg.isSustainPedalOff())
                    lux_pitch_set_sustain(st, msg.isSustainPedalOn() ? 1 : 0);
            }
        }
    }

    // ── LuxMask MIDI (RT-safe) — fanned out to every active per-chain instance,
    // each filtering on ITS OWN bank's MIDI channel / octave offset ──
    {
        const uint32_t maskBits = chainMaskMask_.load(std::memory_order_relaxed);
        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();
            for (uint32_t bits = maskBits, i = 0; bits != 0; bits >>= 1, ++i)
            {
                if ((bits & 1u) == 0) continue;
                auto* chP  = luxmaskMidiChannelParam [i];
                auto* octP = luxmaskOctaveOffsetParam[i];
                if (chP == nullptr || octP == nullptr) continue;
                if (msg.getChannel() != static_cast<int>(chP->load()) + 1) continue;

                if (msg.isControllerOfType(1)) // CC1 mod wheel → this bank's LFO Pos Depth
                {
                    if (auto* p = luxmaskLfoPosDepthParam[i])
                        p->setValueNotifyingHost((float)msg.getControllerValue() / 127.0f);
                    continue;
                }

                const int shifted = msg.getNoteNumber()
                                  + (static_cast<int>(octP->load()) - 2) * 12;
                LuxMaskState* st = lux_mask_instance((int) i);
                if (msg.isNoteOn())
                    lux_mask_note_on(st, shifted, msg.getFloatVelocity());
                else if (msg.isNoteOff())
                    lux_mask_note_off(st, shifted);
                else if (msg.isPitchWheel())
                    lux_mask_set_pitch_bend(st, (msg.getPitchWheelValue() - 8192) / 8192.0f);
                else if (msg.isSustainPedalOn() || msg.isSustainPedalOff())
                    lux_mask_set_sustain(st, msg.isSustainPedalOn() ? 1 : 0);
            }
        }
    }

    // ── LuxSynth MIDI (RT-safe: push into lock-free ring buffer) ─────────────
    {
        const bool lxEnabled = luxsynthEnabledParam->load() > 0.5f;
        if (lxEnabled && g_luxsynth_engine.initialized)
        {
            const int lxCh  = static_cast<int>(luxsynthMidiChannelParam->load()) + 1;
            const int lxOct = static_cast<int>(luxsynthOctaveOffsetParam->load()) - 2;
            for (const auto metadata : midiMessages)
            {
                const auto msg = metadata.getMessage();
                if (msg.getChannel() != lxCh) continue;
                const int shifted = msg.getNoteNumber() + lxOct * 12;
                if (shifted < 0 || shifted > 127) continue;
                if (msg.isNoteOn())
                    luxsynth_push_midi_event(0x90, (uint8_t)shifted, (uint8_t)msg.getVelocity());
                else if (msg.isNoteOff())
                    luxsynth_push_midi_event(0x80, (uint8_t)shifted, 0);
            }
        }
    }

    // ── LuxWave MIDI (RT-safe: push into lock-free ring buffer) ──────────────
    {
        const bool lwEnabled = luxwaveEnabledParam->load() > 0.5f;
        if (lwEnabled && g_luxwave_engine.initialized)
        {
            const int lwCh  = static_cast<int>(luxwaveMidiChannelParam->load()) + 1;
            const int lwOct = static_cast<int>(luxwaveOctaveOffsetParam->load()) - 2;
            for (const auto metadata : midiMessages)
            {
                const auto msg = metadata.getMessage();
                if (msg.getChannel() != lwCh) continue;
                const int shifted = msg.getNoteNumber() + lwOct * 12;
                if (shifted < 0 || shifted > 127) continue;
                if (msg.isNoteOn())
                    luxwave_push_midi_event(0x90, (uint8_t)shifted, (uint8_t)msg.getVelocity());
                else if (msg.isNoteOff())
                    luxwave_push_midi_event(0x80, (uint8_t)shifted, 0);
            }
        }
    }

    // ── FrameSequencer: advance step if sequencer is running ─────────────────
    if (frameSequencer != nullptr)
        frameSequencer->processBlock(getPlayHead(),
                                     buffer.getNumSamples(),
                                     getSampleRate());

    // ── Acquisition gate: brake the live frame-advance rate ──────────────────
    // "Vitesse d'acquisition" — drive the gate clock once per block.  When Off
    // it disables the gate (full-rate); otherwise it grants advances at the
    // chosen rate and the UDP thread holds the frame between grants.
    {
        const int   mode    = static_cast<int> (acqGateModeParam->load());
        const float rateMs  = acqGateRateMsParam->load();
        const int   divIdx  = static_cast<int> (acqGateSyncDivParam->load());
        const int   mdIdx   = static_cast<int> (acqGateMultDivParam->load());

        // Index → period-in-beats (1/1 .. 1/32, assuming a quarter-note beat).
        static constexpr double kSyncDivBeats[6]  = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
        // Index → refresh-rate factor (/32 .. x4); period = base / factor.
        static constexpr double kRefreshFactor[8] = { 1.0/32, 1.0/16, 1.0/8, 1.0/4,
                                                       1.0/2,  1.0,    2.0,   4.0 };
        const double divBeats = kSyncDivBeats [juce::jlimit(0, 5, divIdx)];
        const double refresh  = kRefreshFactor[juce::jlimit(0, 7, mdIdx)];

        AudioImageBuffers* aib = nullptr;
        if (auto* core = getSp3ctraCore())
            aib = core->getAudioImageBuffers();

        acqGate_.process(aib, mode, rateMs, divBeats, refresh,
                         buffer.getNumSamples(), getSampleRate(), getPlayHead());
    }

    // ── Sequencer-gated recording: route the gate slot to the RIGHT engine ──
    // getStep() encodes (sampler,slot): slot = enc % NUM_SLOTS, sampler = enc /
    // NUM_SLOTS. Gate engine A or B with the decoded slot; ungate the other.
    // Sentinels (< 0) ungate both.
    {
        int gateA = -1, gateB = -1;
        if (frameSequencer != nullptr
            && frameSequencer->isEnabled()
            && frameSequencer->isPlaying())
        {
            const int curStep = frameSequencer->getCurrentStep();
            if (curStep >= 0)
            {
                const int enc = frameSequencer->getStep(curStep);
                if (enc >= 0)
                {
                    const int smp  = enc / LuxSamplerConstants::NUM_SLOTS;
                    const int slot = enc % LuxSamplerConstants::NUM_SLOTS;
                    if      (smp == 0) gateA = slot;
                    else if (smp == 1) gateB = slot;
                }
            }
        }
        if (luxSampler)  luxSampler ->setSeqGateSlot(gateA);
        if (luxSamplerB) luxSamplerB->setSeqGateSlot(gateB);
    }

    // ========================================================================
    // 🎯 LUXSTRAL LOCK-FREE DOUBLE-BUFFER CONSUMER (RT-SAFE)
    //
    // Architecture: AudioProcessingThread (producer) writes audio to a double-
    // buffer. processBlock (consumer) reads from it. The key insight is:
    //
    //   processBlock must NEVER output silence when the producer is mid-write.
    //   Instead, it re-outputs the last successfully read buffer.
    //
    // We track which buffer was last consumed via lastConsumedReadIdx.
    // Only signal "consumed" ONCE per new buffer (avoids double-triggering).
    // DO NOT set ready=0 in the consumer — let the producer manage ready flags.
    //
    // The consume/handshake below runs whenever the core is up — INDEPENDENT of
    // deviceEnabled. It paces audioProcessingThread, which renders engine A AND
    // engine B on this signal: gating it on A's output toggle starved engine B
    // down to the 50ms wait timeout (each grain replayed ~4-5x = robotic sound).
    // deviceEnabled now only gates the WRITE into the JUCE buffer.
    // ========================================================================
    const bool luxstralEnabled = (deviceEnabledParam == nullptr || deviceEnabledParam->load() >= 0.5f);
    if (sharedCore && sharedCore->isReady() && luxstral_are_audio_buffers_ready()) {
        extern AudioImageBuffer luxstral_buffers_L[2];
        extern AudioImageBuffer luxstral_buffers_R[2];
        extern volatile int luxstral_buffer_index;
        extern sp3ctra_config_t g_sp3ctra_config;
        
        // Read current buffer index with ACQUIRE (ARM64 memory ordering)
        int readIdx = 1 - __atomic_load_n(&luxstral_buffer_index, __ATOMIC_ACQUIRE);
        
        // Check if the buffer at readIdx has new data
        int leftReady = __atomic_load_n(&luxstral_buffers_L[readIdx].ready, __ATOMIC_ACQUIRE);
        int rightReady = __atomic_load_n(&luxstral_buffers_R[readIdx].ready, __ATOMIC_ACQUIRE);

        // Clamp to the ALLOCATED size, not g_sp3ctra_config.audio_buffer_size —
        // after a host buffer-size change the reallocation can be refused
        // (multi-instance) and reading numSamples would run past the allocation.
        const int synthBufferSize = luxstral_get_audio_buffer_size();
        const int samplesToRead = (numSamples <= synthBufferSize) ? numSamples : synthBufferSize;
        
        if (leftReady && rightReady && readIdx != lastConsumedReadIdx) {
            // ✅ NEW DATA available — copy to JUCE output and signal producer
            float* leftData = luxstral_buffers_L[readIdx].data;
            float* rightData = luxstral_buffers_R[readIdx].data;

                if (leftData && rightData) {
                if (luxstralEnabled) {
                    const float lsVol = luxstralVolumeParam->load();
                    float pk = lsPkBlock_;   // VU: post-volume block peak
                    if (totalNumOutputChannels >= 1) {
                        float* destLeft = buffer.getWritePointer(0);
                        for (int i = 0; i < samplesToRead; ++i) {
                            const float v = leftData[i] * lsVol;
                            destLeft[i] = v;
                            const float a = v < 0.0f ? -v : v;
                            if (a > pk) pk = a;
                        }
                    }
                    if (totalNumOutputChannels >= 2) {
                        float* destRight = buffer.getWritePointer(1);
                        for (int i = 0; i < samplesToRead; ++i) {
                            const float v = rightData[i] * lsVol;
                            destRight[i] = v;
                            const float a = v < 0.0f ? -v : v;
                            if (a > pk) pk = a;
                        }
                    }
                    lsPkBlock_ = pk;
                }

                // Track which buffer we consumed (don't signal twice for same data)
                lastConsumedReadIdx = readIdx;

                // Signal producer that it can generate the next buffer
                // DO NOT set ready=0 — producer manages ready flags
                luxstral_signal_buffer_consumed();
            }
        } else if (luxstralEnabled && leftReady && rightReady) {
            // ♻️ SAME DATA as last time (producer hasn't finished next buffer yet)
            // Re-output the same audio — much better than silence!
            // Count stale re-outputs for RT Profiler diagnostics (always, not only Debug)
            rt_profiler_report_stale_luxstral(&g_vst_rt_profiler);
            float* leftData = luxstral_buffers_L[readIdx].data;
            float* rightData = luxstral_buffers_R[readIdx].data;
            
            if (leftData && rightData) {
                const float lsVol = luxstralVolumeParam->load();
                float pk = lsPkBlock_;   // VU: post-volume block peak
                if (totalNumOutputChannels >= 1) {
                    float* destLeft = buffer.getWritePointer(0);
                    for (int i = 0; i < samplesToRead; ++i) {
                        const float v = leftData[i] * lsVol;
                        destLeft[i] = v;
                        const float a = v < 0.0f ? -v : v;
                        if (a > pk) pk = a;
                    }
                }
                if (totalNumOutputChannels >= 2) {
                    float* destRight = buffer.getWritePointer(1);
                    for (int i = 0; i < samplesToRead; ++i) {
                        const float v = rightData[i] * lsVol;
                        destRight[i] = v;
                        const float a = v < 0.0f ? -v : v;
                        if (a > pk) pk = a;
                    }
                }
                lsPkBlock_ = pk;
            }
            // DO NOT signal consumed — producer is still working on the next buffer
        } else {
            // 🔇 No data ready at all (startup or after long pause)
            // Buffer already cleared — silence is appropriate here
            rt_profiler_report_buffer_miss_luxstral(&g_vst_rt_profiler);
        }
    }

    // ========================================================================
    // 🎯 LUXSYNTH INLINE SYNTHESIS (RT-SAFE, ADDITIVE)
    //
    // The LuxSynth engine is fully RT-safe (no allocation, no lock, no I/O),
    // so it runs directly in processBlock.  This eliminates all double-buffer
    // synchronisation issues (timing mismatch, stale data replay, temporal
    // discontinuities) that occurred with the former async thread approach.
    //
    // MIDI events are pushed to the lock-free ring buffer above, then drained
    // here before generating audio for exactly `numSamples` samples — always
    // matching the DAW buffer size.
    // ========================================================================
    if (sharedCore && sharedCore->isReady() && g_luxsynth_engine.initialized)
    {
        const bool lxEnabled = luxsynthEnabledParam->load() > 0.5f;
        if (lxEnabled)
        {
            // 1. Drain pending MIDI events into engine voices
            luxsynth_process_pending_midi();

            // 2. Generate audio directly — uses preallocated engine buffers
            luxsynth_engine_process(&g_luxsynth_engine, numSamples,
                                    g_luxsynth_engine.output_left,
                                    g_luxsynth_engine.output_right);

            // 3. Mix into JUCE output buffer (additive)
            const float lxVol = luxsynthVolumeParam->load();

            float pk = lxPkBlock_;   // VU: post-volume block peak
            if (totalNumOutputChannels >= 1)
            {
                float* dest = buffer.getWritePointer(0);
                for (int i = 0; i < numSamples; ++i) {
                    const float v = g_luxsynth_engine.output_left[i] * lxVol;
                    dest[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > pk) pk = a;
                }
            }
            if (totalNumOutputChannels >= 2)
            {
                float* dest = buffer.getWritePointer(1);
                for (int i = 0; i < numSamples; ++i) {
                    const float v = g_luxsynth_engine.output_right[i] * lxVol;
                    dest[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > pk) pk = a;
                }
            }
            lxPkBlock_ = pk;
        }
    }

    // ========================================================================
    // 🎯 LUXWAVE INLINE SYNTHESIS (RT-SAFE, WAVETABLE)
    //
    // LuxWave reads the LuxSynth grayscale line as a dynamic wavetable.
    // MIDI pitch controls playback speed through the waveform.
    // Fully RT-safe — runs directly in processBlock.
    // ========================================================================
    if (sharedCore && sharedCore->isReady() && g_luxwave_engine.initialized)
    {
        const bool lwEnabled = luxwaveEnabledParam->load() > 0.5f;
        if (lwEnabled)
        {
            // 1. Update engine config from APVTS (RT-safe: simple struct copy)
            LuxWaveConfig lwCfg;
            lwCfg.attack_ms           = luxwaveAttackMsParam->load();
            lwCfg.decay_ms            = luxwaveDecayMsParam->load();
            lwCfg.sustain_level       = luxwaveSustainLevelParam->load();
            lwCfg.release_ms          = luxwaveReleaseMsParam->load();
            lwCfg.attack_curve        = luxwaveAttackCurveParam->load();
            lwCfg.decay_curve         = luxwaveDecayCurveParam->load();
            lwCfg.release_curve       = luxwaveReleaseCurveParam->load();
            lwCfg.filter_attack_ms    = 20.0f;
            lwCfg.filter_decay_ms     = 150.0f;
            lwCfg.filter_sustain      = 0.5f;
            lwCfg.filter_release_ms   = 300.0f;
            lwCfg.filter_cutoff_hz    = luxwaveFilterCutoffParam->load();
            lwCfg.filter_env_depth_hz = luxwaveFilterEnvDepthParam->load();
            lwCfg.lfo_rate_hz         = luxwaveLfoRateParam->load();
            lwCfg.lfo_depth_semitones = luxwaveLfoDepthParam->load();
            lwCfg.scan_mode           = (LuxWaveScanMode)static_cast<int>(luxwaveScanModeParam->load());
            lwCfg.amplitude           = luxwaveAmplitudeParam->load();
            lwCfg.sample_rate         = (float)getSampleRate();
            lwCfg.buffer_size         = numSamples;
            lwCfg.enabled             = true;
            luxwave_engine_set_config(&g_luxwave_engine, &lwCfg);

            // 2. Drain pending MIDI events
            luxwave_process_pending_midi();

            // 3. Generate audio
            luxwave_engine_process(&g_luxwave_engine, numSamples,
                                   g_luxwave_engine.output_left,
                                   g_luxwave_engine.output_right);

            // 4. Mix into JUCE output buffer (additive)
            const float lwVol = luxwaveVolumeParam->load();
            float pk = lwPkBlock_;   // VU: post-volume block peak
            if (totalNumOutputChannels >= 1)
            {
                float* dest = buffer.getWritePointer(0);
                for (int i = 0; i < numSamples; ++i) {
                    const float v = g_luxwave_engine.output_left[i] * lwVol;
                    dest[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > pk) pk = a;
                }
            }
            if (totalNumOutputChannels >= 2)
            {
                float* dest = buffer.getWritePointer(1);
                for (int i = 0; i < numSamples; ++i) {
                    const float v = g_luxwave_engine.output_right[i] * lwVol;
                    dest[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > pk) pk = a;
                }
            }
            lwPkBlock_ = pk;
        }
    }

    // ── SCORE source preview: mix the auditioned WAV region into the output ──
    // RT-safe: try-lock (skip this block on contention), no I/O, no allocation.
    if (scorePreviewPlaying_.load(std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType sl(scorePreviewLock_);
        if (sl.isLocked())
        {
            const int n  = scorePreviewBuf_.getNumSamples();
            const int pc = scorePreviewBuf_.getNumChannels();
            int pos = scorePreviewPos_;
            constexpr float kPreviewGain = 0.9f;
            for (int i = 0; i < numSamples && pos < n; ++i, ++pos)
                for (int ch = 0; ch < totalNumOutputChannels; ++ch)
                    buffer.addSample(ch, i,
                        scorePreviewBuf_.getSample(juce::jmin(ch, pc - 1), pos) * kPreviewGain);
            scorePreviewPos_ = pos;
            scorePreviewPosAtomic_.store(pos, std::memory_order_release);
            if (pos >= n)
                scorePreviewPlaying_.store(false, std::memory_order_release);
        }
    }

    // Apply master volume — RT-safe: atomic read, O(N) multiply, no lock, no allocation
    if (masterVolumeParam != nullptr)
    {
        const float gain = masterVolumeParam->load();
        if (gain < 0.9999f)
            buffer.applyGain(gain);
    }

    // ── UI VU meters: fold this block's per-engine peaks into the atomics
    // with an exponential release, then reset the block accumulators.
    // Single RT writer; the AUDIO MIX panel reads at UI rate (relaxed).
    {
        auto fold = [](std::atomic<float>& m, float pk)
        {
            const float rel = m.load(std::memory_order_relaxed) * 0.82f;
            m.store(pk > rel ? pk : rel, std::memory_order_relaxed);
        };
        float mpk = 0.0f;
        for (int ch = 0; ch < totalNumOutputChannels; ++ch)
            mpk = juce::jmax(mpk, buffer.getMagnitude(ch, 0, numSamples));
        fold(meterMaster_,   mpk);
        fold(meterLuxStral_, lsPkBlock_);
        fold(meterLuxSynth_, lxPkBlock_);
        fold(meterLuxWave_,  lwPkBlock_);
        lsPkBlock_ = lxPkBlock_ = lwPkBlock_ = 0.0f;
    }

    rt_profiler_callback_end(&g_vst_rt_profiler);
}

//==============================================================================
bool Sp3ctraAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* Sp3ctraAudioProcessor::createEditor()
{
    return new Sp3ctraAudioProcessorEditor (*this);
}

//==============================================================================
// APVTS State Management (automatic save/restore in DAW projects)
void Sp3ctraAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    // Persist the session paths so they survive DAW project reloads and
    // Standalone restarts. ALWAYS written (even when empty): the copied
    // state may still carry the value restored at load time, and skipping
    // the write would resurrect a path the user has since cleared.
    state.setProperty("lastSessionPath",  lastSessionPath,  nullptr);
    state.setProperty("samplerOutputDir", samplerOutputDir, nullptr);
    state.setProperty("scoreWavPath",     scoreWavPath,     nullptr);
    // Synth-split state version — gates the staged migrations in
    // setStateInformation (absent = pre-split blob; 1 = pre per-send enable).
    state.setProperty("synthSplitVersion", 2, nullptr);

    // Non-APVTS module state → child trees. Replace (never append next to) any
    // stale copy restored at load time.
    auto replaceChild = [&state](juce::ValueTree child)
    {
        if (! child.isValid())
            return;
        auto existing = state.getChildWithName(child.getType());
        if (existing.isValid())
            state.removeChild(existing, nullptr);
        state.appendChild(child, nullptr);
    };
    replaceChild(scoreStateToTree());        // SCORE settings + freq override
    replaceChild(seqStateToTree());          // sequencer pattern (steps A/B)
    replaceChild(samplerSlotsStateToTree()); // per-slot params, engines A + B
    replaceChild(mediaSourcesStateToTree()); // M9 — IMAGE/VIDEO paths + camera
    // CHAINS — snapshot the LIVE model, never trust the copy restored/edited
    // earlier: rack edits persist via a deferred callAsync (persistChainModel),
    // so a save taken before that dispatch would write a stale topology; and a
    // fresh session has no CHAINS child at all until the first rack edit.
    // J2 — the chain owns its modules' settings: refresh each module's VALUES
    // from the runtime banks (atomic reads) before serialising.
    snapshotBankValuesIntoModel();
    replaceChild(chainModel_.toValueTree());
    // UUID → pool-slot bindings: the per-instance param banks are keyed by the
    // slot, so the binding must reload identically (else settings would swap).
    replaceChild(poolBindingsToTree());
    // (J3: the per-chain settings memory now lives IN the CHAINS tree —
    // Chain::typeMemory, serialized as CHAIN/MEMORY children. The legacy
    // INSERT_MEMORY blob is no longer written; it is still read once as a
    // migration on load.) J6 — drop the stale copy a migrated session still
    // carries, so old blobs resave as clean v3.
    {
        auto legacy = state.getChildWithName("INSERT_MEMORY");
        if (legacy.isValid())
            state.removeChild(legacy, nullptr);
    }
    replaceChild(midiMap_.toValueTree());    // MIDI CC/Note → param mappings

    std::unique_ptr<juce::XmlElement> xml(state.createXml());

    copyXmlToBinary(*xml, destData);
    log_info("VST", "State saved to DAW project (%d KB)",
             (int) ((destData.getSize() + 1023) / 1024));
}

void Sp3ctraAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // APVTS handles deserialization automatically via ValueTree
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));

    // A failed restore silently keeps every default (topology, SCORE, SEQ,
    // sampler params, session paths) — make that loudly visible in the log.
    if (xmlState == nullptr)
        log_error("VST", "State restore FAILED: corrupt/unreadable state blob "
                         "(%d bytes) — session resets to defaults", sizeInBytes);
    else if (! xmlState->hasTagName(apvts.state.getType()))
        log_error("VST", "State restore FAILED: unexpected root tag '%s' "
                         "(expected '%s') — session resets to defaults",
                  xmlState->getTagName().toRawUTF8(),
                  apvts.state.getType().toString().toRawUTF8());

    if (xmlState.get() != nullptr) {
        if (xmlState->hasTagName(apvts.state.getType())) {
            // Transport open-state is patched into the tree BEFORE replaceState
            // so it IS the restored value — the previous setValueNotifyingHost()
            // push after the restore marked host automation lanes as overridden
            // on every project open (and could write a point in Latch/Write
            // modes). A missing PARAM entry needs no patch: the defaults already
            // match the open-state below.
            //   AUDIO transports (SEQ/SCORE/media sources) open STOPPED so a
            //   project never blasts sound on load (never-auto-run).
            //   The VIDEO SCROLL is a visual output, not audio: it opens RUNNING
            //   so the right-band waterfall scrolls immediately on launch — the
            //   play/pause button already defaults to "running", so button and
            //   param now agree (previously it opened frozen-but-labelled-play,
            //   requiring a pause/play dance to start).
            auto forceRestoredParam = [&xmlState](const juce::String& id, double v)
            {
                for (auto* e : xmlState->getChildWithTagNameIterator("PARAM"))
                    if (e->getStringAttribute("id") == id)
                    { e->setAttribute("value", v); return; }
            };
            forceRestoredParam("videoScrollPaused", 0.0);       // legacy global
            for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
                forceRestoredParam(vsParam(s, "paused"), 0.0);  // per-instance: run
            forceRestoredParam(PARAM_SEQ_TRANSPORT, 0.0);       // Stop
            forceRestoredParam(PARAM_SCORE_PLAYING, 0.0);
            forceRestoredParam(PARAM_IMGSRC_PLAY,   0.0);       // M9 sources
            forceRestoredParam(PARAM_VIDSRC_PLAY,   0.0);

            // Migration — sessions saved before the per-instance insert banks
            // carry single per-type values ("luxpitchAttackMs"). All instances
            // shared them back then, so seed every bank slot with the legacy
            // value. Patch the tree BEFORE replaceState; banked entries already
            // present (new sessions) win — the legacy value is only a fallback.
            {
                std::map<juce::String, double> restored;   // id → value
                for (auto* e : xmlState->getChildWithTagNameIterator("PARAM"))
                    restored[e->getStringAttribute("id")] =
                        e->getDoubleAttribute("value");

                for (const auto& bank : kModuleParamManifest)
                {
                    if (! isPooledInsertType(bank.type))
                        continue;   // legacy per-type ids existed for the
                                    // 5 pooled inserts only (iso-behaviour)
                    for (int i = 0; i < bank.numSuffixes; ++i)
                    {
                        const auto legacyIt = restored.find(
                            juce::String(bank.bankPrefix) + bank.suffixes[i]);
                        if (legacyIt == restored.end())
                            continue;
                        for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
                        {
                            const juce::String bankId =
                                bank.paramId(s, bank.suffixes[i]);
                            if (restored.count(bankId) != 0)
                                continue;   // already saved per instance
                            auto* e = xmlState->createNewChildElement("PARAM");
                            e->setAttribute("id", bankId);
                            e->setAttribute("value", legacyIt->second);
                        }
                    }
                }
            }

            // Migration — synth-split P1: blobs saved before the per-OUT
            // conditioning banks carry the legacy global conditioning params
            // (luxstral*/luxstralB*/luxsynth*). Seed the OUT banks from them
            // so the reloaded session sounds identical. Detected by the
            // absence of the synthSplitVersion root attribute (stamped by
            // getStateInformation since P1). The legacy params stay restored
            // (state compat) but the pipeline no longer reads them. Gamma
            // collapses enable+value → single knob (1.0 = off); Intensity
            // keeps its default (1.0 = unity send).
            const int splitVer = xmlState->getIntAttribute("synthSplitVersion", 0);
            {
                std::map<juce::String, double> restored;   // id → value
                for (auto* e : xmlState->getChildWithTagNameIterator("PARAM"))
                    restored[e->getStringAttribute("id")] =
                        e->getDoubleAttribute("value");

                auto legacy = [&restored](const char* id, double def)
                {
                    const auto it = restored.find(juce::String(id));
                    return it != restored.end() ? it->second : def;
                };
                auto seedBank = [&restored, &xmlState](const juce::String& bankId,
                                                       double v)
                {
                    if (restored.count(bankId) != 0)
                        return;   // an already-banked value always wins
                    auto* e = xmlState->createNewChildElement("PARAM");
                    e->setAttribute("id", bankId);
                    e->setAttribute("value", v);
                };

                // v2 — per-send enable: the 2nd send's power used to be
                // luxstralBEnabled (rack LED of the B slot).
                if (splitVer < 2)
                    seedBank(lsOutParam(1, "enabled"),
                             legacy("luxstralBEnabled", 1.0));

                if (splitVer < 1)
                {

                const double aGamma  = legacy("luxstralGammaEnable", 1.0) >= 0.5
                                     ? legacy("luxstralGammaValue", 1.0) : 1.0;
                const double rangeDb = legacy("luxstralFidelityRangeDb", 50.0);

                // LuxStral OUT — slot 0 = engine A, slot 1 = engine B
                // (Range dB was shared A+B: both slots inherit it).
                seedBank(lsOutParam(0, "negative"),    legacy("luxstralInversion",   1.0));
                seedBank(lsOutParam(0, "dcBlocking"),  legacy("luxstralAcRemoval",   1.0));
                seedBank(lsOutParam(0, "gamma"),       aGamma);
                seedBank(lsOutParam(0, "contrastMin"), legacy("luxstralContrastMin", 0.21));
                seedBank(lsOutParam(0, "rangeDb"),     rangeDb);
                seedBank(lsOutParam(1, "negative"),    legacy("luxstralBInversion",  1.0));
                seedBank(lsOutParam(1, "dcBlocking"),  legacy("luxstralBAcRemoval",  1.0));
                seedBank(lsOutParam(1, "gamma"),       legacy("luxstralBGammaValue", 1.0));
                seedBank(lsOutParam(1, "contrastMin"), legacy("luxstralBContrastMin", 0.21));
                seedBank(lsOutParam(1, "rangeDb"),     rangeDb);

                // LuxSynth OUT slot 0 — and LuxWave OUT slot 0 seeds from the
                // SAME values: before its own bank, LuxWave inherited the
                // LuxSynth-conditioned line (parity of the inheritance).
                const double xInv = legacy("luxsynthInversion", 1.0);
                const double xDc  = legacy("luxsynthAcRemoval", 1.0);
                const double xGam = legacy("luxsynthGammaValue", 1.0);
                seedBank(lxOutParam(0, "negative"),   xInv);
                seedBank(lxOutParam(0, "dcBlocking"), xDc);
                seedBank(lxOutParam(0, "gamma"),      xGam);
                seedBank(lwOutParam(0, "negative"),   xInv);
                seedBank(lwOutParam(0, "dcBlocking"), xDc);
                seedBank(lwOutParam(0, "gamma"),      xGam);

                log_info("VST", "Synth-split migration: legacy conditioning "
                                "params seeded into the per-OUT banks");
                }
            }

            apvts.replaceState(juce::ValueTree::fromXml(*xmlState));

            // Everything below mutates non-APVTS state that UI timers iterate
            // concurrently (chainModel_, SCORE/SEQ trees, engines). Some hosts
            // call setStateInformation from a project-loading thread — apply
            // on the message thread only (replaceState above is thread-safe).
            auto* mm = juce::MessageManager::getInstanceWithoutCreating();
            if (mm == nullptr || mm->isThisTheMessageThread())
            {
                applyRestoredStateOnMessageThread();
            }
            else
            {
                juce::WeakReference<Sp3ctraAudioProcessor> weakThis(this);
                juce::MessageManager::callAsync([weakThis]
                {
                    if (auto* p = weakThis.get())
                        p->applyRestoredStateOnMessageThread();
                });
            }
        }
    }
}

void Sp3ctraAudioProcessor::applyRestoredStateOnMessageThread()
{
    {
        {
            // Transport open-state is patched into the tree BEFORE replaceState
            // (see setStateInformation) — no host-visible pushes on the nominal
            // path. Clearing blanks the waterfall history so the video scroll
            // starts from a fresh (empty) waterfall and scrolls in live data.
            requestVideoScrollClear();

            // Belt-and-suspenders for blobs saved by OLDER plugin versions:
            // replaceState KEEPS the current value of any parameter absent
            // from the restored tree, so the pre-replace patch cannot reach
            // those. Fold each transport to its open-state — host-visible only
            // in this corner case (old preset loaded while something differs).
            {
                auto forceTo = [this](const juce::String& id, float openNorm)
                {
                    if (auto* p = apvts.getParameter(id))
                        if (std::abs(p->getValue() - openNorm) > 1.0e-4f)
                            p->setValueNotifyingHost(openNorm);
                };
                // VIDEO SCROLL opens RUNNING (paused = 0); audio transports STOPPED.
                forceTo("videoScrollPaused", 0.0f);
                for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
                    forceTo(vsParam(s, "paused"), 0.0f);
                forceTo(PARAM_SEQ_TRANSPORT, 0.0f);
                forceTo(PARAM_SCORE_PLAYING, 0.0f);
                forceTo(PARAM_IMGSRC_PLAY,   0.0f);
                forceTo(PARAM_VIDSRC_PLAY,   0.0f);
            }
            // Push the restored SCORE speed/loop into the engine (the listener
            // does not fire for values equal to the pre-restore state).
            if (luxSampler != nullptr)
            {
                luxSampler->setScoreSpeed(
                    apvts.getRawParameterValue(PARAM_SCORE_SPEED)->load());
                luxSampler->setScoreLoopMode(scoreLoopModeFromParams(apvts));
            }
            // Restore last session path — SamplerPageComponent reads this
            // on construction to auto-reload the session.
            lastSessionPath = apvts.state
                .getProperty("lastSessionPath", "").toString();
            // Restore sampler output directory — SamplerSetupPanel and
            // SAVE SESSION read this to bypass the file chooser when set.
            samplerOutputDir = apvts.state
                .getProperty("samplerOutputDir", "").toString();
            scoreWavPath = apvts.state
                .getProperty("scoreWavPath", "").toString();
            log_info("VST", "State restored from DAW project");

            // On state restore, just update g_sp3ctra_config.
            // The actual pipeline start (if needed) happens in prepareToPlay().
            applyConfigurationToCore(false);

            // M9 — restore media paths + camera device BEFORE the chain model:
            // updateMediaSourcePresence() (inside deriveChainRouting) reopens
            // the camera only when a CAMERA module is placed, and needs the
            // persisted device name to be known by then.
            restoreMediaSourcesFromTree(apvts.state.getChildWithName("MEDIA_SOURCES"));
            // Push the restored source params into the engines (the listener
            // does not fire for values equal to the pre-restore state).
            if (imageSource_)
            {
                imageSource_->setPosition (apvts.getRawParameterValue(PARAM_IMGSRC_POS)->load());
                imageSource_->setDurationS(apvts.getRawParameterValue(PARAM_IMGSRC_DUR)->load());
                imageSource_->setLoopMode ((int) apvts.getRawParameterValue(PARAM_IMGSRC_LOOP)->load());
                imageSource_->setEnabled  (apvts.getRawParameterValue(PARAM_IMGSRC_ENABLED)->load() > 0.5f);
            }
            if (videoSource_)
            {
                videoSource_->setLineFrac (apvts.getRawParameterValue(PARAM_VIDSRC_LINE)->load());
                videoSource_->setSpeed    (apvts.getRawParameterValue(PARAM_VIDSRC_SPEED)->load());
                videoSource_->setLoopMode ((int) apvts.getRawParameterValue(PARAM_VIDSRC_LOOP)->load());
                videoSource_->setEnabled  (apvts.getRawParameterValue(PARAM_VIDSRC_ENABLED)->load() > 0.5f);
            }
            if (cameraSource_)
            {
                cameraSource_->setLineFrac(apvts.getRawParameterValue(PARAM_CAMSRC_LINE)->load());
                cameraSource_->setEnabled (apvts.getRawParameterValue(PARAM_CAMSRC_ENABLED)->load() > 0.5f);
            }

            // Pool-slot bindings + per-chain settings memory FIRST: the derive
            // inside loadChainModelFromState() must reuse the saved bindings
            // (the per-instance banks are keyed by slot) instead of assigning
            // fresh ones in a different order.
            restorePoolBindingsFromTree(apvts.state.getChildWithName("POOL_SLOTS"));

            // M6 Phase 2 — restore the chain topology and derive per-chain
            // routing (headless-correct; enable params are already restored).
            loadChainModelFromState();

            // J3 — legacy INSERT_MEMORY blob → the chains' own type memory
            // (one-shot migration; v3 blobs restored it with the model and
            // win). Must run AFTER the model is loaded.
            restoreInsertMemoryFromTree(apvts.state.getChildWithName("INSERT_MEMORY"));

            // J2 — chain-owned settings: project each module's VALUES onto
            // its runtime bank. Idempotent for a v3 blob (the flat PARAMs
            // restored the same values — the only-if-different guard makes it
            // a no-op); pre-v3 sessions have no VALUES and skip naturally.
            // This is the nominal path of the chain presets (J4).
            projectChainValuesToBanks();

            // SCORE settings + frequency override (processor members, not APVTS).
            restoreScoreStateFromTree(apvts.state.getChildWithName("SCORE"));

            // Sequencer pattern — steps are not APVTS params, only their
            // transport/timing is. Timing attrs in the tree were captured
            // together with the APVTS values, so applying both is consistent.
            seqRestoredFromState_ = false;
            if (auto seqTree = apvts.state.getChildWithName("SEQ");
                seqTree.isValid() && frameSequencer != nullptr)
            {
                if (auto seqXml = seqTree.createXml())
                {
                    frameSequencer->loadFromXml(*seqXml);
                    seqRestoredFromState_ = true;
                }
            }

            // Per-slot sampler params (both engines) — the .sp3s auto-load
            // may later re-apply these on top of the freshly loaded banks.
            samplerParamsInState_ =
                apvts.state.getChildWithName("SAMPLER_SLOTS").isValid();
            applySamplerParamsFromState();

            // MIDI CC/Note → param mappings (unknown ids dropped silently).
            midiMap_.restoreFromValueTree(
                apvts.state.getChildWithName("MIDI_MAPPINGS"));

            // Arm the one-shot session auto-load for the first editor.
            if (lastSessionPath.isNotEmpty())
                samplerAutoLoadPending_.store(true, std::memory_order_release);

            if (!coreNeedsInit && sharedCore && sharedCore->isReady())
            {
                // Pipeline already running: hot-reload UDP with restored config.
                log_info("VST", "Restarting UDP with restored settings...");
                if (!sharedCore->restartUdp(
                        static_cast<int>(udpPortParam->load()),
                        getUdpAddressString().toStdString(), ""))
                {
                    log_error("VST", "Failed to restart UDP with restored config!");
                }
                log_info("VST", "UDP restarted → %s:%d",
                         getUdpAddressString().toRawUTF8(),
                         static_cast<int>(udpPortParam->load()));
            }
            // else: coreNeedsInit == true → prepareToPlay() will call startWithConfig()

            // Let an OPEN editor rebuild its rack/panels from the new model.
            if (onStateRestoredUi)
                onStateRestoredUi();
        }
    }
}

//==============================================================================
// Helper to build UDP address string from 4 bytes
juce::String Sp3ctraAudioProcessor::getUdpAddressString() const
{
    int b1 = (int)udpByte1Param->load();
    int b2 = (int)udpByte2Param->load();
    int b3 = (int)udpByte3Param->load();
    int b4 = (int)udpByte4Param->load();
    
    return juce::String::formatted("%d.%d.%d.%d", b1, b2, b3, b4);
}

//==============================================================================
// Parameter Change Listener — DISPATCHER.
// APVTS listeners fire synchronously on whatever thread set the value: the
// audio thread for host automation / MIDI-mapped params (CC1 → LFO depth), a
// loading thread for some hosts. The real handler below does message-thread
// work (applyConfigurationToCore ≈ 60 String lookups + logs, source routing
// under db->mutex, UDP restart with joins/sleeps) — running it on the audio
// thread caused xruns up to guaranteed dropouts. Defer via per-param dirty
// flags drained by the 30 ms timer; values are re-read at apply time.
void Sp3ctraAudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread())
    {
        applyParameterChange(parameterID, newValue);
        return;
    }

    // RT path: atomics only (map lookup = String compares, no alloc/lock).
    const auto it = paramIndexById_.find(parameterID);
    if (it == paramIndexById_.end())
        return;
    paramDirty_[(size_t) it->second].store(true, std::memory_order_relaxed);
    anyParamDirty_.store(true, std::memory_order_release);
}

void Sp3ctraAudioProcessor::timerCallback()
{
    // ── Deferred parameter changes (audio/loader thread → here) ──────────────
    if (anyParamDirty_.exchange(false, std::memory_order_acq_rel))
    {
        for (int i = 0; i < deferredParamIds_.size(); ++i)
            if (paramDirty_[(size_t) i].exchange(false, std::memory_order_acq_rel))
                if (auto* raw = apvts.getRawParameterValue(deferredParamIds_[i]))
                    applyParameterChange(deferredParamIds_[i], raw->load());
    }

    // ── SCORE transport mirror ────────────────────────────────────────────────
    // The SCORE page is a view: it no longer force-stops the score in its
    // destructor, so with no page open somebody must still fold the engine's
    // one-shot natural end back onto the automatable scorePlaying param (the
    // param listener then runs uiStopScore — idempotent). Guard: never fold
    // while a scorePlaying change is still pending in the deferred queue —
    // an automation Play marked dirty between the drain above and this
    // mirror would otherwise be swallowed (param 1, engine not started yet).
    if (luxSampler != nullptr
        && ! (scorePlayingParamIdx_ >= 0
              && paramDirty_[(size_t) scorePlayingParamIdx_].load(std::memory_order_acquire)))
        if (auto* p = apvts.getParameter(PARAM_SCORE_PLAYING))
            if (p->getValue() >= 0.5f && ! luxSampler->isScorePlaying())
                p->setValueNotifyingHost(0.0f);

    // ── Deferred Pitch/Mask/Reverb/Echo/VideoScroll pool resets (see header) ──
    if ((pendingPitchResets_ | pendingMaskResets_ | pendingReverbResets_
         | pendingEchoResets_ | pendingEqResets_ | pendingVideoScrollInits_
         | pendingStagingResets_) != 0
        && juce::Time::getMillisecondCounter() - poolResetArmedMs_ >= 40)
    {
        for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        {
            if ((pendingPitchResets_ >> i) & 1u) lux_pitch_reset(lux_pitch_instance(i));
            if ((pendingMaskResets_  >> i) & 1u) lux_mask_reset(lux_mask_instance(i));
            if ((pendingReverbResets_ >> i) & 1u) lux_reverb_reset(lux_reverb_instance(i));
            if ((pendingEchoResets_   >> i) & 1u) lux_echo_reset(lux_echo_instance(i));
            if ((pendingEqResets_     >> i) & 1u) lux_eq_reset(lux_eq_instance(i));
            if ((pendingVideoScrollInits_ >> i) & 1u)
                video_scroll_init(video_scroll_instance(i));
            if ((pendingStagingResets_ >> i) & 1u)
            {
                synth_staging_set_inactive(i);
                synth_staging_luxsynth_set_inactive(i);
                synth_staging_luxwave_set_inactive(i);
            }
        }
        pendingPitchResets_ = pendingMaskResets_ = pendingVideoScrollInits_ = 0;
        pendingReverbResets_ = pendingEchoResets_ = pendingEqResets_ = 0;
        pendingStagingResets_ = 0;
    }

    // ── RT profiler: drain deferred logs (RT threads never log directly) ─────
    rt_profiler_flush_logs(&g_vst_rt_profiler);
}

// Real parameter handler — message thread only (see dispatcher above).
void Sp3ctraAudioProcessor::applyParameterChange(const juce::String& parameterID, float newValue)
{
    log_debug("VST", "Parameter '%s' changed to %.2f", parameterID.toRawUTF8(), newValue);

    // ── PLAY transports — DAW-automatable commands relayed to the engines ────
    // Host automation may deliver these on the audio thread; every engine call
    // below is a lock-free atomic write (uiPlay/uiHold/uiStop, score setters).
    // seqTransport must be matched BEFORE the generic startsWith("seq") branch.
    if (parameterID == PARAM_SEQ_TRANSPORT)
    {
        if (frameSequencer != nullptr)
        {
            const int mode = static_cast<int>(newValue + 0.5f); // 0=Stop 1=Play 2=Hold
            if (mode == 1)
            {
                if (frameSequencer->isHeld()) frameSequencer->uiResume();
                else                          frameSequencer->uiPlay();
            }
            else if (mode == 2) frameSequencer->uiHold();
            else                frameSequencer->uiStop();
        }
        return;
    }
    if (parameterID == PARAM_SCORE_PLAYING)
    {
        if (luxSampler != nullptr)
        {
            const bool wantPlay = newValue > 0.5f;
            if (wantPlay != luxSampler->isScorePlaying())
            {
                if (wantPlay)
                {
                    // Same as the SCORE page button: push transport settings first.
                    luxSampler->setScoreSpeed(
                        apvts.getRawParameterValue(PARAM_SCORE_SPEED)->load());
                    luxSampler->setScoreLoopMode(scoreLoopModeFromParams(apvts));
                    luxSampler->uiPlayScore();
                }
                else
                    luxSampler->uiStopScore();
            }
        }
        return;
    }
    if (parameterID == PARAM_SCORE_SPEED)
    {
        if (luxSampler != nullptr)
            luxSampler->setScoreSpeed(newValue);
        return;
    }
    if (parameterID == PARAM_SCORE_LOOP || parameterID == PARAM_SCORE_REVERSE)
    {
        if (luxSampler != nullptr)
            luxSampler->setScoreLoopMode(scoreLoopModeFromParams(apvts));
        return;
    }

    // ── M9: IMAGE / VIDEO / CAMERA sources — every engine call is atomic ─────
    if (parameterID == PARAM_IMGSRC_POS)
    {
        if (imageSource_) imageSource_->setPosition(newValue);
        return;
    }
    if (parameterID == PARAM_IMGSRC_DUR)
    {
        if (imageSource_) imageSource_->setDurationS(newValue);
        return;
    }
    if (parameterID == PARAM_IMGSRC_LOOP)
    {
        if (imageSource_) imageSource_->setLoopMode((int) (newValue + 0.5f));
        return;
    }
    if (parameterID == PARAM_IMGSRC_PLAY)
    {
        if (imageSource_) imageSource_->setPlaying(newValue > 0.5f);
        return;
    }
    if (parameterID == PARAM_VIDSRC_LINE)
    {
        if (videoSource_) videoSource_->setLineFrac(newValue);
        return;
    }
    if (parameterID == PARAM_VIDSRC_SPEED)
    {
        if (videoSource_) videoSource_->setSpeed(newValue);
        return;
    }
    if (parameterID == PARAM_VIDSRC_LOOP)
    {
        if (videoSource_) videoSource_->setLoopMode((int) (newValue + 0.5f));
        return;
    }
    if (parameterID == PARAM_VIDSRC_PLAY)
    {
        if (videoSource_) videoSource_->setPlaying(newValue > 0.5f);
        return;
    }
    if (parameterID == PARAM_CAMSRC_LINE)
    {
        if (cameraSource_) cameraSource_->setLineFrac(newValue);
        return;
    }
    // ACTIVE toggles: off deactivates the source in the internal pool (its
    // chain streams blank paper), on republishes the current line instantly.
    if (parameterID == PARAM_IMGSRC_ENABLED)
    {
        if (imageSource_) imageSource_->setEnabled(newValue > 0.5f);
        return;
    }
    if (parameterID == PARAM_VIDSRC_ENABLED)
    {
        if (videoSource_) videoSource_->setEnabled(newValue > 0.5f);
        return;
    }
    if (parameterID == PARAM_CAMSRC_ENABLED)
    {
        if (cameraSource_) cameraSource_->setEnabled(newValue > 0.5f);
        return;
    }

    // 🔧 CRITICAL: LuxStral parameters are automatically synced to g_sp3ctra_config
    // They are read directly by the synthesis engine, NO restart needed!
    // StrokeForge parameters — same hot-reload pattern as LuxStral
    // LuxSampler parameters — update atomic config on LuxSampler
    if (parameterID.startsWith("luxSampler"))
    {
        // Per-engine sampler enable = model presence AND the shared enable
        // param (the rack LED / host automation). Presence alone used to be
        // authoritative, which made the LED a dead toggle: switching it off
        // changed nothing audible while showing "off".
        if (parameterID == PARAM_FS_ENABLED)
        {
            const bool on = newValue > 0.5f;
            if (luxSampler  != nullptr) luxSampler ->setEnabled(samplerAPresent_ && on);
            if (luxSamplerB != nullptr) luxSamplerB->setEnabled(samplerBPresent_ && on);
            return;
        }
        // Per-engine banks: "luxSamplerB*" drives engine B, the legacy
        // "luxSampler*" ids drive engine A (fsEngineParam). The export prefs
        // and output dir stay shared (session-level, not play params).
        const int e = parameterID.startsWith("luxSamplerB") ? 1 : 0;
        LuxSampler* engine = (e == 1) ? luxSamplerB.get() : luxSampler.get();
        if (engine != nullptr)
        {
            engine->setMidiChannel(
                static_cast<int>(*apvts.getRawParameterValue(fsEngineParam(e, "MidiChannel"))) + 1);
            engine->setOctaveOffset(
                static_cast<int>(*apvts.getRawParameterValue(fsEngineParam(e, "OctaveOffset"))) - 2);
            engine->setMaxDuration(*apvts.getRawParameterValue(fsEngineParam(e, "MaxDuration")));
        }
        return;
    }

    // ── FrameSequencer parameters ─────────────────────────────────────────────
    if (parameterID.startsWith("seq") && frameSequencer != nullptr)
    {
        frameSequencer->setEnabled (
            *apvts.getRawParameterValue(PARAM_SEQ_ENABLED)  > 0.5f);
        frameSequencer->setBpm(
            apvts.getRawParameterValue(PARAM_SEQ_BPM)->load());
        frameSequencer->setNumSteps(
            static_cast<int>(apvts.getRawParameterValue(PARAM_SEQ_NSTEPS)->load()));
        frameSequencer->setLooping(
            *apvts.getRawParameterValue(PARAM_SEQ_LOOP)     > 0.5f);
        frameSequencer->setDawSync(
            *apvts.getRawParameterValue(PARAM_SEQ_DAW_SYNC) > 0.5f);
        frameSequencer->setBeatsPerStep(
            static_cast<int>(apvts.getRawParameterValue(PARAM_SEQ_BPS)->load()));
        return;
    }

    bool isStrokeForgeParam = parameterID.startsWith("sf");
    if (isStrokeForgeParam) {
        applyConfigurationToCore(false);
        return;
    }

    // LuxSynth blob detection — independent of StrokeForge (visualizer-only params)
    if (parameterID.startsWith("lxBlob")) {
        applyConfigurationToCore(false);
        return;
    }

    // SPCTR blob detection — IMAGE LUXSTRAL tab (drives visualizer + StrokeForge audio)
    if (parameterID.startsWith("spctrBlob")) {
        applyConfigurationToCore(false);
        return;
    }
    
    bool isLuxStralParam = parameterID.startsWith("luxstral");
    if (isLuxStralParam) {
        // Just update g_sp3ctra_config silently (no restart)
        applyConfigurationToCore(false);

        // 🔧 HOT-RELOAD: Musical parameters (tuning, root note, octaves) change frequency range
        // This triggers fade-out → regenerate → fade-in for smooth transition
        if (parameterID == "luxstralTuning" || 
            parameterID == "luxstralRootNote" || 
            parameterID == "luxstralNumOctaves") {
            log_info("VST", "Musical parameter changed - requesting hot-reload");
            request_frequency_reinit();
        }
        
        // 🔧 HOT-RELOAD: Physiological filter toggle requires wavetable regeneration
        // Waveform amplitudes change when equal-loudness compensation is enabled/disabled
        if (parameterID == "luxstralPhysiologicalFilter") {
            log_info("VST", "Physiological filter %s - requesting wavetable regeneration",
                     newValue > 0.5f ? "ENABLED" : "DISABLED");
            request_frequency_reinit();
        }
        
        // 🔧 HOT-RELOAD: Depth change requires re-weighting all wavetable gains
        if (parameterID == "luxstralPhysiologicalDepth") {
            log_info("VST", "Physiological depth changed to %.2f - requesting wavetable regeneration",
                     newValue);
            request_frequency_reinit();
        }
        
        // 🔧 HOT-RELOAD: Envelope parameters (Attack/Release) require coefficient update
        // Recalculates alpha_up and alpha_down_weighted for all oscillators.
        if (parameterID == "luxstralAttackMs" || parameterID == "luxstralReleaseMs") {
            log_info("VST", "Envelope parameter changed - updating coefficients");
            update_gap_limiter_coefficients();
        }

        return;  // Done - synthesis engine will pick up changes automatically
    }
    
    // Check if UDP parameters changed (need to restart thread)
    bool needsUdpRestart = (parameterID == PARAM_UDP_PORT || 
                           parameterID == PARAM_UDP_BYTE1 ||
                           parameterID == PARAM_UDP_BYTE2 ||
                           parameterID == PARAM_UDP_BYTE3 ||
                           parameterID == PARAM_UDP_BYTE4);
    
    if (needsUdpRestart) {
        // 🔧 CRITICAL: Ignore UDP parameter changes if core not yet initialized
        // This prevents errors during APVTS state restoration at startup
        if (coreNeedsInit) {
            log_debug("VST", "UDP parameter changed (init pending) - restart deferred");
            return;  // Don't restart now, will init properly in setStateInformation/prepareToPlay
        }
        
        // 🔧 BATCH UPDATE: If we're in a batch update, just mark that restart is needed
        // The actual restart will happen once in endUdpBatchUpdate()
        if (udpBatchUpdateActive.load()) {
            udpNeedsRestart.store(true);
            log_debug("VST", "UDP parameter changed (batch mode) - restart deferred");
            return;  // Don't restart now, wait for batch completion
        }
        
        log_info("VST", "UDP parameter changed — restarting shared socket...");
        applyConfigurationToCore(false);  // update g_sp3ctra_config first

        if (sharedCore && sharedCore->isReady())
        {
            if (!sharedCore->restartUdp(
                    static_cast<int>(udpPortParam->load()),
                    getUdpAddressString().toStdString(), ""))
            {
                log_error("VST", "Failed to restart UDP with new config!");
            }
            else
            {
                log_info("VST", "UDP restarted → %s:%d",
                         getUdpAddressString().toRawUTF8(),
                         static_cast<int>(udpPortParam->load()));
            }
        }
    }
    // ── Mix balance crossfader: push derived opacities into APVTS ─────────────
    // This ensures CisVisualizerComponent (which reads APVTS directly) and the
    // C pipeline (which reads g_sp3ctra_config) both stay in sync.
    else if (parameterID == "imageMixBalance")
    {
        const float bal = newValue;
        const float liveOp = std::min(1.0f, 2.0f * bal);
        const float smpOp  = std::min(1.0f, 2.0f * (1.0f - bal));

        if (auto* p = apvts.getParameter("imageLiveOpacity"))
            p->setValueNotifyingHost(p->convertTo0to1(liveOp));
        if (auto* p = apvts.getParameter("imageSamplerOpacity"))
            p->setValueNotifyingHost(p->convertTo0to1(smpOp));

        applyConfigurationToCore(false);
        return;
    }
    else
    {
        // For other non-UDP, non-LuxStral parameters (sensor DPI, log level, visualizer mode)
        applyConfigurationToCore(false);  // needsSocketRestart = false
        
        // RT Profiler stays ALWAYS enabled — profiler output uses log_info (not log_debug)
        // so it is visible regardless of log level. Changing log level only affects
        // the verbose per-metric breakdown (which uses log_debug).
        if (parameterID == PARAM_LOG_LEVEL) {
            log_info("VST", "Log level changed - RT Profiler summary remains visible at INFO");
        }
    }
}

//==============================================================================
// M6 Phase 2 — chain topology ownership (model lives here, not in the editor)
//==============================================================================
void Sp3ctraAudioProcessor::loadChainModelFromState()
{
    auto& state = apvts.state;
    auto t = state.getChildWithName(ChainModel::kChainsTag);
    if (t.isValid())
        chainModel_.fromValueTree(t);
    else
        chainModel_ = ChainModel::makeDefault();
    chainModel_.validateAndRepair();

    // VIDEO SCROLL opens RUNNING (see setStateInformation). "paused" is a
    // chain-owned VALUE, so projectChainValuesToBanks() would otherwise
    // re-apply a saved paused=1 on top of the flat-param patch and re-freeze
    // the waterfall. Normalise the transport bit to "running" in the loaded
    // model so the projection reinforces it instead of fighting it. The
    // artistic settings (speed/zoom/fade/…) are untouched.
    for (auto& ch : chainModel_.chains)
        for (auto& m : ch.modules)
            if (m.type == ModuleType::VideoScroll
                && m.values.isValid()
                && m.values.hasProperty("paused"))
                m.values.setProperty("paused", 0.0, nullptr);

    // Migration: the step sequencer became a dedicated SEQUENCER module. A model
    // saved before that has no Sequencer block — inject one so the sequencer
    // stays reachable in the UI (same spirit as "always keep ≥1 chain").
    // GATED so a deliberate deletion survives reload: from schema v2 on, a
    // missing Sequencer means the user removed it. A v1 tree is only "old"
    // when the session has no SEQ pattern tree either — SEQ serialization
    // shipped together with the sequencer-module era, so its presence proves
    // the save could already contain (or deliberately omit) a Sequencer block.
    {
        const int savedVersion = t.isValid()
            ? (int) t.getProperty(ChainModel::kVersionProp, 1)
            : ChainModel::kSchemaVersion;   // fresh default — already has one
        const bool preSequencerEra =
            savedVersion < 2 && ! state.getChildWithName("SEQ").isValid();
        if (preSequencerEra)
        {
            bool hasSeq = false;
            for (const auto& ch : chainModel_.chains)
                for (const auto& mod : ch.modules)
                    if (mod.type == ModuleType::Sequencer) { hasSeq = true; break; }
            if (! hasSeq && ! chainModel_.chains.empty())
                chainModel_.insert(0, ModuleType::Sequencer,
                                   (int) chainModel_.chains[0].modules.size());
        }
    }

    // Presence baseline so the enable bridge only fires on real transitions.
    chainActiveTypes_.clear();
    chainModel_.deriveActiveTypes(chainActiveTypes_);
    videoScrollSlots_.clear();
    videoScrollSlotIds_.clear();
    luxstralEngines_.clear();
    for (const auto& ch : chainModel_.chains)
        for (const auto& mod : ch.modules)
        {
            if (mod.type == ModuleType::VideoScroll
                && mod.slot >= 0 && mod.slot < CHAIN_MAX_CHAINS)
            {
                videoScrollSlots_.insert(mod.slot);
                videoScrollSlotIds_[mod.slot] = mod.id;   // identity baseline (no reset on load)
            }
            if (mod.type == ModuleType::LuxStral
                && mod.slot >= 0 && mod.slot < ChainModel::kMaxLuxStralEngines)
                luxstralEngines_.insert(mod.slot);
        }

    deriveChainRouting();   // headless-correct per-synth source routing

    // Location baseline for the per-chain settings memory — a session load must
    // never snapshot/reset the freshly restored banks.
    baselineInsertLocations();
}

void Sp3ctraAudioProcessor::deriveChainRouting()
{
    // (M8: the legacy per-synth source routing is gone — the ChainPlan is the
    // single routing authority; see deriveAndPublishChainPlan.)

    // Stable MODULE-INSTANCE → pool-slot binding (keyed by instance UUID). The
    // state belongs to the module: moving it across chains carries its live
    // state; removing / reordering other chains never rebinds it. Slots whose
    // binding just changed carry stale state — reset AFTER the new plan is
    // published at the end of this function.
    const PoolStale staleSlots = updateModulePoolBindings();

    // Active Pitch/Mask/Reverb/Echo instances → MIDI fan-out + config-sync
    // mask, indexed by the INSTANCE'S pool slot (stable across edits and
    // chain moves).
    uint32_t pitchMask = 0, maskMask = 0, reverbMask = 0, echoMask = 0, eqMask = 0;
    for (int c = 0; c < chainModel_.numChains(); ++c)
    {
        for (const auto& m : chainModel_.chains[(size_t) c].modules)
        {
            const int slot = poolSlotForInstance(m.id);
            if (m.type == ModuleType::Pitch)     pitchMask  |= (1u << slot);
            if (m.type == ModuleType::Mask)      maskMask   |= (1u << slot);
            if (m.type == ModuleType::Reverb)    reverbMask |= (1u << slot);
            if (m.type == ModuleType::Echo)      echoMask   |= (1u << slot);
            if (m.type == ModuleType::Equalizer) eqMask     |= (1u << slot);
        }
    }
    chainPitchMask_.store(pitchMask, std::memory_order_relaxed);
    chainMaskMask_.store(maskMask,  std::memory_order_relaxed);
    chainReverbMask_.store(reverbMask, std::memory_order_relaxed);
    chainEchoMask_.store(echoMask,   std::memory_order_relaxed);
    chainEqMask_.store(eqMask,       std::memory_order_relaxed);

    // Per-instance `enabled` sync — must NOT wait for applyConfigurationToCore:
    // a pure topology change (Pitch dragged to another chain, chain removal,
    // session restore) flips no enable param, so no parameterChanged() would
    // refresh the pool configs and the moved module would stay silently
    // bypassed on its new slot until some unrelated param edit.
    {
        auto bankOn = [this](const juce::String& id)
        { return apvts.getRawParameterValue(id)->load() >= 0.5f; };
        for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        {
            lux_pitch_instance(i)->config.enabled =
                (bankOn(lpParam(i, "Enabled")) && ((pitchMask  >> i) & 1u)) ? 1 : 0;
            lux_mask_instance(i)->config.enabled =
                (bankOn(lmParam(i, "Enabled")) && ((maskMask   >> i) & 1u)) ? 1 : 0;
            lux_reverb_instance(i)->config.enabled =
                (bankOn(rvParam(i, "Enabled")) && ((reverbMask >> i) & 1u)) ? 1 : 0;
            lux_echo_instance(i)->config.enabled =
                (bankOn(ecParam(i, "Enabled")) && ((echoMask   >> i) & 1u)) ? 1 : 0;
            lux_eq_instance(i)->config.enabled =
                (bankOn(eqParam(i, "Enabled")) && ((eqMask     >> i) & 1u)) ? 1 : 0;
        }
    }

    // Per-engine sampler enable: a Sampler instance carries its engine index in
    // `slot` (0 = A, 1 = B). An engine is enabled iff its instance is present
    // in the model AND the shared luxSamplerEnabled param (rack LED / host
    // automation) is on — presence alone made the LED a dead toggle.
    bool samplerAPresent = false, samplerBPresent = false;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
        {
            if (m.type == ModuleType::Sampler)
            {
                if (m.slot == 1) samplerBPresent = true;
                else             samplerAPresent = true;   // slot 0 (or unhealed -1)
            }
        }
    samplerAPresent_ = samplerAPresent;
    samplerBPresent_ = samplerBPresent;
    const bool fsParamOn =
        apvts.getRawParameterValue(PARAM_FS_ENABLED)->load() > 0.5f;
    if (luxSampler)  luxSampler ->setEnabled(samplerAPresent && fsParamOn);
    if (luxSamplerB) luxSamplerB->setEnabled(samplerBPresent && fsParamOn);

    // (P4-M5: the "chainInsertOrder" projection is gone with the param —
    // which LuxStral consumes whenever a Sampler sits on its chain — the default
    // topology. (P4-M3: the GLOBAL insert order died with the modulated
    // build — per-chain order comes from each chain's own recipe; the
    // "chainInsertOrder" param remains as a host-visible projection only.)

    // M9 — push IMAGE/VIDEO/CAMERA module presence onto their engines so they
    // publish lines only while placed in a chain.
    updateMediaSourcePresence();

    deriveAndPublishChainPlan();   // RT-safe per-chain recipe for the synth thread

    // Reset the transient state (held voices, LFO phase — config untouched) of
    // every pool slot that just lost its Pitch/Mask instance or changed its
    // chain binding. Without this, a removed instance's held voices would
    // silently resurface when the module (or a new chain) reuses the slot.
    // DEFERRED (not immediate): the publish above only takes effect from the
    // NEXT frame — the UDP/feeder thread may still be running the CURRENT
    // frame with the OLD plan, actively writing these very pool instances.
    // The processor timer executes the reset ≥40 ms later (frames last ~1 ms).
    {
        const uint32_t lostPitch  = (prevPitchSlots_  & ~pitchMask)  | staleSlots.pitch;
        const uint32_t lostMask   = (prevMaskSlots_   & ~maskMask)   | staleSlots.mask;
        const uint32_t lostReverb = (prevReverbSlots_ & ~reverbMask) | staleSlots.reverb;
        const uint32_t lostEcho   = (prevEchoSlots_   & ~echoMask)   | staleSlots.echo;
        const uint32_t lostEq     = (prevEqSlots_     & ~eqMask)     | staleSlots.eq;
        pendingPitchResets_  |= lostPitch;
        pendingMaskResets_   |= lostMask;
        pendingReverbResets_ |= lostReverb;
        pendingEchoResets_   |= lostEcho;
        pendingEqResets_     |= lostEq;
        // A slot ACTIVE in the new plan must not be reset by a pending bit
        // armed for a previous removal (remove + re-add within the 40 ms
        // window): the deferred reset would wipe — and race — the freshly
        // active instance. Trade-off: such a fast re-add skips the clean-
        // start reset (held voices may resurface — the old, benign quirk).
        pendingPitchResets_  &= ~pitchMask;
        pendingMaskResets_   &= ~maskMask;
        pendingReverbResets_ &= ~reverbMask;
        pendingEchoResets_   &= ~echoMask;
        pendingEqResets_     &= ~eqMask;
        if ((lostPitch | lostMask | lostReverb | lostEcho | lostEq) != 0)
            poolResetArmedMs_ = juce::Time::getMillisecondCounter();
        prevPitchSlots_  = pitchMask;
        prevMaskSlots_   = maskMask;
        prevReverbSlots_ = reverbMask;
        prevEchoSlots_   = echoMask;
        prevEqSlots_     = eqMask;
    }
}

//==============================================================================
// Stable MODULE-INSTANCE → pool-slot binding (message thread).
// Instances keep their existing slot (keyed by ModuleInstance UUID) — the
// state belongs to the MODULE, so moving it to another chain carries its live
// state along. Vanished instances release their slot; new instances take the
// lowest free slot of THEIR TYPE'S pool. Returns per-type masks of slots whose
// binding changed (released or freshly assigned) — their pool state is stale.
//==============================================================================
Sp3ctraAudioProcessor::PoolStale Sp3ctraAudioProcessor::updateModulePoolBindings()
{
    PoolStale stale;
    auto staleFor = [&stale](ModuleType t) -> uint32_t&
    {
        switch (t)
        {
            case ModuleType::Pitch:     return stale.pitch;
            case ModuleType::Mask:      return stale.mask;
            case ModuleType::Reverb:    return stale.reverb;
            case ModuleType::Equalizer: return stale.eq;
            case ModuleType::Echo:
            default:                    return stale.echo;
        }
    };
    auto isPooled = [](ModuleType t)
    {
        return t == ModuleType::Pitch  || t == ModuleType::Mask
            || t == ModuleType::Reverb || t == ModuleType::Echo
            || t == ModuleType::Equalizer;
    };

    std::map<juce::Uuid, ModuleType> live;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (isPooled(m.type))
                live[m.id] = m.type;

    for (auto it = modulePoolSlots_.begin(); it != modulePoolSlots_.end();)
    {
        const auto liveIt = live.find(it->first);
        // Type mismatch only happens on a corrupt/hand-edited POOL_SLOTS seed —
        // drop the binding and let the instance take a fresh slot of its type.
        if (liveIt == live.end() || liveIt->second != it->second.type)
        {
            staleFor(it->second.type) |= (1u << it->second.slot); // module gone → stale
            it = modulePoolSlots_.erase(it);
        }
        else
            ++it;
    }

    // Per-type used masks over the surviving bindings.
    std::map<ModuleType, uint32_t> used;
    for (const auto& binding : modulePoolSlots_)
        used[binding.second.type] |= (1u << binding.second.slot);

    for (const auto& [id, type] : live)
    {
        if (modulePoolSlots_.count(id) != 0)
            continue;
        uint32_t& u = used[type];
        for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
            if (((u >> s) & 1u) == 0)
            {
                modulePoolSlots_[id] = { s, type };
                u                |= (1u << s);
                staleFor(type)   |= (1u << s);    // fresh binding → start clean
                break;
            }
        // The model allows at most one instance of a pooled type per chain and
        // caps chains at kMaxChains == CHAIN_MAX_CHAINS, so a free slot always
        // exists for a legal model.
        jassert(modulePoolSlots_.count(id) != 0);
    }
    return stale;
}

int Sp3ctraAudioProcessor::poolSlotForInstance(const juce::Uuid& moduleId) const noexcept
{
    const auto it = modulePoolSlots_.find(moduleId);
    return it != modulePoolSlots_.end() ? it->second.slot : 0;
}

//==============================================================================
// MIDI-follow — decode a mapped parameter id back to the module instance that
// owns it (inverse of the per-instance bank id helpers in PluginProcessor.h).
//==============================================================================
Sp3ctraAudioProcessor::ParamNavTarget
Sp3ctraAudioProcessor::navTargetForParam(const juce::String& id) const
{
    ParamNavTarget t;

    // Pooled inserts: bank slot IS the POOL slot (keyed by UUID). Inverse lookup.
    auto poolInstance = [this](ModuleType type, int slot) -> juce::Uuid
    {
        for (const auto& [uuid, b] : modulePoolSlots_)
            if (b.type == type && b.slot == slot)
                return uuid;
        return juce::Uuid::null();
    };
    // Slotted / singleton types live in the chain model; slot < 0 = first of type.
    auto chainInstance = [this](ModuleType type, int slot) -> juce::Uuid
    {
        for (const auto& ch : chainModel_.chains)
            for (const auto& mi : ch.modules)
                if (mi.type == type && (slot < 0 || mi.slot == slot))
                    return mi.id;
        return juce::Uuid::null();
    };
    // Match a banked id "<prefix><digits>_..." and pull out the slot.
    auto banked = [&id](const char* prefix, int& outSlot) -> bool
    {
        const juce::String p(prefix);
        if (! id.startsWith(p))
            return false;
        const juce::String rest = id.substring(p.length());
        const int us = rest.indexOfChar('_');
        if (us <= 0)
            return false;
        const juce::String num = rest.substring(0, us);
        if (! num.containsOnly("0123456789"))
            return false;
        outSlot = num.getIntValue();
        return true;
    };

    int slot = -1;
    // Order matters: the banked "...Out"/"luxSamplerB" families must be tested
    // before their generic "luxstral"/"luxSampler" prefixes.
    if      (banked("luxpitch",  slot)) { t.type = ModuleType::Pitch;      t.instanceId = poolInstance (t.type, slot); }
    else if (banked("luxmask",   slot)) { t.type = ModuleType::Mask;       t.instanceId = poolInstance (t.type, slot); }
    else if (banked("luxreverb", slot)) { t.type = ModuleType::Reverb;     t.instanceId = poolInstance (t.type, slot); }
    else if (banked("luxecho",   slot)) { t.type = ModuleType::Echo;       t.instanceId = poolInstance (t.type, slot); }
    else if (banked("luxeq",     slot)) { t.type = ModuleType::Equalizer;  t.instanceId = poolInstance (t.type, slot); }
    else if (banked("videoScroll", slot) || banked("videoMix", slot))
                                        { t.type = ModuleType::VideoScroll; t.instanceId = chainInstance(t.type, slot); }
    else if (banked("luxstralOut", slot)) { t.type = ModuleType::LuxStral; t.instanceId = chainInstance(t.type, slot); }
    else if (banked("luxsynthOut", slot)) { t.type = ModuleType::LuxSynth; t.instanceId = chainInstance(t.type, -1); }
    else if (banked("luxwaveOut",  slot)) { t.type = ModuleType::LuxWave;  t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("luxSamplerB")) { t.type = ModuleType::Sampler; t.instanceId = chainInstance(t.type, 1); }
    else if (id.startsWith("luxSampler"))  { t.type = ModuleType::Sampler; t.instanceId = chainInstance(t.type, 0); }
    // Virtual (non-APVTS) sampler targets — REC/PLAY actions and per-slot value
    // params. Their synthetic id encodes the engine as "smp:e{E}:…" (E = 0/1),
    // and the model stores that engine index in the module's slot. So a mapped
    // key that records/plays a slot follows to that sampler's page.
    else if (id.startsWith("smp:e1"))      { t.type = ModuleType::Sampler; t.instanceId = chainInstance(t.type, 1); }
    else if (id.startsWith("smp:e0"))      { t.type = ModuleType::Sampler; t.instanceId = chainInstance(t.type, 0); }
    // Synth ENGINE params (own page). StrokeForge (sf*) / blob (spctr*) belong
    // to LuxStral.
    else if (id.startsWith("luxstral") || id.startsWith("sf") || id.startsWith("spctr"))
                                        { t.type = ModuleType::LuxStral; t.engineView = true; t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("luxsynth")) { t.type = ModuleType::LuxSynth; t.engineView = true; t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("luxwave"))  { t.type = ModuleType::LuxWave;  t.engineView = true; t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("score"))    { t.type = ModuleType::Score;    t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("timbre"))   { t.type = ModuleType::Timbre;   t.instanceId = chainInstance(t.type, -1); }
    else
        return t;   // global / source / master param — no module to navigate to

    t.valid = ! t.instanceId.isNull();
    return t;
}

//==============================================================================
// IVirtualMidiSink — NON-APVTS mapping targets: the LuxSampler per-slot play
// params (Speed/Loop/EQ floor/fades…) and the REC/PLAY/SAVE action buttons.
// virtualResolve runs on the message thread; the rest run on the audio thread
// and only touch atomics (LuxSampler setters + the per-slot action pulses).
//==============================================================================
int Sp3ctraAudioProcessor::virtualResolve(const juce::String& paramId) const
{
    return SamplerMidiTargets::resolve(paramId);
}

int Sp3ctraAudioProcessor::virtualSteps(int targetId) const noexcept
{
    return SamplerMidiTargets::steps(SamplerMidiTargets::tKind(targetId));
}

float Sp3ctraAudioProcessor::virtualRead(int targetId) const noexcept
{
    const int  e = SamplerMidiTargets::tEngine(targetId);
    LuxSampler* fs = (e == 1) ? luxSamplerB.get() : luxSampler.get();
    if (fs == nullptr) return 0.0f;
    return SamplerMidiTargets::read(*fs, SamplerMidiTargets::tSlot(targetId),
                                    SamplerMidiTargets::tKind(targetId));
}

void Sp3ctraAudioProcessor::virtualApply(int targetId, float norm01) noexcept
{
    const auto kind = SamplerMidiTargets::tKind(targetId);
    const int  e    = SamplerMidiTargets::tEngine(targetId) & 1;
    const int  s    = SamplerMidiTargets::tSlot(targetId) % kSmpSlots;

    if (SamplerMidiTargets::isAction(kind))
    {
        // Action "press" — latch a pulse for the open SlotEditor to run on the
        // message thread (uiToggleRecord / uiPlaySlot / save are non-RT).
        switch (kind)
        {
            case SamplerMidiTargets::Kind::Rec:
                if (! smpRecHeld[e][s].exchange(true, std::memory_order_acq_rel))
                    smpRecPressed[e][s].store(true, std::memory_order_release);
                break;
            case SamplerMidiTargets::Kind::Play:
                if (! smpPlayHeld[e][s].exchange(true, std::memory_order_acq_rel))
                    smpPlayPressed[e][s].store(true, std::memory_order_release);
                break;
            case SamplerMidiTargets::Kind::Save:
                smpSaveTrigger[e][s].store(true, std::memory_order_release);
                break;
            case SamplerMidiTargets::Kind::Clear:
                smpClearTrigger[e][s].store(true, std::memory_order_release);
                break;
            default: break;
        }
        return;
    }

    // EQ band — non-RT to apply (parse + LUT rebuild), so latch the normalised
    // value; the open SlotEditor drains it on the message thread.
    if (kind == SamplerMidiTargets::Kind::EqBand)
    {
        const int band = SamplerMidiTargets::tBand(targetId) % LuxSampler::kEqBands;
        smpEqPending[e][s][band].store(juce::jlimit(0.0f, 1.0f, norm01),
                                       std::memory_order_release);
        smpValueTouchWhere_.store((e << 8) | s, std::memory_order_relaxed);
        smpValueTouchGen_  .fetch_add(1u, std::memory_order_release);
        return;
    }

    // Value target — apply straight to the engine (atomic store), then flag the
    // touch so the open SlotEditor refreshes its sliders if it shows this slot.
    LuxSampler* fs = (e == 1) ? luxSamplerB.get() : luxSampler.get();
    if (fs == nullptr) return;
    SamplerMidiTargets::apply(*fs, s, kind, norm01);
    smpValueTouchWhere_.store((e << 8) | s, std::memory_order_relaxed);
    smpValueTouchGen_  .fetch_add(1u, std::memory_order_release);
}

void Sp3ctraAudioProcessor::virtualRelease(int targetId) noexcept
{
    const auto kind = SamplerMidiTargets::tKind(targetId);
    const int  e    = SamplerMidiTargets::tEngine(targetId) & 1;
    const int  s    = SamplerMidiTargets::tSlot(targetId) % kSmpSlots;

    // Momentary action "release" — mirror the press latch.
    if (kind == SamplerMidiTargets::Kind::Rec)
    {
        if (smpRecHeld[e][s].exchange(false, std::memory_order_acq_rel))
            smpRecReleased[e][s].store(true, std::memory_order_release);
    }
    else if (kind == SamplerMidiTargets::Kind::Play)
    {
        if (smpPlayHeld[e][s].exchange(false, std::memory_order_acq_rel))
            smpPlayReleased[e][s].store(true, std::memory_order_release);
    }
}

//==============================================================================
// REC / PLAY transport-button mode (per engine). Choice index 1 = "Momentary".
//==============================================================================
bool Sp3ctraAudioProcessor::samplerRecMomentary(int engine) const noexcept
{
    if (auto* v = apvts.getRawParameterValue(fsEngineParam(engine & 1, "RecMode")))
        return v->load() > 0.5f;
    return false;
}

bool Sp3ctraAudioProcessor::samplerPlayMomentary(int engine) const noexcept
{
    if (auto* v = apvts.getRawParameterValue(fsEngineParam(engine & 1, "PlayMode")))
        return v->load() > 0.5f;
    return false;
}

//==============================================================================
// UUID → pool-slot binding persistence. The binding keys each instance's APVTS
// param bank, so it must survive a reload: rebuilding it in a different order
// would silently swap two same-type instances' settings.
//==============================================================================
juce::ValueTree Sp3ctraAudioProcessor::poolBindingsToTree() const
{
    juce::ValueTree t("POOL_SLOTS");
    for (const auto& [uuid, b] : modulePoolSlots_)
    {
        juce::ValueTree e("BIND");
        e.setProperty("uuid", uuid.toString(), nullptr);
        e.setProperty("type", juce::String(moduleTypeId(b.type)), nullptr);
        e.setProperty("slot", b.slot, nullptr);
        t.appendChild(e, nullptr);
    }
    return t;
}

void Sp3ctraAudioProcessor::restorePoolBindingsFromTree(const juce::ValueTree& t)
{
    modulePoolSlots_.clear();
    if (! t.isValid())
        return;
    std::map<ModuleType, uint32_t> used;   // reject duplicate slots (corrupt blob)
    for (const auto& e : t)
    {
        if (! e.hasType(juce::Identifier("BIND")))
            continue;
        ModuleType type;
        if (! moduleTypeFromId(e.getProperty("type").toString(), type)
            || ! isPooledInsertType(type))
            continue;
        const int slot = (int) e.getProperty("slot", -1);
        if (slot < 0 || slot >= ChainModel::kMaxChains)
            continue;
        if ((used[type] >> slot) & 1u)
            continue;
        const juce::String u = e.getProperty("uuid").toString();
        if (u.isEmpty())
            continue;
        modulePoolSlots_[juce::Uuid(u)] = { slot, type };
        used[type] |= (1u << slot);
    }
    // updateModulePoolBindings() (next derive) prunes entries whose instance is
    // absent from the restored model and assigns slots to any unseeded one.
}

//==============================================================================
// J3 — per-chain settings memory, generalized to EVERY manifest type (pooled
// inserts, VideoScroll, OUT sends, sampler engines) and stored IN THE CHAIN
// (Chain::typeMemory — serialized with the model, carried by presets).
// Diffs the model against the previous instance locations:
//   • instance removed (or moved to another chain) → its LAST chain snapshots
//     the bank values under its type (Enabled excluded);
//   • instance added → its bank is reset to defaults (never inherit a dead
//     instance's values via slot reuse), then the hosting chain's remembered
//     settings are applied (chain inheritance), then the module starts
//     enabled. Moved/kept instances carry their settings (stable slots).
// Runs on every model edit, AFTER updateModulePoolBindings() (fresh slots).
//==============================================================================
namespace
{
    inline bool isEnableSuffix(const char* sfx)
    {
        return std::strcmp(sfx, "Enabled") == 0
            || std::strcmp(sfx, "enabled") == 0;
    }
}

int Sp3ctraAudioProcessor::bankSlotForModule(const ModuleInstance& m) const
{
    const auto* d = moduleParamManifest(m.type);
    if (d == nullptr)
        return 0;
    return isPooledInsertType(m.type)
               ? poolSlotForInstance(m.id)
               : juce::jlimit(0, d->numSlots - 1, m.slot >= 0 ? m.slot : 0);
}

void Sp3ctraAudioProcessor::updateInsertParamMemory()
{
    std::map<juce::Uuid, InsertLoc> now;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (moduleParamManifest(m.type) != nullptr)
                now[m.id] = { ch.id, m.type, bankSlotForModule(m) };

    auto chainByUuid = [this](const juce::Uuid& id) -> Chain*
    {
        for (auto& ch : chainModel_.chains)
            if (ch.id == id)
                return &ch;
        return nullptr;
    };

    // The banks of departed instances still hold their values (pool resets only
    // touch the transient C state) — snapshot them into the chain's memory.
    // (A memory of a REMOVED chain needs no cleanup: it died with the chain.)
    for (const auto& [uuid, prev] : prevInsertLoc_)
    {
        const auto it = now.find(uuid);
        if (it != now.end() && it->second.chain == prev.chain)
            continue;
        Chain* src = chainByUuid(prev.chain);
        const auto* d = moduleParamManifest(prev.type);
        if (src == nullptr || d == nullptr)
            continue;
        juce::ValueTree mem(ChainModel::kValuesTag);
        for (int i = 0; i < d->numSuffixes; ++i)
        {
            const char* sfx = d->suffixes[i];
            if (isEnableSuffix(sfx))
                continue;   // a re-added module always starts enabled
            if (auto* raw = apvts.getRawParameterValue(d->paramId(prev.slot, sfx)))
                mem.setProperty(juce::Identifier(sfx), (double) raw->load(),
                                nullptr);
        }
        src->typeMemory[prev.type] = std::move(mem);
    }

    for (const auto& [uuid, loc] : now)
    {
        if (prevInsertLoc_.count(uuid) != 0)
            continue;   // moved/kept instances carry their settings with them
        const auto* d = moduleParamManifest(loc.type);
        if (d == nullptr)
            continue;

        for (int i = 0; i < d->numSuffixes; ++i)
            if (auto* p = apvts.getParameter(d->paramId(loc.slot, d->suffixes[i])))
                if (p->getValue() != p->getDefaultValue())
                    p->setValueNotifyingHost(p->getDefaultValue());

        // Chain inheritance: the hosting chain's remembered settings.
        if (Chain* host = chainByUuid(loc.chain))
        {
            const auto memIt = host->typeMemory.find(loc.type);
            if (memIt != host->typeMemory.end() && memIt->second.isValid())
                for (int i = 0; i < d->numSuffixes; ++i)
                {
                    const juce::Identifier sfx(d->suffixes[i]);
                    if (! memIt->second.hasProperty(sfx))
                        continue;
                    if (auto* p = apvts.getParameter(
                            d->paramId(loc.slot, d->suffixes[i])))
                        p->setValueNotifyingHost(p->convertTo0to1(
                            (float) (double) memIt->second.getProperty(sfx)));
                }
        }

        // Newly placed ⇒ enabled (same semantics as the former type-level
        // enable bridge and the VideoScroll slot forcing).
        for (const char* en : { "Enabled", "enabled" })
            if (auto* p = apvts.getParameter(d->paramId(loc.slot, en)))
            {
                if (p->getValue() < 0.5f)
                    p->setValueNotifyingHost(1.0f);
                break;
            }
    }

    prevInsertLoc_ = std::move(now);
}

void Sp3ctraAudioProcessor::baselineInsertLocations()
{
    // Session load: the banks were restored WITH their instances — record the
    // locations without snapshotting or resetting anything.
    prevInsertLoc_.clear();
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (moduleParamManifest(m.type) != nullptr)
                prevInsertLoc_[m.id] = { ch.id, m.type, bankSlotForModule(m) };
}

void Sp3ctraAudioProcessor::restoreInsertMemoryFromTree(const juce::ValueTree& t)
{
    // J3 — one-shot MIGRATION of the legacy INSERT_MEMORY blob into the
    // chains' own type memory (Chain::typeMemory). Only fills chains that
    // carry no memory of that type yet (a v3+ blob already restored it via
    // the CHAIN/MEMORY children). Call AFTER loadChainModelFromState().
    if (! t.isValid())
        return;
    for (const auto& e : t)
    {
        if (! e.hasType(juce::Identifier("MEM")))
            continue;
        ModuleType type;
        if (! moduleTypeFromId(e.getProperty("type").toString(), type))
            continue;
        const auto* d = moduleParamManifest(type);
        const juce::String chainId = e.getProperty("chain").toString();
        if (d == nullptr || chainId.isEmpty())
            continue;
        for (auto& ch : chainModel_.chains)
        {
            if (ch.id.toString() != chainId)
                continue;
            if (ch.typeMemory.count(type) != 0)
                break;   // v3 memory wins
            juce::ValueTree mem(ChainModel::kValuesTag);
            for (int i = 0; i < d->numSuffixes; ++i)
            {
                const juce::Identifier sfx(d->suffixes[i]);
                if (e.hasProperty(sfx))
                    mem.setProperty(sfx, e.getProperty(sfx), nullptr);
            }
            if (mem.getNumProperties() > 0)
                ch.typeMemory[type] = std::move(mem);
            break;
        }
    }
}

//==============================================================================
// Contextual visualizer — the selected module drives the plan's selection tap.
//==============================================================================
void Sp3ctraAudioProcessor::setVisualizerTapModule(const juce::Uuid& moduleId)
{
    if (vizTapModuleId_ == moduleId)
        return;
    vizTapModuleId_ = moduleId;

    // Blank the tap first: if the new target's chain is silent/unfed, the view
    // must show white — never the PREVIOUS selection's last frame.
    if (auto* core = getSp3ctraCore())
        if (auto* ab = core->getAudioImageBuffers())
            audio_image_buffers_clear_selection_tap(ab);

    deriveAndPublishChainPlan();   // republish with the new viz-tap position
}

//==============================================================================
// Derive the per-synth chain recipe from the model and publish it RT-safely.
// Each synth (≤1 chain) gets: its source kind, the ordered Pitch/Mask inserts
// upstream of it (each bound to its chain's per-instance state pool slot), and
// whether a sampler/score sits upstream. Consumed by the synthesis thread.
//==============================================================================
void Sp3ctraAudioProcessor::deriveAndPublishChainPlan()
{
    ChainPlan plan;
    memset(&plan, 0, sizeof(plan));

    // Only modules UPSTREAM of the synth count as its source: "order is
    // significant" (ChainModel.h) — a source dragged BELOW the synth used to
    // feed it anyway, contradicting how Pitch/Mask below the synth are ignored.
    auto sourceKind = [](const Chain& ch, int limit) -> int
    {
        for (int i = 0; i < limit && i < (int) ch.modules.size(); ++i)
        {
            const auto& m = ch.modules[(size_t) i];
            if (m.type == ModuleType::Sp3ctra) return CHAIN_SRC_LIVE;
            if (m.type == ModuleType::Image)   return CHAIN_SRC_IMAGE;
            if (m.type == ModuleType::Video)   return CHAIN_SRC_VIDEO;
            if (m.type == ModuleType::Camera)  return CHAIN_SRC_CAMERA;
        }
        return CHAIN_SRC_NONE;
    };

    // Shared recipe builder: source + ordered inserts of ONE chain, up to (not
    // including) `limitIdx`. Used both for synth chains (limit = synth index)
    // and probe-only chains (limit = whole chain).
    auto fillFromChain = [&](SynthChainPlan& sp, int chainIdx, int limitIdx)
    {
        const auto& ch = chainModel_.chains[(size_t) chainIdx];
        sp.present        = 1;
        sp.source_kind    = sourceKind(ch, limitIdx);
        sp.viz_tap_insert = -1;   // set below when this chain hosts the selection

        for (int i = 0; i < limitIdx && i < (int) ch.modules.size(); ++i)
        {
            const ModuleInstance& mi = ch.modules[(size_t) i];
            const ModuleType t = mi.type;
            if ((t == ModuleType::Pitch || t == ModuleType::Mask
                 || t == ModuleType::Reverb || t == ModuleType::Echo
                 || t == ModuleType::Equalizer)
                && sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
            {
                sp.insert_id[sp.num_inserts] =
                      (t == ModuleType::Pitch) ? IMAGE_CHAIN_INSERT_LUXPITCH
                    : (t == ModuleType::Mask)  ? IMAGE_CHAIN_INSERT_LUXMASK
                    : (t == ModuleType::Reverb)? IMAGE_CHAIN_INSERT_LUXREVERB
                    : (t == ModuleType::Echo)  ? IMAGE_CHAIN_INSERT_LUXECHO
                    :                            IMAGE_CHAIN_INSERT_LUXEQ;
                // Pool slot bound to THIS INSTANCE's UUID — stable across edits
                // and chain moves (must match deriveChainRouting's masks).
                sp.insert_state_idx[sp.num_inserts] = poolSlotForInstance(mi.id);
                sp.num_inserts++;
            }
            else if (t == ModuleType::VideoScroll
                     && mi.slot >= 0 && mi.slot < CHAIN_MAX_CHAINS
                     && sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
            {
                // PASS-THROUGH PROBE: insert_state_idx is the PER-INSTANCE slot (0..7).
                sp.insert_id[sp.num_inserts]        = IMAGE_CHAIN_INSERT_VIDEOSCROLL;
                sp.insert_state_idx[sp.num_inserts] = mi.slot;
                sp.num_inserts++;
            }
            else if ((t == ModuleType::LuxStral || t == ModuleType::LuxSynth
                      || t == ModuleType::LuxWave)
                     && sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
            {
                // OUT SEND MARKER (M3/M6) — pass-through; locates the send so
                // the chain executor taps the stream at its position.
                // insert_state_idx = the send's conditioning-bank slot
                // (ModuleInstance.slot, per-type pools).
                sp.insert_id[sp.num_inserts] =
                      (t == ModuleType::LuxStral) ? IMAGE_CHAIN_INSERT_OUT_LUXSTRAL
                    : (t == ModuleType::LuxSynth) ? IMAGE_CHAIN_INSERT_OUT_LUXSYNTH
                    :                               IMAGE_CHAIN_INSERT_OUT_LUXWAVE;
                sp.insert_state_idx[sp.num_inserts] =
                    juce::jlimit(0, CHAIN_MAX_CHAINS - 1,
                                 mi.slot >= 0 ? mi.slot : 0);
                sp.num_inserts++;
            }
            else if (t == ModuleType::Sampler)
            {
                sp.has_sampler = 1;
                // Record the sampler's POSITION in the insert list so the executor
                // can feed VideoScroll probes pre- vs post-sampler correctly.
                // insert_state_idx = the instance's ENGINE slot (A=0/B=1) — the
                // per-chain feed records the chain's stream into THAT engine.
                if (sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
                {
                    sp.insert_id[sp.num_inserts]        = IMAGE_CHAIN_INSERT_SAMPLER;
                    sp.insert_state_idx[sp.num_inserts] =
                        juce::jlimit(0, 1, mi.slot >= 0 ? mi.slot : 0);
                    sp.num_inserts++;
                }
            }
            else if (isScoreFamily(t))
            {
                // The score family (kScoreFamily) auditions through the one
                // shared score-player channel (loadScoreFramesFromImage), so
                // every member raises the same plan flag. Guarded: with
                // several of them in one chain only the FIRST position becomes
                // the marker.
                if (! sp.has_score)
                {
                    sp.has_score = 1;
                    // Record the score's POSITION (like the sampler marker) so the
                    // player thread can apply the inserts BELOW the score to the
                    // playback frames (REVERB/ECHO/probes after SCORE).
                    if (sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
                    {
                        sp.insert_id[sp.num_inserts]        = IMAGE_CHAIN_INSERT_SCORE;
                        sp.insert_state_idx[sp.num_inserts] = 0;   // unused for the marker
                        sp.num_inserts++;
                    }
                }
            }

            // Contextual visualizer: the SELECTED module's output position in
            // this chain — "after the inserts pushed so far" (a source module
            // or a pass-through marker maps to the frame at its position).
            if (mi.id == vizTapModuleId_)
                sp.viz_tap_insert = sp.num_inserts;
        }
    };

    // (P4-M4) plan.synth[] is gone — chain[] and ls_send[] are the only
    // recipes; every consumer (executors, players, gates) is plan-driven.

    // Synth-split P3 — LuxStral SENDS: every "→ LUXSTRAL" OUT across all
    // chains becomes one ls_send entry (recipe compiled up to the OUT's
    // position, bank = the instance's slot). The audio-thread mixer blends
    // every staged send into the single engine feed; the legacy synth[A]
    // entry above stays filled for visualizer compatibility only.
    for (int c = 0; c < chainModel_.numChains()
                    && plan.num_ls_sends < CHAIN_MAX_CHAINS; ++c)
    {
        const auto& mods = chainModel_.chains[(size_t) c].modules;
        for (int i = 0; i < (int) mods.size()
                        && plan.num_ls_sends < CHAIN_MAX_CHAINS; ++i)
        {
            const auto& mi = mods[(size_t) i];
            if (mi.type != ModuleType::LuxStral)
                continue;
            LsSendPlan& snd = plan.ls_send[plan.num_ls_sends];
            snd.chain_idx = c;
            snd.bank_slot = juce::jlimit(0, CHAIN_MAX_CHAINS - 1,
                                         mi.slot >= 0 ? mi.slot : 0);
            fillFromChain(snd.recipe, c, i);
            if (mi.id == vizTapModuleId_)
                snd.recipe.viz_tap_insert = snd.recipe.num_inserts;
            ++plan.num_ls_sends;
        }
    }

    // M3 — uniform per-chain recipes: chain[i] mirrors model chain i. A chain
    // is executed (present=1) when something observes its stream: an OUT send
    // (LuxStral staging / Path-B feed), a VideoScroll probe, or the SELECTED
    // module (so zone 1 can show ANY module's stream, even in an otherwise
    // inert chain). Probes capture and OUT markers tap AT THEIR POSITION.
    plan.num_chains = chainModel_.numChains();
    for (int c = 0; c < plan.num_chains && c < CHAIN_MAX_CHAINS; ++c)
    {
        const auto& ch = chainModel_.chains[(size_t) c];
        bool hasOut = false, hasProbe = false, hasVizTarget = false,
             hasPlayer = false;
        for (const auto& m : ch.modules)
        {
            if (m.type == ModuleType::LuxStral || m.type == ModuleType::LuxSynth
                || m.type == ModuleType::LuxWave)
                hasOut = true;
            if (m.type == ModuleType::VideoScroll
                && m.slot >= 0 && m.slot < CHAIN_MAX_CHAINS)
                hasProbe = true;
            if (m.type == ModuleType::Sampler || isScoreFamily(m.type))
                hasPlayer = true;   // mod-bus owner candidate (REC/relay hooks)
            if (m.id == vizTapModuleId_)
                hasVizTarget = true;   // selection tap lives in this chain
        }
        if (! (hasOut || hasProbe || hasVizTarget || hasPlayer))
            continue;   // present stays 0 — nothing observes this chain

        fillFromChain(plan.chain[c], c, (int) ch.modules.size());
    }

    // Deferred staging reset: a chain slot that LOST its "→ LUXSTRAL" or
    // "→ LUXSYNTH" send (module removed / chain deleted / reorder) must stop
    // contributing to the mixes — the producers no longer iterate it, so its
    // last staged frame would linger (the plan-gated mixers already ignore
    // it; the reset guards against chain-index reuse). Reset ≥40 ms later
    // (pool-reset pattern: the in-flight frame may still write under the OLD
    // plan).
    {
        uint32_t sendChains = 0;
        for (int k = 0; k < plan.num_ls_sends; ++k)
            sendChains |= (1u << plan.ls_send[k].chain_idx);
        for (int c = 0; c < plan.num_chains && c < CHAIN_MAX_CHAINS; ++c)
            for (int i = 0; i < plan.chain[c].num_inserts; ++i)
                if (plan.chain[c].insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH
                    || plan.chain[c].insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXWAVE)
                    sendChains |= (1u << c);
        pendingStagingResets_ |= (prevLsSendChains_ & ~sendChains);
        prevLsSendChains_ = sendChains;
        if (pendingStagingResets_ != 0)
            poolResetArmedMs_ = juce::Time::getMillisecondCounter();
    }

    // Multi-chain split (2026-07-13): tell the sampler layer whether engines
    // A and B currently sit on the SAME chain. Gates the playback arbiter and
    // the SCORE relay's cross-engine displacement — on split chains both
    // engines play independently; on a shared chain (one stream) starting one
    // still evicts the other.
    {
        bool share = false;
        for (int c = 0; c < plan.num_chains && !share; ++c)
        {
            const SynthChainPlan& sp = plan.chain[c];
            if (!sp.present || !sp.has_sampler) continue;
            bool hasA = false, hasB = false;
            for (int i = 0; i < sp.num_inserts; ++i)
                if (sp.insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER)
                {
                    if (sp.insert_state_idx[i] == 0) hasA = true;
                    if (sp.insert_state_idx[i] == 1) hasB = true;
                }
            share = hasA && hasB;
        }
        LuxSampler::setEnginesShareChain(share);
    }

    chain_plan_publish(&plan);
}

std::vector<int> Sp3ctraAudioProcessor::activeVideoSlots() const
{
    std::vector<int> out;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (m.type == ModuleType::VideoScroll && m.slot >= 0)
                out.push_back(m.slot);
    std::sort(out.begin(), out.end());
    return out;
}

//==============================================================================
// J2 — the chain OWNS its modules' settings (chantier « chain porteuse »).
// snapshotBankValuesIntoModel: runtime banks → each ModuleInstance.values
// (called at save time; atomic reads, any thread). projectChainValuesToBanks:
// ModuleInstance.values → runtime banks (load/preset time; MESSAGE THREAD
// ONLY, guarded "only if different" so reopening a project never marks host
// automation lanes as touched).
//==============================================================================
void Sp3ctraAudioProcessor::snapshotBankValuesIntoModel()
{
    for (auto& ch : chainModel_.chains)
        for (auto& m : ch.modules)
        {
            const auto* d = moduleParamManifest(m.type);
            if (d == nullptr)
                continue;
            const int slot = isPooledInsertType(m.type)
                                 ? poolSlotForInstance(m.id)
                                 : juce::jlimit(0, d->numSlots - 1,
                                                m.slot >= 0 ? m.slot : 0);
            juce::ValueTree values(ChainModel::kValuesTag);
            for (int i = 0; i < d->numSuffixes; ++i)
            {
                if (auto* raw = apvts.getRawParameterValue(
                        d->paramId(slot, d->suffixes[i])))
                    values.setProperty(juce::Identifier(d->suffixes[i]),
                                       (double) raw->load(), nullptr);
            }
            m.values = std::move(values);
        }
}

void Sp3ctraAudioProcessor::projectChainValuesToBanks()
{
    JUCE_ASSERT_MESSAGE_THREAD
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
        {
            if (! m.values.isValid())
                continue;
            const auto* d = moduleParamManifest(m.type);
            if (d == nullptr)
                continue;
            const int slot = isPooledInsertType(m.type)
                                 ? poolSlotForInstance(m.id)
                                 : juce::jlimit(0, d->numSlots - 1,
                                                m.slot >= 0 ? m.slot : 0);
            for (int i = 0; i < d->numSuffixes; ++i)
            {
                const juce::Identifier key(d->suffixes[i]);
                if (! m.values.hasProperty(key))
                    continue;
                const float target = (float) (double) m.values.getProperty(key);
                const juce::String id = d->paramId(slot, d->suffixes[i]);
                auto* param = apvts.getParameter(id);
                auto* raw   = apvts.getRawParameterValue(id);
                if (param == nullptr || raw == nullptr)
                    continue;
                if (std::abs(raw->load() - target) < 1.0e-6f)
                    continue;   // identical → never touch the host lane
                param->setValueNotifyingHost(
                    param->convertTo0to1(target));
            }
        }
}

void Sp3ctraAudioProcessor::persistChainModel()
{
    auto& state = apvts.state;
    auto existing = state.getChildWithName(ChainModel::kChainsTag);
    if (existing.isValid())
        state.removeChild(existing, nullptr);
    state.appendChild(chainModel_.toValueTree(), nullptr);
}

//==============================================================================
// Non-APVTS module state ↔ session blob (SCORE / SEQ / SAMPLER_SLOTS)
//==============================================================================
juce::ValueTree Sp3ctraAudioProcessor::scoreStateToTree() const
{
    juce::ValueTree t("SCORE");
    const auto& s = scoreSettings_;
    t.setProperty("dynamicRangeDB",      s.dynamicRangeDB,      nullptr);
    t.setProperty("gammaCorrection",     s.gammaCorrection,     nullptr);
    t.setProperty("contrastFactor",      s.contrastFactor,      nullptr);
    t.setProperty("enableDithering",     s.enableDithering,     nullptr);
    t.setProperty("binsPerSecond",       s.binsPerSecond,       nullptr);
    t.setProperty("overlapPreset",       s.overlapPreset,       nullptr);
    t.setProperty("printerDpi",          s.printerDpi,          nullptr);
    t.setProperty("pageFormat",          s.pageFormat,          nullptr);
    t.setProperty("writingSpeed",        s.writingSpeed,        nullptr);
    t.setProperty("spectroHeightMM",     s.spectroHeightMM,     nullptr);
    t.setProperty("spectroHeightManual", s.spectroHeightManual, nullptr);
    t.setProperty("bottomMarginMM",      s.bottomMarginMM,      nullptr);
    t.setProperty("enableHighBoost",     s.enableHighBoost,     nullptr);
    t.setProperty("highBoostAlpha",      s.highBoostAlpha,      nullptr);
    t.setProperty("enableNoiseGate",     s.enableNoiseGate,     nullptr);
    t.setProperty("noiseGateThreshold",  s.noiseGateThreshold,  nullptr);
    t.setProperty("enableHighPassFilter",s.enableHighPassFilter,nullptr);
    t.setProperty("highPassCutoffFreq",  s.highPassCutoffFreq,  nullptr);
    t.setProperty("highPassFilterOrder", s.highPassFilterOrder, nullptr);
    t.setProperty("enableNormalization", s.enableNormalization, nullptr);
    t.setProperty("fftSize",             s.fftSize,             nullptr);
    t.setProperty("startTimeSec",        s.startTimeSec,        nullptr);
    t.setProperty("enableStereoMode",    s.enableStereoMode,    nullptr);
    t.setProperty("enableMultiRes",      s.enableMultiRes,      nullptr);
    // minFreq/maxFreq are recomputed at GENERATE time from the musical range —
    // persisting them would only freeze stale values; deliberately omitted.

    t.setProperty("ovManual",  scoreFreq_.manual,    nullptr);
    t.setProperty("ovTuning",  scoreFreq_.tuning,    nullptr);
    t.setProperty("ovRoot",    scoreFreq_.rootIndex, nullptr);
    t.setProperty("ovOctaves", scoreFreq_.octaves,   nullptr);
    return t;
}

void Sp3ctraAudioProcessor::restoreScoreStateFromTree(const juce::ValueTree& t)
{
    if (! t.isValid())
        return;
    auto& s = scoreSettings_;   // defaults (constructor) fill missing props
    s.dynamicRangeDB       = (double) t.getProperty("dynamicRangeDB",      s.dynamicRangeDB);
    s.gammaCorrection      = (double) t.getProperty("gammaCorrection",     s.gammaCorrection);
    s.contrastFactor       = (double) t.getProperty("contrastFactor",      s.contrastFactor);
    s.enableDithering      = (int)    t.getProperty("enableDithering",     s.enableDithering);
    s.binsPerSecond        = (double) t.getProperty("binsPerSecond",       s.binsPerSecond);
    s.overlapPreset        = (int)    t.getProperty("overlapPreset",       s.overlapPreset);
    s.printerDpi           = (double) t.getProperty("printerDpi",          s.printerDpi);
    s.pageFormat           = (int)    t.getProperty("pageFormat",          s.pageFormat);
    s.writingSpeed         = (double) t.getProperty("writingSpeed",        s.writingSpeed);
    s.spectroHeightMM      = (double) t.getProperty("spectroHeightMM",     s.spectroHeightMM);
    s.spectroHeightManual  = (int)    t.getProperty("spectroHeightManual", s.spectroHeightManual);
    s.bottomMarginMM       = (double) t.getProperty("bottomMarginMM",      s.bottomMarginMM);
    s.enableHighBoost      = (int)    t.getProperty("enableHighBoost",     s.enableHighBoost);
    s.highBoostAlpha       = (double) t.getProperty("highBoostAlpha",      s.highBoostAlpha);
    s.enableNoiseGate      = (int)    t.getProperty("enableNoiseGate",     s.enableNoiseGate);
    s.noiseGateThreshold   = (double) t.getProperty("noiseGateThreshold",  s.noiseGateThreshold);
    s.enableHighPassFilter = (int)    t.getProperty("enableHighPassFilter",s.enableHighPassFilter);
    s.highPassCutoffFreq   = (double) t.getProperty("highPassCutoffFreq",  s.highPassCutoffFreq);
    s.highPassFilterOrder  = (int)    t.getProperty("highPassFilterOrder", s.highPassFilterOrder);
    s.enableNormalization  = (int)    t.getProperty("enableNormalization", s.enableNormalization);
    s.fftSize              = (int)    t.getProperty("fftSize",             s.fftSize);
    s.startTimeSec         = (double) t.getProperty("startTimeSec",        s.startTimeSec);
    s.enableStereoMode     = (int)    t.getProperty("enableStereoMode",    s.enableStereoMode);
    s.enableMultiRes       = (int)    t.getProperty("enableMultiRes",      s.enableMultiRes);
    if (! s.spectroHeightManual)
        s.spectroHeightMM = SCORE_CIS_HEIGHT_MM;   // keep the lock invariant

    scoreFreq_.manual    = (bool)   t.getProperty("ovManual",  scoreFreq_.manual);
    scoreFreq_.tuning    = (double) t.getProperty("ovTuning",  scoreFreq_.tuning);
    scoreFreq_.rootIndex = (int)    t.getProperty("ovRoot",    scoreFreq_.rootIndex);
    scoreFreq_.octaves   = (int)    t.getProperty("ovOctaves", scoreFreq_.octaves);
}

juce::ValueTree Sp3ctraAudioProcessor::seqStateToTree() const
{
    if (frameSequencer == nullptr)
        return {};
    juce::XmlElement xml("SEQ");
    frameSequencer->saveToXml(xml);
    return juce::ValueTree::fromXml(xml);
}

juce::ValueTree Sp3ctraAudioProcessor::samplerSlotsStateToTree() const
{
    juce::ValueTree root("SAMPLER_SLOTS");
    const LuxSampler* engines[] = { luxSampler.get(), luxSamplerB.get() };
    for (int e = 0; e < 2; ++e)
    {
        if (engines[e] == nullptr)
            continue;
        juce::XmlElement engXml("Engine");
        engXml.setAttribute("idx",     e);
        engXml.setAttribute("overdub", engines[e]->getOverdubMode() ? 1 : 0);
        for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
        {
            auto* slotXml = engXml.createNewChildElement("Slot");
            engines[e]->slotParamsToXml(i, *slotXml);
        }
        root.appendChild(juce::ValueTree::fromXml(engXml), nullptr);
    }
    return root;
}

void Sp3ctraAudioProcessor::applySamplerParamsFromState()
{
    auto root = apvts.state.getChildWithName("SAMPLER_SLOTS");
    if (! root.isValid())
        return;
    LuxSampler* engines[] = { luxSampler.get(), luxSamplerB.get() };
    for (const auto& eng : root)
    {
        const int e = (int) eng.getProperty("idx", -1);
        if (e < 0 || e > 1 || engines[e] == nullptr)
            continue;
        engines[e]->setOverdubMode((int) eng.getProperty("overdub", 0) != 0);
        for (const auto& slot : eng)
        {
            if (auto slotXml = slot.createXml())
            {
                const int i = slotXml->getIntAttribute("idx", -1);
                // Settings follow content: a bank that is EMPTY right now
                // starts from clean defaults instead of resurrecting the
                // previous session's knobs. At restore time recordings are
                // not loaded yet (they live in the .sp3s, not the DAW blob) —
                // the auto-load refills the banks and re-runs this overlay,
                // so content-bearing banks do get their saved settings back.
                if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS
                    && ! engines[e]->slotHasContent(i))
                    engines[e]->resetSlotPlayParams(i);
                else
                    engines[e]->slotParamsFromXml(i, *slotXml);
            }
        }
    }
}

//==============================================================================
// M9 — IMAGE / VIDEO / CAMERA source engines: presence + state blob
//==============================================================================
void Sp3ctraAudioProcessor::updateMediaSourcePresence()
{
    bool img = false, vid = false, cam = false;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
        {
            if (m.type == ModuleType::Image)  img = true;
            if (m.type == ModuleType::Video)  vid = true;
            if (m.type == ModuleType::Camera) cam = true;
        }
    if (imageSource_)  imageSource_ ->setModulePresent(img);
    if (videoSource_)  videoSource_ ->setModulePresent(vid);
    if (cameraSource_) cameraSource_->setModulePresent(cam);

    // A CAMERA module placed with a persisted device choice and no open device
    // (fresh session restore, or module re-added) → reopen it. Message thread.
    if (cam && cameraSource_ && ! cameraSource_->isOpen()
        && cameraDeviceName_.isNotEmpty())
    {
        const auto names = CameraSourceEngine::getDeviceNames();
        const int  idx   = names.indexOf(cameraDeviceName_);
        if (idx >= 0)
        {
            juce::String err;
            if (! cameraSource_->openDevice(idx, err))
                log_warning("VST", "Camera reopen failed: %s", err.toRawUTF8());
        }
    }
    // Module removed → release the device (turns the camera light off).
    if (! cam && cameraSource_ && cameraSource_->isOpen())
        cameraSource_->closeDevice();
}

juce::ValueTree Sp3ctraAudioProcessor::mediaSourcesStateToTree() const
{
    juce::ValueTree t("MEDIA_SOURCES");
    if (imageSource_)
        t.setProperty("imagePath", imageSource_->getFile().getFullPathName(), nullptr);
    if (videoSource_)
        t.setProperty("videoPath", videoSource_->getFile().getFullPathName(), nullptr);
    t.setProperty("cameraDevice", cameraDeviceName_, nullptr);
    return t;
}

void Sp3ctraAudioProcessor::restoreMediaSourcesFromTree(const juce::ValueTree& t)
{
    if (! t.isValid())
        return;

    const juce::String imgPath = t.getProperty("imagePath", "").toString();
    if (imageSource_ && imgPath.isNotEmpty())
    {
        juce::String err;
        if (! imageSource_->loadFile(juce::File(imgPath), err))
            log_warning("VST", "Image source restore failed: %s", err.toRawUTF8());
    }

    const juce::String vidPath = t.getProperty("videoPath", "").toString();
    if (videoSource_ && vidPath.isNotEmpty())
    {
        juce::String err;
        if (! videoSource_->loadFile(juce::File(vidPath), err))
            log_warning("VST", "Video source restore failed: %s", err.toRawUTF8());
    }

    cameraDeviceName_ = t.getProperty("cameraDevice", "").toString();
    // The device itself is (re)opened by updateMediaSourcePresence() once the
    // chain model restore confirms a CAMERA module is actually placed.
}

void Sp3ctraAudioProcessor::applyChainEnableBridge()
{
    auto setParam = [this](const juce::String& id, bool on)
    {
        if (id.isEmpty())
            return;
        if (auto* param = apvts.getParameter(id))
        {
            const float v = on ? 1.0f : 0.0f;
            if (param->getValue() != v)
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost(v);
                param->endChangeGesture();
            }
        }
    };

    std::set<ModuleType> now;
    chainModel_.deriveActiveTypes(now);

    // LuxStral's engine enable (deviceEnabled) is handled below on the
    // presence of ANY "→ LUXSTRAL" send; the per-send power lives in the
    // luxstralOut{N}_enabled banks.
    // Pitch/Mask/Reverb/Echo are handled PER INSTANCE in
    // updateInsertParamMemory() (their enable lives in the per-slot bank).
    static const ModuleType kEnableTypes[] = {
        ModuleType::Sampler, ModuleType::Sequencer,
        ModuleType::LuxSynth, ModuleType::LuxWave
    };
    for (auto t : kEnableTypes)
    {
        const bool isNow = now.count(t) > 0;
        const bool was   = chainActiveTypes_.count(t) > 0;
        if (isNow && ! was)
            setParam(moduleEnableParam(t), true);    // newly added ⇒ enable
        else if (! isNow)
            setParam(moduleEnableParam(t), false);   // absent ⇒ force off
    }

    // LuxStral engine enable (same add ⇒ on / absent ⇒ off diff semantics).
    {
        std::set<int> enginesNow;
        for (const auto& ch : chainModel_.chains)
            for (const auto& m : ch.modules)
                if (m.type == ModuleType::LuxStral && m.slot >= 0
                    && m.slot < ChainModel::kMaxLuxStralEngines)
                    enginesNow.insert(m.slot);

        const bool anyNow = ! enginesNow.empty();
        const bool anyWas = ! luxstralEngines_.empty();
        if (anyNow && ! anyWas)
            setParam("deviceEnabled", true);
        else if (! anyNow)
            setParam("deviceEnabled", false);
        luxstralEngines_ = enginesNow;
    }

    chainActiveTypes_ = now;
}

// Free the state of every module that just disappeared from the topology. The
// enable bridge (next call) silences modules that own an enable param, but some
// modules keep state the bridge cannot reach: SCORE has NO enable param (its
// has_score plan flag only routes the engine-B player feed), so removal alone
// never stops it; VideoScroll keeps a captured waterfall ring; Pitch/Mask hold live
// MIDI voices. Recorded content (sampler slots, sequencer pattern, synth params)
// is intentionally preserved — removal only tears down transient/live state.
// Runs AFTER deriveChainRouting() published the new plan (the synth thread has
// stopped pulling the removed modules) and BEFORE applyChainEnableBridge()
// overwrites chainActiveTypes_, so chainActiveTypes_ still holds the old set.
void Sp3ctraAudioProcessor::teardownAbsentModules(const std::set<ModuleType>& now)
{
    auto removed = [&](ModuleType t)
    { return chainActiveTypes_.count(t) > 0 && now.count(t) == 0; };

    // The score family shares the score-player channel: free the frame buffer
    // only when the LAST member leaves (removing SCORE must not cut a playing
    // TIMBRE/MIDI SCORE/VOICE page, and vice versa). SCORE's settings reset
    // stays SCORE-only.
    {
        bool familyRemoved = false, familyPresent = false;
        for (ModuleType t : kScoreFamily)
        {
            familyRemoved  = familyRemoved  || removed(t);
            familyPresent  = familyPresent  || now.count(t) != 0;
        }
        if (familyRemoved && ! familyPresent)
            if (auto* ls = getLuxSampler())
                ls->uiDiscardScore();
    }
    if (removed(ModuleType::Score))
    {
        score_settings_defaults(&scoreSettings_);
        scoreSettings_.writingSpeed = 2.5;   // match the constructor default
    }

    // Pitch / Mask: handled at INSTANCE granularity by deriveChainRouting() —
    // it diffs the per-slot presence masks (and chain→slot rebindings) and
    // resets exactly the pool slots that lost their module, which subsumes the
    // old type-level reset here.

    // VideoScroll: per-instance probe (slot 0..7), may repeat per chain, so diff
    // at slot granularity. Re-init the capture ring of any removed probe so its
    // waterfall image stops being displayed. We are on the message thread — the
    // same thread that consumes the ring — so this cannot race the consumer, and
    // the producer (synth thread) no longer captures to a dropped slot.
    std::set<int> vsNow;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (m.type == ModuleType::VideoScroll
                && m.slot >= 0 && m.slot < CHAIN_MAX_CHAINS)
                vsNow.insert(m.slot);
    for (int slot : videoScrollSlots_)
        if (vsNow.count(slot) == 0)
        {
            // Deferred re-init (= clear ring): the UDP/feeder thread may still
            // be capturing into this slot for the in-flight frame taken with
            // the OLD plan — memset'ing 6.3 MB under it would race the writes.
            // The processor timer runs it ≥40 ms after the publish.
            pendingVideoScrollInits_ |= (1u << slot);
            poolResetArmedMs_ = juce::Time::getMillisecondCounter();
        }
    // A slot re-armed before the deferred init fired (remove + re-add within
    // 40 ms) must keep its ring — the pending memset would wipe and race the
    // freshly active probe.
    for (int slot : vsNow)
        pendingVideoScrollInits_ &= ~(1u << slot);
    // A slot that just (re)appeared is a freshly placed output: force its enable
    // ON so a new VideoScroll block starts visible even if that slot was left
    // disabled by a previously removed instance.
    for (int slot : vsNow)
        if (videoScrollSlots_.count(slot) == 0)
            if (auto* p = apvts.getParameter(vsParam(slot, "enabled")))
                p->setValueNotifyingHost(1.0f);

    // A NEW instance (different UUID) claiming a slot must not inherit the
    // removed instance's parameter bank: existing "VS{N} …" automation lanes
    // and settings would silently drive the new module. Reset the whole
    // videoScroll{N}_* / videoMix{N}_* bank to defaults (enable is owned by
    // the forcing above). Same-UUID instances (a plain re-derive) are
    // untouched, and session loads baseline the map without resetting.
    {
        std::map<int, juce::Uuid> vsIdsNow;
        for (const auto& ch : chainModel_.chains)
            for (const auto& m : ch.modules)
                if (m.type == ModuleType::VideoScroll
                    && m.slot >= 0 && m.slot < CHAIN_MAX_CHAINS)
                    vsIdsNow[m.slot] = m.id;
        for (const auto& entry : vsIdsNow)
        {
            const auto prev = videoScrollSlotIds_.find(entry.first);
            if (prev != videoScrollSlotIds_.end() && prev->second == entry.second)
                continue;   // same instance as before
            const juce::String pfx1 = "videoScroll" + juce::String(entry.first) + "_";
            const juce::String pfx2 = "videoMix"    + juce::String(entry.first) + "_";
            for (auto* param : getParameters())
                if (auto* pw = dynamic_cast<juce::RangedAudioParameter*>(param))
                {
                    if (!pw->paramID.startsWith(pfx1) && !pw->paramID.startsWith(pfx2))
                        continue;
                    if (pw->paramID.endsWith("_enabled"))
                        continue;
                    pw->setValueNotifyingHost(pw->getDefaultValue());
                }
        }
        videoScrollSlotIds_ = std::move(vsIdsNow);
    }
    videoScrollSlots_ = vsNow;
}

bool Sp3ctraAudioProcessor::saveChainPreset(int chainIdx, const juce::File& file)
{
    if (chainIdx < 0 || chainIdx >= chainModel_.numChains())
        return false;
    snapshotBankValuesIntoModel();   // the preset carries the CURRENT settings
    const auto preset = ChainPresetIO::makePresetTree(
        chainModel_.chains[(size_t) chainIdx],
        file.getFileNameWithoutExtension());
    return ChainPresetIO::saveToFile(preset, file);
}

Sp3ctraAudioProcessor::ChainPresetLoadResult
Sp3ctraAudioProcessor::loadChainPreset(const juce::ValueTree& preset,
                                       int targetChainIdx)
{
    ChainPresetLoadResult res;
    const auto ct = preset.getChildWithName(ChainModel::kChainTag);
    if (! ct.isValid())
        return res;

    // Refresh every chain's VALUES first: the projection at the end walks the
    // whole model, and the OTHER chains must project as no-ops (their trees
    // would otherwise hold save-time values and revert live knob moves).
    snapshotBankValuesIntoModel();

    int target = targetChainIdx;
    if (target < 0)
    {
        target = chainModel_.addChain();
        if (target < 0)
            return res;   // 8-chain cap
    }
    else if (target >= chainModel_.numChains())
        return res;

    Chain& ch = chainModel_.chains[(size_t) target];

    // J5 — automation/MIDI stability: a same-type module in the preset lands
    // on the bank slot the OLD composition used, so host lanes and MIDI
    // mappings keep driving "the module of this chain".
    std::map<ModuleType, int> oldSlot;
    for (const auto& m : ch.modules)
        if (moduleParamManifest(m.type) != nullptr)
            oldSlot.try_emplace(m.type, bankSlotForModule(m));

    // "Load into": replace the content, the chain identity survives.
    ch.modules.clear();
    ch.typeMemory.clear();

    for (const auto& mt : ct)
    {
        if (mt.hasType(ChainModel::kMemoryTag))
        {
            ModuleType type;
            if (moduleTypeFromId(
                    mt.getProperty(ChainModel::kTypeProp).toString(), type))
            {
                juce::ValueTree mem(ChainModel::kValuesTag);
                mem.copyPropertiesFrom(mt, nullptr);
                mem.removeProperty(ChainModel::kTypeProp, nullptr);
                ch.typeMemory[type] = std::move(mem);
            }
            continue;
        }
        if (! mt.hasType(ChainModel::kModuleTag))
            continue;
        ModuleType type;
        const juce::String typeId =
            mt.getProperty(ChainModel::kTypeProp).toString();
        if (! moduleTypeFromId(typeId, type))
        {
            res.skipped.add(typeId);   // newer/unknown type
            continue;
        }
        const int at = (int) ch.modules.size();
        if (! chainModel_.insert(target, type, at))
        {
            // Singleton placed elsewhere / exhausted pool / duplicate — skip,
            // the rest of the preset still loads.
            res.skipped.add(moduleDisplayName(type));
            continue;
        }

        ModuleInstance& mi = ch.modules[(size_t) at];
        const auto values = mt.getChildWithName(ChainModel::kValuesTag);
        if (values.isValid())
            mi.values = values.createCopy();

        // J5 pre-seed: reuse the old composition's slot for this type.
        const auto it = oldSlot.find(type);
        if (it != oldSlot.end())
        {
            if (isPooledInsertType(type))
                modulePoolSlots_[mi.id] = { it->second, type };
            else if (ChainModel::hasSlot(type))
                mi.slot = it->second;   // collisions healed below
            oldSlot.erase(it);
        }
    }
    chainModel_.validateAndRepair();

    onChainModelEdited();          // bindings + reset/inherit + bridge + plan
    projectChainValuesToBanks();   // preset VALUES → the fresh banks
    res.chainIdx = target;
    return res;
}

int Sp3ctraAudioProcessor::duplicateChain(int chainIdx)
{
    // Fresh VALUES on every module first, so the copies carry the CURRENT
    // settings (VALUES are otherwise only refreshed at save time).
    snapshotBankValuesIntoModel();
    const int newIdx = chainModel_.duplicateChain(chainIdx);
    if (newIdx < 0)
        return -1;
    // Bindings (fresh pool slots) + new-instance reset/inherit + enable
    // bridge + plan republish…
    onChainModelEdited();
    // …then the copied VALUES override the freshly-reset banks: the duplicate
    // plays with the source chain's exact settings, fully independent.
    projectChainValuesToBanks();
    return newIdx;
}

void Sp3ctraAudioProcessor::onChainModelEdited()
{
    // Routing/masks/plan FIRST: the enable bridge below flips APVTS params which
    // trigger applyConfigurationToCore() (per-instance config sync) — that reads
    // the freshly-computed chainPitch/MaskMask_.
    deriveChainRouting();       // per-synth source routing + instance masks + RT plan

    // Per-chain settings memory + per-instance bank hygiene (reset on claim,
    // restore on re-add, enable on placement) — needs the fresh pool bindings.
    updateInsertParamMemory();

    std::set<ModuleType> now;
    chainModel_.deriveActiveTypes(now);
    teardownAbsentModules(now); // free removed modules' state (uses old chainActiveTypes_)

    applyChainEnableBridge();   // enable params + insert order (diff vs baseline)
    persistChainModel();
}

//==============================================================================
// Apply APVTS parameters to Sp3ctraCore and global C config
// needsSocketRestart: true = full reinit (UDP change), false = just update g_sp3ctra_config
void Sp3ctraAudioProcessor::applyConfigurationToCore(bool needsSocketRestart)
{
    // Read current APVTS parameters
    int udpPort = (int)udpPortParam->load();
    int dpiChoice = (int)sensorDpiParam->load();  // 0=200, 1=400
    int logLevel = (int)logLevelParam->load();
    
    // Map DPI choice to actual DPI value
    int sensorDpi = (dpiChoice == 0) ? 200 : 400;
    
    // Build UDP address from 4 bytes
    juce::String udpAddress = getUdpAddressString();
    
    // Update global C config (used by udpThread) - ALWAYS SAFE, no buffer realloc
    extern sp3ctra_config_t g_sp3ctra_config;
    g_sp3ctra_config.udp_port = udpPort;
    strncpy(g_sp3ctra_config.udp_address, udpAddress.toRawUTF8(), 
            sizeof(g_sp3ctra_config.udp_address) - 1);
    g_sp3ctra_config.udp_address[sizeof(g_sp3ctra_config.udp_address) - 1] = '\0';
    g_sp3ctra_config.sensor_dpi = sensorDpi;
    g_sp3ctra_config.log_level = (log_level_t)logLevel;
    
    // Maximum resolution: always 1 pixel = 1 oscillator (3456 oscillators).
    // The former adaptive reduction (calibrated on ~760 KB multi-table design)
    // is no longer needed: the shared sine table (4 KB, L1-resident) eliminates
    // the cache-miss bottleneck that made high oscillator counts CPU-expensive.
    g_sp3ctra_config.pixels_per_note = 1;
    log_info("VST", "Resolution: pixels_per_note=1 → %d oscillators (max, shared sine table)",
             get_cis_pixels_nb());
    
    // ========================================================================
    // Synchronize LuxStral parameters from APVTS to g_sp3ctra_config
    // ========================================================================
    
    // 🎵 Musical Frequency Calculation from Tuning + Root Note + Num Octaves
    // This eliminates "jumps" caused by dynamic octave recalculation
    float tuning = apvts.getRawParameterValue("luxstralTuning")->load();
    int rootNoteIndex = (int)apvts.getRawParameterValue("luxstralRootNote")->load();
    int numOctaves = (int)apvts.getRawParameterValue("luxstralNumOctaves")->load();
    
    // Convert ComboBox index to MIDI note number
    // Index 0 = C1 = MIDI 24, Index 12 = C2 = MIDI 36, etc.
    int rootNoteMidi = 24 + rootNoteIndex;  // C1 starts at MIDI 24
    
    // Calculate frequency from MIDI note: freq = tuning * 2^((midi - 69) / 12)
    // A4 (MIDI 69) = tuning Hz
    float lowFrequency = tuning * powf(2.0f, (float)(rootNoteMidi - 69) / 12.0f);
    float highFrequency = lowFrequency * powf(2.0f, (float)numOctaves);
    
    // Clamp high frequency to 20 kHz and Nyquist frequency
    if (highFrequency > 20000.0f) {
        highFrequency = 20000.0f;
    }
    
    // Additional safety: clamp to Nyquist frequency if sample rate is known
    extern sp3ctra_config_t g_sp3ctra_config;
    float nyquist = (float)g_sp3ctra_config.sampling_frequency * 0.5f;
    if (nyquist > 0 && highFrequency > nyquist) {
        log_warning("VST", "High frequency %.1f Hz clamped to Nyquist %.1f Hz", highFrequency, nyquist);
        highFrequency = nyquist;
    }
    
    // Store calculated frequencies
    g_sp3ctra_config.low_frequency = lowFrequency;
    g_sp3ctra_config.high_frequency = highFrequency;
    g_sp3ctra_config.start_frequency = lowFrequency;  // Backward compatibility
    
    // Store the fixed number of octaves (no more dynamic calculation!)
    g_sp3ctra_config.num_octaves = numOctaves;
    
    log_debug("VST", "Musical config: tuning=%.1f Hz, root=%d (MIDI %d), octaves=%d -> %.1f - %.1f Hz",
              tuning, rootNoteIndex, rootNoteMidi, numOctaves, lowFrequency, highFrequency);
    
    // Envelope Parameters
    g_sp3ctra_config.tau_up_base_ms = apvts.getRawParameterValue("luxstralAttackMs")->load();
    g_sp3ctra_config.tau_down_base_ms = apvts.getRawParameterValue("luxstralReleaseMs")->load();
    
    // Stereo Processing
    g_sp3ctra_config.stereo_mode_enabled = 
        (int)apvts.getRawParameterValue("luxstralStereoEnable")->load();
    g_sp3ctra_config.stereo_temperature_amplification = 
        apvts.getRawParameterValue("luxstralStereoTempAmp")->load();
    
    // Dynamics Processing
    g_sp3ctra_config.noise_gate_threshold =
        apvts.getRawParameterValue("luxstralNoiseGateThreshold")->load();
    g_sp3ctra_config.soft_limit_threshold =
        apvts.getRawParameterValue("luxstralSoftLimitThreshold")->load();
    g_sp3ctra_config.soft_limit_knee =
        apvts.getRawParameterValue("luxstralSoftLimitKnee")->load();

    // Phase management (mode + sensitivity + position + drift)
    g_sp3ctra_config.luxstral_phase_mode =
        (int)apvts.getRawParameterValue("luxstralPhaseMode")->load();
    g_sp3ctra_config.luxstral_phase_sensitivity =
        apvts.getRawParameterValue("luxstralPhaseSensitivity")->load();
    g_sp3ctra_config.luxstral_phase_position =
        apvts.getRawParameterValue("luxstralPhasePosition")->load();
    g_sp3ctra_config.luxstral_phase_drift_cents =
        apvts.getRawParameterValue("luxstralPhaseDriftCents")->load();

    // Performance
    g_sp3ctra_config.num_workers = 
        (int)apvts.getRawParameterValue("luxstralNumWorkers")->load();
    
    // Physiological Filter (Equal-Loudness Compensation on wavetables)
    g_sp3ctra_config.physiological_filter_enabled = 
        (int)apvts.getRawParameterValue("luxstralPhysiologicalFilter")->load();
    // FIX: this field was never written → depth was 0.0f (zero-init) → all gains = 1.000
    g_sp3ctra_config.physiological_correction_depth =
        apvts.getRawParameterValue("luxstralPhysiologicalDepth")->load();
    
    // ========================================================================
    // StrokeForge — Blob-to-note mapping with waveform morphing
    // Blob detection params (threshold, min width, merge gap) are now set
    // exclusively by spctrBlob* (IMAGE LUXSTRAL tab) in the block below.
    // ========================================================================
    g_sp3ctra_config.strokeforge_enabled =
        (int)apvts.getRawParameterValue("sfEnabled")->load();
    g_sp3ctra_config.strokeforge_morph_width_scale =
        apvts.getRawParameterValue("sfMorphWidthScale")->load();
    g_sp3ctra_config.strokeforge_blob_focus_sigma =
        apvts.getRawParameterValue("sfBlobFocusSigma")->load();
    g_sp3ctra_config.strokeforge_spectral_width_threshold =
        apvts.getRawParameterValue("sfSpectralWidthThreshold")->load();
    g_sp3ctra_config.strokeforge_focus_only =
        (int)apvts.getRawParameterValue("sfFocusOnly")->load();

    /* ── Image Pipeline live controls ──────────────────────────────────────── */
    /* Blob detection now runs only when StrokeForge is in use (enabled or       */
    /* focus-only) — see image_pipeline.c Stage 9 — to save CPU when unused.     */
    // ── Mix balance crossfader → derived live/sampler opacities ───────────────
    // balance=0.0 → smpOp=1.0, liveOp=0.0 (full Sampler)
    // balance=0.5 → smpOp=1.0, liveOp=1.0 (equal, full darken blend)
    // balance=1.0 → smpOp=0.0, liveOp=1.0 (full Live)
    {
        const float bal = apvts.getRawParameterValue("imageMixBalance")->load();
        const float liveOp = std::min(1.0f, 2.0f * bal);
        const float smpOp  = std::min(1.0f, 2.0f * (1.0f - bal));
        g_sp3ctra_config.image_live_opacity    = liveOp;
        g_sp3ctra_config.image_sampler_opacity = smpOp;
    }
    g_sp3ctra_config.image_freeze_mode   =
        static_cast<int>(apvts.getRawParameterValue("imageFreezeMode")->load());
    g_sp3ctra_config.image_fade_in_ms    =
        static_cast<int>(apvts.getRawParameterValue("imageFadeInMs")->load());
    g_sp3ctra_config.sampler_gamma        =
        apvts.getRawParameterValue("samplerGamma")->load();
    g_sp3ctra_config.sampler_contrast_min =
        apvts.getRawParameterValue("samplerContrastMin")->load();
    g_sp3ctra_config.sampler_freeze_mode  =
        static_cast<int>(apvts.getRawParameterValue("samplerFreezeMode")->load());
    g_sp3ctra_config.sampler_fade_in_ms   =
        static_cast<int>(apvts.getRawParameterValue("samplerFadeInMs")->load());
    g_sp3ctra_config.raw_freeze_mode      =
        static_cast<int>(apvts.getRawParameterValue("rawFreezeMode")->load());
    g_sp3ctra_config.raw_fade_in_ms       =
        static_cast<int>(apvts.getRawParameterValue("rawFadeInMs")->load());

    // ========================================================================
    // Per-path pipeline routing — source selection, inversion, AC removal
    //
    // APVTS choice indices:  0="S - Sampler", 1="M - Mix", 2="L - Live", 3="P - LuxPitch"
    // ImageSourceType enum:  SAMPLER=0, LIVE=1, MIX=2, LUXPITCH=3
    // Mapping table: choice → enum
    // ========================================================================
    {
        // Routing: the ChainPlan is the single authority (M7/M8) — an engine
        // is fed by the OUT modules placed in chains; the per-OUT banks below
        // are the only conditioning.

        // M4 — core-side LuxSynth engine feed (luxsynth_feed_tick): FFT bins
        // choice + temporal smoothing, mirrored from the APVTS params the UI
        // FFT view uses (same values → view matches what the engine hears).
        g_sp3ctra_config.lx_fft_bins_choice =
            static_cast<int>(apvts.getRawParameterValue("lxFftBins")->load());
        g_sp3ctra_config.lx_fft_smoothing =
            apvts.getRawParameterValue("lxFftSmoothing")->load();

        // ── Synth-split P1 — per-OUT conditioning banks → g_sp3ctra_config ──
        // contrast_min / range_db are LuxStral-only.
        for (int s = 0; s < LUX_OUT_MAX_SLOTS; ++s)
        {
            auto rawf = [this](const juce::String& id) {
                auto* v = apvts.getRawParameterValue(id);
                return v ? v->load() : 0.0f;
            };

            lux_out_params_t* ls = &g_sp3ctra_config.luxstral_out[s];
            ls->negative     = (int)rawf(lsOutParam(s, "negative"));
            ls->dc_blocking  = (int)rawf(lsOutParam(s, "dcBlocking"));
            ls->gamma        = rawf(lsOutParam(s, "gamma"));
            ls->contrast_min = rawf(lsOutParam(s, "contrastMin"));
            ls->range_db     = rawf(lsOutParam(s, "rangeDb"));
            ls->intensity    = rawf(lsOutParam(s, "intensity"));
            ls->enabled      = (int)rawf(lsOutParam(s, "enabled"));

            lux_out_params_t* lx = &g_sp3ctra_config.luxsynth_out[s];
            lx->negative    = (int)rawf(lxOutParam(s, "negative"));
            lx->dc_blocking = (int)rawf(lxOutParam(s, "dcBlocking"));
            lx->gamma       = rawf(lxOutParam(s, "gamma"));
            lx->intensity   = rawf(lxOutParam(s, "intensity"));
            lx->enabled     = (int)rawf(lxOutParam(s, "enabled"));

            lux_out_params_t* lw = &g_sp3ctra_config.luxwave_out[s];
            lw->negative    = (int)rawf(lwOutParam(s, "negative"));
            lw->dc_blocking = (int)rawf(lwOutParam(s, "dcBlocking"));
            lw->gamma       = rawf(lwOutParam(s, "gamma"));
            lw->intensity   = rawf(lwOutParam(s, "intensity"));
            lw->enabled     = (int)rawf(lwOutParam(s, "enabled"));
        }

        // ── Insert chain order (M1 — modular pipeline core) ──
        // (P4-M3) The GLOBAL insert order is gone — per-chain order comes
        // from each chain's own recipe (the model), "chainInsertOrder" is a
        // host-visible projection only.

        // ── Sync LuxPitch configs — one APVTS bank per pool instance ──
        // Slot i reads luxpitch{i}_* : two Pitch modules on two chains carry
        // fully independent settings. `enabled` stays gated on presence
        // (mask bit i) so an unplaced slot never processes a chain's image.
        {
            const uint32_t pmask = chainPitchMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(lpParam(i, sfx))->load(); };

                LuxPitchConfig c;
                c.enabled                  = static_cast<int>(raw("Enabled"));
                c.polyphony_enabled        = static_cast<int>(raw("Polyphony"));
                c.background_mode          = static_cast<int>(raw("BackgroundMode"));
                c.coupling_mode            = static_cast<int>(raw("CouplingMode"));
                c.free_pixels_per_semitone = raw("FreePixelsPerST");
                c.pitch_bend_range         = raw("PitchBendRange");
                c.attack_ms                = raw("AttackMs");
                c.decay_ms                 = raw("DecayMs");
                c.sustain_level            = raw("SustainLevel");
                c.release_ms               = raw("ReleaseMs");
                c.attack_curve             = raw("AttackCurve");
                c.decay_curve              = raw("DecayCurve");
                c.release_curve            = raw("ReleaseCurve");
                c.glide_time_ms            = raw("GlideMs");
                c.lfo_rate_hz              = raw("LfoRate");
                c.lfo_depth_semitones      = raw("LfoDepth");
                c.velocity_coupling        = static_cast<int>(raw("VelocityCoupling"));
                c.reference_note           = 24 + static_cast<int>(raw("ReferenceNote"));
                if (((pmask >> i) & 1u) == 0) c.enabled = 0;
                lux_pitch_instance(i)->config = c;
            }
        }

        // ── Sync LuxMask configs — one APVTS bank per pool instance ──
        {
            const uint32_t mmask = chainMaskMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(lmParam(i, sfx))->load(); };

                LuxMaskConfig c;
                c.enabled                  = static_cast<int>(raw("Enabled"));
                c.polyphony_enabled        = static_cast<int>(raw("Polyphony"));
                c.background_mode          = static_cast<int>(raw("BackgroundMode"));
                c.coupling_mode            = static_cast<int>(raw("CouplingMode"));
                c.free_pixels_per_semitone = raw("FreePixelsPerST");
                c.pitch_bend_range         = raw("PitchBendRange");
                c.filter_width_pct         = raw("FilterWidth");
                c.filter_offset_pct        = raw("FilterOffset");
                c.filter_slope             = raw("FilterSlope");
                c.attack_ms                = raw("AttackMs");
                c.decay_ms                 = raw("DecayMs");
                c.sustain_level            = raw("SustainLevel");
                c.release_ms               = raw("ReleaseMs");
                c.attack_curve             = raw("AttackCurve");
                c.decay_curve              = raw("DecayCurve");
                c.release_curve            = raw("ReleaseCurve");
                c.glide_time_ms            = raw("GlideMs");
                c.lfo_pos_rate_hz          = raw("LfoPosRate");
                c.lfo_pos_depth_semitones  = raw("LfoPosDepth");
                c.velocity_coupling        = static_cast<int>(raw("VelocityCoupling"));
                c.reference_note           = 24 + static_cast<int>(raw("ReferenceNote"));
                if (((mmask >> i) & 1u) == 0) c.enabled = 0;
                lux_mask_instance(i)->config = c;
            }
        }

        // ── Sync LuxReverb configs — one APVTS bank per pool instance ──
        {
            // Choice order is {Auto, Black, White} — map onto the C-side modes.
            static const int kRvBgChoiceToMode[3] =
                { LUX_REVERB_BG_AUTO, LUX_REVERB_BG_BLACK, LUX_REVERB_BG_WHITE };
            const uint32_t rmask = chainReverbMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(rvParam(i, sfx))->load(); };

                int bgChoice = static_cast<int>(raw("BackgroundMode"));
                if (bgChoice < 0 || bgChoice > 2) bgChoice = 0;

                LuxReverbConfig c  = lux_reverb_config_default();
                c.enabled          = static_cast<int>(raw("Enabled"));
                c.background_mode  = kRvBgChoiceToMode[bgChoice];
                c.decay_s          = raw("Decay");
                c.diffusion        = raw("Diffusion") / 100.0f;
                c.mix              = raw("Mix") / 100.0f;
                if (((rmask >> i) & 1u) == 0) c.enabled = 0;
                lux_reverb_instance(i)->config = c;
            }
        }

        // ── Sync LuxEcho configs — one APVTS bank per pool instance ──
        {
            // Choice order is {Auto, Black, White} — map onto the C-side modes.
            static const int kEcBgChoiceToMode[3] =
                { LUX_ECHO_BG_AUTO, LUX_ECHO_BG_BLACK, LUX_ECHO_BG_WHITE };
            const uint32_t emask = chainEchoMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(ecParam(i, sfx))->load(); };

                int bgChoice = static_cast<int>(raw("BackgroundMode"));
                if (bgChoice < 0 || bgChoice > 2) bgChoice = 0;

                LuxEchoConfig c   = lux_echo_config_default();
                c.enabled         = static_cast<int>(raw("Enabled"));
                c.background_mode = kEcBgChoiceToMode[bgChoice];
                c.delay_lines     = static_cast<int>(raw("Delay"));
                c.feedback        = raw("Feedback") / 100.0f;
                c.mix             = raw("Mix") / 100.0f;
                if (((emask >> i) & 1u) == 0) c.enabled = 0;
                lux_echo_instance(i)->config = c;
            }
        }

        // ── Sync LuxEq configs — one APVTS bank per pool instance ──
        {
            // Choice order is {Auto, Black, White} — map onto the C-side modes.
            static const int kEqBgChoiceToMode[3] =
                { LUX_EQ_BG_AUTO, LUX_EQ_BG_BLACK, LUX_EQ_BG_WHITE };
            const uint32_t qmask = chainEqMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(eqParam(i, sfx))->load(); };

                int bgChoice = static_cast<int>(raw("BackgroundMode"));
                if (bgChoice < 0 || bgChoice > 2) bgChoice = 0;

                LuxEqConfig c     = lux_eq_config_default();
                c.enabled         = static_cast<int>(raw("Enabled"));
                c.background_mode = kEqBgChoiceToMode[bgChoice];
                for (int b = 0; b < LUX_EQ_NUM_BANDS; ++b)
                    c.band_gain_db[b] =
                        raw(("Band" + juce::String(b)).toRawUTF8());
                if (((qmask >> i) & 1u) == 0) c.enabled = 0;
                lux_eq_instance(i)->config = c;
            }
        }

    }

    // ========================================================================
    // LuxSynth blob detection — FULLY ISOLATED from StrokeForge (lxBlob*)
    // These fields feed detectSynthBlobs() in CisVisualizerComponent ONLY.
    // They have zero effect on LuxStral audio synthesis.
    // ========================================================================
    g_sp3ctra_config.luxsynth_blob_threshold  =
        apvts.getRawParameterValue("lxBlobThreshold")->load();
    g_sp3ctra_config.luxsynth_blob_min_width  =
        static_cast<int>(apvts.getRawParameterValue("lxBlobMinWidth")->load());
    g_sp3ctra_config.luxsynth_blob_merge_gap  =
        static_cast<int>(apvts.getRawParameterValue("lxBlobMergeGap")->load());
    g_sp3ctra_config.luxsynth_blob_color_split =
        apvts.getRawParameterValue("lxBlobColorSplit")->load();

    // ========================================================================
    // SPCTR blob detection — IMAGE LUXSTRAL tab (spctrBlob* params)
    // Single source of truth for detectSpctrBlobs() + StrokeForge audio.
    // spctrBlobColorSplit is visualizer-only (no StrokeForge audio equivalent).
    // ========================================================================
    g_sp3ctra_config.strokeforge_blob_base_threshold =
        apvts.getRawParameterValue("spctrBlobThreshold")->load();
    g_sp3ctra_config.strokeforge_blob_min_width =
        static_cast<int>(apvts.getRawParameterValue("spctrBlobMinWidth")->load());
    g_sp3ctra_config.strokeforge_blob_merge_gap =
        static_cast<int>(apvts.getRawParameterValue("spctrBlobMergeGap")->load());

    // Update logger level immediately
    logger_init((log_level_t)logLevel);
    
    // In the shared-core design, the needsSocketRestart path is handled by
    // sharedCore->startWithConfig() (first time) or sharedCore->restartUdp()
    // (hot-reload). applyConfigurationToCore() is now a pure config updater.
    juce::ignoreUnused(needsSocketRestart);
    log_debug("VST", "Config updated — %d DPI, log level %d", sensorDpi, logLevel);
}

//==============================================================================
// UDP Batch Update API Implementation
void Sp3ctraAudioProcessor::beginUdpBatchUpdate()
{
    udpBatchUpdateActive.store(true);
    udpNeedsRestart.store(false);
    log_debug("VST", "UDP batch update started");
}

void Sp3ctraAudioProcessor::endUdpBatchUpdate()
{
    udpBatchUpdateActive.store(false);

    int newPort = static_cast<int>(udpPortParam->load());
    juce::String newAddress = getUdpAddressString();

    log_info("VST", "UDP batch update — restarting shared socket → %s:%d",
             newAddress.toRawUTF8(), newPort);

    applyConfigurationToCore(false);  // update g_sp3ctra_config

    if (sharedCore && sharedCore->isReady())
    {
        if (!sharedCore->restartUdp(newPort, newAddress.toStdString(), ""))
            log_error("VST", "Failed to restart UDP after batch update!");
        else
            log_info("VST", "UDP restarted → %s:%d", newAddress.toRawUTF8(), newPort);
    }

    udpNeedsRestart.store(false);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Sp3ctraAudioProcessor();
}
