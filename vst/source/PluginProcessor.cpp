#include "PluginProcessor.h"
#include "PluginEditor.h"

// C headers still used directly by this file
extern "C" {
    #include "core/context.h"
    #include "config/config_loader.h"
    #include "utils/logger.h"
    #include "utils/rt_profiler.h"
    #include "synthesis/luxstral/synth_luxstral_algorithms.h" // update_gap_limiter_coefficients()
    #include "synthesis/luxstral/vst_adapters.h"              // luxstral_are_audio_buffers_ready(), buffers
    #include "synthesis/luxstral/wave_generation.h"           // request_frequency_reinit() hot-reload
    #include "processing/lux_pitch.h"                         // LuxPitch engine + g_lux_pitch
    #include "synthesis/luxsynth/luxsynth_vst_adapter.h"      // luxsynth_push_midi_event(), buffers, engine
    #include "synthesis/luxwave/luxwave_vst_adapter.h"        // luxwave_push_midi_event(), g_luxwave_engine
}
// Note: synth_luxstral_threading.h / synth_luxstral_runtime.h / AudioProcessingThread.h
// are now included transitively via Sp3ctraSharedCore.h and handled by Sp3ctraSharedCore.

// Global RT Profiler accessible from C threads (audioProcessingThread)
// This must be declared here (not in header) to avoid multiple definition errors
RTProfiler g_vst_rt_profiler = {};

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

    // ── Infrastructure — Stereo enable ───────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralStereoEnable", 1}, "Stereo Enable",
        true, kHiddenBool));

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
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthVolume", 1}, "LuxSynth Vol.",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    // ── Gameplay — Device On ─────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"deviceEnabled", 1}, "Device On", true));

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
        juce::NormalisableRange<float>(2.0f, 500.0f, 1.0f), 400.0f, kHiddenFloat));
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
        juce::ParameterID{"imageFreezeMode", 1}, "Freeze Mode", 0, 2, 0, kHiddenInt));

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
        juce::StringArray{"S - Sampler", "M - Mix", "L - Live", "P - LuxPitch"}, 1));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralInversion", 1}, "LuxStral Inversion", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralAcRemoval", 1}, "LuxStral DC Blocking", true));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxsynthSource", 1}, "LuxSynth Source",
        juce::StringArray{"S - Sampler", "M - Mix", "L - Live", "P - LuxPitch"}, 1));
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
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxpitchBackgroundMode", 1}, "LuxPitch Background",
        juce::StringArray{"Black", "White"}, 0, kHiddenChoice));
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
        0, 2, 0, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"samplerFadeInMs", 1}, "Sampler Fade-In",
        juce::NormalisableRange<float>(0.0f, 2000.0f, 10.0f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // rawFreezeMode: 0=PLAY, 1=HOLD (freeze last raw frame), 2=STOP (white)
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"rawFreezeMode", 1}, "RAW Freeze Mode",
        0, 2, 0, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"rawFadeInMs", 1}, "RAW Fade-In",
        juce::NormalisableRange<float>(0.0f, 2000.0f, 10.0f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // ── LuxSampler ──────────────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxSamplerEnabled", 1}, "LuxSampler Enabled",
        false, kHiddenBool));

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
        juce::NormalisableRange<float>(1.0f, 10.0f, 0.1f), 10.0f, kHiddenFloat));

    // ── FrameSequencer parameters ─────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"seqEnabled",  1}, "Sequencer Enabled", false, kHiddenBool));
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

    // ── Video Scroll — master toggle + live controls ───────────────────────────
    // Hidden from DAW automation (configuration/display parameters).
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoScrollEnabled", 1}, "Video Scroll Enabled",
        false, kHiddenBool));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollMode", 1}, "Video Scroll Mode",
        juce::StringArray{
            "0 deg", "90 deg", "180 deg", "270 deg"
        }, 0, kHiddenChoice));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollSpeed", 1}, "Video Scroll Speed",
        juce::NormalisableRange<float>(0.1f, 20.0f, 0.1f, 0.4f),
        3.0f, kHiddenFloat.withLabel("x")));

    // ── Video Scroll — display configuration (Settings window) ────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollZoom", 1}, "Video Scroll Zoom",
        juce::NormalisableRange<float>(0.5f, 4.0f, 0.05f),
        1.0f, kHiddenFloat.withLabel("x")));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollBrightness", 1}, "Video Brightness",
        juce::NormalisableRange<float>(0.1f, 3.0f, 0.05f),
        1.0f, kHiddenFloat.withLabel("x")));

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
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollSource", 1}, "Video Scroll Source",
        juce::StringArray{"L", "Sample", "Mix", "LuxPitch"}, 0));

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

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollMaxDuration", 1}, "Video Max Seq. Duration (s)",
        juce::NormalisableRange<float>(1.0f, 30.0f, 0.5f), 10.0f, kHiddenFloat));

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
    apvts.addParameterListener("luxpitchGlideMs",          this);
    apvts.addParameterListener("luxpitchLfoRate",          this);
    apvts.addParameterListener("luxpitchLfoDepth",         this);
    apvts.addParameterListener("luxpitchVelocityCoupling", this);
    apvts.addParameterListener("luxpitchSource",           this);
    apvts.addParameterListener("luxpitchMidiChannel",      this);
    apvts.addParameterListener("luxpitchOctaveOffset",     this);
    apvts.addParameterListener("luxpitchReferenceNote",    this);

    // Create LuxSampler (always active, no lazy init needed)
    luxSampler = std::make_unique<LuxSampler>();

    // Create FrameSequencer and wire it to the LuxSampler
    frameSequencer = std::make_unique<FrameSequencer>();
    frameSequencer->setLuxSampler(luxSampler.get());

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

    // Sync LuxSampler config with initial APVTS values
    luxSampler->setEnabled(*apvts.getRawParameterValue(PARAM_FS_ENABLED) > 0.5f);
    luxSampler->setMidiChannel(
        static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_MIDI_CH)) + 1);
    luxSampler->setOctaveOffset(
        static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_OCT_OFFSET)) - 2);
    luxSampler->setMaxDuration(*apvts.getRawParameterValue(PARAM_FS_MAX_DUR));

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
    
    // Initialize LuxPitch instances (both UI and processing thread)
    lux_pitch_init(&g_lux_pitch);
    lux_pitch_init(&g_lux_pitch_proc);

    // Just update g_sp3ctra_config with current APVTS defaults (no socket/buffer creation)
    applyConfigurationToCore(false);
    
    log_info("VST", "Sp3ctraAudioProcessor: Constructor complete (deferred init)");
    log_info("VST", "  - Shared core acquired (ref-count now %ld)",
             sharedCore.use_count());
    log_info("VST", "  - Pipeline start deferred to prepareToPlay()");
}

Sp3ctraAudioProcessor::~Sp3ctraAudioProcessor()
{
    log_info("VST", "=============================================================");
    log_info("VST", "Sp3ctraAudioProcessor: Destructor - Shutting down");
    log_info("VST", "=============================================================");

    // ── LuxSampler FIRST (uses AudioImageBuffers / DoubleBuffer owned by sharedCore) ──
    // Must stop before releasing sharedCore to avoid use-after-free.
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

    // ── LuxSampler player thread (per-instance, non-RT) ────────────────────
    if (luxSampler && sharedCore && sharedCore->getCore())
    {
        luxSampler->startPlayerThread(sharedCore->getCore()->getAudioImageBuffers(),
                                        sharedCore->getCore()->getDoubleBuffer());
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

    // ── LuxPitch MIDI (RT-safe: lock-free pitch shift from notes/pitch-bend) ──
    {
        const int lpCh  = static_cast<int>(apvts.getRawParameterValue("luxpitchMidiChannel")->load()) + 1;
        const int lpOct = static_cast<int>(apvts.getRawParameterValue("luxpitchOctaveOffset")->load()) - 2;
        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();
            if (msg.getChannel() != lpCh) continue;
            const int shifted = msg.getNoteNumber() + lpOct * 12;
            if (msg.isNoteOn())
            {
                lux_pitch_note_on(&g_lux_pitch, shifted, msg.getFloatVelocity());
                lux_pitch_note_on(&g_lux_pitch_proc, shifted, msg.getFloatVelocity());
            }
            else if (msg.isNoteOff())
            {
                lux_pitch_note_off(&g_lux_pitch, shifted);
                lux_pitch_note_off(&g_lux_pitch_proc, shifted);
            }
            else if (msg.isPitchWheel())
            {
                float bend = (msg.getPitchWheelValue() - 8192) / 8192.0f;
                lux_pitch_set_pitch_bend(&g_lux_pitch, bend);
                lux_pitch_set_pitch_bend(&g_lux_pitch_proc, bend);
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

    // ── Sequencer-gated recording: update LuxSampler gate slot ─────────────
    // seqGateSlot = bank the sequencer is currently playing, or -1 (no gate).
    // Runs every audio block; onFrameAssembled() reads it atomically.
    if (luxSampler != nullptr)
    {
        int gateSlot = -1; // no gate: sequencer off / stopped / passthrough step
        if (frameSequencer != nullptr
            && frameSequencer->isEnabled()
            && frameSequencer->isPlaying())
        {
            const int curStep = frameSequencer->getCurrentStep();
            if (curStep >= 0)
                gateSlot = frameSequencer->getStep(curStep); // -1 = passthrough step
        }
        luxSampler->setSeqGateSlot(gateSlot);
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
    // Gated by deviceEnabled (LuxStral toggle). LuxSynth has its own gate.
    // ========================================================================
    const bool luxstralEnabled = (deviceEnabledParam == nullptr || deviceEnabledParam->load() >= 0.5f);
    if (luxstralEnabled && sharedCore && sharedCore->isReady() && luxstral_are_audio_buffers_ready()) {
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
                
                // Track which buffer we consumed (don't signal twice for same data)
                lastConsumedReadIdx = readIdx;
                
                // Signal producer that it can generate the next buffer
                // DO NOT set ready=0 — producer manages ready flags
                luxstral_signal_buffer_consumed();
            }
        } else if (leftReady && rightReady) {
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
    // Persist the last session path so it survives DAW project reloads
    // and Standalone restarts (setStateInformation restores it).
    if (lastSessionPath.isNotEmpty())
        state.setProperty("lastSessionPath", lastSessionPath, nullptr);
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
            // Restore last session path — SamplerPageComponent reads this
            // on construction to auto-reload the session.
            lastSessionPath = apvts.state
                .getProperty("lastSessionPath", "").toString();
            log_info("VST", "State restored from DAW project");
            
            // On state restore, just update g_sp3ctra_config.
            // The actual pipeline start (if needed) happens in prepareToPlay().
            applyConfigurationToCore(false);

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
    
    // 🔧 CRITICAL: LuxStral parameters are automatically synced to g_sp3ctra_config
    // They are read directly by the synthesis engine, NO restart needed!
    // StrokeForge parameters — same hot-reload pattern as LuxStral
    // LuxSampler parameters — update atomic config on LuxSampler
    if (parameterID.startsWith("luxSampler"))
    {
        if (luxSampler != nullptr)
        {
            luxSampler->setEnabled(
                *apvts.getRawParameterValue(PARAM_FS_ENABLED) > 0.5f);
            luxSampler->setMidiChannel(
                static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_MIDI_CH)) + 1);
            luxSampler->setOctaveOffset(
                static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_OCT_OFFSET)) - 2);
            luxSampler->setMaxDuration(
                *apvts.getRawParameterValue(PARAM_FS_MAX_DUR));
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
    /* Always enable blob detection so the BlobVisualizerComponent gets data.   */
    /* sfEnabled controls only the StrokeForge synthesis application.            */
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
        static const int kChoiceToSource[4] = { 0, 2, 1, 3 }; // S→0, M→2, L→1, P→3

        int lsChoice = static_cast<int>(
            apvts.getRawParameterValue("luxstralSource")->load());
        if (lsChoice < 0 || lsChoice > 3) lsChoice = 1; // default M
        g_sp3ctra_config.luxstral_source_type = kChoiceToSource[lsChoice];
        g_sp3ctra_config.luxstral_inversion   =
            static_cast<int>(apvts.getRawParameterValue("luxstralInversion")->load());
        g_sp3ctra_config.luxstral_ac_removal  =
            static_cast<int>(apvts.getRawParameterValue("luxstralAcRemoval")->load());

        int lxChoice = static_cast<int>(
            apvts.getRawParameterValue("luxsynthSource")->load());
        if (lxChoice < 0 || lxChoice > 3) lxChoice = 1;
        g_sp3ctra_config.luxsynth_source_type = kChoiceToSource[lxChoice];
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

        // ── Sync LuxPitch config to BOTH instances (UI + processing thread) ──
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
            lpc.glide_time_ms           = apvts.getRawParameterValue("luxpitchGlideMs")->load();
            lpc.lfo_rate_hz             = apvts.getRawParameterValue("luxpitchLfoRate")->load();
            lpc.lfo_depth_semitones     = apvts.getRawParameterValue("luxpitchLfoDepth")->load();
            lpc.velocity_coupling       = static_cast<int>(apvts.getRawParameterValue("luxpitchVelocityCoupling")->load());
            lpc.reference_note          = 24 + static_cast<int>(apvts.getRawParameterValue("luxpitchReferenceNote")->load());
            g_lux_pitch.config      = lpc;  /* UI / visualizer thread */
            g_lux_pitch_proc.config = lpc;  /* processing / synthesis thread */
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
