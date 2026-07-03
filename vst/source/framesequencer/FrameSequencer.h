#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "../luxsampler/LuxSampler.h"

/**
 * @brief Step sequencer for LuxSampler banks.
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
    /** Max addressable sampler engines (A, B, …). MUST be ≥ the number of
     *  sampler instances the model can hold. Step 1: only slot 0 (A) is used. */
    static constexpr int kMaxSamplers = 8;

    /** Register the ordered sampler engines (by chain placement order:
     *  index 0 = A, 1 = B, …). Message thread / init only. */
    void setSamplers(LuxSampler* const* list, int count) noexcept
    {
        const int n = juce::jlimit(0, kMaxSamplers, count);
        for (int i = 0; i < kMaxSamplers; ++i)
            samplers_[i] = (i < n && list != nullptr) ? list[i] : nullptr;
        numSamplers_.store(n, std::memory_order_release);
    }
    /** Back-compat single-engine wiring (sampler A). */
    void setLuxSampler(LuxSampler* fs) noexcept
    {
        samplers_[0] = fs;
        for (int i = 1; i < kMaxSamplers; ++i) samplers_[i] = nullptr;
        numSamplers_.store(fs != nullptr ? 1 : 0, std::memory_order_release);
    }
    /** Engine for sampler index i (A=0, B=1, …), or nullptr if unset. */
    LuxSampler* getSampler(int i) const noexcept
    {
        return (i >= 0 && i < kMaxSamplers) ? samplers_[i] : nullptr;
    }
    /** Number of registered sampler engines (≥1 once wired). */
    int getNumSamplers() const noexcept { return numSamplers_.load(std::memory_order_relaxed); }

    // ── Configuration (message thread) ───────────────────────────────────────
    // Disabling while playing posts a STOP: processBlock drains transport
    // commands even when disabled, so removing the SEQUENCER module no longer
    // leaves the current step looping forever with no way to stop it.
    void setEnabled      (bool  e) noexcept
    {
        const bool was = enabled.exchange(e, std::memory_order_acq_rel);
        if (was && !e && playing.load(std::memory_order_relaxed))
            stopCmd.store(true, std::memory_order_release);
    }
    void setBpm          (float b) noexcept { bpm.store(b);         }
    void setNumSteps     (int   n) noexcept { numSteps.store(juce::jlimit(1, MAX_STEPS, n)); }
    void setLooping      (bool  l) noexcept { looping.store(l);     }
    void setDawSync      (bool  s) noexcept { dawSync.store(s);     }
    void setBeatsPerStep (int bps) noexcept { beatsPerStep.store(juce::jmax(1, bps)); }

    bool  isEnabled()      const noexcept { return enabled.load();      }
    float getBpm()         const noexcept { return bpm.load();          }
    int   getNumSteps()    const noexcept { return numSteps.load();     }
    bool  isLooping()      const noexcept { return looping.load();      }
    bool  isDawSync()      const noexcept { return dawSync.load();      }
    int   getBeatsPerStep()const noexcept { return beatsPerStep.load(); }

    // ── Step data (message thread write / RT read) ────────────────────────────
    /** Assign an encoded (sampler,slot) value to step i, or a sentinel
     *  (STEP_EMPTY / STEP_LIVE). See encodeStep(). */
    void setStep (int stepIdx, int bankIdx) noexcept;
    /** Returns the encoded step value (≥0 = real slot, <0 = sentinel/invalid). */
    int  getStep (int stepIdx) const noexcept;

    // ── Step value encoding ───────────────────────────────────────────────────
    // A real slot step encodes BOTH the sampler engine and the slot:
    //   enc = samplerIdx * NUM_SLOTS + slot   (A1..A12 = 0..11, B1..B12 = 12..23)
    // Sentinels stay negative, so old patterns (0..11) decode to sampler A.
    static int encodeStep(int samplerIdx, int slot) noexcept
    {
        return samplerIdx * LuxSamplerConstants::NUM_SLOTS + slot;
    }
    static int decodeSampler(int enc) noexcept
    {
        return enc < 0 ? -1 : enc / LuxSamplerConstants::NUM_SLOTS;
    }
    static int decodeSlot(int enc) noexcept
    {
        return enc < 0 ? enc : enc % LuxSamplerConstants::NUM_SLOTS;
    }
    /** Decoded sampler index for step i (−1 for empty/live/invalid). */
    int stepSampler(int stepIdx) const noexcept { return decodeSampler(getStep(stepIdx)); }
    /** Decoded slot for step i (0..11, or the sentinel value). */
    int stepSlot(int stepIdx) const noexcept { return decodeSlot(getStep(stepIdx)); }

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

private:
    // ── Ordered sampler engines (index 0 = A, 1 = B, …) ───────────────────────
    LuxSampler*      samplers_[kMaxSamplers] {};
    std::atomic<int> numSamplers_ { 0 };
    /** Primary engine (sampler A) — owns the single playback channel in step 1;
     *  sentinel steps (LIVE/EMPTY) and transport act on it. */
    LuxSampler* primarySampler() const noexcept { return samplers_[0]; }
    /** Apply fn to every registered engine — transport actions must reach a
     *  step playing/held on ANY engine, not just A. */
    template <typename Fn>
    void forEachSampler(Fn&& fn) const noexcept
    {
        const int n = numSamplers_.load(std::memory_order_relaxed);
        for (int i = 0; i < n && i < kMaxSamplers; ++i)
            if (samplers_[i] != nullptr)
                fn(*samplers_[i]);
    }

    // ── Per-step encoded (sampler,slot) assignment, or a negative sentinel ────
    std::atomic<int> steps[MAX_STEPS];

    // ── Config atomics ────────────────────────────────────────────────────────
    std::atomic<bool>  enabled      { false };
    std::atomic<float> bpm          { 120.0f };
    std::atomic<int>   numSteps     { 16 };
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
    /** Sampler engine index of the slot in rtPrevActiveBank, so triggerStep()
     *  finalises the previous bank on the RIGHT engine when the next step lives
     *  on a different sampler. */
    int    rtPrevActiveSampler   = 0;

    // ── RT helper (atomics only) ──────────────────────────────────────────────
    void triggerStep (int stepIdx) noexcept;
    void rtStop      ()            noexcept;  // RT-safe full stop

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FrameSequencer)
};
