#pragma once

/*
 * LuxSampler.h
 *
 * Main controller for the LuxSampler subsystem.
 * Records, stores and replays the Sp3ctra CIS image stream via MIDI commands.
 *
 * RT safety contract (enforced throughout):
 *   - processMidi() runs on the audio thread → atomics ONLY, no alloc, no lock, no I/O
 *   - onFrameAssembled() runs on udpThread (Non-RT) → alloc allowed on first use
 *   - FramePlayerThread runs Non-RT → alloc/lock/I/O allowed
 *
 * Architecture: see docs/SPEC_LuxSampler.html §9
 */

#include "FadeCurve.h"
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
namespace LuxSamplerConstants
{
    constexpr int     NUM_SLOTS           = 12;
    // Sentinel "slot" index for the SCORE module's internal playback slot.
    // Equals NUM_SLOTS so it never collides with a real slot index. It is used
    // ONLY as the value of activePlaySlot / startPlayCmd and to resolve the
    // dedicated scoreSlot in FramePlayerThread — it must NEVER be used to index
    // any of the NUM_SLOTS-sized arrays (slotState[], currentPlayHead[], …).
    constexpr int     SCORE_SLOT          = NUM_SLOTS;
    // Max frames per slot — sized for the slot duration cap below:
    //   200 DPI sensor → ~2000 fps, 60 s × 2000 = 120 000, ×1.5 safety margin = 180 000.
    // Memory cost: lazy-allocated, ~1.87 GB per actively-used slot (sizeof(CapturedFrame)
    // ≈ 10 382 B × 180 000). Idle slots stay at 0 bytes thanks to FrameSlot::allocate().
    constexpr int     MAX_FRAMES_PER_SLOT = 180000;
    constexpr int     MAX_PIXELS          = 3456;  // Fixed 400 DPI (FIXED_BUFFER_PIXELS)
    constexpr float   MAX_DURATION_S      = 60.0f;

    // Spectral (frequency-axis) multi-point curve per slot.
    constexpr int     MAX_FREQ_PTS        = 32;    // max breakpoints in one band
    constexpr int     FREQ_LUT_N          = 1024;  // normalised look-up-table size
    // Two frequency bands, split at the spectrum midpoint (mirror editor):
    constexpr int     FREQ_BAND_LF        = 0;     // low  freq — left  pixels (bass)
    constexpr int     FREQ_BAND_HF        = 1;     // high freq — right pixels (treble)
    constexpr int     NUM_FREQ_BANDS      = 2;

    // MIDI note bases (C0 = MIDI 12 — Ableton/GM convention)
    constexpr int MIDI_REC_NOTE_BASE  = 12; // C0..B0 → slots 0..11
    constexpr int MIDI_PLAY_NOTE_BASE = 24; // C1..B1 → slots 0..11

    // .fsmp binary file format
    constexpr uint32_t FSMP_MAGIC      = 0x46534D50u; // "FSMP"
    constexpr uint16_t FSMP_VERSION    = 0x0001u;
    constexpr uint32_t FSMP_EOF_MARKER = 0xDEADBEEFu;
}

// ============================================================================
// SamplerSpectralPoint — one breakpoint of the per-slot frequency-axis curve.
// x = frequency position [0..1] (0 = low/bass/left pixel, 1 = high/treble/right).
// y = level [0..1] (1 = keep the pixel as recorded, 0 = push to white/silence).
// ============================================================================
struct SamplerSpectralPoint
{
    float x = 0.0f;
    float y = 1.0f;
};

/** Evaluate a Catmull-Rom–smoothed spectral curve at x ∈ [0..1].
 *  @p pts must be sorted by x with @p n ≥ 1; the curve passes through every point,
 *  is extended flat before the first / after the last, and the result is clamped
 *  to [0..1]. Shared by the RT LUT builder and the UI editor so both render the
 *  same smooth shape. */
inline float samplerSpectralCurveY(const SamplerSpectralPoint* pts, int n, float x) noexcept
{
    if (pts == nullptr || n <= 0) return 1.0f;
    if (n == 1 || x <= pts[0].x)  return pts[0].y;
    if (x >= pts[n - 1].x)        return pts[n - 1].y;

    int i = 0;
    while (i + 1 < n && x > pts[i + 1].x) ++i;      // segment [i, i+1]
    const float span = pts[i + 1].x - pts[i].x;
    const float t    = (span > 1.0e-6f) ? (x - pts[i].x) / span : 0.0f;

    const float y0 = pts[i > 0 ? i - 1 : 0].y;
    const float y1 = pts[i].y;
    const float y2 = pts[i + 1].y;
    const float y3 = pts[i + 2 < n ? i + 2 : n - 1].y;

    const float t2 = t * t, t3 = t2 * t;
    const float y  = 0.5f * ((2.0f * y1)
                     + (-y0 + y2) * t
                     + (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3) * t2
                     + (-y0 + 3.0f * y1 - 3.0f * y2 + y3) * t3);
    return juce::jlimit(0.0f, 1.0f, y);
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
    uint8_t  R[LuxSamplerConstants::MAX_PIXELS] {};
    uint8_t  G[LuxSamplerConstants::MAX_PIXELS] {};
    uint8_t  B[LuxSamplerConstants::MAX_PIXELS] {};
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
                           LuxSamplerConstants::MAX_FRAMES_PER_SLOT);
            capacity = LuxSamplerConstants::MAX_FRAMES_PER_SLOT;
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
// LuxSamplerAtomicState — lock-free RT ↔ Non-RT interface
// RT path (processBlock) writes these; Non-RT threads read them.
// ============================================================================
struct LuxSamplerAtomicState
{
    std::atomic<int>  slotState[LuxSamplerConstants::NUM_SLOTS];
    std::atomic<int>  activePlaySlot     { -1 };   // -1 = none playing
    std::atomic<bool> passthroughEnabled { true };  // false during PLAYING
    /** True ONLY when the sequencer is running and the current step is STEP_LIVE.
     *  Unlike passthroughEnabled (which is also true during normal idle/stop),
     *  this flag is set exclusively by triggerStep(STEP_LIVE) and cleared by
     *  triggerStep(anything_else) and rtStop().
     *  Used by the UDP thread to route live data through the Source=S path. */
    std::atomic<bool> seqLiveStepActive { false };
    /** Set by FrameSequencer::triggerStep() before posting startPlayCmd so that
     *  FramePlayerThread does NOT restore passthroughEnabled when the sample
     *  finishes or the slot has no content.  The sequencer is the only authority
     *  that decides when live resumes (STEP_LIVE or rtStop).
     *  Cleared by: rtStop(), handleNoteOn() play path, uiPlaySlot(). */
    std::atomic<bool> seqControlledPlay { false };

    // Command pulses: set by RT, cleared (exchange) by Non-RT threads
    std::atomic<bool> startRecCmd[LuxSamplerConstants::NUM_SLOTS];
    std::atomic<bool> stopRecCmd[LuxSamplerConstants::NUM_SLOTS];
    std::atomic<int>  startPlayCmd { -1 };   // slot index, -1 = no command
    std::atomic<bool> stopPlayCmd  { false };

    /** Manual SCORE scrub: UI posts a target frame (≥0); FramePlayerThread snaps
     *  the score play head to it on its next tick and disarms (exchange → -1).
     *  -1 = no pending seek. Only consulted for the dedicated score slot. */
    std::atomic<int>  scoreSeekHead { -1 };

    /** Manual SCORE scrub-audition: set while the user click-drags over the
     *  preview of a STOPPED score. FramePlayerThread plays the score slot but
     *  HOLDS the play head (no auto-advance) so the column under the cursor is
     *  re-injected every tick — the user hears a sustained tone that follows the
     *  drag. Cleared on mouse-up (uiEndScoreScrub), which stops like a normal STOP. */
    std::atomic<bool> scoreScrubbing { false };

    /** Silence-injection command — posted from RT (triggerStep(STEP_EMPTY) or
     *  rtStop()) and consumed by FramePlayerThread (Non-RT).
     *  When set, FramePlayerThread writes a full-white (255) frame to
     *  AudioImageBuffers, the sampler snapshot, and preprocessed_data so that
     *  the synthesis engine produces no sound instead of freezing on the last
     *  played frame.
     *  Covers two cases:
     *    • A slot was playing  → checked after the inner playback loop exits.
     *    • Nothing was playing → checked in the outer idle loop (e.g. STEP_EMPTY
     *      after STEP_LIVE, or as the very first step of a sequence). */
    std::atomic<bool> injectSilenceCmd { false };

    LuxSamplerAtomicState() noexcept
    {
        for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
        {
            slotState[i].store(static_cast<int>(SlotState::IDLE),
                               std::memory_order_relaxed);
            startRecCmd[i].store(false, std::memory_order_relaxed);
            stopRecCmd[i].store(false, std::memory_order_relaxed);
        }
    }

    LuxSamplerAtomicState(const LuxSamplerAtomicState&) = delete;
    LuxSamplerAtomicState& operator=(const LuxSamplerAtomicState&) = delete;
};

// ============================================================================
// Forward declarations
// ============================================================================
class LuxSampler;

// ============================================================================
// FramePlayerThread — Non-RT thread that injects recorded frames into synthesis
// ============================================================================
class FramePlayerThread final : public juce::Thread
{
public:
    FramePlayerThread(LuxSampler& sampler,
                      AudioImageBuffers* audioBuffers,
                      DoubleBuffer*      doubleBuffer);
    ~FramePlayerThread() override;
    void run() override;

private:
    LuxSampler&      sampler;
    AudioImageBuffers* audioBuffers;
    DoubleBuffer*      doubleBuffer; // for updating preprocessed_data during playback

    static uint64_t currentTimeUs() noexcept;

    /** Inject a full-white (255) frame into AudioImageBuffers, sampler snapshot,
     *  and preprocessed_data.  Called from the FramePlayerThread (Non-RT) when
     *  playback stops due to STEP_EMPTY, rtStop(), or LoopMode::NONE reaching end.
     *  Prevents the last played frame from freezing in the synthesis pipeline. */
    void injectWhiteFrame() noexcept;

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
// LuxSampler — main controller
// ============================================================================
class LuxSampler
{
public:
    /** @param engineIndex 0 = sampler A, 1 = sampler B (registry slot + arbiter id). */
    explicit LuxSampler(int engineIndex = 0);
    ~LuxSampler();

    // =========================================================================
    // RT path — processBlock  (atomics ONLY — no alloc, no lock, no I/O)
    // =========================================================================
    void processMidi(const juce::MidiBuffer& midiBuffer);

    // =========================================================================
    // Non-RT path — called by udpThread hook after complete line assembled
    // Returns true if the frame was captured for recording.
    //
    // ── Two-phase processing (channel-chain refactor) ─────────────────────
    // The image chain is now: Live → LuxPitch → LuxMask → LuxSampler.
    // udpThread() must therefore call the sampler twice per scanline:
    //
    //   1. onLiveFrameAssembled(R,G,B,n)
    //      – processes pending start/stop commands
    //      – caches the live frame for FramePlayerThread (darken-blend)
    //      – does NOT capture any frame into a recording slot
    //
    //   2. <udpThread runs LuxPitch then LuxMask on the live frame>
    //
    //   3. onModulatedFrameReady(mR,mG,mB,n,line_id)
    //      – mirrors the post-mask frame into the sampler snapshot
    //        (so the Modulated channel stays alive in idle / REC / STEP_LIVE)
    //      – writes the post-mask frame into the active recording slot,
    //        so the recorded sample includes LuxPitch + LuxMask effects
    //
    // The legacy single-call onFrameAssembled() is kept as a backward
    // compatible alias and now simply forwards to the two-phase API.
    // =========================================================================
    bool onLiveFrameAssembled(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                              uint16_t pixel_count);
    bool onModulatedFrameReady(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                               uint16_t pixel_count, uint32_t line_id);

    // Legacy entry point — performs both phases in a row using the live frame
    // as if Pitch/Mask were bypassed.  Kept so existing callers keep building
    // while the refactor lands; udpThread now uses the two-phase API directly.
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

    /** Overdub / extend mode (engine-wide).
     *  When ON, starting a record on a slot that already has content APPENDS
     *  the new frames after the existing take (tape-style "continue") instead of
     *  erasing it. New frame timestamps continue past the previous duration.
     *  When OFF (default), REC replaces the slot content as before.
     *  RT/UDP-safe: atomic store (message thread) / relaxed load (UDP thread). */
    void setOverdubMode(bool on) noexcept { overdubMode_.store(on, std::memory_order_relaxed); }
    bool getOverdubMode() const noexcept  { return overdubMode_.load(std::memory_order_relaxed); }

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
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].startFrac.store(juce::jlimit(0.0f, 1.0f, v),
                                          std::memory_order_relaxed);
    }
    void setSlotEndFrac(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].endFrac.store(juce::jlimit(0.0f, 1.0f, v),
                                        std::memory_order_relaxed);
    }
    void setSlotSpeed(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].speed.store(juce::jlimit(0.01f, 32.0f, v),
                                      std::memory_order_relaxed);
    }
    void setSlotLoopMode(int i, LoopMode m) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].loopMode.store(static_cast<int>(m),
                                         std::memory_order_relaxed);
    }
    /** Resume mode: when true, playback resumes from the last stopped position
     *  instead of restarting from startFrame on each Play press.
     *  Replaces the unimplemented 'priority' field. */
    void setSlotResumeMode(int i, bool r) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].resumeMode.store(r, std::memory_order_relaxed);
    }

    /** Live darken-blend mix amount [0..1]: 0=pure playback, 1=darken(sample,live). */
    void setSlotBlendAmount(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].blendAmount.store(juce::jlimit(0.0f, 1.0f, v),
                                            std::memory_order_relaxed);
    }

    /** Attack fade-in length [0..1], normalised over the active region.
     *  At the start bound the frame is fully white (silent); by attackLen
     *  fraction of the active region it is back to normal brightness. */
    void setSlotAttackLen(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].attackLen.store(juce::jlimit(0.0f, 1.0f, v),
                                          std::memory_order_relaxed);
    }
    /** Decay fade-out length [0..1], normalised over the active region.
     *  Mirrors attackLen: at the end bound the frame is fully white (silent);
     *  decayLen frames before that it is back to normal brightness. */
    void setSlotDecayLen(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].decayLen.store(juce::jlimit(0.0f, 1.0f, v),
                                         std::memory_order_relaxed);
    }
    /** Global brightness lift [0..1]: 0=normal, 1=fully white (silent).
     *  Applied uniformly to every pixel of the playback frame. */
    void setSlotBrightnessLift(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].brightnessLift.store(juce::jlimit(0.0f, 1.0f, v),
                                               std::memory_order_relaxed);
    }
    /** Treble (right-half pixels) fade to white [0..1].
     *  0=no change, 1=all right-half pixels → white (silence). */
    void setSlotTrebleCut(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].trebleCut.store(juce::jlimit(0.0f, 1.0f, v),
                                          std::memory_order_relaxed);
    }
    /** Bass (left-half pixels) fade to white [0..1].
     *  0=no change, 1=all left-half pixels → white (silence). */
    void setSlotBassCut(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].bassCut.store(juce::jlimit(0.0f, 1.0f, v),
                                        std::memory_order_relaxed);
    }

    /** Fade curve type — shared by all fades (Attack, Decay, TrebleCut, BassCut).
     *  Controls the shape of the crossfade transition. */
    void setSlotFadeCurveType(int i, FadeCurveType type) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].fadeCurveType.store(static_cast<int>(type),
                                              std::memory_order_relaxed);
    }
    /** Fade curve power [0.1..10.0] — controls the intensity of the curve shape.
     *  1.0 = neutral (linear-equivalent for EXP/LOG), >1 = sharper, <1 = gentler. */
    void setSlotFadeCurvePower(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].fadeCurvePower.store(juce::jlimit(0.1f, 10.0f, v),
                                               std::memory_order_relaxed);
    }
    /** Loop crossfade / overlap length [0..0.5] of the loop zone.
     *  Smooths the wrap in LOOP / INVERSE by fading the tail into the head. */
    void setSlotLoopOverlap(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].loopOverlap.store(juce::jlimit(0.0f, 0.5f, v),
                                            std::memory_order_relaxed);
    }

    float    getSlotStartFrac(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].startFrac.load(std::memory_order_relaxed);
    }
    float    getSlotEndFrac(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 1.0f;
        return slotParams[i].endFrac.load(std::memory_order_relaxed);
    }
    float    getSlotSpeed(int i) const noexcept
    {
        if (i == LuxSamplerConstants::SCORE_SLOT)
            return scoreParams.speed.load(std::memory_order_relaxed);
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 1.0f;
        return slotParams[i].speed.load(std::memory_order_relaxed);
    }
    LoopMode getSlotLoopMode(int i) const noexcept
    {
        if (i == LuxSamplerConstants::SCORE_SLOT)
            return static_cast<LoopMode>(scoreParams.loopMode.load(std::memory_order_relaxed));
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return LoopMode::LOOP;
        return static_cast<LoopMode>(slotParams[i].loopMode.load(std::memory_order_relaxed));
    }
    bool     getSlotResumeMode(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return false;
        return slotParams[i].resumeMode.load(std::memory_order_relaxed);
    }
    float    getSlotBlendAmount(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].blendAmount.load(std::memory_order_relaxed);
    }
    float    getSlotAttackLen(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].attackLen.load(std::memory_order_relaxed);
    }
    float    getSlotDecayLen(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].decayLen.load(std::memory_order_relaxed);
    }
    float    getSlotBrightnessLift(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].brightnessLift.load(std::memory_order_relaxed);
    }
    float    getSlotTrebleCut(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].trebleCut.load(std::memory_order_relaxed);
    }
    float    getSlotBassCut(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].bassCut.load(std::memory_order_relaxed);
    }
    FadeCurveType getSlotFadeCurveType(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return FadeCurveType::LINEAR;
        return static_cast<FadeCurveType>(
            slotParams[i].fadeCurveType.load(std::memory_order_relaxed));
    }
    float    getSlotFadeCurvePower(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 1.0f;
        return slotParams[i].fadeCurvePower.load(std::memory_order_relaxed);
    }
    float    getSlotLoopOverlap(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].loopOverlap.load(std::memory_order_relaxed);
    }

    // ── Frequency-axis multi-point curves (message thread writes, RT reads LUT) ──
    // Two bands per slot: FREQ_BAND_LF (bass, left pixels) and FREQ_BAND_HF
    // (treble, right pixels), combined into one LUT split at the spectrum midpoint.
    /** Replace slot i / @p band's curve with @p n points (clamped to MAX_FREQ_PTS,
     *  kept sorted by x) and republish the slot LUT. Non-RT (message thread). */
    void setSlotFreqCurve(int i, int band, const SamplerSpectralPoint* pts, int n) noexcept;
    /** Copy slot i / @p band's points into @p out (up to maxN). Returns the count. */
    int  getSlotFreqCurve(int i, int band, SamplerSpectralPoint* out, int maxN) const noexcept;
    /** RT: true when the curve is not flat (worth applying). */
    bool isFreqCurveActive(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return false;
        return freqCurveActive_[i].load(std::memory_order_acquire);
    }
    /** RT: index (0/1) of the currently published LUT buffer for slot i. */
    int  getFreqLutActive(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0;
        return freqLutActive_[i].load(std::memory_order_acquire);
    }
    /** RT: pointer to LUT buffer @p buf (0/1) of slot i — stable during a frame. */
    const float* getFreqLut(int i, int buf) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return nullptr;
        return freqLut_[i][buf & 1];
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
    // SCORE module playback (Non-RT) — reuses FramePlayerThread via the
    // dedicated internal scoreSlot (sentinel activePlaySlot == SCORE_SLOT).
    // The SCORE block in CHAIN 1 plays a generated spectrogram image exactly
    // like a sampler slot, but with its own transport (Play/Stop/Loop/Speed).
    // =========================================================================
    /** Convert a generated spectrogram into playable frames. ONLY the `band`
     *  region (the part a CIS sensor would scan — see ScoreGenRenderer) is read:
     *  each band COLUMN → one CapturedFrame.
     *
     *  The band's vertical axis is LINEAR in frequency over [scoreMinHz,
     *  scoreMaxHz] (bottom = min). The synthesis maps pixel index LOGARITHMICALLY
     *  over the instrument's range; so each output pixel is sampled from the band
     *  at the band row whose linear frequency equals the synth's LOG frequency
     *  for that pixel — making the reconstructed pitches faithful. Frequencies
     *  outside the band map to white (silence).
     *
     *  Passing an empty band falls back to the full image; passing scoreMax<=min
     *  falls back to a plain flipped linear resample. Non-RT — stops any score
     *  playback first. Safe to call from the message thread.
     *
     *  `stereo`: when true the image is a colour composite (left=red, right=blue)
     *  and each frame keeps its R/G/B so LuxStral's colour-temperature panning
     *  reproduces the stereo image. When false the (greyscale) red channel is
     *  copied to R=G=B as before (centred/mono playback). */
    void loadScoreFramesFromImage(const juce::Image& image,
                                  juce::Rectangle<int> band = {},
                                  double scoreMinHz = 0.0,
                                  double scoreMaxHz = 0.0,
                                  bool stereo = false);

    /** Toggle SCORE playback: start if idle (taking over the Modulated channel
     *  like the sampler), stop if already playing. No-op if no frames loaded. */
    void uiPlayScore() noexcept;
    /** Stop SCORE playback and restore live passthrough. */
    void uiStopScore() noexcept;
    /** Full SCORE teardown for module removal: stop playback (so FramePlayerThread
     *  releases scoreSlot), then free the slot buffer and reset its state. After
     *  this scoreHasContent()/isScorePlaying() are both false. Non-RT (takes the
     *  slot mutex), call on the message thread only. */
    void uiDiscardScore();
    /** Begin a scrub-audition on a STOPPED score: take over the synthesis channel
     *  and start FramePlayerThread in held-position mode so the column under the
     *  drag is re-injected continuously (the user hears it). No-op (returns false)
     *  if the score is already playing or has no content. Pair with uiEndScoreScrub
     *  on mouse-up. The play-head must be armed first (uiSeekScore) so the audition
     *  starts at the clicked column. */
    bool uiBeginScoreScrub() noexcept;
    /** End a scrub-audition (mouse-up): stop injection and restore live passthrough,
     *  exactly like a STOP, but keep the play head where the scrub left it. */
    void uiEndScoreScrub() noexcept;
    /** True while a scrub-audition is active (held-position injection). */
    bool isScoreScrubbing() const noexcept
    {
        return atomicState.scoreScrubbing.load(std::memory_order_acquire);
    }
    /** True while the SCORE module is playing (drives the PLAY/STOP button + LED). */
    bool isScorePlaying() const noexcept
    {
        return scorePlaying.load(std::memory_order_acquire);
    }
    /** True once a generated image has been loaded into the score slot. */
    bool scoreHasContent() const noexcept { return scoreSlot.has_content; }
    /** Number of frames (time columns) loaded into the score slot. */
    int  getScoreFrameCount() const noexcept { return scoreSlot.frame_count; }
    /** Current score playback head (frame index). Non-RT safe (atomic). */
    int  getScorePlayHead() const noexcept
    {
        return scorePlayHead.load(std::memory_order_relaxed);
    }
    /** Internal: called by FramePlayerThread to publish the score play head. */
    void notifyScorePlayHead(int head) noexcept
    {
        scorePlayHead.store(head, std::memory_order_relaxed);
    }
    /** Arm a one-shot resume frame for the NEXT uiPlayScore() so the play head
     *  survives a frame reload (e.g. live EQ re-apply) instead of restarting at
     *  0. Pass a frame index ≥0; cleared automatically once consumed. */
    void setScoreResumeHead(int frame) noexcept
    {
        scoreResumeHead.store(frame, std::memory_order_relaxed);
    }
    /** Internal: FramePlayerThread takes the armed resume frame (returns -1 when
     *  none) and disarms it, so a fresh PLAY always starts from the beginning. */
    int consumeScoreResumeHead() noexcept
    {
        return scoreResumeHead.exchange(-1, std::memory_order_relaxed);
    }
    /** Internal: FramePlayerThread takes the armed relay slot (the sampler slot to
     *  resume when SCORE relinquishes the shared channel) and disarms it; returns
     *  -1 when none is armed. */
    int consumeScoreRelaySlot() noexcept
    {
        return scoreRelaySlot_.exchange(-1, std::memory_order_relaxed);
    }
    /** Manually move the score play head to @p frame (UI scrub).
     *  • If playing, FramePlayerThread snaps the live head there on its next tick.
     *  • Either way, arms it as the resume point so a subsequent PLAY starts there,
     *    and publishes it immediately so the UI play-head line follows the drag. */
    void uiSeekScore(int frame) noexcept
    {
        const int n = scoreSlot.frame_count;
        if (n <= 0) return;
        const int f = juce::jlimit(0, n - 1, frame);
        atomicState.scoreSeekHead.store(f, std::memory_order_release); // live seek if playing
        scoreResumeHead.store(f, std::memory_order_relaxed);           // start here on next PLAY
        scorePlayHead.store(f, std::memory_order_relaxed);             // reflect immediately in UI
    }
    void setScoreSpeed(float v) noexcept
    {
        scoreParams.speed.store(juce::jlimit(0.01f, 32.0f, v), std::memory_order_relaxed);
    }
    void setScoreLoopMode(LoopMode m) noexcept
    {
        scoreParams.loopMode.store(static_cast<int>(m), std::memory_order_relaxed);
    }
    /** Internal: called by FramePlayerThread when score playback ends on its own
     *  (LoopMode::NONE reached the end). Non-RT. */
    void notifyScoreStopped() noexcept { scorePlaying.store(false, std::memory_order_release); }
    /** Internal access for FramePlayerThread to the dedicated score slot. */
    FrameSlot& getScoreSlot() noexcept { return scoreSlot; }

    // =========================================================================
    // Timeline / playhead queries (Non-RT, for UI display)
    // =========================================================================
    /** Returns the current frame index being played (updated by FramePlayerThread).
     *  Safe to read from the message thread — atomic relaxed load. */
    int getSlotPlayHead(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0;
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

    /** Non-RT only. Sample the slot's average spectral energy PROFILE across the
     *  frequency (pixel) axis — left = low, right = high — averaged over time.
     *  outProfile[b] ∈ [0..1] (1 = dark = energy). Used as a backdrop behind the
     *  SpectralCurveComponent so the user shapes the filter against the content. */
    void sampleFreqProfileForCurve(int    slotIdx,
                                    float* outProfile,
                                    int    count) const noexcept;

    // =========================================================================
    // Internal: called by FramePlayerThread (Non-RT) to update playhead atomic
    // =========================================================================
    void notifyPlayHead(int i, int head) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            currentPlayHead[i].store(head, std::memory_order_relaxed);
    }
    void saveLastPlayHead(int i, int head) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            lastPlayHead[i].store(head, std::memory_order_relaxed);
    }
    int getLastPlayHead(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0;
        return lastPlayHead[i].load(std::memory_order_relaxed);
    }
    /** Save the direction (+1 or -1) when playback stops.
     *  Used to restore the PINGPONG sense when Resume mode is active. */
    void saveLastDirection(int i, int dir) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            lastDirection[i].store((dir < 0) ? -1 : 1, std::memory_order_relaxed);
    }
    int getLastDirection(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 1;
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

    /** Destructively crop the slot to its current [startFrac, endFrac] region.
     *  Non-RT only (message thread) — stops any ongoing activity on the slot
     *  first, moves the kept frames to the front of the buffer, rebases their
     *  timestamps to t₀=0 and resets startFrac=0 / endFrac=1.  No-op if the slot
     *  is empty or the bounds already cover the whole take. */
    void cropSlotToBounds(int slotIndex);

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
    // Per-slot play-parameter XML — Non-RT only
    //
    // Single source of truth for serialising one slot's play parameters
    // (trim, speed, loop, resume, fades, loop crossfade, frequency curves,
    // label). Shared by the .fslot format, the .sp3s session file and the
    // DAW state blob so no consumer can drift out of sync with the others.
    // =========================================================================
    /** Write slot @p slotIndex's play parameters as attributes on @p xml. */
    void slotParamsToXml(int slotIndex, juce::XmlElement& xml) const;
    /** Apply parameters written by slotParamsToXml. Attributes absent from
     *  @p xml fall back to their defaults. */
    void slotParamsFromXml(int slotIndex, const juce::XmlElement& xml);

    // =========================================================================
    // Per-slot file I/O — Non-RT only (.fslot single-slot format)
    // =========================================================================
    /**
     * Save a single slot (frames + per-slot play parameters) to a .fslot file.
     * The file is fully self-contained: it carries both the raw recorded frames
     * and the SlotPlayParams (speed, loop mode, fades, etc.) as embedded XML.
     *
     * @return true on success; false if slot empty or write failed.
     */
    bool saveSlotToFile(int slotIndex, const juce::File& file) const;

    /**
     * Load a .fslot file into the given destination slot, replacing any
     * existing content. Stops any ongoing record/play on that slot first.
     *
     * @return true on success; false if file invalid or read failed.
     */
    bool loadSlotFromFile(int slotIndex, const juce::File& file);

    // =========================================================================
    // Image export — Non-RT only
    // =========================================================================
    /**
     * Export a slot's recorded frames as a single image file.
     * Each captured frame becomes one row of the output image (X = pixel column,
     * Y = frame index, channels = R/G/B).
     *
     * @param slotIndex  Slot index [0..NUM_SLOTS-1].
     * @param file       Destination file (extension determines container).
     * @param asPng      true → PNG (lossless), false → JPEG (quality 90).
     * @return true on success; false if slot is empty or file write failed.
     */
    bool exportSlotImage(int slotIndex, const juce::File& file, bool asPng) const;

    /**
     * Export all slots containing data to a directory using a stable naming
     * pattern: "<baseName>_slot<NN>_<label>.<ext>".
     * @return number of images successfully written.
     */
    int  exportAllSlotsImages(const juce::File& destDirectory,
                              const juce::String& baseName,
                              bool asPng) const;


    // =========================================================================
    // Internal access for FramePlayerThread
    // =========================================================================
    FrameSlot&               getSlot(int i)  noexcept { return slots[i]; }
    LuxSamplerAtomicState& getAtomicState() noexcept { return atomicState; }

    // =========================================================================
    // Multi-engine registry — each engine registers itself (by index) in its
    // ctor and clears the slot in its dtor. The C hook functions in
    // LuxSampler.cpp reach every live engine (A, B, …) through engineAt().
    // =========================================================================
    static constexpr int kMaxEngines = 2;           // sampler A + sampler B
    static LuxSampler* engineAt(int i) noexcept
    {
        return (i >= 0 && i < kMaxEngines) ? s_engines[i] : nullptr;
    }
    int getEngineIndex() const noexcept { return engineIndex_; }

    // Resampling capture: write the (already chain-modulated) frame into THIS
    // engine's active recording slot. Non-RT (UDP thread). No snapshot side
    // effect — the engine that is playing owns the shared sampler snapshot.
    void recordModulatedFrame(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                              uint16_t pixel_count, uint32_t line_id) noexcept;
    // Mirror a frame into the shared sampler snapshot (idle display). Non-RT.
    void mirrorSamplerSnapshot(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                               uint16_t pixel_count) noexcept;
    // True if THIS engine is currently the one driving the modulated channel
    // (aggregated across engines by lux_sampler_is_playing()).
    bool isDrivingChannel() const noexcept;
    // Playback arbiter: stop playback on every OTHER engine — a single modulated
    // channel means one engine plays at a time. RT-safe (atomic stores only).
    static void stopOtherEnginesPlayback(int exceptIndex) noexcept;

private:
    // -------------------------------------------------------------------------
    // Multi-engine identity / registry (see kMaxEngines)
    // -------------------------------------------------------------------------
    int                     engineIndex_ = 0;
    static LuxSampler*      s_engines[kMaxEngines];
    static std::atomic<int> s_playbackOwner;   // engine index driving the channel, -1 = none

    // -------------------------------------------------------------------------
    // RT state (atomics only)
    // -------------------------------------------------------------------------
    LuxSamplerAtomicState atomicState;

    std::atomic<bool>  enabled     { false };
    std::atomic<int>   midiChannel { 1 };
    std::atomic<int>   octaveOffset{ 0 };
    std::atomic<float> maxDurationS{ 10.0f };
    // Overdub / extend: when ON, REC on a non-empty slot appends instead of erasing.
    std::atomic<bool>  overdubMode_{ false };
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
    FrameSlot slots[LuxSamplerConstants::NUM_SLOTS];

    std::atomic<int> activeRecSlot { -1 }; // -1 = not recording
    uint64_t         recStartTimeUs = 0;   // set when recording starts
    // Timestamp offset for the current record session (UDP thread only):
    // 0 for a fresh take, the slot's prior duration_us when appending (overdub).
    // New frame timestamps = recBaseUs_ + (now - recStartTimeUs).
    uint64_t         recBaseUs_     = 0;

    // -------------------------------------------------------------------------
    // Player thread + shared buffer pointers
    // -------------------------------------------------------------------------
    std::unique_ptr<FramePlayerThread> playerThread;
    AudioImageBuffers* audioBuffers_ = nullptr; // stored by startPlayerThread()
    DoubleBuffer*      doubleBuffer_ = nullptr; // stored by startPlayerThread()

    // -------------------------------------------------------------------------
    // Per-slot play parameters — parallel to slots[], owned by LuxSampler.
    // Written by UI (Non-RT); read by FramePlayerThread (Non-RT).
    // -------------------------------------------------------------------------
    struct SlotPlayParams
    {
        std::atomic<float> startFrac { 0.0f }; // Normalised playback start [0..1]
        std::atomic<float> endFrac   { 1.0f }; // Normalised playback end   [0..1]
        std::atomic<float> speed     { 1.0f }; // Playback speed multiplier [0.1..8]
        std::atomic<int>   loopMode  { static_cast<int>(LoopMode::LOOP) };
        std::atomic<float> loopOverlap { 0.0f };  // Loop crossfade length [0..0.5] of zone
        std::atomic<bool>  resumeMode  { false }; // Resume from last stop position
        std::atomic<float> blendAmount { 0.0f };  // Live darken-blend [0=sample, 1=full]
        std::atomic<float> attackLen      { 0.0f };  // Attack fade-in  [0=none, 1=full region]
        std::atomic<float> decayLen       { 0.0f };  // Decay fade-out  [0=none, 1=full region]
        std::atomic<float> brightnessLift { 0.0f };  // Global brightness lift [0=normal, 1=white]
        std::atomic<float> trebleCut      { 0.0f };  // High-freq fade [0=none, 1=full treble silence]
        std::atomic<float> bassCut        { 0.0f };  // Low-freq  fade [0=none, 1=full bass  silence]
        std::atomic<int>   fadeCurveType  { static_cast<int>(FadeCurveType::LINEAR) };
        std::atomic<float> fadeCurvePower { 1.0f };   // Curve power [0.1..10.0], 1.0=neutral

        SlotPlayParams() = default;
        SlotPlayParams(const SlotPlayParams&)            = delete;
        SlotPlayParams& operator=(const SlotPlayParams&) = delete;
    };

    SlotPlayParams slotParams[LuxSamplerConstants::NUM_SLOTS];

    // -------------------------------------------------------------------------
    // Per-slot frequency-axis multi-point curve.
    //   freqPts_/freqPtCount_ : authoritative breakpoints (message thread only).
    //   freqLut_ (double-buffered) + freqLutActive_ : RT-published look-up table
    //     evaluated per pixel by FramePlayerThread. freqCurveActive_ lets the RT
    //     loop skip the effect entirely when the curve is flat.
    // Single-writer (message thread) / single-reader (player thread) publish.
    // -------------------------------------------------------------------------
    SamplerSpectralPoint freqPts_[LuxSamplerConstants::NUM_SLOTS]
                                 [LuxSamplerConstants::NUM_FREQ_BANDS]
                                 [LuxSamplerConstants::MAX_FREQ_PTS];
    int                  freqPtCount_[LuxSamplerConstants::NUM_SLOTS]
                                     [LuxSamplerConstants::NUM_FREQ_BANDS];
    float                freqLut_[LuxSamplerConstants::NUM_SLOTS][2]
                                 [LuxSamplerConstants::FREQ_LUT_N];
    std::atomic<int>     freqLutActive_[LuxSamplerConstants::NUM_SLOTS];
    std::atomic<bool>    freqCurveActive_[LuxSamplerConstants::NUM_SLOTS];

    /** Initialise every slot's curve to flat (2 points, level 1.0). */
    void initFreqCurveDefaults() noexcept;
    /** Rebuild + publish slot i's LUT from its current points (message thread). */
    void rebuildFreqLut(int i) noexcept;

    // -------------------------------------------------------------------------
    // SCORE module — dedicated internal slot + params, played by the same
    // FramePlayerThread via the SCORE_SLOT sentinel. Independent of the 12
    // sampler slots (never indexes the NUM_SLOTS-sized arrays).
    // -------------------------------------------------------------------------
    FrameSlot         scoreSlot;
    SlotPlayParams    scoreParams;
    std::atomic<bool> scorePlaying  { false };
    std::atomic<int>  scorePlayHead { 0 };
    // One-shot resume frame for the NEXT score play start (-1 = from beginning).
    // Lets the UI preserve the play head across a frame reload (e.g. EQ re-apply)
    // instead of snapping back to 0. Consumed (reset to -1) by FramePlayerThread.
    std::atomic<int>  scoreResumeHead { -1 };
    // Relay: the sampler slot that owned the shared playback channel when SCORE
    // took over (-1 = none). When SCORE stops, FramePlayerThread hands the channel
    // back to this slot so the sampler stream resumes underneath instead of going
    // silent ("SCORE prend le relais ; à l'arrêt, le flux du sampler perdure").
    // Armed by uiPlayScore(); consumed (reset to -1) by FramePlayerThread.
    std::atomic<int>  scoreRelaySlot_ { -1 };

    // Per-slot playhead atomics — written by FramePlayerThread, read by UI.
    // Per-slot playhead atomics — written by FramePlayerThread, read by UI.
    std::atomic<int> currentPlayHead[LuxSamplerConstants::NUM_SLOTS];
    std::atomic<int> lastPlayHead[LuxSamplerConstants::NUM_SLOTS];
    // Last playback direction (+1 / -1) — used to restore PINGPONG sense on resume.
    std::atomic<int> lastDirection[LuxSamplerConstants::NUM_SLOTS];

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
    uint8_t    liveR_[LuxSamplerConstants::MAX_PIXELS] {};
    uint8_t    liveG_[LuxSamplerConstants::MAX_PIXELS] {};
    uint8_t    liveB_[LuxSamplerConstants::MAX_PIXELS] {};
    int        livePixelCount_ = 0;

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static uint64_t currentTimeUs() noexcept;

    // RT handlers — atomics only
    void handleNoteOn (int note, int velocity) noexcept;
    void handleNoteOff(int note)               noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSampler)
};
