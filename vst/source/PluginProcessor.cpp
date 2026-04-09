#include "PluginProcessor.h"
#include "PluginEditor.h"

// C headers still used directly by this file
extern "C" {
    #include "core/context.h"
    #include "config/config_loader.h"
    #include "utils/logger.h"
    #include "utils/rt_profiler.h"
    #include "luxstral/synth_luxstral_algorithms.h" // update_gap_limiter_coefficients()
    #include "luxstral/vst_adapters.h"              // luxstral_are_audio_buffers_ready(), buffers
    #include "luxstral/wave_generation.h"           // request_frequency_reinit() hot-reload
}
// Note: synth_luxstral_threading.h / synth_luxstral_runtime.h / AudioProcessingThread.h
// are now included transitively via Sp3ctraSharedCore.h and handled by Sp3ctraSharedCore.

// Global RT Profiler accessible from C threads (audioProcessingThread)
// This must be declared here (not in header) to avoid multiple definition errors
RTProfiler g_vst_rt_profiler = {0};

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
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f), 4.8f));
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
        juce::NormalisableRange<float>(0.1f, 3.0f, 0.1f), 2.0f));
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

    // ── Gameplay — Master Volume ──────────────────────────────────────────────
    // Applied as output gain in processBlock() — RT-safe atomic read, no lock.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"masterVolume", 1}, "Volume",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    // ── Gameplay — Device On ─────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"deviceEnabled", 1}, "Device On", true));

    // ── Gameplay — StrokeForge enable ────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sfEnabled", 1}, "SF Active", false));

    // ── Gameplay — Blob Threshold ────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfBlobBaseThreshold", 1}, "Blob Thr.",
        juce::NormalisableRange<float>(0.01f, 0.2f, 0.001f), 0.03f));

    // ── Infrastructure — SF internal parameters ───────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sfBlobContrastAdaptive", 1}, "SF Contrast Adaptive",
        true, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfBlobContrastSensitivity", 1}, "SF Contrast Sensitivity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"sfBlobMinWidth", 1}, "SF Blob Min Width",
        1, 50, 20, kHiddenInt));

    // ── Gameplay — Merge Gap ─────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"sfBlobMergeGap", 1}, "Merge Gap", 0, 20, 5));

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

    // ── FrameSampler ──────────────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"frameSamplerEnabled", 1}, "FrameSampler Enabled",
        false, kHiddenBool));

    {
        juce::StringArray midiChannelNames;
        for (int i = 1; i <= 16; ++i)
            midiChannelNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"frameSamplerMidiChannel", 1}, "FrameSampler MIDI Channel",
            midiChannelNames, 0, kHiddenChoice));  // default = Channel 1 (index 0)
    }

    {
        juce::StringArray octaveNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"frameSamplerOctaveOffset", 1}, "FrameSampler Octave Offset",
            octaveNames, 2, kHiddenChoice));  // default index 2 = 0
    }

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"frameSamplerMaxDuration", 1}, "FrameSampler Max Duration",
        juce::NormalisableRange<float>(1.0f, 10.0f, 0.1f), 10.0f, kHiddenFloat));

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
    apvts.addParameterListener("sfBlobBaseThreshold", this);
    apvts.addParameterListener("sfBlobContrastAdaptive", this);
    apvts.addParameterListener("sfBlobContrastSensitivity", this);
    apvts.addParameterListener("sfBlobMinWidth", this);
    apvts.addParameterListener("sfBlobMergeGap", this);
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
    
    // Create FrameSampler (always active, no lazy init needed)
    frameSampler = std::make_unique<FrameSampler>();

    // Register FrameSampler parameter listeners
    apvts.addParameterListener(PARAM_FS_ENABLED,    this);
    apvts.addParameterListener(PARAM_FS_MIDI_CH,    this);
    apvts.addParameterListener(PARAM_FS_OCT_OFFSET, this);
    apvts.addParameterListener(PARAM_FS_MAX_DUR,    this);

    // Sync FrameSampler config with initial APVTS values
    frameSampler->setEnabled(*apvts.getRawParameterValue(PARAM_FS_ENABLED) > 0.5f);
    frameSampler->setMidiChannel(
        static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_MIDI_CH)) + 1);
    frameSampler->setOctaveOffset(
        static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_OCT_OFFSET)) - 2);
    frameSampler->setMaxDuration(*apvts.getRawParameterValue(PARAM_FS_MAX_DUR));

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

    // ── FrameSampler FIRST (uses AudioImageBuffers / DoubleBuffer owned by sharedCore) ──
    // Must stop before releasing sharedCore to avoid use-after-free.
    if (frameSampler)
    {
        log_info("VST", "Stopping FrameSampler player thread...");
        frameSampler->stopPlayerThread();
        frameSampler.reset();
        log_info("VST", "FrameSampler stopped");
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

    // ── FrameSampler player thread (per-instance, non-RT) ────────────────────
    if (frameSampler && sharedCore && sharedCore->getCore())
    {
        frameSampler->startPlayerThread(sharedCore->getCore()->getAudioImageBuffers(),
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

    // ── FrameSampler MIDI (RT-safe: atomics only, no alloc, no lock, no I/O) ──
    if (frameSampler != nullptr)
        frameSampler->processMidi(midiMessages);

    // RT-safe early exit when device is switched off (atomic read, no lock)
    if (deviceEnabledParam != nullptr && deviceEnabledParam->load() < 0.5f)
    {
        rt_profiler_callback_end(&g_vst_rt_profiler);
        return;
    }

    // ========================================================================
    // 🎯 LOCK-FREE DOUBLE-BUFFER CONSUMER (RT-SAFE)
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
    // ========================================================================
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
                if (totalNumOutputChannels >= 1) {
                    float* destLeft = buffer.getWritePointer(0);
                    for (int i = 0; i < samplesToRead; ++i)
                        destLeft[i] = leftData[i];
                }
                if (totalNumOutputChannels >= 2) {
                    float* destRight = buffer.getWritePointer(1);
                    for (int i = 0; i < samplesToRead; ++i)
                        destRight[i] = rightData[i];
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
                if (totalNumOutputChannels >= 1) {
                    float* destLeft = buffer.getWritePointer(0);
                    for (int i = 0; i < samplesToRead; ++i)
                        destLeft[i] = leftData[i];
                }
                if (totalNumOutputChannels >= 2) {
                    float* destRight = buffer.getWritePointer(1);
                    for (int i = 0; i < samplesToRead; ++i)
                        destRight[i] = rightData[i];
                }
            }
            // DO NOT signal consumed — producer is still working on the next buffer
        } else {
            // 🔇 No data ready at all (startup or after long pause)
            // Buffer already cleared — silence is appropriate here
            rt_profiler_report_buffer_miss_luxstral(&g_vst_rt_profiler);
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
    // APVTS handles serialization automatically via ValueTree
    auto state = apvts.copyState();
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
    // FrameSampler parameters — update atomic config on FrameSampler
    if (parameterID.startsWith("frameSampler"))
    {
        if (frameSampler != nullptr)
        {
            frameSampler->setEnabled(
                *apvts.getRawParameterValue(PARAM_FS_ENABLED) > 0.5f);
            frameSampler->setMidiChannel(
                static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_MIDI_CH)) + 1);
            frameSampler->setOctaveOffset(
                static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_OCT_OFFSET)) - 2);
            frameSampler->setMaxDuration(
                *apvts.getRawParameterValue(PARAM_FS_MAX_DUR));
        }
        return;
    }

    bool isStrokeForgeParam = parameterID.startsWith("sf");
    if (isStrokeForgeParam) {
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
    } else {
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
    // 6 active parameters; legacy IDs kept in APVTS for preset compatibility
    // but no longer mapped to g_sp3ctra_config.
    // ========================================================================
    g_sp3ctra_config.strokeforge_enabled =
        (int)apvts.getRawParameterValue("sfEnabled")->load();
    g_sp3ctra_config.strokeforge_blob_base_threshold =
        apvts.getRawParameterValue("sfBlobBaseThreshold")->load();
    g_sp3ctra_config.strokeforge_blob_min_width =
        (int)apvts.getRawParameterValue("sfBlobMinWidth")->load();
    g_sp3ctra_config.strokeforge_blob_merge_gap =
        (int)apvts.getRawParameterValue("sfBlobMergeGap")->load();
    g_sp3ctra_config.strokeforge_morph_width_scale =
        apvts.getRawParameterValue("sfMorphWidthScale")->load();
    g_sp3ctra_config.strokeforge_blob_focus_sigma =
        apvts.getRawParameterValue("sfBlobFocusSigma")->load();
    g_sp3ctra_config.strokeforge_spectral_width_threshold =
        apvts.getRawParameterValue("sfSpectralWidthThreshold")->load();
    g_sp3ctra_config.strokeforge_focus_only =
        (int)apvts.getRawParameterValue("sfFocusOnly")->load();

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
