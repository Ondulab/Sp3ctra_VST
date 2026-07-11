#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Sp3ctraSharedCore.h"  // Process-wide singleton (UDP + image pipeline + LuxStral)
#include "Sp3ctraConstants.h"
#include "luxsampler/LuxSampler.h"
#include "framesequencer/FrameSequencer.h"
#include "processing/AcquisitionGate.h" // "Vitesse d'acquisition" — frame-advance brake clock
#include "ui/ChainModel.h"      // M6 Phase 2 — editable chain topology (owned here)
#include "midi/MidiMappingEngine.h" // MIDI CC/Note → any play param (MIDI learn)
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
// Per-instance bank id helpers + the type→params manifest (J1): moved to
// ui/ModuleParamManifest.h — the single source of truth every consumer of
// "all the params of THIS module instance" iterates.
#include "ui/ModuleParamManifest.h"

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
    //   target  : -1 = idle, otherwise engine * 3 + action
    //             (action: 0 = REC, 1 = PLAY, 2 = SAVE; engine 0 = A, 1 = B)
    //   result  : -1 = no capture yet, otherwise (type << 8) | number
    //             where type: 1 = Note, 2 = CC
    // -------------------------------------------------------------------------
    void setSamplerSelectedSlot(int s) noexcept { samplerSelectedSlot.store(s, std::memory_order_relaxed); }
    int  getSamplerSelectedSlot() const noexcept { return samplerSelectedSlot.load(std::memory_order_relaxed); }

    // REC / PLAY bindings now follow a momentary (press-and-hold) semantic:
    //   pressed  : key down  / CC value crossed >= 64  → start action
    //   released : key up    / CC value crossed <  64  → stop  action
    // SAVE keeps a single trigger-on-press semantic.
    // `engine` selects the sampler instance (0 = A, 1 = B) — bindings are
    // per-engine so a controller button drives ONE instance, not both.
    bool consumeSamplerRecPressed  (int engine) noexcept { return samplerRecPressed  [engine & 1].exchange(false, std::memory_order_acquire); }
    bool consumeSamplerRecReleased (int engine) noexcept { return samplerRecReleased [engine & 1].exchange(false, std::memory_order_acquire); }
    bool consumeSamplerPlayPressed (int engine) noexcept { return samplerPlayPressed [engine & 1].exchange(false, std::memory_order_acquire); }
    bool consumeSamplerPlayReleased(int engine) noexcept { return samplerPlayReleased[engine & 1].exchange(false, std::memory_order_acquire); }
    bool consumeSamplerSaveTrigger (int engine) noexcept { return samplerSaveTriggered[engine & 1].exchange(false, std::memory_order_acquire); }

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

    /** M6 Phase 2 — the editable chain topology lives here (not in the editor) so
     *  per-chain routing applies headless and is reachable by the RT thread.
     *  The ChainRackComponent edits this model directly and calls
     *  onChainModelEdited() afterwards. */
    ChainModel& getChainModel() noexcept { return chainModel_; }

    /** MIDI CC/Note → parameter mapping engine (right-click MIDI Learn on any
     *  play control; per-instance via the banked param ids). */
    MidiMappingEngine& getMidiMap() noexcept { return midiMap_; }

    /** UI VU meters (AUDIO MIX panel) — per-engine post-volume block peaks,
     *  folded with an RT-side release in processBlock. [0..1+], relaxed reads. */
    float meterLuxStral() const noexcept { return meterLuxStral_.load(std::memory_order_relaxed); }
    float meterLuxSynth() const noexcept { return meterLuxSynth_.load(std::memory_order_relaxed); }
    float meterLuxWave()  const noexcept { return meterLuxWave_ .load(std::memory_order_relaxed); }
    float meterMaster()   const noexcept { return meterMaster_  .load(std::memory_order_relaxed); }

    /** Pitch/Mask/FX state-pool slot bound to a MODULE INSTANCE (0..7), or 0
     *  when unknown. The binding is keyed by the ModuleInstance UUID and
     *  STABLE across edits: moving the module to another chain, or removing /
     *  reordering other chains, never rebinds (and thus never corrupts or
     *  swaps) its live state — the state belongs to the module, not to the
     *  chain hosting it (see updateModulePoolBindings). Message thread. */
    int poolSlotForInstance(const juce::Uuid& moduleId) const noexcept;

    /** MIDI-follow auto-navigation — reverse of the per-instance bank id helpers
     *  (lpParam/lmParam/vsParam/lsOutParam/…). Resolves a mapped parameter id to
     *  the module INSTANCE that owns it so the editor can jump to its page.
     *  `engineView` marks a synth ENGINE param (its own page) vs an OUT/send
     *  param (the OUT page). Message thread; best-effort — `valid` is false for
     *  parameters that don't belong to a rack module (global / source / master),
     *  or whose module isn't currently in the rack. */
    struct ParamNavTarget
    {
        bool       valid { false };
        juce::Uuid instanceId;                    ///< instance to select in the rack
        ModuleType type { ModuleType::Sp3ctra };
        bool       engineView { false };          ///< synth engine page vs OUT page
    };
    ParamNavTarget navTargetForParam(const juce::String& paramId) const;

    /** Contextual visualizer (zone 1): sets the SELECTED module instance whose
     *  chain-position output feeds the selection tap. The chain executor then
     *  publishes the stream frame AT that module's position in ITS chain
     *  (SynthChainPlan.viz_tap_insert → selection-tap bus). Clears the tap to
     *  white first so the previous selection's frame never lingers. Message
     *  thread; cheap (plan republish). */
    void setVisualizerTapModule(const juce::Uuid& moduleId);

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
    // Index = sampler engine (0 = A "luxSampler*", 1 = B "luxSamplerB*").
    std::atomic<float>* luxSamplerMidiChannelParam [2] = {};
    std::atomic<float>* luxSamplerRecBindTypeParam [2] = {};
    std::atomic<float>* luxSamplerRecBindNumParam  [2] = {};
    std::atomic<float>* luxSamplerPlayBindTypeParam[2] = {};
    std::atomic<float>* luxSamplerPlayBindNumParam [2] = {};
    std::atomic<float>* luxSamplerSaveBindTypeParam[2] = {};
    std::atomic<float>* luxSamplerSaveBindNumParam [2] = {};
    // Per-instance banks (index = pool slot): each Pitch/Mask instance filters
    // MIDI on ITS OWN channel/octave params.
    std::atomic<float>* luxpitchMidiChannelParam [ChainModel::kMaxChains] = {};
    std::atomic<float>* luxpitchOctaveOffsetParam[ChainModel::kMaxChains] = {};
    std::atomic<float>* luxmaskMidiChannelParam  [ChainModel::kMaxChains] = {};
    std::atomic<float>* luxmaskOctaveOffsetParam [ChainModel::kMaxChains] = {};
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

    // CC1 mod-wheel targets driven from processBlock (setValueNotifyingHost):
    // cached to avoid the juce::String built by apvts.getParameter("literal").
    // Per-instance banks (index = pool slot).
    juce::RangedAudioParameter* luxpitchLfoDepthParam  [ChainModel::kMaxChains] = {};
    juce::RangedAudioParameter* luxmaskLfoPosDepthParam[ChainModel::kMaxChains] = {};

    // UDP Batch Update state (prevents multiple UDP restarts)
    std::atomic<bool> udpBatchUpdateActive{false};
    std::atomic<bool> udpNeedsRestart{false};

    // All Notes Off (panic): set by the UI (message thread), consumed and
    // cleared by processBlock (audio thread) to release every held/stuck note.
    std::atomic<bool> panicRequested{false};

    // MIDI CC/Note → parameter mappings. Constructed after apvts (declaration
    // order below the apvts member matters — it holds a reference to it).
    MidiMappingEngine midiMap_ { apvts };

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
    // LuxStral send slots present last edit — diffed so the engine enable
    // (deviceEnabled) follows the presence of ANY "→ LUXSTRAL" send.
    std::set<int>        luxstralEngines_;
    // Stable MODULE-INSTANCE → pool-slot binding (Pitch/Mask/Reverb/Echo),
    // keyed by the ModuleInstance UUID. A module keeps its pool slot for its
    // whole lifetime — moving it to another chain, or removing / reordering
    // other chains, never rebinds (and thus never corrupts or swaps) its live
    // state. Slots are allocated PER TYPE (each type owns its own state pool).
    // Rebuilt by updateModulePoolBindings() on every model load/edit.
    struct PoolBinding { int slot; ModuleType type; };
    std::map<juce::Uuid, PoolBinding> modulePoolSlots_;
    // The binding keys the per-instance APVTS param BANK (luxpitch{slot}_*…),
    // so it must survive a session reload — a rebuild in a different order
    // would silently swap two instances' settings. Serialized as the
    // POOL_SLOTS child tree; restored (seeded) before the first
    // updateModulePoolBindings() of the load.
    juce::ValueTree poolBindingsToTree() const;
    void restorePoolBindingsFromTree(const juce::ValueTree& t);

    // ── Per-chain memory of pooled-insert settings ────────────────────────────
    // When a Pitch/Mask/Reverb/Echo instance leaves a chain (removed, or moved
    // to another chain), its bank values are snapshotted under (chain UUID,
    // type). Adding the same type back to that chain restores them — the chain
    // "remembers" the module's last settings. Keyed per chain+type because the
    // model allows at most one pooled instance of a type per chain.
    // Serialized as the INSERT_MEMORY child tree. Message thread only.
    struct InsertLoc { juce::Uuid chain; ModuleType type; int slot; };
    std::map<juce::Uuid, InsertLoc> prevInsertLoc_;   // instance UUID → last location
    std::map<std::pair<juce::String, int>,            // (chain UUID str, (int) type)
             std::map<juce::String, float>> insertParamMemory_;  // suffix → raw value
    void updateInsertParamMemory();                   // diff model vs prevInsertLoc_
    void baselineInsertLocations();                   // session load: no snapshot/apply
    juce::ValueTree insertMemoryToTree() const;
    void restoreInsertMemoryFromTree(const juce::ValueTree& t);
    // Contextual visualizer target — the selected module instance (see
    // setVisualizerTapModule). Random/unmatched UUID ⇒ no tap in the plan.
    juce::Uuid vizTapModuleId_;
    // Per-type masks of pool slots whose binding changed in the LAST rebind
    // (released or freshly assigned) — their pool state is stale.
    struct PoolStale { uint32_t pitch = 0, mask = 0, reverb = 0, echo = 0, eq = 0; };
    // Pool slots owning a Pitch/Mask/Reverb/Echo/EQ instance after the LAST
    // derive — diffed to reset instances whose module (or whole chain) was just
    // removed.
    uint32_t prevPitchSlots_  { 0 };
    uint32_t prevMaskSlots_   { 0 };
    uint32_t prevReverbSlots_ { 0 };
    uint32_t prevEchoSlots_   { 0 };
    uint32_t prevEqSlots_     { 0 };
    // Bit i set ⇒ the chain bound to pool slot i has a Pitch/Mask instance →
    // fan MIDI to pool slot i. Default bit 0 = legacy single-instance behaviour.
    std::atomic<uint32_t> chainPitchMask_ { 1 };
    std::atomic<uint32_t> chainMaskMask_  { 1 };
    // Same presence masks for the FX inserts (no MIDI fan-out — config sync only).
    std::atomic<uint32_t> chainReverbMask_ { 0 };
    std::atomic<uint32_t> chainEchoMask_   { 0 };
    std::atomic<uint32_t> chainEqMask_     { 0 };

    // ── UI VU meters (AUDIO MIX) — written by processBlock only ─────────────
    // Per-block peak accumulators (RT thread locals, folded + reset each block)
    // and the atomics the UI reads (peak with exponential release).
    float lsPkBlock_ { 0.0f }, lxPkBlock_ { 0.0f }, lwPkBlock_ { 0.0f };
    std::atomic<float> meterLuxStral_ { 0.0f }, meterLuxSynth_ { 0.0f },
                       meterLuxWave_  { 0.0f }, meterMaster_   { 0.0f };
    // Sampler A/B presence in the model (message thread, set in
    // deriveChainRouting) — combined with the shared luxSamplerEnabled param
    // to drive each engine's setEnabled().
    bool samplerAPresent_ { false };
    bool samplerBPresent_ { false };
    void deriveChainRouting();              // model → pool bindings + enable bridge + plan
    PoolStale updateModulePoolBindings();   // model → modulePoolSlots_; returns per-type slots to reset
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
    // Index = sampler engine (0 = A, 1 = B) — each engine owns its bindings.
    std::atomic<bool> samplerRecHeld    [2] { false, false };
    std::atomic<bool> samplerPlayHeld   [2] { false, false };
    // Edge pulses consumed by the UI timer thread (SlotEditorComponent).
    std::atomic<bool> samplerRecPressed  [2] { false, false };
    std::atomic<bool> samplerRecReleased [2] { false, false };
    std::atomic<bool> samplerPlayPressed [2] { false, false };
    std::atomic<bool> samplerPlayReleased[2] { false, false };
    // SAVE retains the one-shot trigger semantic (no momentary behaviour).
    std::atomic<bool> samplerSaveTriggered[2] { false, false };

    // MIDI Learn: target = -1 idle, otherwise engine * 3 + action
    // (action: 0 REC / 1 PLAY / 2 SAVE — engine 0 = A, 1 = B).
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
    int                                   scorePlayingParamIdx_ { -1 }; // SCORE mirror guard

    // R6 — pool resets deferred past the in-flight frame: chain_plan_publish()
    // makes the NEXT frame stop pulling a removed Pitch/Mask/VideoScroll pool
    // slot, but the UDP/feeder thread may still be processing the CURRENT
    // frame with the OLD plan. Resetting immediately raced those writes; the
    // timer executes the reset ≥40 ms later (frames last ~1 ms).
    uint32_t pendingPitchResets_      { 0 };
    uint32_t pendingMaskResets_       { 0 };
    uint32_t pendingReverbResets_     { 0 };
    uint32_t pendingEchoResets_       { 0 };
    uint32_t pendingEqResets_         { 0 };
    uint32_t pendingVideoScrollInits_ { 0 };
    // M3 — chain slots whose "→ LUXSTRAL" send disappeared: their staging must
    // go inactive (silence) once the in-flight frame is done.
    uint32_t pendingStagingResets_    { 0 };
    uint32_t prevLsSendChains_        { 0 };
    uint32_t poolResetArmedMs_        { 0 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (Sp3ctraAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sp3ctraAudioProcessor)
};
