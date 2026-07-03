#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Sp3ctraSharedCore.h"  // Process-wide singleton (UDP + image pipeline + LuxStral)
#include "Sp3ctraConstants.h"
#include "luxsampler/LuxSampler.h"
#include "framesequencer/FrameSequencer.h"
#include "processing/AcquisitionGate.h" // "Vitesse d'acquisition" — frame-advance brake clock
#include "ui/ChainModel.h"      // M6 Phase 2 — editable chain topology (owned here)
#include <map>                  // chainPoolSlots_ (stable chain → pool-slot binding)

// M9 — IMAGE / VIDEO / CAMERA source engines (owned here, UI binds to them)
class ImageSourceEngine;
class VideoSourceEngine;
class CameraSourceEngine;
class MediaSourceService;

// C headers for RT profiling
extern "C" {
    #include "utils/rt_profiler.h"
    #include "processing/score_engine.h"   // ScoreSettings (offline SCORE generation)
}

//==============================================================================
// VideoScroll per-instance APVTS bank id helpers. A VideoScroll module instance
// owns a slot 0..7 (ModuleInstance.slot); its params live under "videoScroll{slot}_*"
// and its mixer voice under "videoMix{slot}_*". One source of truth shared by the
// contextual panel, the per-instance renderer and the right-band mixer.
inline juce::String vsParam(int slot, const char* suffix)
{ return "videoScroll" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String vsMixParam(int slot, const char* suffix)
{ return "videoMix" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

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
                                public juce::AudioProcessorValueTreeState::Listener,
                                private juce::Timer
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
    // AudioProcessorValueTreeState::Listener interface.
    // Dispatcher only: on the message thread it applies immediately; from the
    // audio (host automation) or a project-loading thread it marks the param
    // dirty and the 30 ms timer applies it on the message thread — the heavy
    // handlers (applyConfigurationToCore, source routing under db->mutex, UDP
    // restart) must never run on the audio thread.
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    //==============================================================================
    // Public accessors for UI
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }

    /** Slots (0..7) of every VIDEO SCROLL output instance currently patched into
     *  a chain, ascending. Drives the right-band mixer's dynamic voice list.
     *  Message-thread only (reads the editable chain model). */
    std::vector<int> activeVideoSlots() const;

    /** UI hook (message thread only): set by the editor, cleared in its
     *  destructor, invoked after a full state restore so an OPEN editor
     *  rebuilds its rack from the new model instead of keeping the old
     *  topology on screen (ghost blocks, stale drops/LEDs). */
    std::function<void()> onStateRestoredUi;

    /** Video-scroll transport — momentary "Stop": freezes (videoScrollPaused)
     *  and clears the waterfall.  The clear is a generation counter polled by
     *  every live VideoDisplayComponent, so it reaches all open views without a
     *  direct pointer and survives a view being (re)created. */
    void     requestVideoScrollClear() noexcept { videoScrollClearGen.fetch_add(1, std::memory_order_release); }
    uint32_t getVideoScrollClearGen() const noexcept { return videoScrollClearGen.load(std::memory_order_acquire); }

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
    /** Sampler engine by index: 0 = A, 1 = B. Out-of-range falls back to A. */
    LuxSampler*    getSampler(int i)  { return (i == 1) ? luxSamplerB.get() : luxSampler.get(); }
    FrameSequencer*  getFrameSequencer()  { return frameSequencer.get();  }

    // M9 — IMAGE / VIDEO / CAMERA source engines (message-thread accessors)
    ImageSourceEngine*  getImageSource()  { return imageSource_.get();  }
    VideoSourceEngine*  getVideoSource()  { return videoSource_.get();  }
    CameraSourceEngine* getCameraSource() { return cameraSource_.get(); }

    /** Persisted camera device name (restored/reopened on session load). */
    void         setCameraDeviceName(const juce::String& n) { cameraDeviceName_ = n; }
    juce::String getCameraDeviceName() const                { return cameraDeviceName_; }

    // -------------------------------------------------------------------------
    // SCORE module — shared generation settings (offline, message-thread only).
    // Owned here so the PLAY page (Generate) and the SETUP panel (parameters)
    // edit the SAME settings. NOT in the APVTS (offline, not host-automatable).
    // -------------------------------------------------------------------------
    ScoreSettings& getScoreSettings() noexcept { return scoreSettings_; }

    // -------------------------------------------------------------------------
    // SCORE source-audio preview — auditions the selected WAV region through the
    // plugin output. Decoded + resampled on the message thread (start), then
    // mixed RT-safe in processBlock (no file I/O or alloc on the audio thread).
    // -------------------------------------------------------------------------
    void   startScorePreview(const juce::File& wav, double startSec, double lengthSec);
    void   pauseScorePreview() noexcept;   ///< stop output, keep position
    bool   resumeScorePreview() noexcept;  ///< continue (restart if at end); false if nothing loaded
    void   stopScorePreview()  noexcept;   ///< stop + rewind to region start
    bool   isScorePreviewPlaying() const noexcept
    { return scorePreviewPlaying_.load(std::memory_order_acquire); }
    double getScorePreviewPositionSec() const noexcept;

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

    /** Path of the WAV last loaded in the SCORE PLAY page. Persisted in the
     *  APVTS state blob so the page reloads it on the next launch. */
    void         setScoreWavPath(const juce::String& p) { scoreWavPath = p; }
    juce::String getScoreWavPath()                const { return scoreWavPath; }

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

    /** M6 Phase 2 — chain-derived source routing.
     *  Each synth reads the channel its chain placement dictates: MODULATED (0)
     *  when an image processor / utility sits upstream of it in its chain, LIVE
     *  (1) otherwise. ChainRackComponent computes these from the model and pushes
     *  them here; applyConfigurationToCore() then feeds g_sp3ctra_config
     *  (replacing the old hardcoded "LuxStral=modulated / LuxSynth=live").
     *  Safe to call from the UI (message) thread. */
    void setChainSourceRouting(int luxstralSrc, int luxsynthSrc) noexcept;
    int  chainLuxstralSource() const noexcept { return chainSrcLuxstral.load(std::memory_order_relaxed); }
    int  chainLuxsynthSource() const noexcept { return chainSrcLuxsynth.load(std::memory_order_relaxed); }

    /** M6 Phase 2 — the editable chain topology lives here (not in the editor) so
     *  per-chain routing applies headless and is reachable by the RT thread.
     *  The ChainRackComponent edits this model directly and calls
     *  onChainModelEdited() afterwards. */
    ChainModel& getChainModel() noexcept { return chainModel_; }

    /** Pitch/Mask state-pool slot bound to chain `chainIdx` (0..7), or 0 when
     *  unknown. The binding is keyed by the chain's UUID and STABLE across
     *  edits: removing / reordering another chain never rebinds this chain's
     *  live Pitch/Mask state (see updateChainPoolBindings). Message thread. */
    int poolSlotForChain(int chainIdx) const noexcept;

    /** Called by the UI after a model mutation: pushes module presence onto the
     *  APVTS enable params, derives the per-synth source routing, and persists
     *  the topology. (Message thread.) */
    void onChainModelEdited();

    /** Loads the topology from apvts.state (or the legacy default) and derives
     *  routing — WITHOUT touching enable params (those are restored from state).
     *  Headless-safe; called from the constructor and setStateInformation. */
    void loadChainModelFromState();

    // -------------------------------------------------------------------------
    // Non-APVTS state riding in the session blob (SCORE / SEQ / SAMPLER_SLOTS
    // child trees). Captured in getStateInformation, restored in
    // setStateInformation; the flags below coordinate the sampler session
    // auto-load so a stale .sp3s can never clobber the freshly restored state.
    // -------------------------------------------------------------------------
    /** True when setStateInformation restored a sequencer pattern — the session
     *  auto-load must then skip the (older) pattern stored in the .sp3s. */
    bool wasSeqRestoredFromState() const noexcept { return seqRestoredFromState_; }

    /** True when the state blob carried per-slot sampler parameters. */
    bool hasStateSamplerParams() const noexcept { return samplerParamsInState_; }

    /** Re-applies the SAMPLER_SLOTS state overlay to both engines (message
     *  thread). Called after a session auto-load so labels/params restored
     *  from the bank file are overridden by the newer state values. */
    void applySamplerParamsFromState();

    /** One-shot: true exactly once after setStateInformation restored a
     *  non-empty lastSessionPath. The first SamplerPageComponent consumes it
     *  to trigger the session auto-load; later editor re-openings must NOT
     *  reload the session over live (unsaved) in-RAM edits. */
    bool consumeSamplerAutoLoadPending() noexcept
    { return samplerAutoLoadPending_.exchange(false, std::memory_order_acq_rel); }

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
    int lastConsumedReadIdxLuxstralB = -1;   // M8 — 2nd LuxStral engine consumer
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
    std::unique_ptr<LuxSampler>   luxSampler;    // engine A (sampler slot 0)
    std::unique_ptr<LuxSampler>   luxSamplerB;   // engine B (sampler slot 1) — 2nd sampler
    std::unique_ptr<FrameSequencer> frameSequencer;

    // M9 — IMAGE / VIDEO / CAMERA source engines + the single service thread
    // that ticks them and pumps the chains when the device is not streaming.
    std::unique_ptr<ImageSourceEngine>  imageSource_;
    std::unique_ptr<VideoSourceEngine>  videoSource_;
    std::unique_ptr<CameraSourceEngine> cameraSource_;
    std::unique_ptr<MediaSourceService> mediaService_;
    juce::String cameraDeviceName_;   // persisted device choice (by name)

    /** Push module presence (Image/Video/Camera placed in some chain) onto the
     *  engines so they publish lines only while their module exists. Called
     *  from deriveChainRouting() on every model change. */
    void updateMediaSourcePresence();
    /** MEDIA_SOURCES state blob child tree (paths + camera device). */
    juce::ValueTree mediaSourcesStateToTree() const;
    void restoreMediaSourcesFromTree(const juce::ValueTree& t);

    // "Vitesse d'acquisition" — brakes the live frame-advance rate (sample-and-
    // hold) of the SP3CTRA source.  Clock only (audio thread); the buffer module
    // enforces the hold.  Driven each block from processBlock by the acqGate*
    // APVTS params.
    AcquisitionGate acqGate_;

    // SCORE generation settings — shared between PLAY page and SETUP panel.
    ScoreSettings     scoreSettings_ {};
    ScoreFreqOverride scoreFreq_ {};

    // SCORE source-audio preview (decoded on message thread, mixed RT-safe).
    juce::SpinLock           scorePreviewLock_;
    juce::AudioBuffer<float> scorePreviewBuf_;             // host-rate, up to 2 ch
    int                      scorePreviewPos_ = 0;         // guarded by scorePreviewLock_
    double                   scorePreviewRate_ = 48000.0;  // host rate at decode time
    std::atomic<int>         scorePreviewPosAtomic_ { 0 }; // lock-free UI playhead
    std::atomic<bool>        scorePreviewPlaying_   { false };
    
    // ✨ VST Parameters via AudioProcessorValueTreeState
    juce::AudioProcessorValueTreeState apvts;
    
    // Parameter IDs (for consistency)
    static constexpr const char* PARAM_DEVICE_ENABLED = "deviceEnabled";
    static constexpr const char* PARAM_UDP_PORT = "udpPort";
    static constexpr const char* PARAM_UDP_BYTE1 = "udpByte1";
    static constexpr const char* PARAM_UDP_BYTE2 = "udpByte2";
    static constexpr const char* PARAM_UDP_BYTE3 = "udpByte3";
    static constexpr const char* PARAM_UDP_BYTE4 = "udpByte4";
    // Device HTTP control-plane address (config.html host). Distinct from the
    // UDP listen/multicast address above — the device's web server is a
    // separate endpoint (default 192.168.100.1) reached by Sp3ctraDeviceClient.
    static constexpr const char* PARAM_DEVICE_IP_BYTE1 = "deviceIpByte1";
    static constexpr const char* PARAM_DEVICE_IP_BYTE2 = "deviceIpByte2";
    static constexpr const char* PARAM_DEVICE_IP_BYTE3 = "deviceIpByte3";
    static constexpr const char* PARAM_DEVICE_IP_BYTE4 = "deviceIpByte4";
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
    static constexpr const char* PARAM_SEQ_TRANSPORT = "seqTransport"; // 0=Stop 1=Play 2=Hold

    // SCORE playback transport parameter IDs (relayed to LuxSampler)
    static constexpr const char* PARAM_SCORE_PLAYING = "scorePlaying";
    static constexpr const char* PARAM_SCORE_LOOP    = "scoreLoop";
    static constexpr const char* PARAM_SCORE_REVERSE = "scoreReverse";
    static constexpr const char* PARAM_SCORE_SPEED   = "scoreSpeed";

    // M9 — IMAGE / VIDEO / CAMERA source parameter IDs (relayed to the engines)
    static constexpr const char* PARAM_IMGSRC_POS   = "imgSrcPos";
    static constexpr const char* PARAM_IMGSRC_DUR   = "imgSrcDuration";
    static constexpr const char* PARAM_IMGSRC_LOOP  = "imgSrcLoop";
    static constexpr const char* PARAM_IMGSRC_PLAY  = "imgSrcPlay";
    static constexpr const char* PARAM_VIDSRC_LINE  = "vidSrcLine";
    static constexpr const char* PARAM_VIDSRC_SPEED = "vidSrcSpeed";
    static constexpr const char* PARAM_VIDSRC_LOOP  = "vidSrcLoop";
    static constexpr const char* PARAM_VIDSRC_PLAY  = "vidSrcPlay";
    static constexpr const char* PARAM_CAMSRC_LINE  = "camSrcLine";

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

    // Cached raw-parameter pointers read by processBlock (audio thread):
    // apvts.getRawParameterValue("literal") builds a juce::String (malloc)
    // on every call, so the RT path reads through these pointers instead.
    std::atomic<float>* luxSamplerMidiChannelParam  = nullptr;
    std::atomic<float>* luxSamplerRecBindTypeParam  = nullptr;
    std::atomic<float>* luxSamplerRecBindNumParam   = nullptr;
    std::atomic<float>* luxSamplerPlayBindTypeParam = nullptr;
    std::atomic<float>* luxSamplerPlayBindNumParam  = nullptr;
    std::atomic<float>* luxSamplerSaveBindTypeParam = nullptr;
    std::atomic<float>* luxSamplerSaveBindNumParam  = nullptr;
    std::atomic<float>* luxpitchMidiChannelParam    = nullptr;
    std::atomic<float>* luxpitchOctaveOffsetParam   = nullptr;
    std::atomic<float>* luxmaskMidiChannelParam     = nullptr;
    std::atomic<float>* luxmaskOctaveOffsetParam    = nullptr;
    std::atomic<float>* luxsynthEnabledParam        = nullptr;
    std::atomic<float>* luxsynthMidiChannelParam    = nullptr;
    std::atomic<float>* luxsynthOctaveOffsetParam   = nullptr;
    std::atomic<float>* luxsynthVolumeParam         = nullptr;
    std::atomic<float>* luxwaveEnabledParam         = nullptr;
    std::atomic<float>* luxwaveMidiChannelParam     = nullptr;
    std::atomic<float>* luxwaveOctaveOffsetParam    = nullptr;
    std::atomic<float>* luxwaveVolumeParam          = nullptr;
    std::atomic<float>* luxwaveAttackMsParam        = nullptr;
    std::atomic<float>* luxwaveDecayMsParam         = nullptr;
    std::atomic<float>* luxwaveSustainLevelParam    = nullptr;
    std::atomic<float>* luxwaveReleaseMsParam       = nullptr;
    std::atomic<float>* luxwaveAttackCurveParam     = nullptr;
    std::atomic<float>* luxwaveDecayCurveParam      = nullptr;
    std::atomic<float>* luxwaveReleaseCurveParam    = nullptr;
    std::atomic<float>* luxwaveFilterCutoffParam    = nullptr;
    std::atomic<float>* luxwaveFilterEnvDepthParam  = nullptr;
    std::atomic<float>* luxwaveLfoRateParam         = nullptr;
    std::atomic<float>* luxwaveLfoDepthParam        = nullptr;
    std::atomic<float>* luxwaveScanModeParam        = nullptr;
    std::atomic<float>* luxwaveAmplitudeParam       = nullptr;
    std::atomic<float>* acqGateModeParam            = nullptr;
    std::atomic<float>* acqGateRateMsParam          = nullptr;
    std::atomic<float>* acqGateSyncDivParam         = nullptr;
    std::atomic<float>* acqGateMultDivParam         = nullptr;
    std::atomic<float>* luxstralVolumeParam         = nullptr;
    std::atomic<float>* luxstralBEnabledParam       = nullptr;
    std::atomic<float>* luxstralBVolumeParam        = nullptr;

    // CC1 mod-wheel targets driven from processBlock (setValueNotifyingHost):
    // cached to avoid the juce::String built by apvts.getParameter("literal").
    juce::RangedAudioParameter* luxpitchLfoDepthParam   = nullptr;
    juce::RangedAudioParameter* luxmaskLfoPosDepthParam = nullptr;

    // UDP Batch Update state (prevents multiple UDP restarts)
    std::atomic<bool> udpBatchUpdateActive{false};
    std::atomic<bool> udpNeedsRestart{false};

    // All Notes Off (panic): set by the UI (message thread), consumed and
    // cleared by processBlock (audio thread) to release every held/stuck note.
    std::atomic<bool> panicRequested{false};

    // M6 Phase 2 — chain-derived source routing (0 = MODULATED, 1 = LIVE).
    // Defaults reproduce the legacy fixed topology (LuxStral=modulated, LuxSynth=live).
    std::atomic<int> chainSrcLuxstral { 0 };
    std::atomic<int> chainSrcLuxsynth { 1 };

    // M6 Phase 2 — authoritative editable topology + last-known presence set
    // (used to diff the enable-param bridge). Message-thread owned.
    ChainModel           chainModel_;
    std::set<ModuleType> chainActiveTypes_;
    // VideoScroll probe slots (0..7) present last edit — diffed to clear the
    // capture ring of any probe that was just removed.
    std::set<int>        videoScrollSlots_;
    // Instance identity per VideoScroll slot: a NEW instance claiming a
    // previously-used slot must not inherit the removed instance's parameter
    // bank / automation values (see teardownAbsentModules).
    std::map<int, juce::Uuid> videoScrollSlotIds_;
    // LuxStral engines (0 = A, 1 = B) present last edit — diffed so each engine's
    // enable param (A = deviceEnabled, B = luxstralBEnabled) follows ITS OWN
    // placement, not type-level presence.
    std::set<int>        luxstralEngines_;
    // Stable chain → Pitch/Mask pool-slot binding, keyed by chain UUID. A chain
    // keeps its pool slot for its whole lifetime, so removing / reordering other
    // chains never rebinds (and thus never corrupts) its live Pitch/Mask state.
    // Rebuilt by updateChainPoolBindings() on every model load/edit.
    std::map<juce::Uuid, int> chainPoolSlots_;
    // Pool slots owning a Pitch/Mask instance after the LAST derive — diffed to
    // reset instances whose module (or whole chain) was just removed.
    uint32_t prevPitchSlots_ { 0 };
    uint32_t prevMaskSlots_  { 0 };
    // Bit i set ⇒ the chain bound to pool slot i has a Pitch/Mask instance →
    // fan MIDI to pool slot i. Default bit 0 = legacy single-instance behaviour.
    std::atomic<uint32_t> chainPitchMask_ { 1 };
    std::atomic<uint32_t> chainMaskMask_  { 1 };
    // M8 — true when a 2nd LuxStral engine (slot B) is placed in the model; gates
    // the additive engine-B mix in processBlock(). Set in deriveChainRouting().
    std::atomic<bool>     luxstralBPresent_ { false };
    // Sampler A/B presence in the model (message thread, set in
    // deriveChainRouting) — combined with the shared luxSamplerEnabled param
    // to drive each engine's setEnabled().
    bool samplerAPresent_ { false };
    bool samplerBPresent_ { false };
    void deriveChainRouting();              // model → setChainSourceRouting + chain plan
    uint32_t updateChainPoolBindings();     // model → chainPoolSlots_; returns slots to reset
    void deriveAndPublishChainPlan();       // model → RT-safe per-synth ChainPlan
    void persistChainModel();              // model → apvts.state <CHAINS>
    void applyChainEnableBridge();         // presence → enable params (diff vs chainActiveTypes_)

    // Non-APVTS state ↔ session blob (see the public flags above).
    juce::ValueTree scoreStateToTree() const;                 // SCORE settings + freq override
    void restoreScoreStateFromTree(const juce::ValueTree& t);
    juce::ValueTree samplerSlotsStateToTree() const;          // both engines × 12 slots (+ overdub)
    juce::ValueTree seqStateToTree() const;                   // sequencer pattern + timing
    bool seqRestoredFromState_    = false;
    bool samplerParamsInState_    = false;
    std::atomic<bool> samplerAutoLoadPending_ { false };
    void teardownAbsentModules(const std::set<ModuleType>& now); // free state of removed modules

    // Video-scroll "Stop" pulse: incremented by the UI; each VideoDisplayComponent
    // polls it (message thread) and clears its history buffer when it advances.
    std::atomic<uint32_t> videoScrollClearGen{0};

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

    /** WAV last loaded in the SCORE PLAY page (see get/setScoreWavPath). */
    juce::String scoreWavPath;
    
    // Note: RT Profiler is now global (g_vst_rt_profiler) to be accessible from C threads

    // Suspend/resume the editor's visualizer from prepareToPlay. Hosts may call
    // prepareToPlay off the message thread; touching the editor Component from
    // there races its destruction — marshal via callAsync + WeakReference then.
    void setVisualizerSuspendedSafely(bool suspend);

    // -------------------------------------------------------------------------
    // Message-thread dispatch machinery (RT safety)
    // -------------------------------------------------------------------------
    // Real parameter handler — message thread only (see parameterChanged()).
    void applyParameterChange(const juce::String& parameterID, float newValue);
    // Non-APVTS part of setStateInformation (chain model, SCORE/SEQ trees,
    // sampler params…) — mutates state read by UI timers, message thread only.
    void applyRestoredStateOnMessageThread();
    // 30 ms message-thread tick: drains dirty deferred params and executes
    // pending pool resets (see pendingPitchResets_ below).
    void timerCallback() override;

    // Deferred parameterChanged dispatch. Multi-producer safe: one dirty flag
    // per parameter + a global "any" flag; the timer drains in index order
    // (ordering across params is not semantically relevant here, values are
    // re-read from the APVTS at apply time so they coalesce).
    juce::StringArray                     deferredParamIds_;   // index → paramID
    std::map<juce::String, int>           paramIndexById_;
    std::unique_ptr<std::atomic<bool>[]>  paramDirty_;
    std::atomic<bool>                     anyParamDirty_ { false };

    // R6 — pool resets deferred past the in-flight frame: chain_plan_publish()
    // makes the NEXT frame stop pulling a removed Pitch/Mask/VideoScroll pool
    // slot, but the UDP/feeder thread may still be processing the CURRENT
    // frame with the OLD plan. Resetting immediately raced those writes; the
    // timer executes the reset ≥40 ms later (frames last ~1 ms).
    uint32_t pendingPitchResets_      { 0 };
    uint32_t pendingMaskResets_       { 0 };
    uint32_t pendingVideoScrollInits_ { 0 };
    uint32_t poolResetArmedMs_        { 0 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (Sp3ctraAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sp3ctraAudioProcessor)
};
