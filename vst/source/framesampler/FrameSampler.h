#pragma once

/*
 * FrameSampler.h
 *
 * Main controller for the FrameSampler subsystem.
 * Records, stores and replays the Sp3ctra CIS image stream via MIDI commands.
 *
 * RT safety contract (enforced throughout):
 *   - processMidi() runs on the audio thread → atomics ONLY, no alloc, no lock, no I/O
 *   - onFrameAssembled() runs on udpThread (Non-RT) → alloc allowed on first use
 *   - FramePlayerThread runs Non-RT → alloc/lock/I/O allowed
 *
 * Architecture: see docs/SPEC_FrameSampler.html §9
 */

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <ctime>

// Forward declarations — full types included in .cpp only
extern "C"
{
    struct AudioImageBuffers;
    struct DoubleBuffer;
}

// ============================================================================
// Constants
// ============================================================================
namespace FrameSamplerConstants
{
    constexpr int     NUM_SLOTS           = 12;
    constexpr int     MAX_FRAMES_PER_SLOT = 30000; // 200 DPI, 10 s, ×1.5 margin
    constexpr int     MAX_PIXELS          = 3456;  // Fixed 400 DPI (FIXED_BUFFER_PIXELS)
    constexpr float   MAX_DURATION_S      = 10.0f;

    // MIDI note bases (C0 = MIDI 12 — Ableton/GM convention)
    constexpr int MIDI_REC_NOTE_BASE  = 12; // C0..B0 → slots 0..11
    constexpr int MIDI_PLAY_NOTE_BASE = 24; // C1..B1 → slots 0..11

    // .fsmp binary file format
    constexpr uint32_t FSMP_MAGIC      = 0x46534D50u; // "FSMP"
    constexpr uint16_t FSMP_VERSION    = 0x0001u;
    constexpr uint32_t FSMP_EOF_MARKER = 0xDEADBEEFu;
}

// ============================================================================
// CapturedFrame — one complete assembled CIS scan line
// sizeof = 8 + 4 + 2 + 3×3456 = 10 382 bytes
// ============================================================================
struct CapturedFrame
{
    uint64_t timestamp_us = 0; // µs relative to slot start (t₀ = 0)
    uint32_t line_id      = 0; // Original UDP line_id (debug/sync)
    uint16_t pixel_count  = 0; // 1728 @200DPI or 3456 @400DPI
    uint8_t  R[FrameSamplerConstants::MAX_PIXELS] {};
    uint8_t  G[FrameSamplerConstants::MAX_PIXELS] {};
    uint8_t  B[FrameSamplerConstants::MAX_PIXELS] {};
};

// ============================================================================
// SlotState — per-slot state machine states
// ============================================================================
enum class SlotState : int
{
    IDLE      = 0,
    ARMED     = 1,
    RECORDING = 2,
    PLAYING   = 3
};

// ============================================================================
// LoopMode — per-slot playback loop behaviour
// ============================================================================
enum class LoopMode : int
{
    NONE     = 0, // Play once, then stop and restore passthrough
    LOOP     = 1, // Loop forward: wrap play_head back to startFrame on overflow
    INVERSE  = 2, // Loop backward: play in reverse, wrap back to endFrame
    PINGPONG = 3  // Alternate forward / backward each time a boundary is reached
};

// ============================================================================
// FrameSlot — storage for one recording slot (lazy heap allocation)
// ============================================================================
struct FrameSlot
{
    std::unique_ptr<CapturedFrame[]> frames; // nullptr until first record
    int      capacity    = 0;
    int      frame_count = 0;
    int      play_head   = 0;
    uint64_t duration_us = 0;
    bool     has_content = false;
    char     label[64]   {};

    bool isAllocated() const noexcept { return frames != nullptr; }

    /** Lazy allocation — Non-RT only. Called at first NoteOn REC. */
    void allocate()
    {
        if (!frames)
        {
            frames   = std::make_unique<CapturedFrame[]>(
                           FrameSamplerConstants::MAX_FRAMES_PER_SLOT);
            capacity = FrameSamplerConstants::MAX_FRAMES_PER_SLOT;
        }
        frame_count = 0;
        play_head   = 0;
        duration_us = 0;
        has_content = false;
    }

    /** Release all heap memory and reset state. */
    void clear() noexcept
    {
        frames.reset();
        capacity    = 0;
        frame_count = 0;
        play_head   = 0;
        duration_us = 0;
        has_content = false;
        label[0]    = '\0';
    }
};

// ============================================================================
// FrameSamplerAtomicState — lock-free RT ↔ Non-RT interface
// RT path (processBlock) writes these; Non-RT threads read them.
// ============================================================================
struct FrameSamplerAtomicState
{
    std::atomic<int>  slotState[FrameSamplerConstants::NUM_SLOTS];
    std::atomic<int>  activePlaySlot     { -1 };   // -1 = none playing
    std::atomic<bool> passthroughEnabled { true };  // false during PLAYING
    /** Set by FrameSequencer::triggerStep() before posting startPlayCmd so that
     *  FramePlayerThread does NOT restore passthroughEnabled when the sample
     *  finishes or the slot has no content.  The sequencer is the only authority
     *  that decides when live resumes (STEP_LIVE or rtStop).
     *  Cleared by: rtStop(), handleNoteOn() play path, uiPlaySlot(). */
    std::atomic<bool> seqControlledPlay { false };

    // Command pulses: set by RT, cleared (exchange) by Non-RT threads
    std::atomic<bool> startRecCmd[FrameSamplerConstants::NUM_SLOTS];
    std::atomic<bool> stopRecCmd[FrameSamplerConstants::NUM_SLOTS];
    std::atomic<int>  startPlayCmd { -1 };   // slot index, -1 = no command
    std::atomic<bool> stopPlayCmd  { false };

    FrameSamplerAtomicState() noexcept
    {
        for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
        {
            slotState[i].store(static_cast<int>(SlotState::IDLE),
                               std::memory_order_relaxed);
            startRecCmd[i].store(false, std::memory_order_relaxed);
            stopRecCmd[i].store(false, std::memory_order_relaxed);
        }
    }

    FrameSamplerAtomicState(const FrameSamplerAtomicState&) = delete;
    FrameSamplerAtomicState& operator=(const FrameSamplerAtomicState&) = delete;
};

// ============================================================================
// Forward declarations
// ============================================================================
class FrameSampler;

// ============================================================================
// FramePlayerThread — Non-RT thread that injects recorded frames into synthesis
// ============================================================================
class FramePlayerThread final : public juce::Thread
{
public:
    FramePlayerThread(FrameSampler& sampler,
                      AudioImageBuffers* audioBuffers,
                      DoubleBuffer*      doubleBuffer);
    ~FramePlayerThread() override;
    void run() override;

private:
    FrameSampler&      sampler;
    AudioImageBuffers* audioBuffers;
    DoubleBuffer*      doubleBuffer; // for updating preprocessed_data during playback

    static uint64_t currentTimeUs() noexcept;

    // ── Transport fade-in state ────────────────────────────────────────────────
    // Tracks transitions of sampler_freeze_mode so that pressing PLAY after
    // HOLD (1) or STOP (2) produces a linear fade-in over sampler_fade_in_ms ms.
    // Members are written/read exclusively on the FramePlayerThread → no sync needed.
    int      transportPrevFreeze_  = 2;    // previous sampler_freeze_mode value
    uint64_t transportFadeStartUs_ = 0;    // µs timestamp when last PLAY was pressed
    float    transportFadeRamp_    = 1.0f; // current ramp multiplier [0..1]

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FramePlayerThread)
};

// ============================================================================
// FrameSampler — main controller
// ============================================================================
class FrameSampler
{
public:
    FrameSampler();
    ~FrameSampler();

    // =========================================================================
    // RT path — processBlock  (atomics ONLY — no alloc, no lock, no I/O)
    // =========================================================================
    void processMidi(const juce::MidiBuffer& midiBuffer);

    // =========================================================================
    // Non-RT path — called by udpThread hook after complete line assembled
    // Returns true if the frame was captured for recording.
    // =========================================================================
    bool onFrameAssembled(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                          uint16_t pixel_count, uint32_t line_id);

    // =========================================================================
    // Thread lifecycle (Non-RT, called from PluginProcessor)
    // =========================================================================
    void startPlayerThread(AudioImageBuffers* audioBuffers,
                           DoubleBuffer*      doubleBuffer);
    void stopPlayerThread();

    // =========================================================================
    // RT-safe queries (atomic reads)
    // =========================================================================
    bool isAnySlotPlaying() const noexcept
    {
        return atomicState.activePlaySlot.load(std::memory_order_acquire) >= 0;
    }
    bool isPassthroughEnabled() const noexcept
    {
        return atomicState.passthroughEnabled.load(std::memory_order_acquire);
    }
    bool isAnySlotRecording() const noexcept
    {
        return activeRecSlot.load(std::memory_order_relaxed) >= 0;
    }
    bool isEnabled() const noexcept
    {
        return enabled.load(std::memory_order_relaxed);
    }
    /** Index of the slot currently active in playback (-1 = none). Non-RT safe. */
    int getActivePlaySlot() const noexcept
    {
        return atomicState.activePlaySlot.load(std::memory_order_acquire);
    }

    // =========================================================================
    // Configuration (message thread / APVTS listener)
    // =========================================================================
    void setEnabled(bool e)          noexcept { enabled.store(e); }
    void setMidiChannel(int ch)      noexcept { midiChannel.store(ch); }   // 1–16
    void setOctaveOffset(int off)    noexcept { octaveOffset.store(off); } // -2..+2
    void setMaxDuration(float secs)  noexcept { maxDurationS.store(secs); }// 1..10

    /** Sequencer-gated recording.
     *  Called from processBlock (RT) every audio block.
     *  - gateSlot >= 0 : only capture frames when activeRecSlot == gateSlot
     *                    (sequencer is enabled + playing + step points at that bank)
     *  - gateSlot == -1 : no gating — frames are always captured (sequencer disabled
     *                     or current step is a passthrough/empty step)
     *  RT-safe: single atomic store, read only in onFrameAssembled (Non-RT). */
    void setSeqGateSlot(int gateSlot) noexcept
    {
        seqGateSlot.store(gateSlot, std::memory_order_relaxed);
    }

    /** Sequencer silent-step flag.
     *  Set by FrameSequencer::triggerStep() (RT) when a STEP_EMPTY (-1) step
     *  is triggered.  CisVisualizerComponent (message thread) reads this to
     *  force the visual display to white (silence) for that step.
     *  Also read by BlobVisualizerComponent to suppress blob detection.
     *  RT-safe: single atomic store (RT) / relaxed load (message thread). */
    void setSeqSilentStep(bool s) noexcept
    {
        seqSilentStepActive.store(s, std::memory_order_relaxed);
    }
    bool isSeqSilentStepActive() const noexcept
    {
        return seqSilentStepActive.load(std::memory_order_relaxed);
    }

    /** Freeze the FramePlayerThread on the current frame (sequencer hold/pause).
     *  When true: play_head does not advance, last injected frame stays visible.
     *  RT-safe: atomic store (message thread) / relaxed load (FramePlayerThread). */
    void setSeqPlayerHeld(bool h) noexcept
    {
        seqPlayerHeld_.store(h, std::memory_order_release);
    }
    bool isSeqPlayerHeld() const noexcept
    {
        return seqPlayerHeld_.load(std::memory_order_relaxed);
    }

    /** Shared final-gray buffer — written by CisVisualizerComponent after
     *  computing localDataGray, read by BlobVisualizerComponent.
     *  Both callers run exclusively on the JUCE message/timer thread so no
     *  locking is required. */
    void setFinalGrayBuffer(const std::vector<uint8_t>& data)
    {
        finalGrayBuffer_ = data;
    }
    const std::vector<uint8_t>& getFinalGrayBuffer() const noexcept
    {
        return finalGrayBuffer_;
    }

    int   getMidiChannel()  const noexcept { return midiChannel.load(); }
    int   getOctaveOffset() const noexcept { return octaveOffset.load(); }
    float getMaxDuration()  const noexcept { return maxDurationS.load(); }

    // =========================================================================
    // Per-slot play parameters (message/timer thread — Non-RT)
    // Written by UI controls; read by FramePlayerThread for playback behaviour.
    // =========================================================================
    void setSlotStartFrac(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].startFrac.store(juce::jlimit(0.0f, 1.0f, v),
                                          std::memory_order_relaxed);
    }
    void setSlotEndFrac(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].endFrac.store(juce::jlimit(0.0f, 1.0f, v),
                                        std::memory_order_relaxed);
    }
    void setSlotSpeed(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].speed.store(juce::jlimit(0.01f, 32.0f, v),
                                      std::memory_order_relaxed);
    }
    void setSlotLoopMode(int i, LoopMode m) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].loopMode.store(static_cast<int>(m),
                                         std::memory_order_relaxed);
    }
    /** Resume mode: when true, playback resumes from the last stopped position
     *  instead of restarting from startFrame on each Play press.
     *  Replaces the unimplemented 'priority' field. */
    void setSlotResumeMode(int i, bool r) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].resumeMode.store(r, std::memory_order_relaxed);
    }

    /** Live darken-blend mix amount [0..1]: 0=pure playback, 1=darken(sample,live). */
    void setSlotBlendAmount(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].blendAmount.store(juce::jlimit(0.0f, 1.0f, v),
                                            std::memory_order_relaxed);
    }

    /** Attack fade-in length [0..1], normalised over the active region.
     *  At the start bound the frame is fully white (silent); by attackLen
     *  fraction of the active region it is back to normal brightness. */
    void setSlotAttackLen(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].attackLen.store(juce::jlimit(0.0f, 1.0f, v),
                                          std::memory_order_relaxed);
    }
    /** Decay fade-out length [0..1], normalised over the active region.
     *  Mirrors attackLen: at the end bound the frame is fully white (silent);
     *  decayLen frames before that it is back to normal brightness. */
    void setSlotDecayLen(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].decayLen.store(juce::jlimit(0.0f, 1.0f, v),
                                         std::memory_order_relaxed);
    }
    /** Global brightness lift [0..1]: 0=normal, 1=fully white (silent).
     *  Applied uniformly to every pixel of the playback frame. */
    void setSlotBrightnessLift(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].brightnessLift.store(juce::jlimit(0.0f, 1.0f, v),
                                               std::memory_order_relaxed);
    }
    /** Treble (right-half pixels) fade to white [0..1].
     *  0=no change, 1=all right-half pixels → white (silence). */
    void setSlotTrebleCut(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].trebleCut.store(juce::jlimit(0.0f, 1.0f, v),
                                          std::memory_order_relaxed);
    }
    /** Bass (left-half pixels) fade to white [0..1].
     *  0=no change, 1=all left-half pixels → white (silence). */
    void setSlotBassCut(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].bassCut.store(juce::jlimit(0.0f, 1.0f, v),
                                        std::memory_order_relaxed);
    }

    float    getSlotStartFrac(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].startFrac.load(std::memory_order_relaxed);
    }
    float    getSlotEndFrac(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 1.0f;
        return slotParams[i].endFrac.load(std::memory_order_relaxed);
    }
    float    getSlotSpeed(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 1.0f;
        return slotParams[i].speed.load(std::memory_order_relaxed);
    }
    LoopMode getSlotLoopMode(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return LoopMode::LOOP;
        return static_cast<LoopMode>(slotParams[i].loopMode.load(std::memory_order_relaxed));
    }
    bool     getSlotResumeMode(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return false;
        return slotParams[i].resumeMode.load(std::memory_order_relaxed);
    }
    float    getSlotBlendAmount(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].blendAmount.load(std::memory_order_relaxed);
    }
    float    getSlotAttackLen(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].attackLen.load(std::memory_order_relaxed);
    }
    float    getSlotDecayLen(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].decayLen.load(std::memory_order_relaxed);
    }
    float    getSlotBrightnessLift(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].brightnessLift.load(std::memory_order_relaxed);
    }
    float    getSlotTrebleCut(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].trebleCut.load(std::memory_order_relaxed);
    }
    float    getSlotBassCut(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].bassCut.load(std::memory_order_relaxed);
    }

    /**
     * Copy the most recent live frame into caller-supplied buffers.
     * Called by FramePlayerThread (Non-RT) for darken-blend.
     * Thread-safe: protected by liveMutex_.
     *
     * @param maxPixels  Maximum number of pixels to copy (must be ≤ MAX_PIXELS).
     * @param outCount   Set to the number of pixels actually copied (0 if no frame yet).
     */
    void getLiveFrame(uint8_t* outR, uint8_t* outG, uint8_t* outB,
                      int maxPixels, int& outCount) noexcept;

    // =========================================================================
    // Non-RT: UI-triggered commands (message/timer thread — atomics only)
    // =========================================================================
    /** Toggle record for slotIndex from the UI.
     *  - If slot is RECORDING   → stop recording.
     *  - If slot is IDLE/ARMED  → start recording immediately (bypass ARMED).
     *  - If slot is PLAYING     → punch-in (stop playback, start recording).
     *  Any ongoing recording on another slot is stopped first (only one at a time). */
    void uiToggleRecord(int slotIndex) noexcept;

    /** Play a slot if it has content and is IDLE.
     *  If the slot is already PLAYING, stop it (restore passthrough).
     *  No-op if the slot is empty or currently recording. */
    void uiPlaySlot(int slotIndex) noexcept;

    // =========================================================================
    // Timeline / playhead queries (Non-RT, for UI display)
    // =========================================================================
    /** Returns the current frame index being played (updated by FramePlayerThread).
     *  Safe to read from the message thread — atomic relaxed load. */
    int getSlotPlayHead(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0;
        return currentPlayHead[i].load(std::memory_order_relaxed);
    }

    /** Sample normalised brightness [0..1] for each of the 'count' timeline columns.
     *  Non-RT only — do NOT call from processBlock. */
    void sampleBrightnessForTimeline(int slotIdx,
                                     float* outBrightness,
                                     int    count) const noexcept;

    /** Non-RT only. Sample bass (left pixels = low freq) and treble
     *  (right pixels = high freq) darkness for timeline spectral display.
     *  bass[k] / treble[k] ∈ [0..1] where 1 = max contrast (dark = sound). */
    void sampleSpectralForTimeline(int    slotIdx,
                                    float* outBass,
                                    float* outTreble,
                                    int    count) const noexcept;

    // =========================================================================
    // Internal: called by FramePlayerThread (Non-RT) to update playhead atomic
    // =========================================================================
    void notifyPlayHead(int i, int head) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            currentPlayHead[i].store(head, std::memory_order_relaxed);
    }
    void saveLastPlayHead(int i, int head) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            lastPlayHead[i].store(head, std::memory_order_relaxed);
    }
    int getLastPlayHead(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0;
        return lastPlayHead[i].load(std::memory_order_relaxed);
    }
    /** Save the direction (+1 or -1) when playback stops.
     *  Used to restore the PINGPONG sense when Resume mode is active. */
    void saveLastDirection(int i, int dir) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            lastDirection[i].store((dir < 0) ? -1 : 1, std::memory_order_relaxed);
    }
    int getLastDirection(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 1;
        const int d = lastDirection[i].load(std::memory_order_relaxed);
        return (d < 0) ? -1 : 1;
    }

    /** Clear all recorded frames from a slot and reset it to IDLE.
     *  Stops any ongoing recording or playback on that slot first. */
    void uiClearSlot(int slotIndex) noexcept;

    // =========================================================================
    // Slot management (Non-RT)
    // =========================================================================
    void clearSlot(int slotIndex);
    void clearAllSlots();

    /** Deep-copy all recorded frames and play parameters from srcIdx to dstIdx.
     *  Non-RT only — stops any ongoing activity on the destination slot first.
     *  No-op if srcIdx == dstIdx or srcIdx has no content. */
    void copySlotTo(int srcIdx, int dstIdx);

    // =========================================================================
    // Slot info queries (Non-RT, for UI polling)
    // =========================================================================
    SlotState   getSlotState(int i)      const noexcept;
    int         getSlotFrameCount(int i) const noexcept;
    uint64_t    getSlotDurationUs(int i) const noexcept;
    bool        slotHasContent(int i)    const noexcept;
    const char* getSlotLabel(int i)      const noexcept;
    void        setSlotLabel(int i, const char* label) noexcept;

    // =========================================================================
    // File I/O — Non-RT only
    // =========================================================================
    bool saveToFile(const juce::File& file) const;
    bool loadFromFile(const juce::File& file);

    // =========================================================================
    // Internal access for FramePlayerThread
    // =========================================================================
    FrameSlot&               getSlot(int i)  noexcept { return slots[i]; }
    FrameSamplerAtomicState& getAtomicState() noexcept { return atomicState; }

    // =========================================================================
    // Static singleton pointer — set in ctor, cleared in dtor.
    // Used by C hook functions in FrameSampler.cpp.
    // Limitation: only one VST instance supported per process.
    // =========================================================================
    static FrameSampler* s_instance;

private:
    // -------------------------------------------------------------------------
    // RT state (atomics only)
    // -------------------------------------------------------------------------
    FrameSamplerAtomicState atomicState;

    std::atomic<bool>  enabled     { false };
    std::atomic<int>   midiChannel { 1 };
    std::atomic<int>   octaveOffset{ 0 };
    std::atomic<float> maxDurationS{ 10.0f };
    // -1 = no gating; 0-11 = only record frames while sequencer step == this bank
    std::atomic<int>   seqGateSlot { -1 };
    // Set by FrameSequencer::triggerStep() when STEP_EMPTY is triggered;
    // cleared when a slot starts playing or STEP_LIVE is triggered.
    std::atomic<bool>  seqSilentStepActive { false };
    // true while the sequencer is in hold/pause — FramePlayerThread freezes
    // play_head and keeps re-outputting the current frame.
    std::atomic<bool>  seqPlayerHeld_      { false };

    // -------------------------------------------------------------------------
    // Non-RT state
    // -------------------------------------------------------------------------
    FrameSlot slots[FrameSamplerConstants::NUM_SLOTS];

    std::atomic<int> activeRecSlot { -1 }; // -1 = not recording
    uint64_t         recStartTimeUs = 0;   // set when recording starts

    // -------------------------------------------------------------------------
    // Player thread + shared buffer pointers
    // -------------------------------------------------------------------------
    std::unique_ptr<FramePlayerThread> playerThread;
    AudioImageBuffers* audioBuffers_ = nullptr; // stored by startPlayerThread()
    DoubleBuffer*      doubleBuffer_ = nullptr; // stored by startPlayerThread()

    // -------------------------------------------------------------------------
    // Per-slot play parameters — parallel to slots[], owned by FrameSampler.
    // Written by UI (Non-RT); read by FramePlayerThread (Non-RT).
    // -------------------------------------------------------------------------
    struct SlotPlayParams
    {
        std::atomic<float> startFrac { 0.0f }; // Normalised playback start [0..1]
        std::atomic<float> endFrac   { 1.0f }; // Normalised playback end   [0..1]
        std::atomic<float> speed     { 1.0f }; // Playback speed multiplier [0.1..8]
        std::atomic<int>   loopMode  { static_cast<int>(LoopMode::LOOP) };
        std::atomic<bool>  resumeMode  { false }; // Resume from last stop position
        std::atomic<float> blendAmount { 0.0f };  // Live darken-blend [0=sample, 1=full]
        std::atomic<float> attackLen      { 0.0f };  // Attack fade-in  [0=none, 1=full region]
        std::atomic<float> decayLen       { 0.0f };  // Decay fade-out  [0=none, 1=full region]
        std::atomic<float> brightnessLift { 0.0f };  // Global brightness lift [0=normal, 1=white]
        std::atomic<float> trebleCut      { 0.0f };  // High-freq fade [0=none, 1=full treble silence]
        std::atomic<float> bassCut        { 0.0f };  // Low-freq  fade [0=none, 1=full bass  silence]

        SlotPlayParams() = default;
        SlotPlayParams(const SlotPlayParams&)            = delete;
        SlotPlayParams& operator=(const SlotPlayParams&) = delete;
    };

    SlotPlayParams slotParams[FrameSamplerConstants::NUM_SLOTS];

    // Per-slot playhead atomics — written by FramePlayerThread, read by UI.
    // Per-slot playhead atomics — written by FramePlayerThread, read by UI.
    std::atomic<int> currentPlayHead[FrameSamplerConstants::NUM_SLOTS];
    std::atomic<int> lastPlayHead[FrameSamplerConstants::NUM_SLOTS];
    // Last playback direction (+1 / -1) — used to restore PINGPONG sense on resume.
    std::atomic<int> lastDirection[FrameSamplerConstants::NUM_SLOTS];

    // -------------------------------------------------------------------------
    // Live frame cache — updated by UDP thread (onFrameAssembled), read by
    // FramePlayerThread for the darken-blend feature.
    // Protected by liveMutex_ (both writers/readers are Non-RT).
    // -------------------------------------------------------------------------
    // -------------------------------------------------------------------------
    // Shared final-gray buffer — message-thread-only (no locking required).
    // Written by CisVisualizerComponent::timerCallback(), read by
    // BlobVisualizerComponent::timerCallback().  Both run on the JUCE message
    // thread so sequential access is guaranteed.
    // -------------------------------------------------------------------------
    std::vector<uint8_t> finalGrayBuffer_;

    // -------------------------------------------------------------------------
    // Slot data mutex — guards all non-atomic access to slots[].frames,
    // slots[].frame_count, slots[].has_content, slots[].duration_us, and
    // slots[].label between the message thread (readers: sampleSpectralForTimeline,
    // sampleBrightnessForTimeline, saveToFile, copySlotTo) and the UDP thread
    // (writer: onFrameAssembled) and any thread calling clearSlot / loadFromFile.
    //
    // RT path (processBlock / processMidi) MUST NOT acquire this mutex.
    // FramePlayerThread acquires it only for the brief frame-pointer read.
    // -------------------------------------------------------------------------
    mutable std::mutex slotsMutex_;

    // -------------------------------------------------------------------------
    // Live frame cache
    // -------------------------------------------------------------------------
    std::mutex liveMutex_;
    uint8_t    liveR_[FrameSamplerConstants::MAX_PIXELS] {};
    uint8_t    liveG_[FrameSamplerConstants::MAX_PIXELS] {};
    uint8_t    liveB_[FrameSamplerConstants::MAX_PIXELS] {};
    int        livePixelCount_ = 0;

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static uint64_t currentTimeUs() noexcept;

    // RT handlers — atomics only
    void handleNoteOn (int note, int velocity) noexcept;
    void handleNoteOff(int note)               noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FrameSampler)
};
