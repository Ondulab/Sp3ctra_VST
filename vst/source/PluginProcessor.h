#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Sp3ctraSharedCore.h"  // Process-wide singleton (UDP + image pipeline + LuxStral)
#include "Sp3ctraConstants.h"
#include "luxsampler/LuxSampler.h"
#include "framesequencer/FrameSequencer.h"

// C headers for RT profiling
extern "C" {
    #include "utils/rt_profiler.h"
    #include "processing/score_engine.h"   // ScoreSettings (offline SCORE generation)
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

    /** Optional output directory chosen by the user in the LuxSampler
     *  settings.  When non-empty, SAVE SESSION writes the .sp3s file
     *  (and the optional PNG/JPEG image exports) into this directory
     *  directly, bypassing the file chooser.  Persisted in the APVTS
     *  state blob alongside lastSessionPath. */
    void           setSamplerOutputDir(const juce::String& p) { samplerOutputDir = p; }
    juce::String   getSamplerOutputDir()              const { return samplerOutputDir; }
    /** Returns the inner Sp3ctraCore owned by the process-wide singleton. */
    Sp3ctraCore* getSp3ctraCore()
    {
        return sharedCore ? sharedCore->getCore() : nullptr;
    }

    /**
     * @brief True once the shared pipeline (UDP socket + threads) has been
     *        successfully started by prepareToPlay().
     *
     * Returns false in Standalone when no audio device is currently selected
     * or active — in this state the UDP receiver is not bound to any port and
     * no CIS data can ever be received, regardless of network configuration.
     *
     * UI thread safe (queries an atomic flag inside Sp3ctraSharedCore).
     */
    bool isPipelineReady() const noexcept
    {
        return sharedCore && sharedCore->isReady();
    }
    LuxSampler*    getLuxSampler()    { return luxSampler.get();    }
    FrameSequencer*  getFrameSequencer()  { return frameSequencer.get();  }

    // -------------------------------------------------------------------------
    // SCORE module — shared generation settings (offline, message-thread only).
    // Owned here so the PLAY page (Generate) and the SETUP panel (parameters)
    // edit the SAME settings. NOT in the APVTS (offline, not host-automatable).
    // -------------------------------------------------------------------------
    ScoreSettings& getScoreSettings() noexcept { return scoreSettings_; }

    /** SCORE frequency-range override. When `manual` is false the SCORE follows
     *  LuxStral's musical Tuning + Root Note + Octaves (read-only mirror in the
     *  UI). When true, the SCORE uses its OWN tuning/root/octaves below. Message
     *  thread only (UI + GENERATE). */
    struct ScoreFreqOverride
    {
        bool   manual    = false;
        double tuning    = 440.0;   ///< A4 reference (Hz), like luxstralTuning
        int    rootIndex = 12;      ///< 0 = C1 … 12 = C2 (luxstralRootNote index)
        int    octaves   = 8;       ///< span in octaves above the root note
    };
    ScoreFreqOverride& getScoreFreqOverride() noexcept { return scoreFreq_; }

    /** Musical frequency range (Hz) driven by LuxStral's Tuning + Root Note +
     *  Octaves (the values LuxStral itself uses). */
    void getMusicalFrequencyRange(double& lowHz, double& highHz) const noexcept;

    /** Frequency range (Hz) the SCORE generation should use: the manual override
     *  when enabled, otherwise the LuxStral range. */
    void getScoreFrequencyRange(double& lowHz, double& highHz) const noexcept;
    
    // Helper to build UDP address string from 4 bytes
    juce::String getUdpAddressString() const;
    
    // UDP Batch Update API (prevents multiple UDP restarts during bulk parameter changes)
    void beginUdpBatchUpdate();
    void endUdpBatchUpdate();

    // -------------------------------------------------------------------------
    // Sampler action button MIDI bindings (REC / PLAY / SAVE on selected slot)
    //
    // The selected slot is mirrored from the UI (SlotEditorComponent) so the
    // RT thread can act on the same slot the user sees on screen.  Pulses are
    // set by processBlock (RT) and consumed by the message thread.
    //
    // MIDI Learn:
    //   target  : -1 = idle, 0 = REC, 1 = PLAY, 2 = SAVE
    //   result  : -1 = no capture yet, otherwise (type << 8) | number
    //             where type: 1 = Note, 2 = CC
    // -------------------------------------------------------------------------
    void setSamplerSelectedSlot(int s) noexcept { samplerSelectedSlot.store(s, std::memory_order_relaxed); }
    int  getSamplerSelectedSlot() const noexcept { return samplerSelectedSlot.load(std::memory_order_relaxed); }

    // REC / PLAY bindings now follow a momentary (press-and-hold) semantic:
    //   pressed  : key down  / CC value crossed >= 64  → start action
    //   released : key up    / CC value crossed <  64  → stop  action
    // SAVE keeps a single trigger-on-press semantic.
    bool consumeSamplerRecPressed()   noexcept { return samplerRecPressed  .exchange(false, std::memory_order_acquire); }
    bool consumeSamplerRecReleased()  noexcept { return samplerRecReleased .exchange(false, std::memory_order_acquire); }
    bool consumeSamplerPlayPressed()  noexcept { return samplerPlayPressed .exchange(false, std::memory_order_acquire); }
    bool consumeSamplerPlayReleased() noexcept { return samplerPlayReleased.exchange(false, std::memory_order_acquire); }
    bool consumeSamplerSaveTrigger()  noexcept { return samplerSaveTriggered.exchange(false, std::memory_order_acquire); }

    /** All Notes Off (panic): ask the audio thread to release every held/stuck
     *  note next block. Safe to call from the UI (message) thread. */
    void requestAllNotesOff() noexcept { panicRequested.store(true, std::memory_order_release); }

    void startSamplerMidiLearn(int target) noexcept
    {
        samplerMidiLearnResult.store(-1, std::memory_order_relaxed);
        samplerMidiLearnTarget.store(target, std::memory_order_release);
    }
    void cancelSamplerMidiLearn() noexcept
    {
        samplerMidiLearnTarget.store(-1, std::memory_order_release);
        samplerMidiLearnResult.store(-1, std::memory_order_relaxed);
    }
    int  getSamplerMidiLearnTarget() const noexcept { return samplerMidiLearnTarget.load(std::memory_order_acquire); }
    int  getSamplerMidiLearnResult() const noexcept { return samplerMidiLearnResult.load(std::memory_order_acquire); }
    void clearSamplerMidiLearnResult() noexcept { samplerMidiLearnResult.store(-1, std::memory_order_relaxed); }

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

    // SCORE generation settings — shared between PLAY page and SETUP panel.
    ScoreSettings     scoreSettings_ {};
    ScoreFreqOverride scoreFreq_ {};
    
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

    // All Notes Off (panic): set by the UI (message thread), consumed and
    // cleared by processBlock (audio thread) to release every held/stuck note.
    std::atomic<bool> panicRequested{false};

    // -------------------------------------------------------------------------
    // Sampler action button MIDI bindings — RT-safe trigger pulses
    // Set by processBlock (audio thread), consumed by message thread.
    // -------------------------------------------------------------------------
    std::atomic<int>  samplerSelectedSlot   { 0 };
    // Momentary state flags: track current "held" state of REC / PLAY bindings
    // (used by processBlock to detect press/release edges on each MIDI event).
    std::atomic<bool> samplerRecHeld        { false };
    std::atomic<bool> samplerPlayHeld       { false };
    // Edge pulses consumed by the UI timer thread (SlotEditorComponent).
    std::atomic<bool> samplerRecPressed     { false };
    std::atomic<bool> samplerRecReleased    { false };
    std::atomic<bool> samplerPlayPressed    { false };
    std::atomic<bool> samplerPlayReleased   { false };
    // SAVE retains the one-shot trigger semantic (no momentary behaviour).
    std::atomic<bool> samplerSaveTriggered  { false };

    // MIDI Learn: target = -1 idle / 0 REC / 1 PLAY / 2 SAVE.
    // Result encoding: -1 = none, else (type << 8) | number  (type: 1=Note, 2=CC).
    std::atomic<int>  samplerMidiLearnTarget{ -1 };
    std::atomic<int>  samplerMidiLearnResult{ -1 };


    /** Full path of the last .sp3s session saved or loaded.
     *  Serialised inside the APVTS state blob (getStateInformation /
     *  setStateInformation) so it survives DAW project reloads and
     *  Standalone restarts. */
    juce::String lastSessionPath;

    /** Optional output directory for LuxSampler SAVE SESSION + image export.
     *  Empty → fallback to file chooser (legacy behaviour). Persisted in
     *  the APVTS state blob alongside lastSessionPath. */
    juce::String samplerOutputDir;
    
    // Note: RT Profiler is now global (g_vst_rt_profiler) to be accessible from C threads
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sp3ctraAudioProcessor)
};
