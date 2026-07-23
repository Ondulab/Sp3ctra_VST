#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "../luxsampler/LuxSampler.h"

/**
 * @brief Step sequencer for ONE LuxSampler engine's banks.
 *
 * Scope
 * -----
 *  The sequencer is INTERNAL to its sampler: one FrameSequencer instance per
 *  LuxSampler engine, addressing only that engine's 12 banks (steps store a
 *  plain slot index 0..11 or a sentinel). It is not a chain module and has no
 *  global reach — cross-engine step addressing was retired with the SEQUENCER
 *  rack module.
 *
 * RT Safety contract
 * ------------------
 *  - processBlock()     → audio thread: atomics ONLY, no alloc, no I/O
 *  - uiPlay/uiStop()    → message thread: writes atomic command flags
 *  - setStep()          → message thread: atomic write, RT reads it
 *  - saveToXml/load()   → message thread: serialisation only
 *
 * Timing modes
 * ------------
 *  DAW sync  : tracks AudioPlayHead PPQ position; each step = beatsPerStep PPQ units.
 *              Pauses when DAW transport is stopped.
 *  Internal  : accumulates phase from (bpm / sampleRate); advances step every
 *              beatsPerStep beats regardless of DAW state.
 */
class FrameSequencer
{
public:
    static constexpr int MAX_STEPS = 32;

    // ── Step sentinel values ──────────────────────────────────────────────────
    /** Step is empty: silence — no slot plays, no live passthrough.
     *  The visual display goes white and blob detection is suppressed. */
    static constexpr int STEP_EMPTY = -1;
    /** Step is an explicit live passthrough: the CIS live signal flows through
     *  as if no sequencer were active.  Assign this value deliberately to a
     *  step cell — it is NOT the default for un-configured steps. */
    static constexpr int STEP_LIVE  = -2;

    FrameSequencer();

    // ── Wiring ───────────────────────────────────────────────────────────────
    /** Bind this sequencer to its sampler engine. Message thread / init only. */
    void setLuxSampler(LuxSampler* fs) noexcept { sampler_ = fs; }
    /** The engine this sequencer drives, or nullptr if unset. */
    LuxSampler* getSampler() const noexcept { return sampler_; }

    // ── Configuration (message thread) ───────────────────────────────────────
    void setBpm          (float b) noexcept { bpm.store(b);         }
    void setNumSteps     (int   n) noexcept { numSteps.store(juce::jlimit(1, MAX_STEPS, n)); }
    void setLooping      (bool  l) noexcept { looping.store(l);     }
    void setDawSync      (bool  s) noexcept { dawSync.store(s);     }
    void setBeatsPerStep (int bps) noexcept { beatsPerStep.store(juce::jmax(1, bps)); }

    float getBpm()         const noexcept { return bpm.load();          }
    int   getNumSteps()    const noexcept { return numSteps.load();     }
    bool  isLooping()      const noexcept { return looping.load();      }
    bool  isDawSync()      const noexcept { return dawSync.load();      }
    int   getBeatsPerStep()const noexcept { return beatsPerStep.load(); }

    // ── Step data (message thread write / RT read) ────────────────────────────
    /** Assign a slot (0..NUM_SLOTS-1) of THIS engine to step i, or a sentinel
     *  (STEP_EMPTY / STEP_LIVE). */
    void setStep (int stepIdx, int bankIdx) noexcept;
    /** Returns the step's slot (≥0) or sentinel (<0). */
    int  getStep (int stepIdx) const noexcept;

    // ── Transport (message thread — converted to RT-safe atomic commands) ─────
    void uiPlay  () noexcept;
    void uiHold  () noexcept;   ///< Freeze on the current step indefinitely.
    void uiStop  () noexcept;
    /** Resume from the current step after a hold (pause).
     *  Clears the held flag without resetting phase or step counter,
     *  so playback continues exactly where it was paused. */
    void uiResume() noexcept;
    bool isPlaying() const noexcept { return playing.load(std::memory_order_relaxed); }
    bool isHeld()   const noexcept { return held.load(std::memory_order_relaxed); }

    // ── RT state queries (safe from any thread) ───────────────────────────────
    /** Currently active step index, or -1 when stopped. */
    int getCurrentStep() const noexcept { return currentStep.load(std::memory_order_relaxed); }

    // ── Audio thread: called every processBlock — atomics ONLY ───────────────
    void processBlock(juce::AudioPlayHead* playHead,
                      int                 numSamples,
                      double              sampleRate) noexcept;

    // ── State serialisation (message thread) ─────────────────────────────────
    void saveToXml   (juce::XmlElement& xml) const;
    bool loadFromXml (const juce::XmlElement& xml);

    /** Migration — load a pattern saved by the retired GLOBAL sequencer, whose
     *  steps encoded (engine, slot) as enc = engine * NUM_SLOTS + slot across
     *  every sampler. Keeps only the steps that addressed @p engineIdx (slot
     *  extracted); sentinel steps (LIVE / EMPTY) acted on the primary engine
     *  and are kept on engine 0 only. Timing attrs are applied as-is. */
    bool loadFromLegacyGlobalXml (const juce::XmlElement& xml, int engineIdx);

private:
    // ── The one engine this sequencer drives ──────────────────────────────────
    LuxSampler* sampler_ = nullptr;

    // ── Per-step slot assignment (0..NUM_SLOTS-1), or a negative sentinel ─────
    std::atomic<int> steps[MAX_STEPS];

    // ── Config atomics ────────────────────────────────────────────────────────
    std::atomic<float> bpm          { 120.0f };
    std::atomic<int>   numSteps     { 8 };    // matches the SeqNumSteps default
    std::atomic<bool>  looping      { true };
    std::atomic<bool>  dawSync      { true };
    std::atomic<int>   beatsPerStep { 1 };

    // ── Transport state ───────────────────────────────────────────────────────
    std::atomic<bool> playing     { false };
    std::atomic<int>  currentStep { -1 };

    // ── RT command pulses (one-shot: UI writes, audio thread consumes) ────────
    std::atomic<bool> startCmd  { false };
    std::atomic<bool> stopCmd   { false };
    std::atomic<bool> holdCmd   { false };  ///< Set by uiHold(); cleared on first RT check.
    /** Set by uiResume(). Consumed once by processBlock to re-anchor
     *  rtLastTriggeredStep to the current ppq/phase WITHOUT triggering
     *  a new step — prevents skipping to the next cell after a pause. */
    std::atomic<bool> resumeCmd { false };

    // ── Hold state: sequencer stays playing but does not advance the step ─────
    std::atomic<bool> held     { false };

    // ── RT-only mutable state (accessed exclusively from the audio thread) ────
    // No synchronisation needed — single writer/reader.
    int    rtLastTriggeredStep   = -1;
    double rtInternalPhaseBeats  = 0.0;   // accumulated beats (internal BPM)
    double rtLastPpqPosition     = -1.0;
    /** Bank index (-1..11) that was LAST triggered (playing OR recording).
     *  Used by triggerStep() to stop/finalise the previous bank before
     *  activating the next one — regardless of whether it was playing or
     *  recording (activePlaySlot is -1 during recording). */
    int    rtPrevActiveBank      = -1;

    // ── RT helper (atomics only) ──────────────────────────────────────────────
    void triggerStep (int stepIdx) noexcept;
    void rtStop      ()            noexcept;  // RT-safe full stop

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FrameSequencer)
};
