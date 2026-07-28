#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Sp3ctraSharedCore.h"  // Process-wide singleton (UDP + image pipeline + LuxStral)
#include "Sp3ctraConstants.h"
#include "luxsampler/LuxSampler.h"
#include "luxsampler/ScorePlayerService.h"
#include "framesequencer/FrameSequencer.h"
#include "processing/AcquisitionGate.h" // "Vitesse d'acquisition" — frame-advance brake clock
#include "ui/ChainModel.h"      // M6 Phase 2 — editable chain topology (owned here)
#include "midi/MidiMappingEngine.h" // MIDI CC/Note → any play param (MIDI learn)
#include "session/SessionManager.h" // working-session persistence (sessions())
#include "session/Sp3ctraPaths.h"   // PathKeys:: + last-dir chooser memory
#include <map>                  // chainPoolSlots_ (stable chain → pool-slot binding)
#include <vector>               // activeVideoSlots()

// M9 — IMAGE / VIDEO / CAMERA source engines (owned here, UI binds to them)
class ImageSourceEngine;
class VideoSourceEngine;
class CameraSourceEngine;
class MediaSourceService;
class SessionManager;   // working-session (project) persistence — session/SessionManager.h

// VIDEO MIX + master-audio recorder (AVAssetWriter → HEVC .mov). Owned here so
// the RT audio tap in processBlock can feed it and it outlives the editor.
class VideoRecorder;

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
                                public IVirtualMidiSink,
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

    /** Build the COMPLETE plugin state as a ValueTree (APVTS + all non-APVTS
     *  child trees: SCORE, SEQS, SAMPLER_SLOTS, MEDIA_SOURCES, CHAINS,
     *  POOL_SLOTS, MIDI_MAPPINGS…). Shared by getStateInformation (host blob)
     *  and the SessionManager (on-disk project.sp3ctra). Read-only snapshot,
     *  safe to call on the message thread.
     *  @p embedBanks appends a SAMPLER_BANKS child holding the recorded audio
     *  (LuxSampler .fsmp blobs) so a DAW project is self-contained. Standalone
     *  sessions keep the banks as sidecar files → embedBanks=false. */
    juce::ValueTree captureFullState(bool embedBanks = false);

    /** Recorded sampler audio (all engines) serialized as a SAMPLER_BANKS tree
     *  (one binary child per engine). Used by captureFullState(true) for DAW. */
    juce::ValueTree samplerBanksToTree();
    /** Restore recorded sampler audio from a SAMPLER_BANKS tree (DAW path). */
    void restoreSamplerBanksFromTree(const juce::ValueTree& banks);

    /** The working-session manager (Standalone persistence). Never null after
     *  construction; drives project.sp3ctra + sidecar banks + assets. */
    SessionManager* sessions() const { return sessions_.get(); }

    /** Apply a state tree previously produced by captureFullState(). Runs the
     *  same staged migrations as setStateInformation, replaces the APVTS state,
     *  then defers the heavy non-APVTS restore to
     *  applyRestoredStateOnMessageThread. Shared by setStateInformation (host
     *  blob) and SessionManager::loadSession (project.sp3ctra from disk).
     *  Takes ownership of the XML element (may be null → logged failure). */
    void applyStateXml(std::unique_ptr<juce::XmlElement> xmlState);

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

    /** Decode an audio file and load it as the LuxStral timbre wavetable
     *  (message thread). rootHzOverride > 0 skips pitch detection. On success
     *  the path is remembered for session persistence (scan restore). */
    bool loadTimbreSampleFile(const juce::File& file, float rootHzOverride,
                              juce::String& errorOut);

    /** Decode an audio file and publish it as the LuxGrain grain material
     *  (message thread; NSDF root detection inside the engine). The path is
     *  remembered for session persistence. */
    bool loadLuxGrainSampleFile(const juce::File& file, juce::String& errorOut);
    void clearLuxGrainSample();
    const juce::String& luxgrainSamplePath() const { return luxgrainSamplePath_; }

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
    LuxSampler*    getLuxSampler()    { return samplers_[0].get(); }

    /** True while ANY sampler slot is actively recording. The session bank
     *  autosave must NOT call LuxSampler::saveToFile mid-capture (it reads the
     *  frames being written). Cheap atomic scan; message-thread safe. */
    bool anySamplerRecording() const noexcept
    {
        for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
            for (int s = 0; s < kSmpSlots; ++s)
                if (smpRecHeld[(size_t) e][s].load(std::memory_order_acquire))
                    return true;
        return false;
    }
    /** Sampler engine by index (0..7 since P6). Out-of-range clamps. */
    LuxSampler*    getSampler(int i) const
    {
        return samplers_[(size_t) juce::jlimit(0, LuxSampler::kMaxEngines - 1, i)].get();
    }
    /** Per-engine step sequencer (internal to sampler @p e). Out-of-range clamps. */
    FrameSequencer*  getFrameSequencer(int e) const
    {
        return frameSequencers_[(size_t) juce::jlimit(0, LuxSampler::kMaxEngines - 1, e)].get();
    }

    // ── P5-M4 — per-instance score playback ─────────────────────────────────
    /** The score channel of the FIRST placed instance of score-family type
     *  @p t (its pool slot is cached at plan derivation), or nullptr when no
     *  such module is in the rack. The generator tabs bind through this —
     *  each family type plays ITS OWN slot (per-instance binding of several
     *  modules of one type is M5). */
    ScoreChannel* getScoreChannel(ModuleType t) noexcept;
    /** Score channel by pool slot (rack LEDs — the module knows its slot). */
    ScoreChannel* getScoreChannelForSlot(int slot) noexcept
    {
        return scorePlayerService_ ? scorePlayerService_->channel(slot) : nullptr;
    }
    /** P5-M5 — is this score pool slot backed by a module in the rack? The
     *  generator pages use it to drop a stale instance binding (the bound
     *  module was removed) back to their type's first instance. Message
     *  thread (mask maintained by deriveAndPublishChainPlan). */
    bool scorePlayerSlotInUse(int slot) const noexcept
    {
        return slot >= 0 && slot < 8
            && ((scoreSlotsPresentMask_ >> slot) & 1u) != 0;
    }

    // M9 — IMAGE / VIDEO / CAMERA source engines (message-thread accessors)
    /** P5-M3 — one IMAGE engine per instance slot (0..7); slot 0 = legacy. */
    ImageSourceEngine*  getImageSource(int slot = 0)
    {
        return imageSources_[(size_t) juce::jlimit(0, 7, slot)].get();
    }
    VideoSourceEngine*  getVideoSource(int slot = 0)
    {
        return videoSources_[(size_t) juce::jlimit(0, 7, slot)].get();
    }
    CameraSourceEngine* getCameraSource(int slot = 0)
    {
        return cameraSources_[(size_t) juce::jlimit(0, 7, slot)].get();
    }

    /** Persisted camera device name (restored/reopened on session load). */
    void setCameraDeviceName(int slot, const juce::String& n)
    { cameraDeviceNames_[(size_t) juce::jlimit(0, 7, slot)] = n; }
    juce::String getCameraDeviceName(int slot) const
    { return cameraDeviceNames_[(size_t) juce::jlimit(0, 7, slot)]; }

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

    // REC / PLAY / SAVE are now triggered through the unified MIDI-mapping engine
    // (right-click MIDI-Learn on the buttons — see IVirtualMidiSink below). The
    // audio thread only latches per-(engine, slot) pulses here; the open
    // SlotEditor drains them on the message thread and runs the actual action.
    //   REC / PLAY : momentary — press/release pulses.
    //   SAVE       : one-shot  — trigger pulse.
    // "Fixed slot per button": each mapping targets ONE slot, so pulses are
    // addressed by (engine, slot), not by the editor's current selection.
    bool consumeSmpRecPressed  (int e, int s) noexcept { return smpRecPressed  [(size_t) juce::jlimit(0, LuxSampler::kMaxEngines - 1, e)][s % LuxSamplerConstants::NUM_SLOTS].exchange(false, std::memory_order_acquire); }
    bool consumeSmpRecReleased (int e, int s) noexcept { return smpRecReleased [(size_t) juce::jlimit(0, LuxSampler::kMaxEngines - 1, e)][s % LuxSamplerConstants::NUM_SLOTS].exchange(false, std::memory_order_acquire); }
    bool consumeSmpPlayPressed (int e, int s) noexcept { return smpPlayPressed [(size_t) juce::jlimit(0, LuxSampler::kMaxEngines - 1, e)][s % LuxSamplerConstants::NUM_SLOTS].exchange(false, std::memory_order_acquire); }
    bool consumeSmpPlayReleased(int e, int s) noexcept { return smpPlayReleased[(size_t) juce::jlimit(0, LuxSampler::kMaxEngines - 1, e)][s % LuxSamplerConstants::NUM_SLOTS].exchange(false, std::memory_order_acquire); }
    bool consumeSmpSaveTrigger (int e, int s) noexcept { return smpSaveTrigger [(size_t) juce::jlimit(0, LuxSampler::kMaxEngines - 1, e)][s % LuxSamplerConstants::NUM_SLOTS].exchange(false, std::memory_order_acquire); }
    bool consumeSmpClearTrigger(int e, int s) noexcept { return smpClearTrigger[(size_t) juce::jlimit(0, LuxSampler::kMaxEngines - 1, e)][s % LuxSamplerConstants::NUM_SLOTS].exchange(false, std::memory_order_acquire); }

    /** REC / PLAY transport-button mode for a sampler engine (0 = A, 1 = B):
     *  true = Momentary (press-to-start / release-to-stop), false = Toggle
     *  (bistable click). Read by the SlotEditor for both the UI buttons and the
     *  MIDI action-pulse drain so the two paths agree. Message thread. */
    bool samplerRecMomentary (int engine) const noexcept;
    bool samplerPlayMomentary(int engine) const noexcept;

    /** Pending MIDI EQ-band value for (engine, slot, band): a normalised 0..1
     *  gain latched by the audio thread, or -1 when nothing is pending. Drained
     *  by the open SlotEditor (message thread) which applies it via
     *  LuxSampler::setSlotEqBandGain (non-RT). */
    float consumeSmpEqPending(int e, int s, int band) noexcept
    {
        auto& a = smpEqPending[(size_t) juce::jlimit(0, LuxSampler::kMaxEngines - 1, e)][s % LuxSamplerConstants::NUM_SLOTS]
                              [band % LuxSampler::kEqBands];
        if (a.load(std::memory_order_relaxed) < 0.0f) return -1.0f;
        return a.exchange(-1.0f, std::memory_order_acq_rel);
    }

    /** MIDI-touch signal for VALUE targets: monotonic generation + last-touched
     *  location (engine<<8|slot). The open SlotEditor refreshes its sliders when
     *  a mapped controller moved a value on the slot it is showing. */
    uint32_t smpValueTouchGen()   const noexcept { return smpValueTouchGen_.load(std::memory_order_acquire); }
    int      smpValueTouchWhere() const noexcept { return smpValueTouchWhere_.load(std::memory_order_relaxed); }

    //==========================================================================
    // IVirtualMidiSink — NON-APVTS mapping targets for the sampler play params /
    // action buttons (implemented in PluginProcessor.cpp via SamplerMidiTargets).
    //==========================================================================
    int   virtualResolve(const juce::String& paramId) const override;
    int   virtualSteps  (int targetId) const noexcept override;
    float virtualRead   (int targetId) const noexcept override;
    void  virtualApply  (int targetId, float norm01) noexcept override;
    void  virtualRelease(int targetId) noexcept override;

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
    float meterLuxGrain() const noexcept { return meterLuxGrain_.load(std::memory_order_relaxed); }
    float meterMaster()   const noexcept { return meterMaster_  .load(std::memory_order_relaxed); }

    //==========================================================================
    // VIDEO MIX recording (macOS). The editor's VideoMixerComponent renders a
    // fixed-resolution composite and feeds pushRecordVideoFrame(); processBlock
    // taps the mastered stereo into pushRecordAudio-style pushAudio(). All A/V
    // muxing happens on the recorder's own writer thread.
    /** Begin recording a .mov. Video is locked to w×h; audio uses the current
     *  sample rate / channel count. Returns false + fills err on failure.
     *  Message thread. */
    bool startVideoRecording(const juce::File& out, int w, int h, double fps,
                             juce::String& err);
    /** Stop + finalise the file (safe if not recording). Message thread. */
    void stopVideoRecording();
    bool isVideoRecording() const noexcept;
    /** Render thread: hand the latest composite to the recorder (no-op if idle). */
    void pushRecordVideoFrame(const juce::Image& composite, double tSeconds);

    /** AUDIO MIX — number of OUT sends placed per engine across all chains
     *  (deriveAndPublishChainPlan). 0 = the engine is hidden from the mixer
     *  AND its render is skipped entirely (zero-CPU contract). */
    int sendsLuxStral() const noexcept { return sendCountLuxStral_.load(std::memory_order_relaxed); }
    int sendsLuxSynth() const noexcept { return sendCountLuxSynth_.load(std::memory_order_relaxed); }
    int sendsLuxWave()  const noexcept { return sendCountLuxWave_ .load(std::memory_order_relaxed); }
    int sendsLuxGrain() const noexcept { return sendCountLuxGrain_.load(std::memory_order_relaxed); }

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

    /** J3 — duplicate chain `chainIdx` (fresh UUIDs, settings carried over via
     *  the copied VALUES; modules that can't be duplicated — singletons,
     *  exhausted pools — are dropped). Returns the new chain's index or -1.
     *  Message thread. */
    int duplicateChain(int chainIdx);

    /** J4 — write chain `chainIdx` (fresh VALUES + type memory) as a
     *  .sp3chain preset. Atomic write; returns false on any I/O error. */
    bool saveChainPreset(int chainIdx, const juce::File& file);

    /** J4 — load a .sp3chain preset tree. targetChainIdx >= 0 replaces that
     *  chain's content (its UUID survives; same-type modules keep their bank
     *  slot — J5 automation/MIDI stability); -1 appends a new chain. Modules
     *  the placement rules refuse here (singletons placed elsewhere,
     *  exhausted pools) are SKIPPED, never a total failure — their names come
     *  back in `skipped`. chainIdx == -1 when nothing could be loaded.
     *  Message thread. */
    struct ChainPresetLoadResult { int chainIdx = -1; juce::StringArray skipped; };
    ChainPresetLoadResult loadChainPreset(const juce::ValueTree& preset,
                                          int targetChainIdx);

    /** Loads the topology from apvts.state (or the legacy default) and derives
     *  routing — WITHOUT touching enable params (those are restored from state).
     *  Headless-safe; called from the constructor and setStateInformation. */
    void loadChainModelFromState();

    // -------------------------------------------------------------------------
    // Non-APVTS state riding in the session blob (SCORE / SEQ / SAMPLER_SLOTS
    // child trees). Captured in getStateInformation, restored in
    // setStateInformation; the flags below let the one-shot legacy .sp3s
    // import (Sp3sImporter) keep the newer state copies of params/pattern.
    // -------------------------------------------------------------------------
    /** True when setStateInformation restored a sequencer pattern — the legacy
     *  .sp3s import must then skip the (older) pattern stored in the file. */
    bool wasSeqRestoredFromState() const noexcept { return seqRestoredFromState_; }

    /** True when the state blob carried per-slot sampler parameters. */
    bool hasStateSamplerParams() const noexcept { return samplerParamsInState_; }

    /** Re-applies the SAMPLER_SLOTS state overlay to both engines (message
     *  thread). Called after a legacy bank import so labels/params restored
     *  from the bank file are overridden by the newer state values. */
    void applySamplerParamsFromState();

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
    // P6 — sampler engines ×8 (0 = A legacy, 1 = B legacy, 2..7).
    std::array<std::unique_ptr<LuxSampler>, LuxSampler::kMaxEngines> samplers_;
    // Working-session (project) persistence — constructed in the ctor after the
    // samplers exist. Standalone-facing; owns project.sp3ctra + sidecar banks.
    std::unique_ptr<SessionManager> sessions_;
    // Catch-all session-dirty hook: every APVTS param edit lands in the state
    // tree via JUCE's flush timer (message thread), and every non-APVTS prop
    // write (layout, persisted trees) hits it directly — one listener covers
    // them all. MUST be re-attached after apvts.replaceState (new tree object);
    // restore storms are silenced by the SessionManager suppress flag.
    struct SessionDirtyListener final : juce::ValueTree::Listener
    {
        Sp3ctraAudioProcessor& p;
        explicit SessionDirtyListener(Sp3ctraAudioProcessor& proc) : p(proc) {}
        void mark() { if (p.sessions_) p.sessions_->markStateDirty(); }
        void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { mark(); }
        void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override            { mark(); }
        void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override     { mark(); }
        void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override             { mark(); }
    };
    std::unique_ptr<SessionDirtyListener> sessionDirtyListener_;
    // P5-M4 — the per-instance score players (8 slots, one 1 kHz thread).
    std::unique_ptr<ScorePlayerService> scorePlayerService_;
    /** First placed pool slot per score-family type (kScoreFamily order),
     *  -1 = type absent. Written at plan derivation (message thread), read
     *  by getScoreChannel() — atomics so the transport mirror in the timer
     *  and MIDI-mapped param changes need no model lock. */
    std::atomic<int> scoreFamilySlot_[4] { {-1}, {-1}, {-1}, {-1} };
    /** Pool slots present in the model at the LAST derivation — the diff
     *  discards a removed instance's frames (per-slot teardown). */
    uint8_t scoreSlotsPresentMask_ = 0;
    /** One step sequencer PER sampler engine — internal to the sampler since
     *  the SEQUENCER rack module was retired (each addresses only its own
     *  engine's banks; params live in the luxSampler{N}_Seq* banks). */
    std::array<std::unique_ptr<FrameSequencer>, LuxSampler::kMaxEngines> frameSequencers_;

    // M9 — IMAGE / VIDEO / CAMERA source engines + the single service thread
    // that ticks them and pumps the chains when the device is not streaming.
    std::array<std::unique_ptr<ImageSourceEngine>, 8>  imageSources_;   // P5-M3
    std::array<std::unique_ptr<VideoSourceEngine>, 8>  videoSources_;   // P5-M3
    std::array<std::unique_ptr<CameraSourceEngine>, 8> cameraSources_;  // P5-M3
    std::unique_ptr<MediaSourceService> mediaService_;
    std::array<juce::String, 8> cameraDeviceNames_;   // persisted devices (by name, per slot)

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

    // FrameSequencer parameter IDs — per sampler engine since the sequencer
    // became internal to the sampler: fsEngineParam(e, "SeqBpm" / "SeqNumSteps"
    // / "SeqLoop" / "SeqDawSync" / "SeqBeatsPerStep" / "SeqTransport").
    // The retired GLOBAL ids ("seqBpm"…) only survive in old state blobs and
    // are migrated in setStateInformation via the legacy SEQ pattern tree.

    // SCORE playback transport parameter IDs (relayed to LuxSampler)
    static constexpr const char* PARAM_SCORE_PLAYING = "scorePlaying";
    static constexpr const char* PARAM_SCORE_LOOP    = "scoreLoop";
    static constexpr const char* PARAM_SCORE_REVERSE = "scoreReverse";
    static constexpr const char* PARAM_SCORE_SPEED   = "scoreSpeed";

    // M9 — IMAGE / VIDEO / CAMERA source parameter IDs (relayed to the engines)
    static constexpr const char* PARAM_IMGSRC_POS     = "imgSrcPos";
    static constexpr const char* PARAM_IMGSRC_DUR     = "imgSrcDuration";
    static constexpr const char* PARAM_IMGSRC_LOOP    = "imgSrcLoop";
    static constexpr const char* PARAM_IMGSRC_PLAY    = "imgSrcPlay";
    static constexpr const char* PARAM_IMGSRC_ENABLED = "imgSrcEnabled";
    static constexpr const char* PARAM_VIDSRC_LINE    = "vidSrcLine";
    static constexpr const char* PARAM_VIDSRC_SPEED   = "vidSrcSpeed";
    static constexpr const char* PARAM_VIDSRC_LOOP    = "vidSrcLoop";
    static constexpr const char* PARAM_VIDSRC_PLAY    = "vidSrcPlay";
    static constexpr const char* PARAM_VIDSRC_ENABLED = "vidSrcEnabled";
    static constexpr const char* PARAM_CAMSRC_LINE    = "camSrcLine";
    static constexpr const char* PARAM_CAMSRC_ENABLED = "camSrcEnabled";

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
    // MIDI channel drives note-routing (which channel triggers slot playback) and
    // engine-B's channel filter — kept; the old REC/PLAY/SAVE bind params were
    // removed with the bespoke settings MIDI system (now unified MIDI-Learn).
    std::atomic<float>* luxSamplerMidiChannelParam [LuxSampler::kMaxEngines] = {};
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
    std::atomic<float>* luxgrainEnabledParam        = nullptr;
    std::atomic<float>* luxgrainVolumeParam         = nullptr;
    std::atomic<float>* luxgrainDensityParam        = nullptr;
    std::atomic<float>* luxgrainDensityShapeParam   = nullptr;
    std::atomic<float>* luxgrainSpreadParam         = nullptr;
    std::atomic<float>* luxgrainSizeMinParam        = nullptr;
    std::atomic<float>* luxgrainSizeMaxParam        = nullptr;
    std::atomic<float>* luxgrainTextureParam        = nullptr;
    std::atomic<float>* luxgrainJitterParam         = nullptr;
    std::atomic<float>* luxgrainWidthParam          = nullptr;
    std::atomic<float>* luxgrainAmpFollowParam      = nullptr;
    std::atomic<float>* luxgrainEnvShapeParam       = nullptr;
    std::atomic<float>* luxgrainColorPanParam       = nullptr;
    std::atomic<float>* luxgrainEdgeParam           = nullptr;
    std::atomic<float>* luxgrainBandsParam          = nullptr;
    std::atomic<float>* luxgrainMaterialParam       = nullptr;
    std::atomic<float>* luxgrainScrubParam          = nullptr;
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
    std::set<int>        luxstralSends_;
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

    // ── J3 — per-chain memory of module settings (ALL manifest types) ────────
    // When an instance leaves a chain (removed, or moved to another chain),
    // its bank values are snapshotted into Chain::typeMemory (the CHAIN owns
    // it — serialized with the model, carried by presets). Adding the same
    // type back to that chain restores them. Keyed per chain+type: the model
    // allows at most one instance of a type per chain (VideoScroll excepted —
    // its multi-instances share the type memory, acceptable). Message thread.
    struct InsertLoc { juce::Uuid chain; ModuleType type; int slot; };
    std::map<juce::Uuid, InsertLoc> prevInsertLoc_;   // instance UUID → last location
    int  bankSlotForModule(const ModuleInstance& m) const; // pool slot or instance slot
    void updateInsertParamMemory();                   // diff model vs prevInsertLoc_
    void baselineInsertLocations();                   // session load: no snapshot/apply
    void restoreInsertMemoryFromTree(const juce::ValueTree& t); // legacy blob → typeMemory
    // Contextual visualizer target — the selected module instance (see
    // setVisualizerTapModule). Random/unmatched UUID ⇒ no tap in the plan.
    juce::Uuid vizTapModuleId_;
    // Per-type masks of pool slots whose binding changed in the LAST rebind
    // (released or freshly assigned) — their pool state is stale.
    struct PoolStale { uint32_t pitch = 0, mask = 0, reverb = 0, echo = 0, eq = 0, harmo = 0; };
    // Pool slots owning a Pitch/Mask/Reverb/Echo/EQ/Harmo instance after the
    // LAST derive — diffed to reset instances whose module (or whole chain) was
    // just removed.
    uint32_t prevPitchSlots_  { 0 };
    uint32_t prevMaskSlots_   { 0 };
    uint32_t prevReverbSlots_ { 0 };
    uint32_t prevEchoSlots_   { 0 };
    uint32_t prevEqSlots_     { 0 };
    uint32_t prevHarmoSlots_  { 0 };
    // Bit i set ⇒ the chain bound to pool slot i has a Pitch/Mask instance →
    // fan MIDI to pool slot i. Default bit 0 = legacy single-instance behaviour.
    std::atomic<uint32_t> chainPitchMask_ { 1 };
    std::atomic<uint32_t> chainMaskMask_  { 1 };
    // Same presence masks for the FX inserts (no MIDI fan-out — config sync only).
    std::atomic<uint32_t> chainReverbMask_ { 0 };
    std::atomic<uint32_t> chainEchoMask_   { 0 };
    std::atomic<uint32_t> chainEqMask_     { 0 };
    std::atomic<uint32_t> chainHarmoMask_  { 0 };

    // ── UI VU meters (AUDIO MIX) — written by processBlock only ─────────────
    // Per-block peak accumulators (RT thread locals, folded + reset each block)
    // and the atomics the UI reads (peak with exponential release).
    float lsPkBlock_ { 0.0f }, lxPkBlock_ { 0.0f }, lwPkBlock_ { 0.0f },
          lgPkBlock_ { 0.0f };
    std::atomic<float> meterLuxStral_ { 0.0f }, meterLuxSynth_ { 0.0f },
                       meterLuxWave_  { 0.0f }, meterLuxGrain_ { 0.0f },
                       meterMaster_   { 0.0f };
    // VIDEO MIX recorder (owned) + RT gate. recActive_ (release/acquire) lets
    // processBlock skip the audio tap entirely when not recording.
    std::unique_ptr<VideoRecorder> videoRecorder_;
    std::atomic<bool>              recActive_ { false };
    // Per-engine OUT send counts (message thread writes in
    // deriveAndPublishChainPlan; UI + processBlock read). 0 → the engine's
    // render is skipped (no CPU) and its AUDIO MIX strip is hidden.
    std::atomic<int> sendCountLuxStral_ { 0 }, sendCountLuxSynth_ { 0 },
                     sendCountLuxWave_  { 0 }, sendCountLuxGrain_ { 0 };
    // Per-engine bitmask of the OUT sends' bank slots present in the model
    // (message thread writes in deriveAndPublishChainPlan; processBlock reads).
    // ANDed each block with the banks' `enabled` flags (g_sp3ctra_config) so a
    // send DISABLED via its rack LED starves the engine exactly like a removed
    // one: OFF = silence, uniformly — freeze stays a transport (HOLD) feature.
    std::atomic<uint32_t> sendSlotsLuxStral_ { 0 }, sendSlotsLuxSynth_ { 0 },
                          sendSlotsLuxWave_  { 0 }, sendSlotsLuxGrain_ { 0 };
    // Zero-CPU drain (audio thread only), indexed LuxStral/LuxSynth/LuxWave/
    // LuxGrain. When an engine stops being fed, its render gate is held open
    // for a drain window so the feeds' no-send contract (50-tick debounce +
    // silence push + latch/crossfade) and the voices' release complete BEFORE
    // the render collapses — closing instantly froze spectrum+voices, which
    // resurrected as a stale "last sound" on re-enable. Blocks, not samples:
    // the feed debounce also counts one tick per block.
    static constexpr int kEngineDrainBlocks = 64;
    int  engineDrainBlocks_[4] { 0, 0, 0, 0 };
    bool engineFed_[4]  { false, false, false, false };   // this block
    bool engineGate_[4] { false, false, false, false };   // fed OR draining
    // Per-engine sampler presence in the model (message thread, set in
    // deriveChainRouting) — combined with EACH engine's own enable param
    // (fsEngineParam(e,"Enabled")) to drive that engine's setEnabled().
    std::array<bool, LuxSampler::kMaxEngines> samplerPresent_ {};
    void deriveChainRouting();              // model → pool bindings + enable bridge + plan
    void primeScoreTransports();            // push each family type's speed/loop/reverse → its slot
    PoolStale updateModulePoolBindings();   // model → modulePoolSlots_; returns per-type slots to reset
    void deriveAndPublishChainPlan();       // model → RT-safe per-synth ChainPlan
    void persistChainModel();              // model → apvts.state <CHAINS>
    // J2 — chain-owned settings (chantier « chain porteuse ») —————————————
    void snapshotBankValuesIntoModel();    // runtime banks → ModuleInstance.values (save)
    void projectChainValuesToBanks();      // values → runtime banks (load/preset, msg thread)
    void applyChainEnableBridge();         // presence → enable params (diff vs chainActiveTypes_)

    // Non-APVTS state ↔ session blob (see the public flags above).
    juce::ValueTree scoreStateToTree() const;                 // SCORE settings + freq override
    void restoreScoreStateFromTree(const juce::ValueTree& t);
    juce::ValueTree luxstralWavetableToTree() const;          // user timbre wavetable (harmonics)
    void restoreLuxstralWavetableFromTree(const juce::ValueTree& t);
    /** Timbre scan position (luxstralTimbrePos) — changes are coalesced here
     *  and drained on the 30 ms timer: one re-extraction per tick max, never
     *  one per automation/drag event. */
    std::atomic<float> timbreScanPos_ { 0.5f };
    std::atomic<bool>  timbreScanPending_ { false };
    juce::String timbreSamplePath_;                            // full path ("" = none)
    juce::String luxgrainSamplePath_;                          // LuxGrain material ("" = none)
    double timbreScanLastMs_ = 0.0;   // playhead dt reference (0 = stopped)
    juce::ValueTree samplerSlotsStateToTree() const;          // both engines × 12 slots (+ overdub)
    juce::ValueTree seqStateToTree() const;                   // sequencer pattern + timing
    bool seqRestoredFromState_    = false;
    bool samplerParamsInState_    = false;
    void teardownAbsentModules(const std::set<ModuleType>& now); // free state of removed modules

    // Video-scroll "Stop" pulse: incremented by the UI; each VideoDisplayComponent
    // polls it (message thread) and clears its history buffer when it advances.
    std::atomic<uint32_t> videoScrollClearGen{0};

    // -------------------------------------------------------------------------
    // Sampler action triggers (REC / PLAY / SAVE) — RT-safe pulses fired by the
    // MIDI-mapping engine (virtualApply/virtualRelease) and drained by the open
    // SlotEditor. Addressed by [engine][slot] because each mapping targets ONE
    // fixed slot. REC/PLAY are momentary (held → press/release edges); SAVE is
    // one-shot.
    // -------------------------------------------------------------------------
    static constexpr int kSmpSlots = LuxSamplerConstants::NUM_SLOTS;
    std::atomic<int>  samplerSelectedSlot { 0 };   // mirror of the editor selection
    std::atomic<bool> smpRecHeld     [LuxSampler::kMaxEngines][kSmpSlots] {};
    std::atomic<bool> smpPlayHeld    [LuxSampler::kMaxEngines][kSmpSlots] {};
    std::atomic<bool> smpRecPressed  [LuxSampler::kMaxEngines][kSmpSlots] {};
    std::atomic<bool> smpRecReleased [LuxSampler::kMaxEngines][kSmpSlots] {};
    std::atomic<bool> smpPlayPressed [LuxSampler::kMaxEngines][kSmpSlots] {};
    std::atomic<bool> smpPlayReleased[LuxSampler::kMaxEngines][kSmpSlots] {};
    std::atomic<bool> smpSaveTrigger [LuxSampler::kMaxEngines][kSmpSlots] {};
    std::atomic<bool> smpClearTrigger[LuxSampler::kMaxEngines][kSmpSlots] {};
    // Pending MIDI EQ-band values (normalised 0..1; -1 = none). Applied on the
    // message thread (setSlotEqBandGain is non-RT). Seeded to -1 in the ctor.
    std::atomic<float> smpEqPending[LuxSampler::kMaxEngines][kSmpSlots][LuxSampler::kEqBands];

    // MIDI-touch signal for VALUE targets (see smpValueTouchGen/Where above):
    // bumped by virtualApply so the open SlotEditor can refresh its sliders.
    std::atomic<uint32_t> smpValueTouchGen_   { 0 };
    std::atomic<int>      smpValueTouchWhere_ { -1 };


    /** LEGACY — full path of the last .sp3s written by the retired sampler
     *  session feature. Only read once at restore time to migrate the old
     *  file's audio banks into the project session (Sp3sImporter), then
     *  cleared forever. Still serialised so pre-migration blobs round-trip. */
    juce::String lastSessionPath;

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

    // Config-resync coalescing (message thread only). applyConfigurationToCore()
    // re-reads ~60 APVTS params into g_sp3ctra_config on EVERY call — firing it
    // per parameter turned a state restore into hundreds of full resyncs (and
    // 3 log lines each). The "silent config update" dispatch branches now just
    // raise these flags; drainPendingConfig() performs ONE resync per drain
    // (timer tick / end of restore). bulkParamApply_ silences the per-parameter
    // logs while a restore or a deferred-automation batch is being applied.
    bool bulkParamApply_    = false;  // suppress per-param + hot-reload logs in bulk
    bool configResyncPending_ = false;
    bool freqReinitPending_   = false;
    bool coeffUpdatePending_  = false;
    bool workerPoolRestartPending_ = false;
    // Apply any pending g_sp3ctra_config resync / wavetable reinit / envelope
    // coefficient rebuild, once, on the message thread. Idempotent.
    void drainPendingConfig();

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
    uint32_t pendingHarmoResets_      { 0 };
    uint32_t pendingVideoScrollInits_ { 0 };
    // M3 — chain slots whose "→ LUXSTRAL" send disappeared: their staging must
    // go inactive (silence) once the in-flight frame is done.
    uint32_t pendingStagingResets_    { 0 };
    uint32_t prevLsSendChains_        { 0 };
    uint32_t poolResetArmedMs_        { 0 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (Sp3ctraAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sp3ctraAudioProcessor)
};
