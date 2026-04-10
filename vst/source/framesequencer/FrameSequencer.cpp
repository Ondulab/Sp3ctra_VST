/*
 * FrameSequencer.cpp
 *
 * Step sequencer driving FrameSampler bank playback.
 * See FrameSequencer.h for RT safety contract and timing modes.
 */

#include "FrameSequencer.h"

// ============================================================================
// Constructor
// ============================================================================

FrameSequencer::FrameSequencer()
{
    for (int i = 0; i < MAX_STEPS; ++i)
        steps[i].store(-1, std::memory_order_relaxed);
}

// ============================================================================
// Step data
// ============================================================================

void FrameSequencer::setStep(int stepIdx, int bankIdx) noexcept
{
    if (stepIdx < 0 || stepIdx >= MAX_STEPS) return;
    // bankIdx must be -1 (empty) or 0..11 (valid bank)
    const int clamped = (bankIdx < 0) ? -1
                      : juce::jlimit(0, FrameSamplerConstants::NUM_SLOTS - 1, bankIdx);
    steps[stepIdx].store(clamped, std::memory_order_release);
}

int FrameSequencer::getStep(int stepIdx) const noexcept
{
    if (stepIdx < 0 || stepIdx >= MAX_STEPS) return -1;
    return steps[stepIdx].load(std::memory_order_acquire);
}

// ============================================================================
// Transport — message thread (writes atomic command flags consumed by RT)
// ============================================================================

void FrameSequencer::uiPlay() noexcept
{
    startCmd.store(true, std::memory_order_release);
}

void FrameSequencer::uiStop() noexcept
{
    stopCmd.store(true, std::memory_order_release);
}

// ============================================================================
// RT helpers — atomics ONLY
// ============================================================================

void FrameSequencer::triggerStep(int stepIdx) noexcept
{
    if (frameSampler == nullptr) return;

    const int bankIdx = steps[stepIdx].load(std::memory_order_relaxed);
    auto& as = frameSampler->getAtomicState();

    // ── 1. Finalise the PREVIOUS active bank (playing OR recording) ───────────
    // activePlaySlot only tracks PLAYING banks; rtPrevActiveBank covers RECORDING
    // banks too (activePlaySlot is -1 while a bank is recording).
    if (rtPrevActiveBank >= 0 && rtPrevActiveBank != bankIdx)
    {
        const auto prevSt = static_cast<SlotState>(
            as.slotState[rtPrevActiveBank].load(std::memory_order_relaxed));

        if (prevSt == SlotState::RECORDING)
        {
            // Stop recording: onFrameAssembled() will finalise the slot.
            as.stopRecCmd[rtPrevActiveBank].store(true, std::memory_order_release);
            as.slotState[rtPrevActiveBank].store(static_cast<int>(SlotState::IDLE),
                                                  std::memory_order_release);
        }
        else if (prevSt == SlotState::PLAYING)
        {
            as.slotState[rtPrevActiveBank].store(static_cast<int>(SlotState::IDLE),
                                                  std::memory_order_release);
        }
        // ARMED / IDLE → no action required
    }

    // Also stop any other slot that happens to be playing (safety net).
    const int curPlay = as.activePlaySlot.load(std::memory_order_relaxed);
    if (curPlay >= 0 && curPlay != bankIdx)
    {
        as.slotState[curPlay].store(static_cast<int>(SlotState::IDLE),
                                    std::memory_order_release);
    }

    // ── 2. Handle the new step ────────────────────────────────────────────────
    if (bankIdx < 0)
    {
        // Empty step → restore passthrough, stop any ongoing play
        as.stopPlayCmd.store(true,  std::memory_order_release);
        as.activePlaySlot.store(-1, std::memory_order_release);
        as.passthroughEnabled.store(true, std::memory_order_release);
        rtPrevActiveBank = -1;
        return;
    }

    const auto curSt = static_cast<SlotState>(
        as.slotState[bankIdx].load(std::memory_order_relaxed));

    if (curSt == SlotState::ARMED || curSt == SlotState::RECORDING)
    {
        // ── Sequencer-triggered recording ─────────────────────────────────────
        // Bank was armed (user pressed REC) → start capturing frames now.
        // Post startRecCmd so onFrameAssembled() sets activeRecSlot and resets
        // the slot buffer.  Passthrough remains ON during recording.
        as.slotState[bankIdx].store(static_cast<int>(SlotState::RECORDING),
                                     std::memory_order_release);
        as.startRecCmd[bankIdx].store(true, std::memory_order_release);
        // Do NOT modify activePlaySlot / passthroughEnabled during recording.
    }
    else
    {
        // ── Normal bank playback ───────────────────────────────────────────────
        // IDLE (with or without content) → FramePlayerThread will revert to IDLE
        // if has_content == false (silent trigger is harmless).
        as.slotState[bankIdx].store(static_cast<int>(SlotState::PLAYING),
                                     std::memory_order_release);
        as.activePlaySlot.store(bankIdx, std::memory_order_release);
        as.startPlayCmd.store(bankIdx,   std::memory_order_release);
        as.passthroughEnabled.store(false, std::memory_order_release);
    }

    rtPrevActiveBank = bankIdx;
}

void FrameSequencer::rtStop() noexcept
{
    playing.store(false, std::memory_order_release);
    currentStep.store(-1, std::memory_order_release);
    rtLastTriggeredStep  = -1;
    rtInternalPhaseBeats = 0.0;
    rtLastPpqPosition    = -1.0;
    rtPrevActiveBank     = -1;

    if (frameSampler != nullptr)
    {
        auto& as = frameSampler->getAtomicState();
        const int cp = as.activePlaySlot.load(std::memory_order_relaxed);
        if (cp >= 0)
        {
            as.slotState[cp].store(static_cast<int>(SlotState::IDLE),
                                    std::memory_order_release);
        }
        as.stopPlayCmd.store(true,  std::memory_order_release);
        as.activePlaySlot.store(-1, std::memory_order_release);
        as.passthroughEnabled.store(true, std::memory_order_release);
    }
}

// ============================================================================
// processBlock — audio thread, RT-safe
// ============================================================================

void FrameSequencer::processBlock(juce::AudioPlayHead* playHead,
                                   int                 numSamples,
                                   double              sampleRate) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    // ── 1. Handle stop command ────────────────────────────────────────────────
    if (stopCmd.exchange(false, std::memory_order_acq_rel))
    {
        rtStop();
        return;
    }

    // ── 2. Handle start command ───────────────────────────────────────────────
    if (startCmd.exchange(false, std::memory_order_acq_rel))
    {
        rtLastTriggeredStep  = -1;
        rtInternalPhaseBeats = 0.0;
        rtLastPpqPosition    = -1.0;
        playing.store(true, std::memory_order_release);
        // The step-advance logic below will trigger step 0 on this very block.
    }

    if (!playing.load(std::memory_order_relaxed)) return;

    const int    nSteps       = numSteps.load(std::memory_order_relaxed);
    const int    bpsVal       = beatsPerStep.load(std::memory_order_relaxed);
    const double bpsD         = static_cast<double>(bpsVal > 0 ? bpsVal : 1);

    if (nSteps <= 0) return;

    // ── 3. DAW sync mode ─────────────────────────────────────────────────────
    if (dawSync.load(std::memory_order_relaxed) && playHead != nullptr)
    {
        const auto posInfo = playHead->getPosition();
        if (!posInfo.hasValue()) return;

        // If DAW transport is not running, pause (do not stop) the sequencer.
        if (!posInfo->getIsPlaying()) return;

        const auto ppqOpt = posInfo->getPpqPosition();
        if (!ppqOpt.hasValue()) return;

        const double ppq = *ppqOpt;
        if (ppq < 0.0) return;

        // Determine which sequencer step corresponds to the current beat.
        // Wrap with modulo to create a looping grid regardless of looping flag
        // (loop flag only used for internal mode where we manage the counter).
        const int absoluteStep = static_cast<int>(ppq / bpsD);
        const int stepInSeq    = absoluteStep % nSteps;

        if (stepInSeq != rtLastTriggeredStep)
        {
            rtLastTriggeredStep = stepInSeq;
            currentStep.store(stepInSeq, std::memory_order_release);
            triggerStep(stepInSeq);
        }
        return;
    }

    // ── 4. Internal BPM mode ─────────────────────────────────────────────────
    {
        const double bpmVal       = static_cast<double>(bpm.load(std::memory_order_relaxed));
        const double safeRate     = (sampleRate > 0.0) ? sampleRate : 44100.0;
        const double beatsPerSmpl = bpmVal / (60.0 * safeRate);
        const double increment    = beatsPerSmpl * static_cast<double>(numSamples) / bpsD;

        rtInternalPhaseBeats += increment;

        if (rtInternalPhaseBeats >= 1.0)
        {
            rtInternalPhaseBeats -= 1.0;

            // Advance step counter
            int nextStep = rtLastTriggeredStep + 1;
            if (nextStep < 0) nextStep = 0; // first trigger after start

            if (nextStep >= nSteps)
            {
                if (looping.load(std::memory_order_relaxed))
                {
                    nextStep = 0;
                }
                else
                {
                    // End of sequence — stop
                    rtStop();
                    return;
                }
            }

            rtLastTriggeredStep = nextStep;
            currentStep.store(nextStep, std::memory_order_release);
            triggerStep(nextStep);
        }
    }
}

// ============================================================================
// State serialisation (message thread)
// ============================================================================

void FrameSequencer::saveToXml(juce::XmlElement& xml) const
{
    xml.setAttribute("seq_bpm",      static_cast<double>(bpm.load()));
    xml.setAttribute("seq_numSteps", numSteps.load());
    xml.setAttribute("seq_loop",     looping.load()      ? 1 : 0);
    xml.setAttribute("seq_dawSync",  dawSync.load()       ? 1 : 0);
    xml.setAttribute("seq_bps",      beatsPerStep.load());

    for (int i = 0; i < MAX_STEPS; ++i)
        xml.setAttribute("seq_step_" + juce::String(i),
                         steps[i].load(std::memory_order_relaxed));
}

bool FrameSequencer::loadFromXml(const juce::XmlElement& xml)
{
    setBpm         (static_cast<float>(xml.getDoubleAttribute("seq_bpm",      120.0)));
    setNumSteps    (xml.getIntAttribute("seq_numSteps", 16));
    setLooping     (xml.getIntAttribute("seq_loop",     1) != 0);
    setDawSync     (xml.getIntAttribute("seq_dawSync",  1) != 0);
    setBeatsPerStep(xml.getIntAttribute("seq_bps",      1));

    for (int i = 0; i < MAX_STEPS; ++i)
        setStep(i, xml.getIntAttribute("seq_step_" + juce::String(i), -1));

    return true;
}
