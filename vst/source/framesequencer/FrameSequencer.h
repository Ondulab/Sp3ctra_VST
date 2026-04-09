#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "../framesampler/FrameSampler.h"

/**
 * @brief Step sequencer for FrameSampler banks.
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

    FrameSequencer();

    // ── Wiring ───────────────────────────────────────────────────────────────
    void setFrameSampler(FrameSampler* fs) noexcept { frameSampler = fs; }

    // ── Configuration (message thread) ───────────────────────────────────────
    void setEnabled      (bool  e) noexcept { enabled.store(e);     }
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
    /** Assign a bank (0–11) to step i, or -1 to leave it empty. */
    void setStep (int stepIdx, int bankIdx) noexcept;
    /** Returns the bank index (0–11) or -1 if empty/invalid. */
    int  getStep (int stepIdx) const noexcept;

    // ── Transport (message thread — converted to RT-safe atomic commands) ─────
    void uiPlay  () noexcept;
    void uiStop  () noexcept;
    bool isPlaying() const noexcept { return playing.load(std::memory_order_relaxed); }

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
    FrameSampler* frameSampler = nullptr;

    // ── Per-step bank assignment (-1 = empty, 0–11 = bank index) ─────────────
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
    std::atomic<bool> startCmd { false };
    std::atomic<bool> stopCmd  { false };

    // ── RT-only mutable state (accessed exclusively from the audio thread) ────
    // No synchronisation needed — single writer/reader.
    int    rtLastTriggeredStep   = -1;
    double rtInternalPhaseBeats  = 0.0;   // accumulated beats (internal BPM)
    double rtLastPpqPosition     = -1.0;

    // ── RT helper (atomics only) ──────────────────────────────────────────────
    void triggerStep (int stepIdx) noexcept;
    void rtStop      ()            noexcept;  // RT-safe full stop

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FrameSequencer)
};
