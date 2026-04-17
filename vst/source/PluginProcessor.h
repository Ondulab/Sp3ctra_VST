#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Sp3ctraSharedCore.h"  // Process-wide singleton (UDP + image pipeline + LuxStral)
#include "Sp3ctraConstants.h"
#include "luxsampler/LuxSampler.h"
#include "framesequencer/FrameSequencer.h"

// C headers for RT profiling
extern "C" {
    #include "utils/rt_profiler.h"
}

//==============================================================================
/**
 * @brief Sp3ctra VST Audio Processor
 * 
 * Main VST plugin class that integrates:
 * - UDP reception thread (IMAGE_DATA + IMU packets)
 * - Core synthesis engine (Sp3ctraCore)
 * - Audio processing (processBlock)
 * - VST parameters (APVTS with UDP config, sensor DPI, log level)
 */
class Sp3ctraAudioProcessor  : public juce::AudioProcessor,
                                public juce::AudioProcessorValueTreeState::Listener
{
public:
    //==============================================================================
    Sp3ctraAudioProcessor();
    ~Sp3ctraAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    
    //==============================================================================
    // AudioProcessorValueTreeState::Listener interface
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    
    //==============================================================================
    // Public accessors for UI
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    /** Path of the last session saved or loaded by SamplerPageComponent.
     *  Persisted inside the APVTS state blob so the DAW project and
     *  the Standalone app both restore the session on next launch. */
    void           setLastSessionPath(const juce::String& p) { lastSessionPath = p; }
    juce::String   getLastSessionPath()                const { return lastSessionPath; }
    /** Returns the inner Sp3ctraCore owned by the process-wide singleton. */
    Sp3ctraCore* getSp3ctraCore()
    {
        return sharedCore ? sharedCore->getCore() : nullptr;
    }
    LuxSampler*    getLuxSampler()    { return luxSampler.get();    }
    FrameSequencer*  getFrameSequencer()  { return frameSequencer.get();  }
    
    // Helper to build UDP address string from 4 bytes
    juce::String getUdpAddressString() const;
    
    // UDP Batch Update API (prevents multiple UDP restarts during bulk parameter changes)
    void beginUdpBatchUpdate();
    void endUdpBatchUpdate();

private:
    //==============================================================================
    // Helper to create parameter layout
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    // Apply updated configuration to Sp3ctraCore
    // needsSocketRestart: true = full reinit (UDP change), false = just update g_sp3ctra_config
    void applyConfigurationToCore(bool needsSocketRestart = true);
    
    //==============================================================================
    // Initialization state flags
    // luxstralInitialized is now queried via sharedCore->isReady() — kept locally
    // only to mirror the old pixel-per-note change detection logic.
    bool coreNeedsInit = true;  // Lazy init: wait for setStateInformation() before starting UDP
    
    // 🔧 DOUBLE-BUFFER CONSUMER TRACKING:
    // Tracks which buffer index was last consumed by processBlock.
    // This prevents signaling "consumed" twice for the same data,
    // and allows re-outputting old audio instead of silence when producer is mid-write.
    // -1 = no buffer consumed yet (startup)
    int lastConsumedReadIdx = -1;
    int lastConsumedReadIdxLuxSynth = -1;
    // pixels_per_note used during the last synth_IfftInit() call.
    // If it changes on SR switch (e.g. 96kHz→48kHz: ppn 4→2), the waves[]
    // array must be reallocated via synth_luxstral_cleanup() + synth_IfftInit().
    // Otherwise init_waves() writes past the old allocation → Bus Error (SIGBUS).
    int lastInitPixelsPerNote = 0;
    
    // Test tone phase accumulator (fallback if LuxStral not working)
    // Note: testTonePhase removed - no longer using 440Hz fallback tone
    
    // ✨ Sp3ctra Shared Core Integration
    // The shared_ptr keeps the singleton alive as long as this instance exists.
    // The last instance to be destroyed will tear down UDP + synthesis threads.
    std::shared_ptr<Sp3ctraSharedCore> sharedCore;
    std::unique_ptr<LuxSampler>   luxSampler;
    std::unique_ptr<FrameSequencer> frameSequencer;
    
    // ✨ VST Parameters via AudioProcessorValueTreeState
    juce::AudioProcessorValueTreeState apvts;
    
    // Parameter IDs (for consistency)
    static constexpr const char* PARAM_DEVICE_ENABLED = "deviceEnabled";
    static constexpr const char* PARAM_UDP_PORT = "udpPort";
    static constexpr const char* PARAM_UDP_BYTE1 = "udpByte1";
    static constexpr const char* PARAM_UDP_BYTE2 = "udpByte2";
    static constexpr const char* PARAM_UDP_BYTE3 = "udpByte3";
    static constexpr const char* PARAM_UDP_BYTE4 = "udpByte4";
    static constexpr const char* PARAM_SENSOR_DPI = "sensorDpi";
    static constexpr const char* PARAM_LOG_LEVEL = "logLevel";
    static constexpr const char* PARAM_VISUALIZER_MODE = "visualizerMode";

    // LuxSampler parameter IDs
    static constexpr const char* PARAM_FS_ENABLED      = "luxSamplerEnabled";
    static constexpr const char* PARAM_FS_MIDI_CH      = "luxSamplerMidiChannel";
    static constexpr const char* PARAM_FS_OCT_OFFSET   = "luxSamplerOctaveOffset";
    static constexpr const char* PARAM_FS_MAX_DUR      = "luxSamplerMaxDuration";

    // FrameSequencer parameter IDs
    static constexpr const char* PARAM_SEQ_ENABLED  = "seqEnabled";
    static constexpr const char* PARAM_SEQ_BPM      = "seqBpm";
    static constexpr const char* PARAM_SEQ_NSTEPS   = "seqNumSteps";
    static constexpr const char* PARAM_SEQ_LOOP     = "seqLoop";
    static constexpr const char* PARAM_SEQ_DAW_SYNC = "seqDawSync";
    static constexpr const char* PARAM_SEQ_BPS      = "seqBeatsPerStep";
    
    // Quick access to parameters (cached, no atomic overhead)
    std::atomic<float>* udpPortParam = nullptr;
    std::atomic<float>* udpByte1Param = nullptr;
    std::atomic<float>* udpByte2Param = nullptr;
    std::atomic<float>* udpByte3Param = nullptr;
    std::atomic<float>* udpByte4Param = nullptr;
    std::atomic<float>* sensorDpiParam = nullptr;
    std::atomic<float>* logLevelParam = nullptr;
    std::atomic<float>* deviceEnabledParam  = nullptr;
    std::atomic<float>* visualizerModeParam = nullptr;
    std::atomic<float>* masterVolumeParam   = nullptr;  // RT output gain
    
    // UDP Batch Update state (prevents multiple UDP restarts)
    std::atomic<bool> udpBatchUpdateActive{false};
    std::atomic<bool> udpNeedsRestart{false};

    /** Full path of the last .sp3s session saved or loaded.
     *  Serialised inside the APVTS state blob (getStateInformation /
     *  setStateInformation) so it survives DAW project reloads and
     *  Standalone restarts. */
    juce::String lastSessionPath;
    
    // Note: RT Profiler is now global (g_vst_rt_profiler) to be accessible from C threads
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sp3ctraAudioProcessor)
};
