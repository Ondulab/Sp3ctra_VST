#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "AudioProcessingThread.h"  // Separate thread for synth_AudioProcess

// Include C headers for global config access
extern "C" {
    #include "core/context.h"
    #include "utils/logger.h"
    #include "utils/rt_profiler.h"
    #include "luxstral/synth_luxstral.h"           // LuxStral synthesis engine
    #include "luxstral/synth_luxstral_algorithms.h" // Envelope coefficient update
    #include "luxstral/vst_adapters.h"             // Audio buffer init functions
    #include "luxstral/wave_generation.h"          // Hot-reload frequency API
}

// Global RT Profiler accessible from C threads (audioProcessingThread)
// This must be declared here (not in header) to avoid multiple definition errors
RTProfiler g_vst_rt_profiler = {0};

//==============================================================================
// Create parameter layout (called once during construction)
juce::AudioProcessorValueTreeState::ParameterLayout Sp3ctraAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    
    // UDP Port (1024 - 65535)
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        PARAM_UDP_PORT,
        "UDP Port",
        1024, 65535,
        Sp3ctraConstants::DEFAULT_UDP_PORT
    ));
    
    // UDP Address - 4 separate bytes (0-255 each)
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        PARAM_UDP_BYTE1, "UDP Byte 1", 0, 255, 192));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        PARAM_UDP_BYTE2, "UDP Byte 2", 0, 255, 168));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        PARAM_UDP_BYTE3, "UDP Byte 3", 0, 255, 100));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        PARAM_UDP_BYTE4, "UDP Byte 4", 0, 255, 10));
    
    // Sensor DPI (200 or 400)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        PARAM_SENSOR_DPI,
        "Sensor DPI",
        juce::StringArray{"200 DPI", "400 DPI"},
        1  // Default = 400 DPI
    ));
    
    // Log Level
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        PARAM_LOG_LEVEL,
        "Log Level",
        juce::StringArray{"Error", "Warning", "Info", "Debug"},
        Sp3ctraConstants::DEFAULT_LOG_LEVEL  // Default = Info (2)
    ));
    
    // Visualizer Mode (0 = Image, 1 = Waveform, 2 = Inverted Waveform)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        PARAM_VISUALIZER_MODE,
        "Visualizer Mode",
        juce::StringArray{"Image", "Waveform", "Inverted Waveform"},
        2  // Default = Inverted Waveform mode
    ));
    
    // ========================================================================
    // LUXSTRAL SYNTHESIS PARAMETERS
    // Musical approach: Tuning + Root Note + Num Octaves
    // This eliminates "jumps" when changing frequency range continuously
    // ========================================================================
    
    // Tuning (A4 reference frequency, standard = 440 Hz)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "luxstralTuning",
        "LuxStral Tuning (A4)",
        juce::NormalisableRange<float>(415.0f, 466.0f, 0.1f),  // A4 baroque to A4 sharp
        440.0f,  // Standard concert pitch
        "Hz"
    ));
    
    // Root Note (MIDI note number: C0=12, C1=24, C2=36, ..., C8=108)
    // ComboBox with all chromatic notes from C1 to C6
    juce::StringArray noteNames;
    const char* noteLetters[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    for (int octave = 1; octave <= 6; octave++) {
        for (int note = 0; note < 12; note++) {
            noteNames.add(juce::String(noteLetters[note]) + juce::String(octave));
        }
    }
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        "luxstralRootNote",
        "LuxStral Root Note",
        noteNames,
        12  // Default: C2 (index 12 = C1 is 0, so C2 is index 12)
    ));
    
    // Number of Octaves (integer, 1-10)
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        "luxstralNumOctaves",
        "LuxStral Num Octaves",
        1, 10,
        8  // Default: 8 octaves
    ));
    
    // Envelope Parameters (very fast response for LuxStral)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "luxstralAttackMs",
        "LuxStral Attack Time",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f),  // Skewed, 0.5ms to 5000ms
        0.5f,  // tau_up_base_ms = 0.5 (as specified in config)
        "ms"
    ));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "luxstralReleaseMs",
        "LuxStral Release Time",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f),  // Skewed, 0.5ms to 5000ms
        0.5f,  // tau_down_base_ms = 0.5 (as specified in config)
        "ms"
    ));
    
    // Image Processing - LuxStral pipeline: RGB → Grayscale → Inversion → Gamma → Averaging → Contrast
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "luxstralInvertIntensity",
        "LuxStral Invert Intensity (dark pixels louder)",
        true  // invert_intensity = 1 (as specified in config)
    ));
    
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "luxstralGammaEnable",
        "LuxStral Gamma Correction Enable",
        true  // enable_non_linear_mapping = 1 (as specified in config)
    ));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "luxstralGammaValue",
        "LuxStral Gamma Value",
        juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f),
        4.8f,  // gamma_value = 4.8 (as specified in config)
        ""
    ));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "luxstralContrastMin",
        "LuxStral Contrast Min",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f),
        0.21f,  // contrast_min = 0.21 (as specified in config)
        ""
    ));
    
    // Stereo Processing
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "luxstralStereoEnable",
        "LuxStral Stereo Mode Enable",
        true  // stereo_mode_enabled = 1 (as specified in config)
    ));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "luxstralStereoTempAmp",
        "LuxStral Stereo Temperature Amplification",
        juce::NormalisableRange<float>(0.0f, 5.0f, 0.01f),
        2.5f,  // stereo_temperature_amplification = 2.5
        ""
    ));
    
    // Dynamics Processing (summation_normalization)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "luxstralVolumeWeightingExp",
        "LuxStral Volume Weighting Exponent",
        juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f),
        0.1f,  // volume_weighting_exponent = 0.1 (strong domination)
        ""
    ));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "luxstralSummationResponseExp",
        "LuxStral Summation Response Exponent",
        juce::NormalisableRange<float>(0.1f, 3.0f, 0.1f),
        2.0f,  // summation_response_exponent = 2.0
        ""
    ));
    
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "luxstralNoiseGateThreshold",
        "LuxStral Noise Gate Threshold",
        juce::NormalisableRange<float>(0.0f, 0.1f, 0.001f),
        0.005f,  // noise_gate_threshold = 0.005
        ""
    ));
    
    // Performance
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        "luxstralNumWorkers",
        "LuxStral Worker Threads",
        1, 8,
        8  // num_workers = 8 (as specified in config)
    ));
    
    // Physiological Filter (Equal-Loudness Compensation)
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        "luxstralPhysiologicalFilter",
        "LuxStral Equal-Loudness Compensation",
        false  // Disabled by default (flat response)
    ));

    // Physiological correction depth (0.0 = no correction, 1.0 = full A-weighting inverse)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        "luxstralPhysiologicalDepth",
        "LuxStral Equal-Loudness Depth",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f),
        0.5f,  // Default: 50% of full A-weighting inverse
        ""
    ));
    
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
    visualizerModeParam = apvts.getRawParameterValue(PARAM_VISUALIZER_MODE);
    
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
    
    // Create Sp3ctra core (but do NOT initialize yet - lazy init)
    sp3ctraCore = std::make_unique<Sp3ctraCore>();
    
    // 🔧 LAZY INITIALIZATION: Do NOT start UDP here!
    // The DAW will call setStateInformation() with saved parameters BEFORE prepareToPlay().
    // If we init now with default params, we'd have to shutdown and reinit when state is restored.
    // Instead, we defer initialization to setStateInformation() or prepareToPlay() (whichever comes first).
    
    // Just update g_sp3ctra_config with APVTS values (no socket/buffer creation)
    applyConfigurationToCore(false);  // false = don't call sp3ctraCore->initialize()
    
    log_info("VST", "Sp3ctraAudioProcessor: Constructor complete (deferred init)");
    log_info("VST", "  - Waiting for DAW state restoration or prepareToPlay()");
    log_info("VST", "  - Parameters managed by APVTS (saved in DAW project)");
}

Sp3ctraAudioProcessor::~Sp3ctraAudioProcessor()
{
    log_info("VST", "=============================================================");
    log_info("VST", "Sp3ctraAudioProcessor: Destructor - Shutting down");
    log_info("VST", "=============================================================");
    
    // 🎵 CRITICAL: Stop audio processing thread FIRST (before UDP and LuxStral cleanup)
    // This thread calls synth_AudioProcess() which accesses audio buffers
    if (audioProcessingThread) {
        log_info("VST", "Stopping AudioProcessingThread...");
        audioProcessingThread->requestStop();
        audioProcessingThread->stopThread(2000);  // 2 second timeout
        audioProcessingThread.reset();
        log_info("VST", "AudioProcessingThread stopped");
    }
    
    // Stop UDP thread (blocks until thread exits)
    if (udpThread) {
        log_info("VST", "Stopping UDP thread...");
        udpThread->requestStop();
        udpThread->stopThread(2000);  // 2 second timeout
        udpThread.reset();
        log_info("VST", "UDP thread stopped");
    }
    
    // Cleanup LuxStral engine (AFTER both threads are stopped!)
    if (luxstralInitialized) {
        log_info("VST", "Cleaning up LuxStral engine...");
        synth_luxstral_cleanup();
        luxstralInitialized = false;
        log_info("VST", "LuxStral cleanup complete");
    }
    
    // Cleanup core (closes socket, frees buffers)
    if (sp3ctraCore) {
        log_info("VST", "Shutting down core...");
        sp3ctraCore->shutdown();
        sp3ctraCore.reset();
        log_info("VST", "Core shutdown complete");
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
    // CRITICAL: Do NOT disable entire editor - it breaks Metal shader compilation!
    if (auto* editor = dynamic_cast<Sp3ctraAudioProcessorEditor*>(getActiveEditor())) {
        editor->suspendVisualizer();
    }
    
    log_info("VST", "=============================================================");
    log_info("VST", "prepareToPlay - SR=%.1f Hz, BS=%d samples", sampleRate, samplesPerBlock);
    
    // 🔧 LAZY INIT: If Core not yet initialized (new plugin, no saved state),
    // initialize now with default APVTS parameters
    if (coreNeedsInit) {
        log_info("VST", "First-time Core initialization (new plugin, no saved state)...");
        
        // Initialize Core with default parameters (creates buffers + UDP socket)
        applyConfigurationToCore(true);  // true = full init
        
        // Start UDP receiver thread (socket already created by applyConfigurationToCore)
        udpThread = std::make_unique<UdpReceiverThread>(sp3ctraCore.get());
        udpThread->startThread();
        
        coreNeedsInit = false;
        
        log_info("VST", "Core initialized - UDP listening on %s:%d",
            getUdpAddressString().toRawUTF8(),
            (int)udpPortParam->load());
    }
    
    // Update global config with audio parameters
    extern sp3ctra_config_t g_sp3ctra_config;
    
    // Check if sample rate changed (important for Nyquist frequency clamp)
    int oldSampleRate = g_sp3ctra_config.sampling_frequency;
    
    // Set audio parameters
    g_sp3ctra_config.sampling_frequency = (int)sampleRate;
    g_sp3ctra_config.audio_buffer_size = samplesPerBlock;
    
    // ========================================================================
    // 🔧 ADAPTIVE PERFORMANCE FIX: Detect sample rate / buffer size issues
    // ========================================================================
    // Calculate available time budget per buffer (in microseconds)
    double bufferDurationUs = (samplesPerBlock / sampleRate) * 1000000.0;
    
    // Empirical threshold: synthesis takes ~2200µs per buffer with 8 workers
    // This is safe at 48kHz (2667µs budget) but overloaded at 96kHz (1333µs budget)
    const double SYNTHESIS_TIME_ESTIMATE_US = 2200.0;
    double loadRatio = SYNTHESIS_TIME_ESTIMATE_US / bufferDurationUs;
    
    if (loadRatio > 1.0) {
        log_warning("VST", "⚠️  PERFORMANCE WARNING: Sample rate too high for current buffer size!");
        log_warning("VST", "    Sample Rate: %.0f Hz, Buffer Size: %d samples", sampleRate, samplesPerBlock);
        log_warning("VST", "    Budget per buffer: %.0f µs, Estimated synthesis time: %.0f µs", 
                    bufferDurationUs, SYNTHESIS_TIME_ESTIMATE_US);
        log_warning("VST", "    Load ratio: %.1f%% (synthesis takes %.1fx available time)", 
                    loadRatio * 100.0, loadRatio);
        log_warning("VST", "");
        log_warning("VST", "🔧 RECOMMENDED FIXES:");
        log_warning("VST", "    1. Increase DAW buffer size to %.0f samples or more", 
                    SYNTHESIS_TIME_ESTIMATE_US * sampleRate / 1000000.0);
        log_warning("VST", "    2. Or reduce sample rate to 48 kHz");
        log_warning("VST", "    3. Or reduce LuxStral worker threads from 8 to 4-6");
        log_warning("VST", "");
    } else {
        log_info("VST", "✅ Performance headroom: %.0f µs budget, ~%.0f µs synthesis (%.1f%% load)", 
                 bufferDurationUs, SYNTHESIS_TIME_ESTIMATE_US, loadRatio * 100.0);
    }
    
    // Initialize RT profiler for performance monitoring
    rt_profiler_init(&g_vst_rt_profiler, (int)sampleRate, samplesPerBlock);
    
    // Enable profiler only if log level is Debug
    bool enableProfiler = (g_sp3ctra_config.log_level == LOG_LEVEL_DEBUG);
    rt_profiler_set_enabled(&g_vst_rt_profiler, enableProfiler ? 1 : 0);
    
    if (enableProfiler) {
        log_info("VST", "RT Profiler enabled (log level = Debug) - reporting every 500 frames");
    }
    
    // Musical scale parameters (required for wave generation)
    g_sp3ctra_config.semitone_per_octave = 12;  // Standard musical scale
    g_sp3ctra_config.comma_per_semitone = 36;   // Default granularity
    
    // 🔧 CRITICAL: Recalculate frequencies with new sample rate (Nyquist clamp)
    if (oldSampleRate != (int)sampleRate) {
        log_info("VST", "Sample rate changed from %d to %d Hz - recalculating frequencies", 
                 oldSampleRate, (int)sampleRate);
        applyConfigurationToCore(false);  // Recalculate with new Nyquist limit
    }
    
    // 🛑 CRITICAL FIX: Stop AudioProcessingThread WITHOUT touching the worker pool!
    // The worker pool uses MAX_BUFFER_SIZE for its buffers and does NOT need to be restarted.
    // Only luxstral_buffers_L/R need to be reallocated (done by luxstral_init_audio_buffers).
    if (audioProcessingThread) {
        log_info("VST", "Stopping AudioProcessingThread for buffer reallocation...");
        
        // 🔧 SIMPLIFIED: Just stop the audio processing thread
        // DO NOT signal workers to exit! They stay alive and ready for the new thread.
        audioProcessingThread->requestStop();
        audioProcessingThread->stopThread(2000);
        audioProcessingThread.reset();
        
        log_info("VST", "AudioProcessingThread stopped (worker pool untouched)");
    }
    
    // STATIC ALLOCATION: Buffers are pre-allocated for MAX_BUFFER_SIZE (4096)
    // NO cleanup/reinit needed! Buffers already exist and are large enough
    // This prevents crashes when DAW changes buffer size (256 → 512 → 1024, etc.)
    
    // Always (re)initialize audio buffers if buffer size changed
    if (luxstral_init_audio_buffers(samplesPerBlock) != 0) {
        log_error("VST", "Failed to initialize audio buffers");
        return;
    }

    // Initialize LuxStral on first call only
    if (!luxstralInitialized) {
        log_info("VST", "First-time initialization of LuxStral...");
        
        // Initialize callback synchronization system
        luxstral_init_callback_sync();
        
        // Initialize LuxStral synthesis engine
        int result = synth_IfftInit();
        
        if (result == 0) {
            luxstralInitialized = true;
            log_info("VST", "LuxStral initialized successfully");
        } else {
            log_error("VST", "LuxStral initialization FAILED");
            return;
        }
    } else {
        log_info("VST", "LuxStral already initialized");
    }
    
    // Restart audio processing thread with new buffer size
    // 🔧 RT PRIORITY: Use JUCE's highest thread priority
    // This ensures the audio synthesis thread runs with highest priority to avoid glitches
    log_info("VST", "Starting AudioProcessingThread with RT priority...");
    audioProcessingThread = std::make_unique<AudioProcessingThread>(sp3ctraCore.get());
    audioProcessingThread->startThread(juce::Thread::Priority::highest);  // Maximum priority
    log_info("VST", "AudioProcessingThread started with Priority::highest");
    
    log_info("VST", "=============================================================");
    
    // 🛡️ PROTECTION: Resume visualizer now that reconfiguration is complete
    if (auto* editor = dynamic_cast<Sp3ctraAudioProcessorEditor*>(getActiveEditor())) {
        editor->resumeVisualizer();
    }
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
    
    // Only read audio if LuxStral engine is initialized
    // CRITICAL: synth_AudioProcess() is called by AudioProcessingThread, NOT here!
    // This callback must be RT-safe: NO blocking calls, NO nanosleep(), NO mutex locks
    if (luxstralInitialized && sp3ctraCore && luxstral_are_audio_buffers_ready()) {
        // Get audio data from LuxStral synthesis buffers (filled by AudioProcessingThread)
        extern AudioImageBuffer luxstral_buffers_L[2];
        extern AudioImageBuffer luxstral_buffers_R[2];
        extern volatile int luxstral_buffer_index;
        
        // Get read buffer (opposite of write buffer)
        int readIdx = 1 - luxstral_buffer_index;
        
        // RT Profiler: Report buffer miss if data not ready
        if (!luxstral_buffers_L[readIdx].ready || !luxstral_buffers_R[readIdx].ready) {
            rt_profiler_report_buffer_miss_luxstral(&g_vst_rt_profiler);
        }
        
        // Check if we have ready audio data from LuxStral synthesis
        if (luxstral_buffers_L[readIdx].ready && luxstral_buffers_R[readIdx].ready) {
            float* leftData = luxstral_buffers_L[readIdx].data;
            float* rightData = luxstral_buffers_R[readIdx].data;
            
            if (leftData && rightData) {
                // Copy LuxStral stereo audio to JUCE buffer
                if (totalNumOutputChannels >= 1) {
                    float* destLeft = buffer.getWritePointer(0);
                    for (int i = 0; i < numSamples; ++i) {
                        destLeft[i] = leftData[i];
                    }
                }
                
                if (totalNumOutputChannels >= 2) {
                    float* destRight = buffer.getWritePointer(1);
                    for (int i = 0; i < numSamples; ++i) {
                        destRight[i] = rightData[i];
                    }
                }
                
                // Mark buffer as consumed (atomic-safe)
                luxstral_buffers_L[readIdx].ready = 0;
                luxstral_buffers_R[readIdx].ready = 0;
                
                // 🎯 VST SYNCHRONIZATION FIX: Signal audioProcessingThread that buffer was consumed
                // This wakes up the synthesis thread so it can generate the next buffer
                luxstral_signal_buffer_consumed();
            }
        }
        // If no data ready, buffer stays silent (already cleared)
    }
    // No fallback tone - silence when no data available
    
    rt_profiler_callback_end(&g_vst_rt_profiler);
    
    juce::ignoreUnused(midiMessages);
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
            
            // 🔧 LAZY INIT: First-time initialization with restored parameters
            if (coreNeedsInit) {
                log_info("VST", "First-time Core initialization with restored settings...");
                
                // Initialize Core with restored parameters (creates buffers + UDP socket)
                applyConfigurationToCore(true);  // true = full init
                
                // Start UDP receiver thread
                udpThread = std::make_unique<UdpReceiverThread>(sp3ctraCore.get());
                udpThread->startThread();
                
                coreNeedsInit = false;
                
                log_info("VST", "Core initialized - UDP listening on %s:%d",
                    getUdpAddressString().toRawUTF8(),
                    (int)udpPortParam->load());
            } else {
                // Already initialized - just restart UDP if config changed
                if (udpThread) {
                    log_info("VST", "Restarting UDP with restored settings...");
                    udpThread->requestStop();
                    udpThread->stopThread(2000);
                    udpThread.reset();
                }
                
                // Update config (no buffer reinit needed)
                applyConfigurationToCore(false);
                
                // 🔧 FIX: Restart UDP socket with restored config (buffers untouched)
                if (!sp3ctraCore->restartUdp(
                        (int)udpPortParam->load(),
                        getUdpAddressString().toStdString(),
                        ""  // multicast interface - auto-detect
                    )) {
                    log_error("VST", "Failed to restart UDP with restored config!");
                }
                
                // Restart UDP thread AFTER socket is created
                udpThread = std::make_unique<UdpReceiverThread>(sp3ctraCore.get());
                udpThread->startThread();
                
                log_info("VST", "UDP restarted with %s:%d",
                    getUdpAddressString().toRawUTF8(),
                    (int)udpPortParam->load());
            }
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
        
        log_info("VST", "UDP parameter changed - restarting socket...");
        
        // 🔧 CRITICAL FIX: requestStop() FIRST, then close socket to unblock recvfrom()
        if (udpThread) {
            udpThread->requestStop();  // Sets ctx->running = 0
        }

        // Close socket completely (not just shutdown) to force recvfrom() exit
        if (sp3ctraCore) {
            sp3ctraCore->closeUdpSocket();
        }

        // Wait for thread to exit
        if (udpThread) {
            udpThread->stopThread(1500);
            udpThread.reset();
        }
        
        // Update g_sp3ctra_config with new UDP parameters
        applyConfigurationToCore(false);
        
        // 🔧 FIX: Restart UDP socket with new config (buffers untouched)
        // This closes the old socket and creates a new one with updated port/address
        if (!sp3ctraCore->restartUdp(
                (int)udpPortParam->load(),
                getUdpAddressString().toStdString(),
                ""  // multicast interface - auto-detect
            )) {
            log_error("VST", "Failed to restart UDP with new config!");
        }
        
        // Restart UDP thread AFTER socket is created
        udpThread = std::make_unique<UdpReceiverThread>(sp3ctraCore.get());
        udpThread->startThread();
        
        log_info("VST", "UDP restarted with %s:%d (buffers untouched)",
            getUdpAddressString().toRawUTF8(),
            (int)udpPortParam->load());
    } else {
        // For other non-UDP, non-LuxStral parameters (sensor DPI, log level, visualizer mode)
        applyConfigurationToCore(false);  // needsSocketRestart = false
        
        // Enable/disable RT profiler dynamically based on log level
        if (parameterID == PARAM_LOG_LEVEL) {
            extern sp3ctra_config_t g_sp3ctra_config;
            bool enableProfiler = (g_sp3ctra_config.log_level == LOG_LEVEL_DEBUG);
            rt_profiler_set_enabled(&g_vst_rt_profiler, enableProfiler ? 1 : 0);
            
            if (enableProfiler) {
                log_info("VST", "RT Profiler enabled (log level = Debug)");
            } else {
                log_info("VST", "RT Profiler disabled (log level < Debug)");
            }
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
    
    // 🔧 CRITICAL FIX: Initialize pixels_per_note (required for LuxStral)
    // Default value = 1 (maximum resolution: 1 pixel = 1 note/comma)
    g_sp3ctra_config.pixels_per_note = 1;
    
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
    
    // Update logger level immediately
    logger_init((log_level_t)logLevel);
    
    // 🔧 CRITICAL: Only initialize Sp3ctraCore if socket restart is needed
    // This prevents destroying buffers while UDP thread is using them!
    if (needsSocketRestart && sp3ctraCore) {
        // Create ActiveConfig for Sp3ctraCore
        Sp3ctraCore::ActiveConfig config;
        config.udpPort = udpPort;
        config.udpAddress = udpAddress.toStdString();
        config.multicastInterface = "";  // Auto-detect
        config.logLevel = logLevel;
        
        // Apply to core (this will restart UDP socket and reinit buffers)
        if (!sp3ctraCore->initialize(config)) {
            log_warning("VST", "Failed to apply configuration");
        } else {
            log_info("VST", "Configuration applied (full init) - %s:%d, %d DPI, log level %d",
                udpAddress.toRawUTF8(), udpPort, sensorDpi, logLevel);
        }
    } else {
        // Just update config - NO buffer reinit
        log_debug("VST", "Config updated (no buffer reinit) - %d DPI, log level %d",
            sensorDpi, logLevel);
    }
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
    
    // 🔧 DEBUG: Always force restart to expose the bug
    // (Skip comparison - always restart even if config unchanged)
    int newPort = (int)udpPortParam->load();
    juce::String newAddress = getUdpAddressString();
    
    log_info("VST", "UDP batch update - FORCING restart to %s:%d",
        newAddress.toRawUTF8(), newPort);
    
    // 🔧 CRITICAL FIX: Set ctx->running=0 FIRST, then close socket
    if (udpThread) {
        udpThread->requestStop();  // Sets ctx->running = 0
    }

    // Close socket completely (not just shutdown) to force recvfrom() exit
    if (sp3ctraCore) {
        sp3ctraCore->closeUdpSocket();
    }

    // Wait for thread to exit
    if (udpThread) {
        udpThread->stopThread(1500);
        udpThread.reset();
    }
    
    // Update g_sp3ctra_config with current UDP parameters
    applyConfigurationToCore(false);
    
    // Restart UDP socket with new config (buffers untouched)
    if (!sp3ctraCore->restartUdp(newPort, newAddress.toStdString(), "")) {
        log_error("VST", "Failed to restart UDP after batch update!");
    }
    
    // Restart UDP thread AFTER socket is created
    udpThread = std::make_unique<UdpReceiverThread>(sp3ctraCore.get());
    udpThread->startThread();
    
    log_info("VST", "UDP restarted with %s:%d (FORCED)", newAddress.toRawUTF8(), newPort);
    
    udpNeedsRestart.store(false);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Sp3ctraAudioProcessor();
}
