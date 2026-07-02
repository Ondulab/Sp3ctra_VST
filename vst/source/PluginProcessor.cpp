#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>   // SCORE source-audio preview

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
    #include "processing/video_scroll.h"                      // VideoScroll capture-ring pool
    #include "processing/image_chain.h"                       // Insert chain executor (order + taps)
    #include "processing/chain_plan.h"                         // M6 Phase 2 — RT chain descriptor
    #include "synthesis/luxsynth/luxsynth_vst_adapter.h"      // luxsynth_push_midi_event(), buffers, engine
    #include "synthesis/luxwave/luxwave_vst_adapter.h"        // luxwave_push_midi_event(), g_luxwave_engine

    // M8 — engine B envelope hot-reload (declared in luxstral_engine.h, whose
    // full include drags the worker-pool types into this TU; prototype suffices).
    void synth_luxstral_update_engine_b_envelope(void);
}
// Note: synth_luxstral_threading.h / synth_luxstral_runtime.h / AudioProcessingThread.h
// are now included transitively via Sp3ctraSharedCore.h and handled by Sp3ctraSharedCore.

// Global RT Profiler accessible from C threads (audioProcessingThread)
// This must be declared here (not in header) to avoid multiple definition errors
RTProfiler g_vst_rt_profiler = {};

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

    // ── Infrastructure — Image pipeline flags ────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralInvertIntensity", 1}, "Invert Intensity",
        true, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralGammaEnable", 1}, "Gamma Enable",
        true, kHiddenBool));

    // ── Gameplay — Gamma / Contrast ──────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralGammaValue", 1}, "Gamma",
        juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralContrastMin", 1}, "Contrast Min",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.21f));

    // ── Gameplay — Stereo enable (PLAY-page badge toggle) ────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralStereoEnable", 1}, "Stereo Enable",
        true));

    // ── Gameplay — Stereo temperature ────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralStereoTempAmp", 1}, "Stereo Temp.",
        juce::NormalisableRange<float>(0.0f, 5.0f, 0.01f), 2.5f));

    // ── Infrastructure — Volume weighting ────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralVolumeWeightingExp", 1}, "Vol. Weight Exp.",
        juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f), 0.1f, kHiddenFloat));

    // ── Gameplay — Summation exponent / Noise gate ───────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralSummationResponseExp", 1}, "Sum. Exp.",
        juce::NormalisableRange<float>(2.0f, 10.0f, 0.1f), 2.0f));
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
    // M8 — dual-engine: independent gain for the 2nd LuxStral engine (B).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralBVolume", 1}, "LuxStral B Vol.",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthVolume", 1}, "LuxSynth Vol.",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    // ── Gameplay — Device On ─────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"deviceEnabled", 1}, "Device On", true));
    // M8 — independent enable for the 2nd LuxStral engine (B).
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralBEnabled", 1}, "LuxStral B On", true));

    // ── Setup — Soft limiter (LuxStral A) ────────────────────────────────────
    // These IDs were referenced by LuxStralSetupPanel but never created — the
    // sliders were silently inert (JUCE skips attachments on unknown IDs).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralSoftLimitThreshold", 1}, "Soft Limit Thr.",
        juce::NormalisableRange<float>(0.1f, 1.0f, 0.01f), 0.8f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralSoftLimitKnee", 1}, "Soft Limit Knee",
        juce::NormalisableRange<float>(0.01f, 1.0f, 0.01f), 0.2f, kHiddenFloat));

    // ── M8 — LuxStral engine B: independent PLAY/SETUP parameter set ─────────
    // Mirrors of the engine-A knobs bound by LuxStralTabComponent /
    // LuxStralSetupPanel when the rack selects the B instance (slot 1).
    // Tuning / octaves / physiological filter stay SHARED (B clones A's
    // oscillator table — v1); StrokeForge settings are shared too.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralBInversion", 1}, "B Inversion", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralBAcRemoval", 1}, "B DC Blocking", true));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralBGammaValue", 1}, "B Gamma",
        juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralBContrastMin", 1}, "B Contrast Min",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.21f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralBAttackMs", 1}, "B Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 0.5f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralBReleaseMs", 1}, "B Release",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 0.5f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralBSummationResponseExp", 1}, "B Sum. Exp.",
        juce::NormalisableRange<float>(2.0f, 10.0f, 0.1f), 2.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralBNoiseGateThreshold", 1}, "B Noise Gate",
        juce::NormalisableRange<float>(0.0f, 0.1f, 0.001f), 0.005f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralBStereoEnable", 1}, "B Stereo Enable",
        true));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralBStereoTempAmp", 1}, "B Stereo Temp.",
        juce::NormalisableRange<float>(0.0f, 5.0f, 0.01f), 2.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralBSoftLimitThreshold", 1}, "B Soft Limit Thr.",
        juce::NormalisableRange<float>(0.1f, 1.0f, 0.01f), 0.8f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralBSoftLimitKnee", 1}, "B Soft Limit Knee",
        juce::NormalisableRange<float>(0.01f, 1.0f, 0.01f), 0.2f, kHiddenFloat));

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

    // ── Pipeline routing — per-path source selection & toggles ────────────────
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxstralSource", 1}, "LuxStral Source",
        juce::StringArray{"S - Sampler", "M - Mix", "L - Live", "P - LuxPitch", "K - LuxMask"}, 1));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralInversion", 1}, "LuxStral Inversion", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralAcRemoval", 1}, "LuxStral DC Blocking", true));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxsynthSource", 1}, "LuxSynth Source",
        juce::StringArray{"S - Sampler", "M - Mix", "L - Live", "P - LuxPitch", "K - LuxMask"}, 1));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxsynthInversion", 1}, "LuxSynth Inversion", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxsynthAcRemoval", 1}, "LuxSynth DC Blocking", true));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthGammaValue", 1}, "LuxSynth Gamma",
        juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));

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

    // Spectral
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthGamma", 1}, "LS Gamma",
        juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
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

    // ── LuxPitch Parameters ───────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxpitchEnabled", 1}, "LuxPitch Enabled", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxpitchPolyphony", 1}, "LuxPitch Polyphony", false));
    // Insert chain order (M1 — modular pipeline core): which insert runs
    // first inside the Modulated channel.
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"chainInsertOrder", 1}, "Chain Insert Order",
        juce::StringArray{"Pitch > Mask", "Mask > Pitch"}, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxpitchBackgroundMode", 1}, "LuxPitch Background",
        juce::StringArray{"Black", "White"}, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxpitchCouplingMode", 1}, "LuxPitch Coupling",
        juce::StringArray{"LuxStral", "Free"}, 0, kHiddenChoice));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchFreePixelsPerST", 1}, "LP px/semitone",
        juce::NormalisableRange<float>(1.0f, 200.0f, 0.5f), 36.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchPitchBendRange", 1}, "LP PB Range",
        juce::NormalisableRange<float>(0.0f, 24.0f, 0.5f), 2.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("st")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchAttackMs", 1}, "LP Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 10.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchDecayMs", 1}, "LP Decay",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 50.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchSustainLevel", 1}, "LP Sustain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchReleaseMs", 1}, "LP Release",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    // Per-segment curvature [-1,1] (0 = linear). Set visually by bending each
    // envelope segment; MIDI-mappable like every other param.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchAttackCurve", 1}, "LP Attack Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchDecayCurve", 1}, "LP Decay Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchReleaseCurve", 1}, "LP Release Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchGlideMs", 1}, "LP Glide",
        juce::NormalisableRange<float>(0.0f, 5000.0f, 1.0f, 0.3f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchLfoRate", 1}, "LP LFO Rate",
        juce::NormalisableRange<float>(0.0f, 20.0f, 0.01f), 5.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxpitchLfoDepth", 1}, "LP LFO Depth",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("st")));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxpitchVelocityCoupling", 1}, "LP Velocity", false));
    // LuxPitch source selector (S/M/L — cannot take itself)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxpitchSource", 1}, "LuxPitch Source",
        juce::StringArray{"S - Sampler", "M - Mix", "L - Live"}, 1));
    // LuxPitch MIDI infrastructure (settings tab)
    {
        juce::StringArray lpMidiChNames;
        for (int i = 1; i <= 16; ++i)
            lpMidiChNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxpitchMidiChannel", 1}, "LuxPitch MIDI Channel",
            lpMidiChNames, 0, kHiddenChoice));
    }
    {
        juce::StringArray octNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxpitchOctaveOffset", 1}, "LuxPitch Octave Offset",
            octNames, 2, kHiddenChoice));
    }
    {
        // Reference note C1..B6 (72 items), default A3 = index 33
        juce::StringArray lpNoteNames;
        const char* lpNoteLetters[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        for (int oct = 1; oct <= 6; ++oct)
            for (int n = 0; n < 12; ++n)
                lpNoteNames.add(juce::String(lpNoteLetters[n]) + juce::String(oct));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxpitchReferenceNote", 1}, "LuxPitch Reference Note",
            lpNoteNames, 33, kHiddenChoice));  // A3 = index 33
    }

    // ── LuxMask Parameters ────────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxmaskEnabled", 1}, "LuxMask Enabled", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxmaskPolyphony", 1}, "LuxMask Polyphony", false));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxmaskBackgroundMode", 1}, "LuxMask Background",
        juce::StringArray{"Black", "White"}, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxmaskCouplingMode", 1}, "LuxMask Coupling",
        juce::StringArray{"LuxStral", "Free"}, 0, kHiddenChoice));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskFreePixelsPerST", 1}, "LM px/semitone",
        juce::NormalisableRange<float>(1.0f, 200.0f, 0.5f), 36.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskPitchBendRange", 1}, "LM PB Range",
        juce::NormalisableRange<float>(0.0f, 24.0f, 0.5f), 2.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("st")));

    // ── Spatial bandpass filter driven by the ADSR ──────────────────────────
    // Always a bandpass centred on the played note (keyboard tracking).  The
    // ADSR output is the openness (0 = closed to nothing, 1 = full width).
    //   Width : band width at full open, % of image.
    //   Offset: band-centre offset from the note, % of image, decoupled from
    //           width.  Openness-scaled, so the band sweeps from the note out to
    //           the offset as the ADSR opens (glide-like attack).
    //   Slope : edge steepness (1 = sharp, 0 = soft).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskFilterWidth", 1}, "LM Filter Width",
        juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 30.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskFilterOffset", 1}, "LM Filter Offset",
        juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("%")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskFilterSlope", 1}, "LM Filter Slope",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));

    // ADSR (drives the filter cutoff/openness)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskAttackMs", 1}, "LM Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 20.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskDecayMs", 1}, "LM Decay",
        juce::NormalisableRange<float>(0.5f, 10000.0f, 0.1f, 0.3f), 120.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskSustainLevel", 1}, "LM Sustain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskReleaseMs", 1}, "LM Release",
        juce::NormalisableRange<float>(0.5f, 10000.0f, 0.1f, 0.3f), 200.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    // Per-segment curvature [-1,1] (0 = linear). Set visually by bending each
    // alpha-envelope segment; MIDI-mappable like every other param.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskAttackCurve", 1}, "LM Attack Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskDecayCurve", 1}, "LM Decay Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskReleaseCurve", 1}, "LM Release Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.5f));
    // Glide
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskGlideMs", 1}, "LM Glide",
        juce::NormalisableRange<float>(0.0f, 5000.0f, 1.0f, 0.3f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // Position LFO only (vibrato).  The width LFO has been removed because a
    // periodic wobble on width never sounded musical — width is now entirely
    // gesture-driven by the velocity / release bloom envelopes above.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskLfoPosRate", 1}, "LM LFO Pos Rate",
        juce::NormalisableRange<float>(0.0f, 30.0f, 0.01f), 5.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxmaskLfoPosDepth", 1}, "LM LFO Pos Depth",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("st")));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxmaskVelocityCoupling", 1}, "LM Velocity", false));

    // LuxMask source selector (S/M/L)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxmaskSource", 1}, "LuxMask Source",
        juce::StringArray{"S - Sampler", "M - Mix", "L - Live"}, 1));

    // LuxMask MIDI infrastructure (settings tab)
    {
        juce::StringArray lmMidiChNames;
        for (int i = 1; i <= 16; ++i)
            lmMidiChNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxmaskMidiChannel", 1}, "LuxMask MIDI Channel",
            lmMidiChNames, 0, kHiddenChoice));
    }
    {
        juce::StringArray octNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxmaskOctaveOffset", 1}, "LuxMask Octave Offset",
            octNames, 2, kHiddenChoice));
    }
    {
        // Reference note C1..B6 (72 items), default A3 = index 33
        juce::StringArray lmNoteNames;
        const char* lmNoteLetters[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        for (int oct = 1; oct <= 6; ++oct)
            for (int n = 0; n < 12; ++n)
                lmNoteNames.add(juce::String(lmNoteLetters[n]) + juce::String(oct));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxmaskReferenceNote", 1}, "LuxMask Reference Note",
            lmNoteNames, 33, kHiddenChoice));  // A3 = index 33
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
        juce::NormalisableRange<float>(0.0f, 2000.0f, 10.0f), 100.0f,
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

    // ── LuxSampler action button MIDI bindings (REC / PLAY / SAVE) ───────────
    // Type: 0 = Off (disabled), 1 = Note, 2 = CC
    // Number: 0..127 MIDI note number or CC controller index.
    // Triggering rule:
    //   - Note: triggers on NoteOn (velocity > 0).
    //   - CC:   triggers on CC value >= 64 (rising edge).
    {
        juce::StringArray bindTypeNames { "Off", "Note", "CC" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerRecBindType", 1},
            "LuxSampler REC Bind Type", bindTypeNames, 0, kHiddenChoice));
        params.push_back(std::make_unique<juce::AudioParameterInt>(
            juce::ParameterID{"luxSamplerRecBindNum",  1},
            "LuxSampler REC Bind Number", 0, 127, 0, kHiddenInt));

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerPlayBindType", 1},
            "LuxSampler PLAY Bind Type", bindTypeNames, 0, kHiddenChoice));
        params.push_back(std::make_unique<juce::AudioParameterInt>(
            juce::ParameterID{"luxSamplerPlayBindNum",  1},
            "LuxSampler PLAY Bind Number", 0, 127, 0, kHiddenInt));

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerSaveBindType", 1},
            "LuxSampler SAVE Bind Type", bindTypeNames, 0, kHiddenChoice));
        params.push_back(std::make_unique<juce::AudioParameterInt>(
            juce::ParameterID{"luxSamplerSaveBindNum",  1},
            "LuxSampler SAVE Bind Number", 0, 127, 0, kHiddenInt));
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
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"seqNumSteps", 1}, "Seq Steps", 1, 32, 16));
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

    // ── Video Scroll — master toggle + live controls ───────────────────────────
    // Hidden from DAW automation (configuration/display parameters).
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoScrollEnabled", 1}, "Video Scroll Enabled",
        false, kHiddenBool));

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

    // ── Video: live performance params ────────────────────────────────────────
    // Source = which synth engine's input image we visualize. We follow the
    // engine's own source routing (Sampler/Live/Mix/LuxPitch/LuxMask) so the
    // waterfall always matches what the audio engine actually sees.
    //   0 = LuxStral          (follows luxstral_source_type)
    //   1 = LuxSynth/LuxWave  (follows luxsynth_source_type — LuxWave shares it)
    //   2 = AllSynth          (50/50 blend of the two streams above)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollSource", 1}, "Video Scroll Source",
        juce::StringArray{"LuxStral", "LuxSynth/LuxWave", "AllSynth"}, 0));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollDirection", 1}, "Video Scroll Direction",
        juce::StringArray{"Forward", "Reverse"}, 0));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollExposure", 1}, "Video Exposure",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollBlendMode", 1}, "Video Blend Mode",
        juce::StringArray{"Mix", "Add", "Screen", "Mask"}, 0));

    // ── Video: configuration (Settings tab) ───────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollBpm", 1}, "Video Scroll BPM",
        juce::NormalisableRange<float>(40.0f, 240.0f, 0.1f), 120.0f, kHiddenFloat));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoScrollMidiSync", 1}, "Video MIDI Sync",
        false, kHiddenBool));

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
    apvts.addParameterListener("luxstralInvertIntensity", this);
    apvts.addParameterListener("luxstralGammaEnable", this);
    apvts.addParameterListener("luxstralGammaValue", this);
    apvts.addParameterListener("luxstralContrastMin", this);
    apvts.addParameterListener("luxstralStereoEnable", this);
    apvts.addParameterListener("luxstralStereoTempAmp", this);
    apvts.addParameterListener("luxstralVolumeWeightingExp", this);
    apvts.addParameterListener("luxstralSummationResponseExp", this);
    apvts.addParameterListener("luxstralNoiseGateThreshold", this);
    apvts.addParameterListener("luxstralNumWorkers", this);
    apvts.addParameterListener("luxstralPhysiologicalFilter", this);
    apvts.addParameterListener("luxstralPhysiologicalDepth", this);
    apvts.addParameterListener("luxstralSoftLimitThreshold", this);
    apvts.addParameterListener("luxstralSoftLimitKnee", this);
    // M8 — LuxStral engine B parameter set (independent PLAY/SETUP)
    apvts.addParameterListener("luxstralBInversion", this);
    apvts.addParameterListener("luxstralBAcRemoval", this);
    apvts.addParameterListener("luxstralBGammaValue", this);
    apvts.addParameterListener("luxstralBContrastMin", this);
    apvts.addParameterListener("luxstralBAttackMs", this);
    apvts.addParameterListener("luxstralBReleaseMs", this);
    apvts.addParameterListener("luxstralBSummationResponseExp", this);
    apvts.addParameterListener("luxstralBNoiseGateThreshold", this);
    apvts.addParameterListener("luxstralBStereoEnable", this);
    apvts.addParameterListener("luxstralBStereoTempAmp", this);
    apvts.addParameterListener("luxstralBSoftLimitThreshold", this);
    apvts.addParameterListener("luxstralBSoftLimitKnee", this);
    
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

    // Per-path pipeline routing (source selector, inversion, AC removal)
    apvts.addParameterListener("luxstralSource",       this);
    apvts.addParameterListener("luxstralInversion",    this);
    apvts.addParameterListener("luxstralAcRemoval",    this);
    apvts.addParameterListener("luxsynthSource",       this);
    apvts.addParameterListener("luxsynthInversion",    this);
    apvts.addParameterListener("luxsynthAcRemoval",    this);
    apvts.addParameterListener("luxsynthGammaValue",   this);

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

    // LuxPitch parameter listeners
    apvts.addParameterListener("luxpitchEnabled",          this);
    apvts.addParameterListener("luxpitchPolyphony",        this);
    apvts.addParameterListener("luxpitchBackgroundMode",   this);
    apvts.addParameterListener("luxpitchCouplingMode",     this);
    apvts.addParameterListener("luxpitchFreePixelsPerST",  this);
    apvts.addParameterListener("luxpitchPitchBendRange",   this);
    apvts.addParameterListener("luxpitchAttackMs",         this);
    apvts.addParameterListener("luxpitchDecayMs",          this);
    apvts.addParameterListener("luxpitchSustainLevel",     this);
    apvts.addParameterListener("luxpitchReleaseMs",        this);
    apvts.addParameterListener("luxpitchAttackCurve",      this);
    apvts.addParameterListener("luxpitchDecayCurve",       this);
    apvts.addParameterListener("luxpitchReleaseCurve",     this);
    apvts.addParameterListener("luxpitchGlideMs",          this);
    apvts.addParameterListener("luxpitchLfoRate",          this);
    apvts.addParameterListener("luxpitchLfoDepth",         this);
    apvts.addParameterListener("luxpitchVelocityCoupling", this);
    apvts.addParameterListener("luxpitchSource",           this);
    apvts.addParameterListener("luxpitchMidiChannel",      this);
    apvts.addParameterListener("luxpitchOctaveOffset",     this);
    apvts.addParameterListener("luxpitchReferenceNote",    this);

    // LuxMask parameter listeners
    apvts.addParameterListener("luxmaskEnabled",           this);
    apvts.addParameterListener("luxmaskPolyphony",         this);
    apvts.addParameterListener("luxmaskBackgroundMode",    this);
    apvts.addParameterListener("luxmaskCouplingMode",      this);
    apvts.addParameterListener("luxmaskFreePixelsPerST",   this);
    apvts.addParameterListener("luxmaskPitchBendRange",    this);
    apvts.addParameterListener("luxmaskFilterWidth",       this);
    apvts.addParameterListener("luxmaskFilterOffset",      this);
    apvts.addParameterListener("luxmaskFilterSlope",       this);
    apvts.addParameterListener("luxmaskAttackMs",          this);
    apvts.addParameterListener("luxmaskDecayMs",           this);
    apvts.addParameterListener("luxmaskSustainLevel",      this);
    apvts.addParameterListener("luxmaskReleaseMs",         this);
    apvts.addParameterListener("luxmaskAttackCurve",       this);
    apvts.addParameterListener("luxmaskDecayCurve",        this);
    apvts.addParameterListener("luxmaskReleaseCurve",      this);
    apvts.addParameterListener("luxmaskGlideMs",           this);
    apvts.addParameterListener("luxmaskLfoPosRate",        this);
    apvts.addParameterListener("luxmaskLfoPosDepth",       this);
    apvts.addParameterListener("luxmaskVelocityCoupling",  this);
    apvts.addParameterListener("luxmaskSource",            this);
    apvts.addParameterListener("luxmaskMidiChannel",       this);
    apvts.addParameterListener("luxmaskOctaveOffset",      this);
    apvts.addParameterListener("luxmaskReferenceNote",     this);

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

    // Register LuxSampler parameter listeners
    apvts.addParameterListener(PARAM_FS_ENABLED,    this);
    apvts.addParameterListener(PARAM_FS_MIDI_CH,    this);
    apvts.addParameterListener(PARAM_FS_OCT_OFFSET, this);
    apvts.addParameterListener(PARAM_FS_MAX_DUR,    this);

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

    // Engine B: mirror the shared settings for now (per-B APVTS params land in
    // Part B). Distinct MIDI channel so direct MIDI doesn't double-trigger.
    // Per-engine enable is set authoritatively by deriveChainRouting().
    if (luxSamplerB)
    {
        luxSamplerB->setOctaveOffset(
            static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_OCT_OFFSET)) - 2);
        luxSamplerB->setMaxDuration(*apvts.getRawParameterValue(PARAM_FS_MAX_DUR));
        luxSamplerB->setMidiChannel(2);
    }

    // ── Acquire the process-wide shared core ─────────────────────────────────
    // If this is the FIRST plugin instance in this DAW process → creates the
    // singleton (UDP socket, image pipeline, synthesis engine not yet started).
    // If a SECOND instance is being created → returns the existing singleton.
    // The shared_ptr keeps the singleton alive for this instance's lifetime.
    sharedCore = Sp3ctraSharedCore::acquire();
    
    // 🔧 LAZY INITIALIZATION: Do NOT start the shared pipeline here.
    // The DAW calls setStateInformation() with saved parameters BEFORE prepareToPlay().
    // We defer startWithConfig() to prepareToPlay() so we have the correct
    // sample rate and buffer size when initializing LuxStral.
    
    // Initialize the LuxPitch / LuxMask processing instances.
    // Since the single-snapshot refactor (M2) there is ONE simulation per
    // insert: the synthesis-thread instance.  Visualizers read the published
    // insert taps (audio_image_buffers_get_insert_tap_pointers) instead of
    // re-simulating.
    // M6 Phase 2 — init the whole per-chain instance pool (slot 0 == legacy).
    lux_pitch_init_all();
    lux_mask_init_all();
    video_scroll_init_all();   // init 8 VideoScroll capture rings (RT pool) before the synth thread starts

    // Just update g_sp3ctra_config with current APVTS defaults (no socket/buffer creation)
    applyConfigurationToCore(false);

    // M6 Phase 2 — build the default chain topology and derive routing. A saved
    // session reloads it later in setStateInformation().
    loadChainModelFromState();

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

    // ── LuxSampler FIRST (uses AudioImageBuffers / DoubleBuffer owned by sharedCore) ──
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
void Sp3ctraAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // 🛡️ PROTECTION: Suspend visualizer to prevent Metal/CoreGraphics race
    if (auto* editor = dynamic_cast<Sp3ctraAudioProcessorEditor*>(getActiveEditor()))
        editor->suspendVisualizer();

    log_info("VST", "=============================================================");
    log_info("VST", "prepareToPlay - SR=%.1f Hz, BS=%d samples", sampleRate, samplesPerBlock);

    // ── Update global audio parameters (needed by startWithConfig) ───────────
    extern sp3ctra_config_t g_sp3ctra_config;
    int oldSampleRate = g_sp3ctra_config.sampling_frequency;
    g_sp3ctra_config.sampling_frequency = static_cast<int>(sampleRate);
    g_sp3ctra_config.audio_buffer_size   = samplesPerBlock;
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
    lastConsumedReadIdxLuxstralB = -1;   // M8 — 2nd LuxStral engine

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
            if (auto* ed = dynamic_cast<Sp3ctraAudioProcessorEditor*>(getActiveEditor()))
                ed->resumeVisualizer();
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
    }

    // ── LuxSampler player threads (per-instance, non-RT) ───────────────────
    if (sharedCore && sharedCore->getCore())
    {
        auto* aib = sharedCore->getCore()->getAudioImageBuffers();
        auto* dbf = sharedCore->getCore()->getDoubleBuffer();
        if (luxSampler)  luxSampler->startPlayerThread(aib, dbf);
        if (luxSamplerB) luxSamplerB->startPlayerThread(aib, dbf);
    }

    log_info("VST", "=============================================================");

    if (auto* editor = dynamic_cast<Sp3ctraAudioProcessorEditor*>(getActiveEditor()))
        editor->resumeVisualizer();
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

    // ── LuxSampler MIDI (RT-safe: atomics only, no alloc, no lock, no I/O) ──
    if (luxSampler != nullptr)
        luxSampler->processMidi(midiMessages);

    // ── LuxSampler action button MIDI bindings (REC / PLAY / SAVE) ─────────
    // RT-safe: only atomic reads/writes, no allocation, no logging.
    // Triggers are consumed by SlotEditorComponent::timerCallback() (UI thread).
    {
        const int samplerCh =
            static_cast<int>(apvts.getRawParameterValue("luxSamplerMidiChannel")->load()) + 1;

        const int recType  = static_cast<int>(apvts.getRawParameterValue("luxSamplerRecBindType")->load());
        const int recNum   = static_cast<int>(apvts.getRawParameterValue("luxSamplerRecBindNum") ->load());
        const int playType = static_cast<int>(apvts.getRawParameterValue("luxSamplerPlayBindType")->load());
        const int playNum  = static_cast<int>(apvts.getRawParameterValue("luxSamplerPlayBindNum") ->load());
        const int saveType = static_cast<int>(apvts.getRawParameterValue("luxSamplerSaveBindType")->load());
        const int saveNum  = static_cast<int>(apvts.getRawParameterValue("luxSamplerSaveBindNum") ->load());

        const int learnTarget = samplerMidiLearnTarget.load(std::memory_order_acquire);

        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();
            if (msg.getChannel() != samplerCh) continue;

            // MIDI Learn: capture first matching event then exit learn mode.
            // Only captured when learn mode is active AND no result has been
            // captured yet (-1).
            if (learnTarget >= 0
                && samplerMidiLearnResult.load(std::memory_order_relaxed) == -1)
            {
                int captured = -1;
                if (msg.isNoteOn())
                    captured = (1 << 8) | (msg.getNoteNumber() & 0x7F);
                else if (msg.isController())
                    captured = (2 << 8) | (msg.getControllerNumber() & 0x7F);

                if (captured >= 0)
                {
                    samplerMidiLearnResult.store(captured, std::memory_order_release);
                    // Stop learning — UI thread will apply the result.
                    samplerMidiLearnTarget.store(-1, std::memory_order_release);
                    continue; // do not also trigger an action on the learning event
                }
            }

            // Momentary (press-and-hold) binding detection.
            // Returns:
            //   +1  if this MIDI event represents a "press"   for (type, number)
            //   -1  if this MIDI event represents a "release" for (type, number)
            //    0  otherwise (event not relevant for this binding)
            //
            // Press / release semantics:
            //   - Note bindings : NoteOn (vel > 0) → press, NoteOff (or NoteOn vel 0) → release
            //   - CC   bindings : value >= 64       → press, value <  64               → release
            //
            // NOTE: this lambda is called once per binding per MIDI event so the
            //       same event can update REC and PLAY independently.
            auto matchesBindingEdge = [&](int type, int number) -> int
            {
                if (type == 1) // Note
                {
                    if (msg.getNoteNumber() != number) return 0;
                    if (msg.isNoteOn() && msg.getVelocity() > 0) return +1;
                    if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0)) return -1;
                    return 0;
                }
                if (type == 2) // CC
                {
                    if (!msg.isController()) return 0;
                    if (msg.getControllerNumber() != number) return 0;
                    return (msg.getControllerValue() >= 64) ? +1 : -1;
                }
                return 0;
            };

            // REC binding — momentary
            {
                const int edge = matchesBindingEdge(recType, recNum);
                if (edge > 0)
                {
                    // Press: only emit "pressed" pulse on rising edge (not held → held).
                    if (!samplerRecHeld.exchange(true, std::memory_order_acq_rel))
                        samplerRecPressed.store(true, std::memory_order_release);
                }
                else if (edge < 0)
                {
                    // Release: only emit "released" pulse on falling edge.
                    if (samplerRecHeld.exchange(false, std::memory_order_acq_rel))
                        samplerRecReleased.store(true, std::memory_order_release);
                }
            }

            // PLAY binding — momentary
            {
                const int edge = matchesBindingEdge(playType, playNum);
                if (edge > 0)
                {
                    if (!samplerPlayHeld.exchange(true, std::memory_order_acq_rel))
                        samplerPlayPressed.store(true, std::memory_order_release);
                }
                else if (edge < 0)
                {
                    if (samplerPlayHeld.exchange(false, std::memory_order_acq_rel))
                        samplerPlayReleased.store(true, std::memory_order_release);
                }
            }

            // SAVE binding — one-shot trigger on press (release ignored)
            if (matchesBindingEdge(saveType, saveNum) > 0)
                samplerSaveTriggered.store(true, std::memory_order_release);
        }
    }


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

    // ── LuxPitch MIDI (RT-safe) — fanned out to every active per-chain instance ──
    {
        const int lpCh  = static_cast<int>(apvts.getRawParameterValue("luxpitchMidiChannel")->load()) + 1;
        const int lpOct = static_cast<int>(apvts.getRawParameterValue("luxpitchOctaveOffset")->load()) - 2;
        const uint32_t pitchBits = chainPitchMask_.load(std::memory_order_relaxed);
        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();
            if (msg.getChannel() != lpCh) continue;

            if (msg.isControllerOfType(1)) // CC1 mod wheel → drives the LFO Depth slider
            {
                // The wheel and the on-screen "LFO Depth" slider are a single
                // control (shared param) — handle once, not per instance.
                if (auto* p = apvts.getParameter("luxpitchLfoDepth"))
                    p->setValueNotifyingHost((float)msg.getControllerValue() / 127.0f);
                continue;
            }

            const int shifted = msg.getNoteNumber() + lpOct * 12;
            for (uint32_t bits = pitchBits, i = 0; bits != 0; bits >>= 1, ++i)
            {
                if ((bits & 1u) == 0) continue;
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

    // ── LuxMask MIDI (RT-safe) — fanned out to every active per-chain instance ──
    {
        const int lmCh  = static_cast<int>(apvts.getRawParameterValue("luxmaskMidiChannel")->load()) + 1;
        const int lmOct = static_cast<int>(apvts.getRawParameterValue("luxmaskOctaveOffset")->load()) - 2;
        const uint32_t maskBits = chainMaskMask_.load(std::memory_order_relaxed);
        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();
            if (msg.getChannel() != lmCh) continue;

            if (msg.isControllerOfType(1)) // CC1 mod wheel → drives the LFO Pos Depth slider (shared param)
            {
                if (auto* p = apvts.getParameter("luxmaskLfoPosDepth"))
                    p->setValueNotifyingHost((float)msg.getControllerValue() / 127.0f);
                continue;
            }

            const int shifted = msg.getNoteNumber() + lmOct * 12;
            for (uint32_t bits = maskBits, i = 0; bits != 0; bits >>= 1, ++i)
            {
                if ((bits & 1u) == 0) continue;
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
        const bool lxEnabled = apvts.getRawParameterValue("luxsynthEnabled")->load() > 0.5f;
        if (lxEnabled && g_luxsynth_engine.initialized)
        {
            const int lxCh  = static_cast<int>(apvts.getRawParameterValue("luxsynthMidiChannel")->load()) + 1;
            const int lxOct = static_cast<int>(apvts.getRawParameterValue("luxsynthOctaveOffset")->load()) - 2;
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
        const bool lwEnabled = apvts.getRawParameterValue("luxwaveEnabled")->load() > 0.5f;
        if (lwEnabled && g_luxwave_engine.initialized)
        {
            const int lwCh  = static_cast<int>(apvts.getRawParameterValue("luxwaveMidiChannel")->load()) + 1;
            const int lwOct = static_cast<int>(apvts.getRawParameterValue("luxwaveOctaveOffset")->load()) - 2;
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
        const int   mode    = static_cast<int> (apvts.getRawParameterValue("acqGateMode")->load());
        const float rateMs  = apvts.getRawParameterValue("acqGateRateMs")->load();
        const int   divIdx  = static_cast<int> (apvts.getRawParameterValue("acqGateSyncDiv")->load());
        const int   mdIdx   = static_cast<int> (apvts.getRawParameterValue("acqGateMultDiv")->load());

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
        
        const int synthBufferSize = g_sp3ctra_config.audio_buffer_size;
        const int samplesToRead = (numSamples <= synthBufferSize) ? numSamples : synthBufferSize;
        
        if (leftReady && rightReady && readIdx != lastConsumedReadIdx) {
            // ✅ NEW DATA available — copy to JUCE output and signal producer
            float* leftData = luxstral_buffers_L[readIdx].data;
            float* rightData = luxstral_buffers_R[readIdx].data;

                if (leftData && rightData) {
                if (luxstralEnabled) {
                    const float lsVol = apvts.getRawParameterValue("luxstralVolume")->load();
                    if (totalNumOutputChannels >= 1) {
                        float* destLeft = buffer.getWritePointer(0);
                        for (int i = 0; i < samplesToRead; ++i)
                            destLeft[i] = leftData[i] * lsVol;
                    }
                    if (totalNumOutputChannels >= 2) {
                        float* destRight = buffer.getWritePointer(1);
                        for (int i = 0; i < samplesToRead; ++i)
                            destRight[i] = rightData[i] * lsVol;
                    }
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
                const float lsVol = apvts.getRawParameterValue("luxstralVolume")->load();
                if (totalNumOutputChannels >= 1) {
                    float* destLeft = buffer.getWritePointer(0);
                    for (int i = 0; i < samplesToRead; ++i)
                        destLeft[i] = leftData[i] * lsVol;
                }
                if (totalNumOutputChannels >= 2) {
                    float* destRight = buffer.getWritePointer(1);
                    for (int i = 0; i < samplesToRead; ++i)
                        destRight[i] = rightData[i] * lsVol;
                }
            }
            // DO NOT signal consumed — producer is still working on the next buffer
        } else {
            // 🔇 No data ready at all (startup or after long pause)
            // Buffer already cleared — silence is appropriate here
            rt_profiler_report_buffer_miss_luxstral(&g_vst_rt_profiler);
        }
    }

    // ========================================================================
    // 🎯 LUXSTRAL ENGINE B (M8 — dual-engine, ADDITIVE mix)
    //
    // The 2nd LuxStral engine renders in the SAME audio-thread iteration as A
    // (paced by A's consumed-buffer handshake), so it needs NO separate handshake
    // here — we just read its own double-buffer and ADD it (engine A above WROTE
    // the buffer; A + B + LuxSynth + LuxWave sum). Gated by model presence + its
    // own volume. Independent chain input is prepared in multithreading.c.
    // ========================================================================
    const bool luxstralBEnabled = apvts.getRawParameterValue("luxstralBEnabled")->load() >= 0.5f;
    if (luxstralBEnabled && luxstralBPresent_.load(std::memory_order_relaxed)
        && sharedCore && sharedCore->isReady() && luxstral_are_audio_buffers_ready()) {
        extern AudioImageBuffer luxstral_b_buffers_L[2];
        extern AudioImageBuffer luxstral_b_buffers_R[2];
        extern volatile int luxstral_b_buffer_index;
        extern sp3ctra_config_t g_sp3ctra_config;

        int readIdx = 1 - __atomic_load_n(&luxstral_b_buffer_index, __ATOMIC_ACQUIRE);
        int leftReady  = __atomic_load_n(&luxstral_b_buffers_L[readIdx].ready, __ATOMIC_ACQUIRE);
        int rightReady = __atomic_load_n(&luxstral_b_buffers_R[readIdx].ready, __ATOMIC_ACQUIRE);

        const int synthBufferSize = g_sp3ctra_config.audio_buffer_size;
        const int samplesToRead = (numSamples <= synthBufferSize) ? numSamples : synthBufferSize;

        if (leftReady && rightReady) {
            // New OR stale frame — either way, add it (continuous 2nd voice).
            if (readIdx == lastConsumedReadIdxLuxstralB)
                rt_profiler_report_stale_luxstral(&g_vst_rt_profiler);
            float* leftData  = luxstral_b_buffers_L[readIdx].data;
            float* rightData = luxstral_b_buffers_R[readIdx].data;
            if (leftData && rightData) {
                const float lsVolB = apvts.getRawParameterValue("luxstralBVolume")->load();
                if (totalNumOutputChannels >= 1) {
                    float* destLeft = buffer.getWritePointer(0);
                    for (int i = 0; i < samplesToRead; ++i)
                        destLeft[i] += leftData[i] * lsVolB;
                }
                if (totalNumOutputChannels >= 2) {
                    float* destRight = buffer.getWritePointer(1);
                    for (int i = 0; i < samplesToRead; ++i)
                        destRight[i] += rightData[i] * lsVolB;
                }
                lastConsumedReadIdxLuxstralB = readIdx;
            }
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
        const bool lxEnabled = apvts.getRawParameterValue("luxsynthEnabled")->load() > 0.5f;
        if (lxEnabled)
        {
            // 1. Drain pending MIDI events into engine voices
            luxsynth_process_pending_midi();

            // 2. Generate audio directly — uses preallocated engine buffers
            luxsynth_engine_process(&g_luxsynth_engine, numSamples,
                                    g_luxsynth_engine.output_left,
                                    g_luxsynth_engine.output_right);

            // 3. Mix into JUCE output buffer (additive)
            const float lxVol = apvts.getRawParameterValue("luxsynthVolume")->load();

            if (totalNumOutputChannels >= 1)
            {
                float* dest = buffer.getWritePointer(0);
                for (int i = 0; i < numSamples; ++i)
                    dest[i] += g_luxsynth_engine.output_left[i] * lxVol;
            }
            if (totalNumOutputChannels >= 2)
            {
                float* dest = buffer.getWritePointer(1);
                for (int i = 0; i < numSamples; ++i)
                    dest[i] += g_luxsynth_engine.output_right[i] * lxVol;
            }
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
        const bool lwEnabled = apvts.getRawParameterValue("luxwaveEnabled")->load() > 0.5f;
        if (lwEnabled)
        {
            // 1. Update engine config from APVTS (RT-safe: simple struct copy)
            LuxWaveConfig lwCfg;
            lwCfg.attack_ms           = apvts.getRawParameterValue("luxwaveAttackMs")->load();
            lwCfg.decay_ms            = apvts.getRawParameterValue("luxwaveDecayMs")->load();
            lwCfg.sustain_level       = apvts.getRawParameterValue("luxwaveSustainLevel")->load();
            lwCfg.release_ms          = apvts.getRawParameterValue("luxwaveReleaseMs")->load();
            lwCfg.attack_curve        = apvts.getRawParameterValue("luxwaveAttackCurve")->load();
            lwCfg.decay_curve         = apvts.getRawParameterValue("luxwaveDecayCurve")->load();
            lwCfg.release_curve       = apvts.getRawParameterValue("luxwaveReleaseCurve")->load();
            lwCfg.filter_attack_ms    = 20.0f;
            lwCfg.filter_decay_ms     = 150.0f;
            lwCfg.filter_sustain      = 0.5f;
            lwCfg.filter_release_ms   = 300.0f;
            lwCfg.filter_cutoff_hz    = apvts.getRawParameterValue("luxwaveFilterCutoff")->load();
            lwCfg.filter_env_depth_hz = apvts.getRawParameterValue("luxwaveFilterEnvDepth")->load();
            lwCfg.lfo_rate_hz         = apvts.getRawParameterValue("luxwaveLfoRate")->load();
            lwCfg.lfo_depth_semitones = apvts.getRawParameterValue("luxwaveLfoDepth")->load();
            lwCfg.scan_mode           = (LuxWaveScanMode)static_cast<int>(apvts.getRawParameterValue("luxwaveScanMode")->load());
            lwCfg.amplitude           = apvts.getRawParameterValue("luxwaveAmplitude")->load();
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
            const float lwVol = apvts.getRawParameterValue("luxwaveVolume")->load();
            if (totalNumOutputChannels >= 1)
            {
                float* dest = buffer.getWritePointer(0);
                for (int i = 0; i < numSamples; ++i)
                    dest[i] += g_luxwave_engine.output_left[i] * lwVol;
            }
            if (totalNumOutputChannels >= 2)
            {
                float* dest = buffer.getWritePointer(1);
                for (int i = 0; i < numSamples; ++i)
                    dest[i] += g_luxwave_engine.output_right[i] * lwVol;
            }
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

    std::unique_ptr<juce::XmlElement> xml(state.createXml());

    copyXmlToBinary(*xml, destData);
    log_info("VST", "State saved to DAW project");
}

void Sp3ctraAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // APVTS handles deserialization automatically via ValueTree
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    
    if (xmlState.get() != nullptr) {
        if (xmlState->hasTagName(apvts.state.getType())) {
            apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
            // Transport must never auto-run on open: force the video scroll to
            // STOP regardless of what the saved session had. A session stored
            // while playing would otherwise resume scrolling the moment the
            // plugin/window opens. setValueNotifyingHost keeps the Play/Pause
            // button's toggle state in sync. Clearing makes it a true Stop
            // (frozen + blank), not just a pause.
            if (auto* p = apvts.getParameter("videoScrollPaused"))
                p->setValueNotifyingHost(1.0f);
            requestVideoScrollClear();
            // Same never-auto-run rule for the sequencer and SCORE transports:
            // a session saved while playing must open stopped.
            if (auto* p = apvts.getParameter(PARAM_SEQ_TRANSPORT))
                p->setValueNotifyingHost(0.0f);   // Stop
            if (auto* p = apvts.getParameter(PARAM_SCORE_PLAYING))
                p->setValueNotifyingHost(0.0f);
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

            // M6 Phase 2 — restore the chain topology and derive per-chain
            // routing (headless-correct; enable params are already restored).
            loadChainModelFromState();

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
// Parameter Change Listener (called when user modifies parameters in UI)
void Sp3ctraAudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
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

    // 🔧 CRITICAL: LuxStral parameters are automatically synced to g_sp3ctra_config
    // They are read directly by the synthesis engine, NO restart needed!
    // StrokeForge parameters — same hot-reload pattern as LuxStral
    // LuxSampler parameters — update atomic config on LuxSampler
    if (parameterID.startsWith("luxSampler"))
    {
        // NOTE: setEnabled is NOT applied here — per-engine sampler enable is
        // owned by deriveChainRouting() (module presence drives A/B). The shared
        // settings below apply to both engines (per-B params land in Part B).
        const int midiCh = static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_MIDI_CH)) + 1;
        const int oct    = static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_OCT_OFFSET)) - 2;
        const float dur  = *apvts.getRawParameterValue(PARAM_FS_MAX_DUR);
        if (luxSampler != nullptr)
        {
            luxSampler->setMidiChannel(midiCh);
            luxSampler->setOctaveOffset(oct);
            luxSampler->setMaxDuration(dur);
        }
        if (luxSamplerB != nullptr)
        {
            luxSamplerB->setOctaveOffset(oct);
            luxSamplerB->setMaxDuration(dur);
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

        // 🔧 SOURCE SWITCH: Reset preprocessed data so stale data from the
        // previous source doesn't persist.  The correct thread (UDP or
        // FramePlayerThread) will write fresh data for the new source.
        if (parameterID == "luxstralSource") {
            if (sharedCore && sharedCore->getCore()) {
                auto* db = sharedCore->getCore()->getDoubleBuffer();
                if (db) {
                    pthread_mutex_lock(&db->mutex);
                    db->dataReady = 0;
                    db->preprocessed_data.timestamp_us = 0;
                    pthread_mutex_unlock(&db->mutex);
                    log_info("VST", "Source changed — preprocessed data reset");
                }
            }
        }

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
        // Recalculates alpha_up and alpha_down_weighted for all oscillators
        if (parameterID == "luxstralAttackMs" || parameterID == "luxstralReleaseMs") {
            log_info("VST", "Envelope parameter changed - updating coefficients");
            update_gap_limiter_coefficients();
        }

        // M8 — engine B envelope: recompute B's private alphas from its own taus
        // (update_gap_limiter_coefficients() above only touches A's waves[]).
        if (parameterID == "luxstralBAttackMs" || parameterID == "luxstralBReleaseMs") {
            log_info("VST", "Engine B envelope parameter changed - updating coefficients");
            synth_luxstral_update_engine_b_envelope();
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
// M6 Phase 2 — chain-derived source routing (message thread → C config).
// Stores the per-synth channel and pushes it straight into g_sp3ctra_config so
// the change is audible immediately, without waiting for the next param sync.
void Sp3ctraAudioProcessor::setChainSourceRouting(int luxstralSrc, int luxsynthSrc) noexcept
{
    luxstralSrc = (luxstralSrc == 1) ? 1 : 0;   // clamp to {MODULATED, LIVE}
    luxsynthSrc = (luxsynthSrc == 1) ? 1 : 0;
    chainSrcLuxstral.store(luxstralSrc, std::memory_order_relaxed);
    chainSrcLuxsynth.store(luxsynthSrc, std::memory_order_relaxed);
    g_sp3ctra_config.luxstral_source_type = luxstralSrc;
    g_sp3ctra_config.luxsynth_source_type = luxsynthSrc;
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

    // Migration: the step sequencer became a dedicated SEQUENCER module. A model
    // saved before that has no Sequencer block — inject one so the sequencer stays
    // reachable in the UI (same spirit as "always keep ≥1 chain").
    {
        bool hasSeq = false;
        for (const auto& ch : chainModel_.chains)
            for (const auto& mod : ch.modules)
                if (mod.type == ModuleType::Sequencer) { hasSeq = true; break; }
        if (! hasSeq && ! chainModel_.chains.empty())
            chainModel_.insert(0, ModuleType::Sequencer,
                               (int) chainModel_.chains[0].modules.size());
    }

    // Presence baseline so the enable bridge only fires on real transitions.
    chainActiveTypes_.clear();
    chainModel_.deriveActiveTypes(chainActiveTypes_);
    videoScrollSlots_.clear();
    luxstralEngines_.clear();
    for (const auto& ch : chainModel_.chains)
        for (const auto& mod : ch.modules)
        {
            if (mod.type == ModuleType::VideoScroll
                && mod.slot >= 0 && mod.slot < CHAIN_MAX_CHAINS)
                videoScrollSlots_.insert(mod.slot);
            if (mod.type == ModuleType::LuxStral
                && mod.slot >= 0 && mod.slot < ChainModel::kMaxLuxStralEngines)
                luxstralEngines_.insert(mod.slot);
        }

    deriveChainRouting();   // headless-correct per-synth source routing
}

void Sp3ctraAudioProcessor::deriveChainRouting()
{
    // Engine A specifically (slot 0) — this global routing drives engine A; engine
    // B reads its own DoubleBuffer (source_type_override), independent of this.
    const int luxstralSrc = chainModel_.sourceChannelForSynth(ModuleType::LuxStral, 0, /*engineSlot*/ 0);
    int       luxsynthSrc = chainModel_.sourceChannelForSynth(ModuleType::LuxSynth, 1);
    if (luxsynthSrc == 1)   // LuxSynth absent/live → let a placed LuxWave decide
        luxsynthSrc = chainModel_.sourceChannelForSynth(ModuleType::LuxWave, luxsynthSrc);
    setChainSourceRouting(luxstralSrc, luxsynthSrc);

    // Stable chain → Pitch/Mask pool-slot binding (keyed by chain UUID). Binding
    // by chain POSITION would rebind every chain below a removed one to a
    // different state pool (inheriting its held voices / stale state). Slots
    // whose binding just changed carry stale state — reset AFTER the new plan
    // is published at the end of this function.
    const uint32_t staleSlots = updateChainPoolBindings();

    // Active per-chain Pitch/Mask instances → MIDI fan-out + config-sync mask,
    // indexed by POOL SLOT (stable across edits), not by chain position.
    uint32_t pitchMask = 0, maskMask = 0;
    for (int c = 0; c < chainModel_.numChains(); ++c)
    {
        const int slot = poolSlotForChain(c);
        for (const auto& m : chainModel_.chains[(size_t) c].modules)
        {
            if (m.type == ModuleType::Pitch) pitchMask |= (1u << slot);
            if (m.type == ModuleType::Mask)  maskMask  |= (1u << slot);
        }
    }
    chainPitchMask_.store(pitchMask, std::memory_order_relaxed);
    chainMaskMask_.store(maskMask,  std::memory_order_relaxed);

    // Per-instance `enabled` sync — must NOT wait for applyConfigurationToCore:
    // a pure topology change (Pitch dragged to another chain, chain removal,
    // session restore) flips no enable param, so no parameterChanged() would
    // refresh the pool configs and the moved module would stay silently
    // bypassed on its new slot until some unrelated param edit.
    {
        const bool lpOn = apvts.getRawParameterValue("luxpitchEnabled")->load() >= 0.5f;
        const bool lmOn = apvts.getRawParameterValue("luxmaskEnabled")->load() >= 0.5f;
        for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        {
            lux_pitch_instance(i)->config.enabled = (lpOn && ((pitchMask >> i) & 1u)) ? 1 : 0;
            lux_mask_instance(i)->config.enabled  = (lmOn && ((maskMask  >> i) & 1u)) ? 1 : 0;
        }
    }

    // Per-engine sampler enable: a Sampler instance carries its engine index in
    // `slot` (0 = A, 1 = B). An engine is enabled iff its instance is present in
    // the model. Authoritative — overrides the shared luxSamplerEnabled param.
    bool samplerAPresent = false, samplerBPresent = false;
    bool luxstralBPresent = false;                        // M8 — 2nd LuxStral engine
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
        {
            if (m.type == ModuleType::Sampler)
            {
                if (m.slot == 1) samplerBPresent = true;
                else             samplerAPresent = true;   // slot 0 (or unhealed -1)
            }
            else if (m.type == ModuleType::LuxStral && m.slot == 1)
                luxstralBPresent = true;
        }
    if (luxSampler)  luxSampler ->setEnabled(samplerAPresent);
    if (luxSamplerB) luxSamplerB->setEnabled(samplerBPresent);
    luxstralBPresent_.store(luxstralBPresent, std::memory_order_relaxed);

    // Insert order for the GLOBAL Modulated channel (image_chain_process_inserts),
    // which LuxStral consumes whenever a Sampler sits on its chain — the default
    // topology. Applied here directly (RT-safe atomic store) because the
    // "chainInsertOrder" APVTS param has no parameter listener, so a pure reorder
    // would otherwise never refresh the global order until some unrelated param
    // change triggered applyConfigurationToCore(). The setParam() in
    // applyChainEnableBridge() still runs, for host display + persistence.
    image_chain_set_order(chainModel_.isMaskBeforePitch()
        ? IMAGE_CHAIN_ORDER_MASK_PITCH : IMAGE_CHAIN_ORDER_PITCH_MASK);

    deriveAndPublishChainPlan();   // RT-safe per-chain recipe for the synth thread

    // Reset the transient state (held voices, LFO phase — config untouched) of
    // every pool slot that just lost its Pitch/Mask instance or changed its
    // chain binding. Runs AFTER the publish above: the synth thread no longer
    // pulls these pools, and we are on the message thread like every other
    // pool-state writer. Without this, a removed instance's held voices would
    // silently resurface when the module (or a new chain) reuses the slot.
    {
        const uint32_t lostPitch = (prevPitchSlots_ & ~pitchMask) | staleSlots;
        const uint32_t lostMask  = (prevMaskSlots_  & ~maskMask)  | staleSlots;
        for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        {
            if ((lostPitch >> i) & 1u) lux_pitch_reset(lux_pitch_instance(i));
            if ((lostMask  >> i) & 1u) lux_mask_reset(lux_mask_instance(i));
        }
        prevPitchSlots_ = pitchMask;
        prevMaskSlots_  = maskMask;
    }
}

//==============================================================================
// Stable chain → Pitch/Mask pool-slot binding (message thread).
// Chains keep their existing slot (keyed by UUID); vanished chains release
// theirs; new chains take the lowest free slot. Returns the mask of slots whose
// binding changed (released or freshly assigned) — their pool state is stale.
//==============================================================================
uint32_t Sp3ctraAudioProcessor::updateChainPoolBindings()
{
    uint32_t stale = 0;

    std::set<juce::Uuid> live;
    for (const auto& ch : chainModel_.chains)
        live.insert(ch.id);

    for (auto it = chainPoolSlots_.begin(); it != chainPoolSlots_.end();)
    {
        if (live.count(it->first) == 0)
        {
            stale |= (1u << it->second);          // chain gone → slot state stale
            it = chainPoolSlots_.erase(it);
        }
        else
            ++it;
    }

    uint32_t used = 0;
    for (const auto& binding : chainPoolSlots_)
        used |= (1u << binding.second);

    for (const auto& ch : chainModel_.chains)
    {
        if (chainPoolSlots_.count(ch.id) != 0)
            continue;
        for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
            if (((used >> s) & 1u) == 0)
            {
                chainPoolSlots_[ch.id] = s;
                used  |= (1u << s);
                stale |= (1u << s);               // fresh binding → start clean
                break;
            }
        // The model caps chains at kMaxChains == CHAIN_MAX_CHAINS, so a free
        // slot always exists for a legal model.
        jassert(chainPoolSlots_.count(ch.id) != 0);
    }
    return stale;
}

int Sp3ctraAudioProcessor::poolSlotForChain(int chainIdx) const noexcept
{
    if (chainIdx < 0 || chainIdx >= chainModel_.numChains())
        return 0;
    const auto it = chainPoolSlots_.find(chainModel_.chains[(size_t) chainIdx].id);
    return it != chainPoolSlots_.end() ? it->second : 0;
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

    auto sourceKind = [](const Chain& ch) -> int
    {
        for (const auto& m : ch.modules)
        {
            if (m.type == ModuleType::Sp3ctra) return CHAIN_SRC_LIVE;
            if (m.type == ModuleType::Image)   return CHAIN_SRC_IMAGE;
            if (m.type == ModuleType::Video)   return CHAIN_SRC_VIDEO;
        }
        return CHAIN_SRC_NONE;
    };

    // engineSlot >= 0 additionally matches ModuleInstance.slot — used to tell the
    // two LuxStral engines apart (A = slot 0, B = slot 1); -1 = match by type only.
    auto fill = [&](ModuleType synth, int slot, int engineSlot = -1)
    {
        int ci = -1, idx = -1;
        for (int c = 0; c < chainModel_.numChains() && ci < 0; ++c)
        {
            const auto& mods = chainModel_.chains[(size_t) c].modules;
            for (int i = 0; i < (int) mods.size(); ++i)
                if (mods[(size_t) i].type == synth
                    && (engineSlot < 0 || mods[(size_t) i].slot == engineSlot))
                { ci = c; idx = i; break; }
        }
        SynthChainPlan& sp = plan.synth[slot];
        if (ci < 0) { sp.present = 0; return; }

        const auto& ch = chainModel_.chains[(size_t) ci];
        sp.present     = 1;
        sp.source_kind = sourceKind(ch);
        // Pitch/Mask pool slot bound to this chain's UUID — stable across edits
        // (must match the masks derived in deriveChainRouting).
        const int stateIdx = poolSlotForChain(ci);

        for (int i = 0; i < idx; ++i)
        {
            const ModuleInstance& mi = ch.modules[(size_t) i];
            const ModuleType t = mi.type;
            if ((t == ModuleType::Pitch || t == ModuleType::Mask)
                && sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
            {
                sp.insert_id[sp.num_inserts] = (t == ModuleType::Pitch)
                    ? IMAGE_CHAIN_INSERT_LUXPITCH : IMAGE_CHAIN_INSERT_LUXMASK;
                sp.insert_state_idx[sp.num_inserts] = stateIdx;   // chain-derived Pitch/Mask pool slot
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
            else if (t == ModuleType::Sampler)
            {
                sp.has_sampler = 1;
                // Record the sampler's POSITION in the insert list so the executor
                // can feed VideoScroll probes pre- vs post-sampler correctly.
                if (sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
                {
                    sp.insert_id[sp.num_inserts]        = IMAGE_CHAIN_INSERT_SAMPLER;
                    sp.insert_state_idx[sp.num_inserts] = 0;   // unused for the marker
                    sp.num_inserts++;
                }
            }
            else if (t == ModuleType::Score)   sp.has_score   = 1;
        }
    };

    fill(ModuleType::LuxStral, CHAIN_SYNTH_LUXSTRAL,   0);  // engine A (slot 0)
    fill(ModuleType::LuxSynth, CHAIN_SYNTH_LUXSYNTH);
    fill(ModuleType::LuxWave,  CHAIN_SYNTH_LUXWAVE);
    fill(ModuleType::LuxStral, CHAIN_SYNTH_LUXSTRAL_B, 1);  // engine B (slot 1)

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
                engines[e]->slotParamsFromXml(i, *slotXml);
            }
        }
    }
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

    // LuxStral is handled PER ENGINE below (A = deviceEnabled, B =
    // luxstralBEnabled) — a type-level diff would flip A's param when only B
    // was added/removed, and would leave A audible (raw live fallback) after
    // its block is removed while B stays placed.
    static const ModuleType kEnableTypes[] = {
        ModuleType::Pitch, ModuleType::Mask, ModuleType::Sampler, ModuleType::Sequencer,
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

    // Per-engine LuxStral enable (same add ⇒ on / absent ⇒ off diff semantics).
    {
        std::set<int> enginesNow;
        for (const auto& ch : chainModel_.chains)
            for (const auto& m : ch.modules)
                if (m.type == ModuleType::LuxStral && m.slot >= 0
                    && m.slot < ChainModel::kMaxLuxStralEngines)
                    enginesNow.insert(m.slot);

        static const char* kEngineParam[ChainModel::kMaxLuxStralEngines] =
            { "deviceEnabled", "luxstralBEnabled" };
        for (int s = 0; s < ChainModel::kMaxLuxStralEngines; ++s)
        {
            const bool isNow = enginesNow.count(s) > 0;
            const bool was   = luxstralEngines_.count(s) > 0;
            if (isNow && ! was)
                setParam(kEngineParam[s], true);
            else if (! isNow)
                setParam(kEngineParam[s], false);
        }
        luxstralEngines_ = enginesNow;
    }

    setParam("chainInsertOrder", chainModel_.isMaskBeforePitch());
    chainActiveTypes_ = now;
}

// Free the state of every module that just disappeared from the topology. The
// enable bridge (next call) silences modules that own an enable param, but some
// modules keep state the bridge cannot reach: SCORE has NO enable param and its
// has_score plan flag is never read by the RT thread, so removal alone never
// stops it; VideoScroll keeps a captured waterfall ring; Pitch/Mask hold live
// MIDI voices. Recorded content (sampler slots, sequencer pattern, synth params)
// is intentionally preserved — removal only tears down transient/live state.
// Runs AFTER deriveChainRouting() published the new plan (the synth thread has
// stopped pulling the removed modules) and BEFORE applyChainEnableBridge()
// overwrites chainActiveTypes_, so chainActiveTypes_ still holds the old set.
void Sp3ctraAudioProcessor::teardownAbsentModules(const std::set<ModuleType>& now)
{
    auto removed = [&](ModuleType t)
    { return chainActiveTypes_.count(t) > 0 && now.count(t) == 0; };

    // SCORE: stop playback, free the frame buffer, reset settings to defaults.
    if (removed(ModuleType::Score))
    {
        if (auto* ls = getLuxSampler())
            ls->uiDiscardScore();
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
            video_scroll_init(video_scroll_instance(slot));   // re-init = clear ring
    // A slot that just (re)appeared is a freshly placed output: force its enable
    // ON so a new VideoScroll block starts visible even if that slot was left
    // disabled by a previously removed instance.
    for (int slot : vsNow)
        if (videoScrollSlots_.count(slot) == 0)
            if (auto* p = apvts.getParameter(vsParam(slot, "enabled")))
                p->setValueNotifyingHost(1.0f);
    videoScrollSlots_ = vsNow;
}

void Sp3ctraAudioProcessor::onChainModelEdited()
{
    // Routing/masks/plan FIRST: the enable bridge below flips APVTS params which
    // trigger applyConfigurationToCore() (per-instance config sync) — that reads
    // the freshly-computed chainPitch/MaskMask_.
    deriveChainRouting();       // per-synth source routing + instance masks + RT plan

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
    
    // Image Processing - LuxStral pipeline: RGB → Grayscale → Inversion → Gamma → Averaging → Contrast
    g_sp3ctra_config.invert_intensity = 
        (int)apvts.getRawParameterValue("luxstralInvertIntensity")->load();
    g_sp3ctra_config.additive_enable_non_linear_mapping = 
        (int)apvts.getRawParameterValue("luxstralGammaEnable")->load();
    g_sp3ctra_config.additive_gamma_value = 
        apvts.getRawParameterValue("luxstralGammaValue")->load();
    g_sp3ctra_config.additive_contrast_min = 
        apvts.getRawParameterValue("luxstralContrastMin")->load();
    
    // Stereo Processing
    g_sp3ctra_config.stereo_mode_enabled = 
        (int)apvts.getRawParameterValue("luxstralStereoEnable")->load();
    g_sp3ctra_config.stereo_temperature_amplification = 
        apvts.getRawParameterValue("luxstralStereoTempAmp")->load();
    
    // Dynamics Processing (summation_normalization)
    g_sp3ctra_config.volume_weighting_exponent =
        apvts.getRawParameterValue("luxstralVolumeWeightingExp")->load();
    g_sp3ctra_config.summation_response_exponent =
        apvts.getRawParameterValue("luxstralSummationResponseExp")->load();
    g_sp3ctra_config.noise_gate_threshold =
        apvts.getRawParameterValue("luxstralNoiseGateThreshold")->load();
    g_sp3ctra_config.soft_limit_threshold =
        apvts.getRawParameterValue("luxstralSoftLimitThreshold")->load();
    g_sp3ctra_config.soft_limit_knee =
        apvts.getRawParameterValue("luxstralSoftLimitKnee")->load();

    // M8 — LuxStral engine B: independent PLAY/SETUP mirror (engine A keeps
    // the legacy fields above; engine B + its pipeline read luxstral_b_*).
    g_sp3ctra_config.luxstral_b_inversion =
        (int)apvts.getRawParameterValue("luxstralBInversion")->load();
    g_sp3ctra_config.luxstral_b_ac_removal =
        (int)apvts.getRawParameterValue("luxstralBAcRemoval")->load();
    g_sp3ctra_config.luxstral_b_gamma_value =
        apvts.getRawParameterValue("luxstralBGammaValue")->load();
    g_sp3ctra_config.luxstral_b_contrast_min =
        apvts.getRawParameterValue("luxstralBContrastMin")->load();
    g_sp3ctra_config.luxstral_b_tau_up_base_ms =
        apvts.getRawParameterValue("luxstralBAttackMs")->load();
    g_sp3ctra_config.luxstral_b_tau_down_base_ms =
        apvts.getRawParameterValue("luxstralBReleaseMs")->load();
    g_sp3ctra_config.luxstral_b_summation_response_exponent =
        apvts.getRawParameterValue("luxstralBSummationResponseExp")->load();
    g_sp3ctra_config.luxstral_b_noise_gate_threshold =
        apvts.getRawParameterValue("luxstralBNoiseGateThreshold")->load();
    g_sp3ctra_config.luxstral_b_stereo_mode_enabled =
        (int)apvts.getRawParameterValue("luxstralBStereoEnable")->load();
    g_sp3ctra_config.luxstral_b_stereo_temperature_amplification =
        apvts.getRawParameterValue("luxstralBStereoTempAmp")->load();
    g_sp3ctra_config.luxstral_b_soft_limit_threshold =
        apvts.getRawParameterValue("luxstralBSoftLimitThreshold")->load();
    g_sp3ctra_config.luxstral_b_soft_limit_knee =
        apvts.getRawParameterValue("luxstralBSoftLimitKnee")->load();
    
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
        // ── Source follows CHAIN PLACEMENT (chains are fixed for now) ─────────
        // The per-engine "Source" dropdown is retired: an engine reads the
        // signal of the chain it sits on, and is gated by that chain's transport.
        //   • Chain 1 (Source ► Pitch ► Mask ► Sampler ► LuxStral)
        //         → LuxStral reads the modulated frame      (IMAGE_SOURCE_MODULATED = 0)
        //   • Chain 2 (Source ► LuxSynth ► LuxWave)
        //         → LuxSynth + LuxWave read the raw live CIS (IMAGE_SOURCE_LIVE = 1)
        // The luxstralSource / luxsynthSource params are kept (plumbing) for the
        // future modular-chain routing, but their value no longer drives audio.
        // ── M6 Phase 2 — model-driven (ChainRackComponent → setChainSourceRouting).
        // Read the chain-derived routing instead of hardcoding; defaults (0, 1)
        // reproduce the legacy fixed topology before any edit.
        g_sp3ctra_config.luxstral_source_type = chainSrcLuxstral.load(std::memory_order_relaxed);
        g_sp3ctra_config.luxsynth_source_type = chainSrcLuxsynth.load(std::memory_order_relaxed);

        g_sp3ctra_config.luxstral_inversion   =
            static_cast<int>(apvts.getRawParameterValue("luxstralInversion")->load());
        g_sp3ctra_config.luxstral_ac_removal  =
            static_cast<int>(apvts.getRawParameterValue("luxstralAcRemoval")->load());

        g_sp3ctra_config.luxsynth_inversion   =
            static_cast<int>(apvts.getRawParameterValue("luxsynthInversion")->load());
        g_sp3ctra_config.luxsynth_ac_removal  =
            static_cast<int>(apvts.getRawParameterValue("luxsynthAcRemoval")->load());
        // Image Processing - LuxSynth pipeline: gamma is always active (no enable flag).
        // preprocess_luxsynth() skips it as a no-op when gamma_value == 1.0.
        g_sp3ctra_config.luxsynth_gamma_value =
            apvts.getRawParameterValue("luxsynthGammaValue")->load();

        // ── LuxPitch source routing (S/M/L — no P option for its own source) ──
        {
            static const int kLpChoiceToSrc[3] = { 0, 2, 1 }; // S→SAMPLER, M→MIX, L→LIVE
            int lpSrcChoice = static_cast<int>(
                apvts.getRawParameterValue("luxpitchSource")->load());
            if (lpSrcChoice < 0 || lpSrcChoice > 2) lpSrcChoice = 1;
            g_sp3ctra_config.luxpitch_source_type = kLpChoiceToSrc[lpSrcChoice];
        }

        // ── LuxMask source routing (S/M/L — no K option for its own source) ──
        {
            static const int kLmChoiceToSrc[3] = { 0, 2, 1 }; // S→SAMPLER, M→MIX, L→LIVE
            int lmSrcChoice = static_cast<int>(
                apvts.getRawParameterValue("luxmaskSource")->load());
            if (lmSrcChoice < 0 || lmSrcChoice > 2) lmSrcChoice = 1;
            g_sp3ctra_config.luxmask_source_type = kLmChoiceToSrc[lmSrcChoice];
        }

        // ── Insert chain order (M1 — modular pipeline core) ──
        // Derived from the CHAIN MODEL, not the "chainInsertOrder" param: the
        // model is the single source of truth for topology; the param is only
        // its host-visible projection (kept in sync by applyChainEnableBridge).
        // Reading the param here would let a host automation of it desync the
        // global order from the per-chain plans derived from the model.
        image_chain_set_order(chainModel_.isMaskBeforePitch()
            ? IMAGE_CHAIN_ORDER_MASK_PITCH : IMAGE_CHAIN_ORDER_PITCH_MASK);

        // ── Sync LuxPitch config to the processing instance ──
        {
            LuxPitchConfig lpc;
            lpc.enabled                 = static_cast<int>(apvts.getRawParameterValue("luxpitchEnabled")->load());
            lpc.polyphony_enabled       = static_cast<int>(apvts.getRawParameterValue("luxpitchPolyphony")->load());
            lpc.background_mode         = static_cast<int>(apvts.getRawParameterValue("luxpitchBackgroundMode")->load());
            lpc.coupling_mode           = static_cast<int>(apvts.getRawParameterValue("luxpitchCouplingMode")->load());
            lpc.free_pixels_per_semitone = apvts.getRawParameterValue("luxpitchFreePixelsPerST")->load();
            lpc.pitch_bend_range        = apvts.getRawParameterValue("luxpitchPitchBendRange")->load();
            lpc.attack_ms               = apvts.getRawParameterValue("luxpitchAttackMs")->load();
            lpc.decay_ms                = apvts.getRawParameterValue("luxpitchDecayMs")->load();
            lpc.sustain_level           = apvts.getRawParameterValue("luxpitchSustainLevel")->load();
            lpc.release_ms              = apvts.getRawParameterValue("luxpitchReleaseMs")->load();
            lpc.attack_curve            = apvts.getRawParameterValue("luxpitchAttackCurve")->load();
            lpc.decay_curve             = apvts.getRawParameterValue("luxpitchDecayCurve")->load();
            lpc.release_curve           = apvts.getRawParameterValue("luxpitchReleaseCurve")->load();
            lpc.glide_time_ms           = apvts.getRawParameterValue("luxpitchGlideMs")->load();
            lpc.lfo_rate_hz             = apvts.getRawParameterValue("luxpitchLfoRate")->load();
            lpc.lfo_depth_semitones     = apvts.getRawParameterValue("luxpitchLfoDepth")->load();
            lpc.velocity_coupling       = static_cast<int>(apvts.getRawParameterValue("luxpitchVelocityCoupling")->load());
            lpc.reference_note          = 24 + static_cast<int>(apvts.getRawParameterValue("luxpitchReferenceNote")->load());
            // M6 Phase 2 — settings are shared per type, but `enabled` is per
            // instance (= presence in chain i) so a Pitch on one chain never
            // processes another chain's image. Slot i is active iff chain i has Pitch.
            {
                const uint32_t pmask = chainPitchMask_.load(std::memory_order_relaxed);
                for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
                {
                    LuxPitchConfig c = lpc;
                    if (((pmask >> i) & 1u) == 0) c.enabled = 0;
                    lux_pitch_instance(i)->config = c;
                }
            }
        }

        // ── Sync LuxMask config to the processing instance ──
        {
            LuxMaskConfig lmc;
            lmc.enabled                  = static_cast<int>(apvts.getRawParameterValue("luxmaskEnabled")->load());
            lmc.polyphony_enabled        = static_cast<int>(apvts.getRawParameterValue("luxmaskPolyphony")->load());
            lmc.background_mode          = static_cast<int>(apvts.getRawParameterValue("luxmaskBackgroundMode")->load());
            lmc.coupling_mode            = static_cast<int>(apvts.getRawParameterValue("luxmaskCouplingMode")->load());
            lmc.free_pixels_per_semitone = apvts.getRawParameterValue("luxmaskFreePixelsPerST")->load();
            lmc.pitch_bend_range         = apvts.getRawParameterValue("luxmaskPitchBendRange")->load();
            lmc.filter_width_pct         = apvts.getRawParameterValue("luxmaskFilterWidth")->load();
            lmc.filter_offset_pct        = apvts.getRawParameterValue("luxmaskFilterOffset")->load();
            lmc.filter_slope             = apvts.getRawParameterValue("luxmaskFilterSlope")->load();
            lmc.attack_ms                = apvts.getRawParameterValue("luxmaskAttackMs")->load();
            lmc.decay_ms                 = apvts.getRawParameterValue("luxmaskDecayMs")->load();
            lmc.sustain_level            = apvts.getRawParameterValue("luxmaskSustainLevel")->load();
            lmc.release_ms               = apvts.getRawParameterValue("luxmaskReleaseMs")->load();
            lmc.attack_curve             = apvts.getRawParameterValue("luxmaskAttackCurve")->load();
            lmc.decay_curve              = apvts.getRawParameterValue("luxmaskDecayCurve")->load();
            lmc.release_curve            = apvts.getRawParameterValue("luxmaskReleaseCurve")->load();
            lmc.glide_time_ms            = apvts.getRawParameterValue("luxmaskGlideMs")->load();
            lmc.lfo_pos_rate_hz          = apvts.getRawParameterValue("luxmaskLfoPosRate")->load();
            lmc.lfo_pos_depth_semitones  = apvts.getRawParameterValue("luxmaskLfoPosDepth")->load();
            lmc.velocity_coupling        = static_cast<int>(apvts.getRawParameterValue("luxmaskVelocityCoupling")->load());
            lmc.reference_note           = 24 + static_cast<int>(apvts.getRawParameterValue("luxmaskReferenceNote")->load());
            // M6 Phase 2 — settings shared per type, `enabled` per instance (presence).
            {
                const uint32_t mmask = chainMaskMask_.load(std::memory_order_relaxed);
                for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
                {
                    LuxMaskConfig c = lmc;
                    if (((mmask >> i) & 1u) == 0) c.enabled = 0;
                    lux_mask_instance(i)->config = c;
                }
            }
        }

        log_debug("VST", "Per-path routing: LS source=%d inv=%d ac=%d  |  LX source=%d inv=%d ac=%d gamma=%.2f",
                  g_sp3ctra_config.luxstral_source_type,
                  g_sp3ctra_config.luxstral_inversion,
                  g_sp3ctra_config.luxstral_ac_removal,
                  g_sp3ctra_config.luxsynth_source_type,
                  g_sp3ctra_config.luxsynth_inversion,
                  g_sp3ctra_config.luxsynth_ac_removal,
                  (double)g_sp3ctra_config.luxsynth_gamma_value);
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
