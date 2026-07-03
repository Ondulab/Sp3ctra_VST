/*
 * FrameSequencer.cpp
 *
 * Step sequencer driving LuxSampler bank playback.
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
    // Valid values: STEP_LIVE (-2), STEP_EMPTY (-1), or an encoded (sampler,slot).
    int clamped;
    if (bankIdx == STEP_LIVE)
        clamped = STEP_LIVE;
    else if (bankIdx < 0)
        clamped = STEP_EMPTY;
    else
    {
        // Clamp to the registered sampler count so a pattern can't address an
        // engine that isn't wired yet (jmax(1,…) keeps sampler A loadable even
        // before wiring).
        const int n      = juce::jmax(1, numSamplers_.load(std::memory_order_relaxed));
        const int maxEnc = n * LuxSamplerConstants::NUM_SLOTS - 1;
        clamped = juce::jlimit(0, maxEnc, bankIdx);
    }
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
    // Clear hold so the sequencer resumes advancing from the current step.
    // A full restart from step 0 is intentional: keeps behaviour consistent
    // whether the sequencer was held mid-sequence or fully stopped.
    held.store(false, std::memory_order_release);
    startCmd.store(true, std::memory_order_release);
    // Release the player of EVERY engine (a held step may live on B).
    forEachSampler([](LuxSampler& s) { s.setSeqPlayerHeld(false); });
}

void FrameSequencer::uiHold() noexcept
{
    // Set held immediately on the message thread so that isHeld() returns true
    // before processBlock has a chance to consume holdCmd.  Without this there
    // is a race window: if PLAY is pressed before the next processBlock cycle,
    // isHeld() == false → uiPlay() is called → sequence resets to step 0.
    held.store(true, std::memory_order_release);
    // holdCmd signals the audio thread (belt-and-suspenders + future RT side-effects).
    holdCmd.store(true, std::memory_order_release);
    // Freeze the player of EVERY engine — a step playing on engine B did not
    // freeze (only A was held).
    forEachSampler([](LuxSampler& s) { s.setSeqPlayerHeld(true); });
}

void FrameSequencer::uiStop() noexcept
{
    held.store(false, std::memory_order_release);
    stopCmd.store(true, std::memory_order_release);
    // Ensure no engine's player is left in a frozen state after stop.
    forEachSampler([](LuxSampler& s) { s.setSeqPlayerHeld(false); });
}

void FrameSequencer::uiResume() noexcept
{
    // Resume from the current step after a hold (pause).
    // Clear held so processBlock resumes step-advance logic.
    // Also post resumeCmd so processBlock re-anchors rtLastTriggeredStep
    // to the current DAW ppq — prevents skipping to the next step when
    // the DAW timeline advanced during the pause (DAW sync mode).
    held.store(false, std::memory_order_release);
    resumeCmd.store(true, std::memory_order_release);
    // Unfreeze every engine's player: play_head resumes where it stopped.
    forEachSampler([](LuxSampler& s) { s.setSeqPlayerHeld(false); });
}

// ============================================================================
// RT helpers — atomics ONLY
// ============================================================================

void FrameSequencer::triggerStep(int stepIdx) noexcept
{
    const int enc = steps[stepIdx].load(std::memory_order_relaxed);

    // Decode (sampler, slot). Sentinels (< 0) act on the primary engine — the
    // single playback channel in step 1.
    const int n          = numSamplers_.load(std::memory_order_relaxed);
    const int samplerIdx = (enc < 0) ? 0 : (enc / LuxSamplerConstants::NUM_SLOTS);
    const int bankIdx    = (enc < 0) ? enc : (enc % LuxSamplerConstants::NUM_SLOTS);

    LuxSampler* tgt = (samplerIdx >= 0 && samplerIdx < n) ? samplers_[samplerIdx]
                                                          : samplers_[0];
    if (tgt == nullptr) return;
    auto& as = tgt->getAtomicState();

    // ── 1. Finalise the PREVIOUS active bank (playing OR recording) ───────────
    // activePlaySlot only tracks PLAYING banks; rtPrevActiveBank covers RECORDING
    // banks too (activePlaySlot is -1 while a bank is recording). The previous
    // bank may live on a DIFFERENT engine, so finalise it on rtPrevActiveSampler.
    const bool sameAsPrev = (rtPrevActiveSampler == samplerIdx
                             && rtPrevActiveBank  == bankIdx);
    if (rtPrevActiveBank >= 0 && ! sameAsPrev)
    {
        LuxSampler* prev = (rtPrevActiveSampler >= 0 && rtPrevActiveSampler < n)
                              ? samplers_[rtPrevActiveSampler] : nullptr;
        if (prev != nullptr)
        {
            auto& pas = prev->getAtomicState();
            const auto prevSt = static_cast<SlotState>(
                pas.slotState[rtPrevActiveBank].load(std::memory_order_relaxed));

            if (prevSt == SlotState::RECORDING)
            {
                // Stop recording: onFrameAssembled() will finalise the slot.
                pas.stopRecCmd[rtPrevActiveBank].store(true, std::memory_order_release);
                pas.slotState[rtPrevActiveBank].store(static_cast<int>(SlotState::IDLE),
                                                       std::memory_order_release);
            }
            else if (prevSt == SlotState::PLAYING)
            {
                pas.slotState[rtPrevActiveBank].store(static_cast<int>(SlotState::IDLE),
                                                       std::memory_order_release);
            }
            // ARMED / IDLE → no action required
        }
    }

    // Also stop any other slot that happens to be playing on the TARGET engine.
    // curPlay may be the SCORE_SLOT sentinel (== NUM_SLOTS) — never index the
    // NUM_SLOTS-sized slotState[] with it (out-of-bounds write).
    const int curPlay = as.activePlaySlot.load(std::memory_order_relaxed);
    if (curPlay >= 0 && curPlay != bankIdx
        && curPlay < LuxSamplerConstants::NUM_SLOTS)
    {
        as.slotState[curPlay].store(static_cast<int>(SlotState::IDLE),
                                    std::memory_order_release);
    }

    // ── 2. Handle the new step ────────────────────────────────────────────────
    if (bankIdx == STEP_LIVE)
    {
        // Explicit LIVE step: restore CIS live passthrough (old default behaviour).
        if (as.activePlaySlot.load(std::memory_order_relaxed) >= 0)
            as.stopPlayCmd.store(true, std::memory_order_release);
        as.activePlaySlot.store(-1, std::memory_order_release);
        as.passthroughEnabled.store(true, std::memory_order_release);
        as.seqLiveStepActive.store(true, std::memory_order_release);  // STEP_LIVE only
        tgt->setSeqSilentStep(false); // clear silence flag — live is active
        rtPrevActiveBank    = STEP_LIVE;
        rtPrevActiveSampler = samplerIdx;
        return;
    }

    if (bankIdx == STEP_EMPTY)
    {
        // Empty / silence step: stop any playing slot AND suppress live passthrough.
        if (as.activePlaySlot.load(std::memory_order_relaxed) >= 0)
            as.stopPlayCmd.store(true, std::memory_order_release);
        as.activePlaySlot.store(-1,    std::memory_order_release);
        as.passthroughEnabled.store(false, std::memory_order_release); // suppress live
        as.seqLiveStepActive.store(false, std::memory_order_release);  // not STEP_LIVE
        tgt->setSeqSilentStep(true); // tell visualizer to show white
        // FIX(silence): inject a full-white (255) frame so the last sampler frame
        // does not freeze in AudioImageBuffers / preprocessed_data during silence.
        as.injectSilenceCmd.store(true, std::memory_order_release);
        rtPrevActiveBank    = STEP_EMPTY;
        rtPrevActiveSampler = samplerIdx;
        return;
    }

    // bankIdx is a valid slot (0..11) on engine samplerIdx.
    // Clear the silent-step flag: a real slot is about to play.
    tgt->setSeqSilentStep(false);

    const auto curSt = static_cast<SlotState>(
        as.slotState[bankIdx].load(std::memory_order_relaxed));

    if (curSt == SlotState::ARMED || curSt == SlotState::RECORDING)
    {
        // ── Sequencer-triggered recording ─────────────────────────────────────
        // Bank was armed (user pressed REC) → start capturing frames now.
        as.slotState[bankIdx].store(static_cast<int>(SlotState::RECORDING),
                                     std::memory_order_release);
        as.startRecCmd[bankIdx].store(true, std::memory_order_release);
        // Do NOT modify activePlaySlot / passthroughEnabled during recording.
    }
    else
    {
        // ── Normal bank playback ───────────────────────────────────────────────
        // Mark this play as sequencer-controlled BEFORE startPlayCmd so that
        // FramePlayerThread never restores passthroughEnabled on its own when the
        // sample finishes; only the sequencer (STEP_LIVE / rtStop) re-enables live.
        as.seqLiveStepActive.store(false, std::memory_order_release);  // not STEP_LIVE
        as.seqControlledPlay.store(true,  std::memory_order_release);
        as.stopPlayCmd.store(false, std::memory_order_release);
        as.slotState[bankIdx].store(static_cast<int>(SlotState::PLAYING),
                                     std::memory_order_release);
        as.activePlaySlot.store(bankIdx, std::memory_order_release);
        as.startPlayCmd.store(bankIdx,   std::memory_order_release);
        as.passthroughEnabled.store(false, std::memory_order_release);
    }

    rtPrevActiveBank    = bankIdx;
    rtPrevActiveSampler = samplerIdx;
}

void FrameSequencer::rtStop() noexcept
{
    playing.store(false, std::memory_order_release);
    currentStep.store(-1, std::memory_order_release);
    const int prevBank    = rtPrevActiveBank;      // capture before the reset below
    const int prevSampler = rtPrevActiveSampler;
    rtLastTriggeredStep  = -1;
    rtInternalPhaseBeats = 0.0;
    rtLastPpqPosition    = -1.0;
    rtPrevActiveBank     = -1;
    rtPrevActiveSampler  = 0;

    const int n = juce::jmax(1, numSamplers_.load(std::memory_order_relaxed));

    // Finalise a bank the sequencer left RECORDING (activePlaySlot is -1 while
    // recording, so the per-engine loop below cannot see it) — mirrors
    // triggerStep()'s previous-bank finalisation.
    if (prevBank >= 0 && prevBank < LuxSamplerConstants::NUM_SLOTS
        && prevSampler >= 0 && prevSampler < n)
        if (LuxSampler* prev = samplers_[prevSampler])
        {
            auto& pas = prev->getAtomicState();
            if (static_cast<SlotState>(pas.slotState[prevBank].load(
                    std::memory_order_relaxed)) == SlotState::RECORDING)
            {
                pas.stopRecCmd[prevBank].store(true, std::memory_order_release);
                pas.slotState[prevBank].store(static_cast<int>(SlotState::IDLE),
                                              std::memory_order_release);
            }
        }

    // Stop playback + release sequencer ownership on EVERY engine — a step
    // playing on engine B kept looping after STOP (only A was reset) with
    // B's passthrough left false → live feed dead. The white silence frame
    // is injected once, by the engine that owned the channel (else A).
    bool silencePosted = false;
    for (int i = 0; i < n && i < kMaxSamplers; ++i)
    {
        LuxSampler* s = samplers_[i];
        if (s == nullptr) continue;
        auto& as = s->getAtomicState();
        // cp may be the SCORE_SLOT sentinel (== NUM_SLOTS) — never index the
        // NUM_SLOTS-sized slotState[] with it (out-of-bounds write).
        const int cp = as.activePlaySlot.load(std::memory_order_relaxed);
        if (cp >= 0 && cp < LuxSamplerConstants::NUM_SLOTS)
            as.slotState[cp].store(static_cast<int>(SlotState::IDLE),
                                    std::memory_order_release);
        as.stopPlayCmd.store(true,  std::memory_order_release);
        as.activePlaySlot.store(-1, std::memory_order_release);
        // Release sequencer ownership so FramePlayerThread's tail cleanup
        // can safely restore passthrough if it is still running.
        as.seqControlledPlay.store(false, std::memory_order_release);
        as.seqLiveStepActive.store(false, std::memory_order_release);  // clear on stop
        // FIX(silence): inject ONE white frame so the last sampler frame does
        // not freeze in AudioImageBuffers / preprocessed_data after stop;
        // passthrough=true lets the live UDP stream take over naturally.
        if (cp >= 0 && !silencePosted)
        {
            as.injectSilenceCmd.store(true, std::memory_order_release);
            silencePosted = true;
        }
        as.passthroughEnabled.store(true, std::memory_order_release);
    }
    if (!silencePosted)
        if (auto* s = primarySampler())
            s->getAtomicState().injectSilenceCmd.store(true, std::memory_order_release);
}

// ============================================================================
// processBlock — audio thread, RT-safe
// ============================================================================

void FrameSequencer::processBlock(juce::AudioPlayHead* playHead,
                                   int                 numSamples,
                                   double              sampleRate) noexcept
{
    if (!enabled.load(std::memory_order_relaxed))
    {
        // Even while disabled (module removed from the rack), transport
        // commands must be drained: a queued STOP has to stop the looping
        // slot (it kept playing forever with the live feed suppressed and no
        // way to stop it), and a stale START must not fire minutes later when
        // the module is re-added.
        if (stopCmd.exchange(false, std::memory_order_acq_rel))
            rtStop();
        startCmd.store(false, std::memory_order_release);
        holdCmd.store(false, std::memory_order_release);
        resumeCmd.store(false, std::memory_order_release);
        return;
    }

    // ── 1. Handle stop command ────────────────────────────────────────────────
    if (stopCmd.exchange(false, std::memory_order_acq_rel))
    {
        rtStop();
        return;
    }

    // ── 2. Handle hold command (freeze step advancement, keep slot playing) ───
    if (holdCmd.exchange(false, std::memory_order_acq_rel))
        held.store(true, std::memory_order_release);

    // ── 2.5. Handle resume command: re-anchor without triggering a new step ───
    if (resumeCmd.exchange(false, std::memory_order_acq_rel))
    {
        // In DAW sync mode the DAW's ppq kept advancing during the hold.
        // Re-anchor rtLastTriggeredStep so the first unpaused processBlock
        // does NOT see a stale step index and trigger an unwanted step.
        if (dawSync.load(std::memory_order_relaxed) && playHead != nullptr)
        {
            const auto posInfo = playHead->getPosition();
            if (posInfo.hasValue())
            {
                const auto ppqOpt = posInfo->getPpqPosition();
                if (ppqOpt.hasValue() && *ppqOpt >= 0.0)
                {
                    const int nS   = numSteps.load(std::memory_order_relaxed);
                    const int bps  = beatsPerStep.load(std::memory_order_relaxed);
                    const double d = static_cast<double>(bps > 0 ? bps : 1);
                    const int abs  = static_cast<int>(*ppqOpt / d);
                    if (nS > 0) rtLastTriggeredStep = abs % nS;
                }
            }
        }
        // Internal BPM mode: rtInternalPhaseBeats was frozen during hold
        // (processBlock returned early), so it continues from the exact
        // position where it stopped — no re-anchoring needed.
    }

    // ── 3. Handle start command ───────────────────────────────────────────────
    if (startCmd.exchange(false, std::memory_order_acq_rel))
    {
        held.store(false, std::memory_order_release); // play always clears hold
        rtLastTriggeredStep  = -1;
        // Internal-BPM mode: pre-load a full phase so the accumulator below
        // fires on THIS very block — step 0 otherwise only sounded after one
        // whole step duration (a full second at 60 BPM), unlike DAW-sync mode
        // which triggers immediately. Unused in DAW-sync mode; uiResume()
        // keeps the frozen phase (separate resumeCmd path).
        rtInternalPhaseBeats = 1.0;
        rtLastPpqPosition    = -1.0;
        playing.store(true, std::memory_order_release);
        // The step-advance logic below will trigger step 0 on this very block.
    }

    if (!playing.load(std::memory_order_relaxed)) return;

    // ── 4. If held: keep current slot alive, do not advance ──────────────────
    if (held.load(std::memory_order_relaxed)) return;

    const int    nSteps       = numSteps.load(std::memory_order_relaxed);
    const int    bpsVal       = beatsPerStep.load(std::memory_order_relaxed);
    const double bpsD         = static_cast<double>(bpsVal > 0 ? bpsVal : 1);

    if (nSteps <= 0) return;

    // ── 5. DAW sync mode ─────────────────────────────────────────────────────
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
