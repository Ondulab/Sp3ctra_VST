#pragma once

/*
 * LuxSampler.h
 *
 * Main controller for the LuxSampler subsystem.
 * Records, stores and replays the Sp3ctra CIS image stream via MIDI commands.
 *
 * RT safety contract (enforced throughout):
 *   - processMidi() runs on the audio thread → atomics ONLY, no alloc, no lock, no I/O
 *   - the producer hooks run on udpThread (Non-RT) → alloc allowed on first use
 *   - FramePlayerThread runs Non-RT → alloc/lock/I/O allowed
 *
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

    // Image EQ (SCORE-style graphic EQ): octave-boundary gain nodes over the pixel
    // axis, converted to a darkness shift (gain / EQ_DYN_RANGE_DB) at playback.
    // 24 dB maps to the full darkness span, so a band at +24 dB reaches full black
    // (max material) and at −24 dB reaches full white (total mask/silence).
    constexpr int     MAX_EQ_NODES        = 16;    // ≥ (octaves+1) of the widest range
    constexpr float   EQ_DYN_RANGE_DB     = 24.0f; // dB span that maps to full darkness

    // (The former C1..B1 PLAY-note range was removed with the multi-bank mixer:
    //  banks are no longer note-addressed. Playback is UI-driven and MIDI-
    //  mappable per bank through the unified MIDI-Learn PLAY targets.)

    // Max banks exposed in the UI (SETUP "Banks" 1..6, default 4). The engine
    // keeps NUM_SLOTS internal slots for file/session compatibility; only the
    // first 1..MAX_UI_BANKS are reachable from the bank grid.
    constexpr int MAX_UI_BANKS = 6;

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
    // (1 = former ARMED — removed with note-triggered recording; value left as a
    //  gap so RECORDING / PLAYING keep their persisted numeric values)
    RECORDING = 2,
    PLAYING   = 3
};

// ============================================================================
// LoopMode — per-slot playback loop behaviour
//
// The sampler UI presents this as THREE composable toggles (forward →,
// backward ←, repeat ⟲) rather than exclusive modes — see composeLoopMode /
// decomposeLoopMode below. The enum stays the persisted value (XML "loopMode"
// int, values 0..3 unchanged for old sessions).
// ============================================================================
enum class LoopMode : int
{
    NONE           = 0, // Play once forward, then stop and restore passthrough
    LOOP           = 1, // Loop forward: wrap play_head back to startFrame on overflow
    INVERSE        = 2, // Loop backward: play in reverse, wrap back to endFrame
    PINGPONG       = 3, // Alternate forward / backward each time a boundary is reached
    ONCE_BACKWARD  = 4, // Play once backward (end → start), then stop
    ONCE_ROUNDTRIP = 5  // Play forward then backward ONCE (one bounce), then stop
};

/** Toggle combo (fwd, bwd, repeat) → LoopMode. Callers guarantee at least one
 *  direction is on (the UI / MIDI apply ignore edits that would clear both). */
inline LoopMode composeLoopMode(bool fwd, bool bwd, bool repeat) noexcept
{
    if (repeat)
        return (fwd && bwd) ? LoopMode::PINGPONG
                            : (bwd ? LoopMode::INVERSE : LoopMode::LOOP);
    return (fwd && bwd) ? LoopMode::ONCE_ROUNDTRIP
                        : (bwd ? LoopMode::ONCE_BACKWARD : LoopMode::NONE);
}

/** LoopMode → toggle combo (inverse of composeLoopMode). */
inline void decomposeLoopMode(LoopMode m, bool& fwd, bool& bwd, bool& repeat) noexcept
{
    switch (m)
    {
        case LoopMode::NONE:           fwd = true;  bwd = false; repeat = false; break;
        case LoopMode::LOOP:           fwd = true;  bwd = false; repeat = true;  break;
        case LoopMode::INVERSE:        fwd = false; bwd = true;  repeat = true;  break;
        case LoopMode::PINGPONG:       fwd = true;  bwd = true;  repeat = true;  break;
        case LoopMode::ONCE_BACKWARD:  fwd = false; bwd = true;  repeat = false; break;
        case LoopMode::ONCE_ROUNDTRIP: fwd = true;  bwd = true;  repeat = false; break;
        default:                       fwd = true;  bwd = false; repeat = true;  break;
    }
}

// ============================================================================
// SlotMixMode — how one bank's playback frame is composited into the master
// frame when several banks play simultaneously (multi-bank mixer, mirrors the
// VIDEO MIX per-output blend). All maths run in the darkness domain
// (255 = white = silence); the per-bank level (1 − brightnessLift) pre-fades
// the voice toward white for ADD/DARKEN and acts as the opacity for MIX.
// ============================================================================
enum class SlotMixMode : int
{
    MIX    = 0, // Normal blend: master = lerp(master, voice, level)
    ADD    = 1, // Energy add (linear burn): darkness_master += level·darkness_voice
    DARKEN = 2  // Material union: master = min(master, prefaded voice) — default
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

    // ── Multi-voice playback (multi-bank simultaneous play, 2026-07-13) ───────
    // One VoiceCtx per simultaneously-playing sampler slot. All members are
    // touched exclusively on the FramePlayerThread.
    struct VoiceCtx
    {
        int      slot           = -1;    // slot index
        int      direction      = 1;     // +1 forward / -1 backward
        float    frameAcc       = 0.0f;  // sub-frame speed accumulator
        int      prevStartFrame = -1;
        int      prevEndFrame   = -1;
        bool     firstRangeInit = true;
        LoopMode prevLoopMode   = LoopMode::LOOP;
        bool     active         = false;
    };

    /** Advance one voice by one 1 ms tick and render its processed frame
     *  (trim/loop/attack/decay/floor/EQ) into outR/G/B (zero-filled
     *  beyond outNb). Returns false when the voice ended this tick
     *  (LoopMode::NONE reached its boundary). */
    bool tickVoice(VoiceCtx& v, FrameSlot& slot,
                   uint8_t* outR, uint8_t* outG, uint8_t* outB, int& outNb);

    /** Composite one voice frame into the master frame (darkness domain,
     *  255 = white = identity). level ∈ [0..1] fades the voice toward white
     *  (ADD/DARKEN) or acts as the blend opacity (MIX). */
    static void compositeVoice(uint8_t* mR, uint8_t* mG, uint8_t* mB,
                               const uint8_t* vR, const uint8_t* vG,
                               const uint8_t* vB, int vNb,
                               float level, SlotMixMode mode) noexcept;

    /** Shared injection tail for the composited frame: sampler snapshot,
     *  self-resampling record, live darken-blend, transport fade, sends
     *  staging, chain inserts, visual mix bus + preprocessed commit. */
    void outputFrame(uint8_t* workR, uint8_t* workG, uint8_t* workB, int nb,
                     float liveBlendAmount);

    /** One sampler playback session — the voice set follows slotState[]:
     *  every PLAYING slot is a voice, composited per-bank (level + mix mode). */
    void runSamplerSession();

    // Transport fade-in on Play REMOVED (2026-07-13): sampler starts at full
    // opacity immediately, so no cross-iteration fade-ramp state is needed.

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

    // (processMidi removed 2026-07-13: banks are no longer note-addressed —
    //  per-bank PLAY/REC live in the unified MIDI-Learn targets instead.)

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
    //   3. (P4-M3) capture is POSITIONAL: the chain executor records at each
    //      SAMPLER marker (lux_sampler_record_chain_frame / _record_input_frame)
    //      — the old global modulated-bus phase is gone.
    // =========================================================================
    bool onLiveFrameAssembled(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                              uint16_t pixel_count);
    // MIX/darken-blend reference = the module's INPUT at its SAMPLER marker
    // (fed per engine by the chain executor — P4 doctrine).
    void cacheInputFrame(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                         uint16_t pixel_count) noexcept;

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
    // true→false with activity in flight finalises the recording and stops
    // playback (the command drains in onLiveFrameAssembled stop running once
    // disabled — a REC stayed armed forever after the module was removed).
    void setEnabled(bool e)          noexcept;
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
        // 0 is a legal speed: the play head freezes and the current frame
        // sustains (drone) — move it manually with requestSlotSeek().
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].speed.store(juce::jlimit(0.0f, 32.0f, v),
                                      std::memory_order_relaxed);
    }

    /** Manual play-head seek (timeline scrub, only honoured while the slot is
     *  PLAYING — the player consumes it each tick). @p frac is absolute over
     *  the WHOLE slot [0..1]; the player clamps it into [start, end). */
    void requestSlotSeek(int i, float frac) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].seekFrac.store(juce::jlimit(0.0f, 1.0f, frac),
                                         std::memory_order_release);
    }
    /** Player thread only: fetch-and-clear the pending seek (<0 = none). */
    float consumeSlotSeek(int i) noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return -1.0f;
        return slotParams[i].seekFrac.exchange(-1.0f, std::memory_order_acq_rel);
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
     *  Since the multi-bank mixer this IS the bank's mix level, inverted
     *  (fader = 1 − lift): it pre-fades the voice toward white before it is
     *  composited into the master frame (see SlotMixMode). */
    void setSlotBrightnessLift(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].brightnessLift.store(juce::jlimit(0.0f, 1.0f, v),
                                               std::memory_order_relaxed);
    }
    /** Per-bank mix mode — how this bank composites into the master frame
     *  when several banks play simultaneously (see SlotMixMode). */
    void setSlotMixMode(int i, SlotMixMode m) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].mixMode.store(static_cast<int>(m),
                                        std::memory_order_relaxed);
    }
    SlotMixMode getSlotMixMode(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return SlotMixMode::DARKEN;
        return static_cast<SlotMixMode>(
            slotParams[i].mixMode.load(std::memory_order_relaxed));
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

    /** Legacy shared fade curve type — writes BOTH attack and decay so old
     *  callers keep working; new UI uses the per-fade setters below. */
    void setSlotFadeCurveType(int i, FadeCurveType type) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
        {
            const int t = static_cast<int>(type);
            slotParams[i].fadeCurveType.store(t, std::memory_order_relaxed);
            slotParams[i].attackCurveType.store(t, std::memory_order_relaxed);
            slotParams[i].decayCurveType.store(t, std::memory_order_relaxed);
        }
    }
    /** Legacy shared fade curve power — writes BOTH attack and decay. */
    void setSlotFadeCurvePower(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
        {
            const float p = juce::jlimit(0.1f, 10.0f, v);
            slotParams[i].fadeCurvePower.store(p, std::memory_order_relaxed);
            slotParams[i].attackCurvePower.store(p, std::memory_order_relaxed);
            slotParams[i].decayCurvePower.store(p, std::memory_order_relaxed);
        }
    }
    /** Per-fade curve type (independent attack / decay). */
    void setSlotAttackCurveType(int i, FadeCurveType type) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].attackCurveType.store(static_cast<int>(type),
                                                std::memory_order_relaxed);
    }
    void setSlotDecayCurveType(int i, FadeCurveType type) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].decayCurveType.store(static_cast<int>(type),
                                               std::memory_order_relaxed);
    }
    /** Per-fade curve power [0.1..10.0] (independent attack / decay). */
    void setSlotAttackCurvePower(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].attackCurvePower.store(juce::jlimit(0.1f, 10.0f, v),
                                                 std::memory_order_relaxed);
    }
    void setSlotDecayCurvePower(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].decayCurvePower.store(juce::jlimit(0.1f, 10.0f, v),
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
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 1.0f;
        return slotParams[i].speed.load(std::memory_order_relaxed);
    }
    LoopMode getSlotLoopMode(int i) const noexcept
    {
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
    FadeCurveType getSlotAttackCurveType(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return FadeCurveType::LINEAR;
        return static_cast<FadeCurveType>(
            slotParams[i].attackCurveType.load(std::memory_order_relaxed));
    }
    float    getSlotAttackCurvePower(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 1.0f;
        return slotParams[i].attackCurvePower.load(std::memory_order_relaxed);
    }
    FadeCurveType getSlotDecayCurveType(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return FadeCurveType::LINEAR;
        return static_cast<FadeCurveType>(
            slotParams[i].decayCurveType.load(std::memory_order_relaxed));
    }
    float    getSlotDecayCurvePower(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 1.0f;
        return slotParams[i].decayCurvePower.load(std::memory_order_relaxed);
    }
    /** Pre-EQ material floor [0..1]: remove darkness below this (→ white) before
     *  the EQ. 1.0 = total white mask. */
    void setSlotEqFloor(int i, float v) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            slotParams[i].eqFloor.store(juce::jlimit(0.0f, 1.0f, v),
                                        std::memory_order_relaxed);
    }
    float    getSlotEqFloor(int i) const noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].eqFloor.load(std::memory_order_relaxed);
    }
    // ── Image EQ (SCORE-style ±dB, boost + cut) — message thread writes, RT reads LUT ──
    // Stored as an encoded string in ScoreEqComponent::encodeState() format
    // ("minF|maxF|g0;g1;…"); rebuildFreqLut() turns it into a per-position dB LUT.
    /** Set slot i's EQ from an encoded curve string; republishes the LUT. Non-RT. */
    void setSlotEq(int i, const juce::String& encoded) noexcept;
    /** Return slot i's encoded EQ string ("" when flat). Non-RT. */
    juce::String getSlotEq(int i) const;

    // Per-band EQ gain access (message thread) — the slot EQ is a fixed 9-node
    // octave grid over kEqMinHz..kEqMaxHz (see ScoreEqComponent). Lets the MIDI
    // mapping tweak one band without the curve editor. Non-RT (parse/re-encode).
    static constexpr int    kEqBands = 9;
    static constexpr double kEqMinHz = 65.41;
    static constexpr double kEqMaxHz = 16744.04;
    /** Gain (dB, ±24) of EQ band @p band [0..8] on slot @p slot; 0 when flat. */
    float getSlotEqBandGain(int slot, int band) const noexcept;
    /** Set EQ band @p band [0..8] of slot @p slot to @p gainDb (clamped ±24),
     *  preserving the other bands; republishes the LUT. Non-RT. */
    void  setSlotEqBandGain(int slot, int band, float gainDb) noexcept;
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
     *  - If slot is IDLE        → start recording immediately.
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

    /** One-shot force-resume (module re-enable): the NEXT voice start for this
     *  slot resumes from lastPlayHead even when the slot's own Resume mode is
     *  OFF. Armed by setEnabled(true) for the banks it restarts, consumed by
     *  tickVoice's first range init. */
    void armForceResume(int i) noexcept
    {
        if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS)
            forceResumeOnce_[i].store(true, std::memory_order_release);
    }
    bool consumeForceResume(int i) noexcept
    {
        if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return false;
        return forceResumeOnce_[i].exchange(false, std::memory_order_acq_rel);
    }

    /** Clear all recorded frames from a slot and reset it to IDLE.
     *  Stops any ongoing recording or playback on that slot first. */
    void uiClearSlot(int slotIndex) noexcept;

    /** Reset every play parameter of a slot to factory defaults — trim, speed,
     *  loop, resume, fades, mixer (level + mix mode), EQ, floor, label.
     *  Settings follow content: applied to banks restored EMPTY at startup
     *  (state overlay) and by the NEW SESSION reset. Non-RT (message thread). */
    void resetSlotPlayParams(int slotIndex) noexcept;

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
    // (trim, speed, loop, resume, fades, frequency curves,
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
    // Image → slot (Non-RT, message thread only)
    // =========================================================================
    /** Replace @p slotIndex's content with an image file: each image row
     *  becomes one CIS frame (planar RGB, MAX_PIXELS wide), timestamps spread
     *  evenly over @p durationUs (0 → 5 s, the IMAGE module's default scan
     *  time). @p rotQuarters rotates the picture CLOCKWISE in quarter turns
     *  before conversion. The source path + rotation are remembered per slot
     *  and serialised with the slot params (srcImagePath / srcImageRot) in
     *  every container (.fslot / .sp3s / DAW state), so the bank can be
     *  re-rotated LOSSLESSLY later. Trim bounds reset to full.
     *  @return true on success (decode + conversion). */
    bool loadSlotFromImageFile(int slotIndex, const juce::File& imageFile,
                               int rotQuarters, uint64_t durationUs = 0);

    /** Rotate a bank loaded from an image: re-renders the frames from the
     *  remembered source file at (current + delta) quarter turns, keeping the
     *  bank's current duration. @return false when the slot has no source
     *  image (recorded banks) or the file no longer exists. */
    bool rotateSlotImage(int slotIndex, int deltaQuarters);

    /** Source image behind this bank ("" = recorded/.fslot content). */
    juce::String getSlotSourcePath(int i) const;
    /** Applied rotation of the source image, quarter turns CW (0..3). */
    int          getSlotSourceRot (int i) const;

    /** Re-render every image-bound bank from its remembered source picture
     *  (path + rotation), preserving trim bounds, label and duration.
     *  Restore helper: the persisted binding is authoritative over whatever
     *  frames a session file brought back (stale orientation), and it
     *  resurrects image banks even when NO session file exists — the bank
     *  frames themselves only live in the .sp3s. Non-RT (restore thread). */
    void rebuildImageBoundSlots();

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
     * Render a slot's recorded frames into an RGB image, Non-RT only (locks
     * slotsMutex_). Native orientation: X = pixel column (frequency), Y = frame
     * index (time, earliest on top) — this is what exportSlotImage writes.
     *
     * @param maxW/maxH  When > 0, nearest-neighbour downsample so the result fits
     *                   within maxW × maxH (0 = full resolution).
     * @param timeHorizontal  When true, transpose for the slot-editor backdrop:
     *                        X = frame (time, left→right), Y = pixel with treble
     *                        (high index) on top and bass (low index) on bottom.
     * @return An RGB image, or an invalid Image if the slot is empty.
     */
    juce::Image renderSlotImage(int slotIndex,
                                int maxW = 0,
                                int maxH = 0,
                                bool timeHorizontal = false) const;

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
    static constexpr int kMaxEngines = 8;           // P6 — sampler engines ×8 (0=A legacy, 1=B legacy, 2..7)
    static LuxSampler* engineAt(int i) noexcept
    {
        return (i >= 0 && i < kMaxEngines)
                   ? s_engines[i].load(std::memory_order_acquire) : nullptr;
    }
    // Pin API for the C fan-out hooks (UDP thread): busy++ BEFORE loading the
    // pointer so ~LuxSampler can wait for in-flight hook calls to drain after
    // unregistering (quiescence). Always pair pinEngine() with unpinEngine(),
    // even when the returned pointer is null.
    static LuxSampler* pinEngine(int i) noexcept
    {
        if (i < 0 || i >= kMaxEngines) return nullptr;
        s_engineBusy[i].fetch_add(1, std::memory_order_acq_rel);
        return s_engines[i].load(std::memory_order_acquire);
    }
    static void unpinEngine(int i) noexcept
    {
        if (i < 0 || i >= kMaxEngines) return;
        s_engineBusy[i].fetch_sub(1, std::memory_order_acq_rel);
    }
    int getEngineIndex() const noexcept { return engineIndex_; }
    char engineTag() const noexcept { return (char) ('A' + engineIndex_); }

    // =========================================================================
    // Player-release handshake (message thread ⇄ FramePlayerThread).
    // playerBusyMask_ mirrors the slots the player is CURRENTLY dereferencing
    // (bit i for slots[i] — multi-voice playback holds several at once);
    // 0 when idle. Every message-thread path that
    // frees/replaces a slot's frame buffer must stop playback and then
    // waitForPlayerRelease() before touching the buffer — stop commands are
    // asynchronous and the player reads frames WITHOUT slotsMutex_.
    // =========================================================================
    void addPlayerBusySlot(int slot) noexcept
    {
        playerBusyMask_.fetch_or(1u << slot, std::memory_order_acq_rel);
    }
    void removePlayerBusySlot(int slot) noexcept
    {
        playerBusyMask_.fetch_and(~(1u << slot), std::memory_order_acq_rel);
    }
    void clearPlayerBusyMask() noexcept
    {
        playerBusyMask_.store(0, std::memory_order_release);
    }
    bool isPlaybackSuspended() const noexcept
    {
        return playbackSuspended_.load(std::memory_order_acquire);
    }
    // Message thread only. slotIndex >= 0: wait until the player no longer works
    // on that slot; slotIndex < 0: wait until the player is fully idle. Bounded.
    void waitForPlayerRelease(int slotIndex, int timeoutMs = 100) const noexcept;

    // Resampling capture: write the (already chain-modulated) frame into THIS
    // engine's active recording slot. Non-RT (UDP thread / FramePlayerThread).
    // No snapshot side effect — the display owner owns the shared snapshot.
    void recordModulatedFrame(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                              uint16_t pixel_count, uint32_t line_id) noexcept;
    // True if THIS engine is currently the one driving the modulated channel
    // (aggregated across engines by lux_sampler_is_playing()).
    bool isDrivingChannel() const noexcept;
    // Playback arbiter — SCOPED to shared-chain topologies (multi-chain split,
    // 2026-07-13; per-PAIR since P6): engines on DIFFERENT chains each own
    // their stream and play simultaneously — the arbiter is a no-op between
    // them. Only an engine sharing a chain with the starting one (same
    // stream, e.g. A→B resampling rack) is evicted. RT-safe (atomic stores).
    static void stopOtherEnginesPlayback(int exceptIndex) noexcept;
    // Published by the processor on every chain-plan derive: bit j of
    // masks[i] = "engines i and j have a SAMPLER marker on the same chain".
    // Gates the playback arbiter per pair (only meaningful on a shared stream).
    static void setEngineShareMasks(const uint8_t masks[kMaxEngines]) noexcept
    {
        for (int i = 0; i < kMaxEngines; ++i)
            s_shareMask[i].store(masks[i], std::memory_order_release);
    }
    static bool enginesShareChainPair(int a, int b) noexcept
    {
        if (a < 0 || a >= kMaxEngines || b < 0 || b >= kMaxEngines) return false;
        return ((s_shareMask[a].load(std::memory_order_acquire) >> b) & 1u) != 0;
    }

private:
    // -------------------------------------------------------------------------
    // Multi-engine identity / registry (see kMaxEngines)
    // -------------------------------------------------------------------------
    int                     engineIndex_ = 0;
    bool                    registered_  = false; // this instance owns s_engines[engineIndex_]
    static std::atomic<LuxSampler*> s_engines[kMaxEngines];
    static std::atomic<int>         s_engineBusy[kMaxEngines]; // in-flight hook calls per slot
    static std::atomic<uint8_t> s_shareMask[kMaxEngines]; // per-pair chain sharing (plan-derived)

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

    // Bitmask of the slots the player is currently dereferencing (bit i for
    // slots[i]) — multi-voice playback can hold
    // several slots at once. 0 when idle — see waitForPlayerRelease().
    std::atomic<uint32_t> playerBusyMask_ { 0 };
    // True while saveToFile/saveSlotToFile copy frames chunk by chunk: the
    // UDP-thread rec-command drain is frozen meanwhile (a non-overdub START
    // rewrites frames[0..] and would corrupt the interleaved chunk copies).
    // mutable: the save methods are const. Commands stay queued.
    mutable std::atomic<bool> saveInProgress_ { false };
    // While true the player must not pick up new startPlayCmd commands (bulk
    // slot replacement in progress, e.g. loadFromFile). Commands stay queued.
    std::atomic<bool> playbackSuspended_ { false };

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
        std::atomic<bool>  resumeMode  { false }; // Resume from last stop position
        std::atomic<float> blendAmount { 0.0f };  // Live darken-blend [0=sample, 1=full]
        std::atomic<float> attackLen      { 0.0f };  // Attack fade-in  [0=none, 1=full region]
        std::atomic<float> decayLen       { 0.0f };  // Decay fade-out  [0=none, 1=full region]
        std::atomic<float> brightnessLift { 0.0f };  // Bank mix level, inverted [0=full, 1=white]
        std::atomic<int>   mixMode { static_cast<int>(SlotMixMode::DARKEN) }; // composite rule
        std::atomic<float> trebleCut      { 0.0f };  // High-freq fade [0=none, 1=full treble silence]
        std::atomic<float> bassCut        { 0.0f };  // Low-freq  fade [0=none, 1=full bass  silence]
        std::atomic<int>   fadeCurveType  { static_cast<int>(FadeCurveType::LINEAR) };
        std::atomic<float> fadeCurvePower { 1.0f };   // Curve power [0.1..10.0], 1.0=neutral
        // Independent attack / decay fade shaping (see setSlotAttackCurveType etc.).
        std::atomic<int>   attackCurveType  { static_cast<int>(FadeCurveType::LINEAR) };
        std::atomic<float> attackCurvePower { 1.0f };
        std::atomic<int>   decayCurveType   { static_cast<int>(FadeCurveType::LINEAR) };
        std::atomic<float> decayCurvePower  { 1.0f };
        // Pre-EQ material floor [0..1]: darkness below this is pushed to white
        // (removed) BEFORE the EQ, so boosting cannot resurrect the noise floor
        // into black bands. 1.0 = everything removed (total white mask).
        std::atomic<float> eqFloor          { 0.0f };
        // Pending manual play-head seek (absolute slot frac, <0 = none) —
        // written by the UI (timeline scrub), consumed by the player thread.
        std::atomic<float> seekFrac         { -1.0f };

        SlotPlayParams() = default;
        SlotPlayParams(const SlotPlayParams&)            = delete;
        SlotPlayParams& operator=(const SlotPlayParams&) = delete;
    };

    SlotPlayParams slotParams[LuxSamplerConstants::NUM_SLOTS];

    // -------------------------------------------------------------------------
    // Per-slot image EQ (SCORE-style ±dB, boost + cut).
    //   eqState_ : authoritative encoded curve (message thread only), in
    //     ScoreEqComponent::encodeState() format ("minF|maxF|g0;g1;…").
    //   freqLut_ (double-buffered) + freqLutActive_ : RT-published look-up table
    //     holding a GAIN IN dB per normalised pixel position, evaluated per pixel
    //     by FramePlayerThread. freqCurveActive_ lets the RT loop skip the effect
    //     entirely when the curve is flat.
    // Single-writer (message thread) / single-reader (player thread) publish.
    // -------------------------------------------------------------------------
    // Source image behind a bank loaded via loadSlotFromImageFile (message
    // thread only): full path + applied rotation (quarter turns CW). An empty
    // path marks recorded / .fslot-only content — not re-rotatable. Serialised
    // with the slot params (srcImagePath / srcImageRot) in every container.
    juce::String slotSrcPath_[LuxSamplerConstants::NUM_SLOTS];
    int          slotSrcRot_ [LuxSamplerConstants::NUM_SLOTS] {};

    juce::String         eqState_[LuxSamplerConstants::NUM_SLOTS];
    float                freqLut_[LuxSamplerConstants::NUM_SLOTS][2]
                                 [LuxSamplerConstants::FREQ_LUT_N];
    std::atomic<int>     freqLutActive_[LuxSamplerConstants::NUM_SLOTS];
    std::atomic<bool>    freqCurveActive_[LuxSamplerConstants::NUM_SLOTS];

    /** Initialise every slot's curve to flat (2 points, level 1.0). */
    void initFreqCurveDefaults() noexcept;
    /** Reset slot i's frequency curve to the flat default and republish its LUT
     *  (message thread). Called by the clear paths so a wiped slot does not keep
     *  applying a leftover vertical (HF/LF) filter to its next recording. */
    void resetSlotFreqCurve(int i) noexcept;
    /** Rebuild + publish slot i's LUT from its current points (message thread). */
    void rebuildFreqLut(int i) noexcept;
    /** Reset slot i's edit handles (start/end/attack/decay/floor) to defaults so a
     *  cleared slot starts fresh. Called by the clear paths. */
    void resetSlotEditHandles(int i) noexcept;

    // Per-slot playhead atomics — written by FramePlayerThread, read by UI.
    // Per-slot playhead atomics — written by FramePlayerThread, read by UI.
    std::atomic<int> currentPlayHead[LuxSamplerConstants::NUM_SLOTS];
    std::atomic<int> lastPlayHead[LuxSamplerConstants::NUM_SLOTS];
    // Last playback direction (+1 / -1) — used to restore PINGPONG sense on resume.
    std::atomic<int> lastDirection[LuxSamplerConstants::NUM_SLOTS];
    // One-shot force-resume flags (module re-enable) — see armForceResume().
    std::atomic<bool> forceResumeOnce_[LuxSamplerConstants::NUM_SLOTS] {};
    // Banks that were PLAYING when the module was disabled — restarted (with
    // force-resume) on re-enable. Message thread only (setEnabled).
    uint32_t resumeOnEnableMask_ = 0;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSampler)
};
