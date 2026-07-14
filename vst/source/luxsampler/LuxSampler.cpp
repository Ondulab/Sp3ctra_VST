/*
 * LuxSampler.cpp
 *
 * Implementation of the LuxSampler subsystem.
 * See LuxSampler.h for architecture notes.
 */

#include "LuxSampler.h"
#include "lux_sampler_hooks.h"   // forward decls so hooks can call each other in any order

extern "C" {
    #include "audio_image_buffers.h"
    #include "multithreading.h"         // DoubleBuffer + pthread mutex
    #include "../processing/image_preprocessor.h" // PreprocessedImageData
#include "../processing/image_pipeline.h"     // pipeline_process_frame, pipeline_build_config_sampler
    #include "logger.h"
}

#include <sys/time.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>

// ============================================================================
// Static instance (singleton for C hook access)
// ============================================================================
std::atomic<LuxSampler*> LuxSampler::s_engines[LuxSampler::kMaxEngines] = { nullptr, nullptr };
std::atomic<int>         LuxSampler::s_engineBusy[LuxSampler::kMaxEngines] = { 0, 0 };
std::atomic<bool> LuxSampler::s_enginesShareChain{ false };

// ============================================================================
// C-linkage hook functions — called from udpThread() in multithreading.c.
// They now fan out across ALL live sampler engines (A, B, …) via the registry.
// ============================================================================
extern "C"
{
    /* All fan-outs below pin the engine slot (busy counter) around the call so
     * that ~LuxSampler can unregister and then wait for in-flight hook calls to
     * drain — the hooks run on the UDP thread, which outlives any single plugin
     * instance (shared core). See LuxSampler::pinEngine(). */
    /* Producer hooks (P4-M3 — positional capture):
     *
     *   1. lux_sampler_on_live_frame_assembled() — line assembled. Each
     *      engine drains its rec commands + caches the live frame (darken-
     *      blend). Never captures.
     *
     *   2. lux_sampler_record_chain_frame() / lux_sampler_record_input_frame()
     *      — called BY THE CHAIN EXECUTOR at each SAMPLER marker's position
     *      (idle walk / playback spans): REC captures the chain's own stream
     *      exactly where the module sits. The old global modulated-bus hook
     *      (on_modulated_frame_ready) is gone.
     */
    void lux_sampler_on_live_frame_assembled(const uint8_t* R,
                                              const uint8_t* G,
                                              const uint8_t* B,
                                              uint16_t       pixel_count)
    {
        for (int i = 0; i < LuxSampler::kMaxEngines; ++i)
        {
            if (auto* e = LuxSampler::pinEngine(i))
                e->onLiveFrameAssembled(R, G, B, pixel_count);
            LuxSampler::unpinEngine(i);
        }
    }

    void lux_sampler_record_chain_frame(int engine_slot,
                                        const uint8_t* R,
                                        const uint8_t* G,
                                        const uint8_t* B,
                                        uint16_t       pixel_count)
    {
        // Per-chain sampler feed: a SAMPLER marker executed positionally
        // records ITS OWN chain's stream into ITS engine's armed slot (idle
        // only — during playback the resampling path owns every recording).
        if (engine_slot < 0 || engine_slot >= LuxSampler::kMaxEngines)
            return;
        static std::atomic<uint32_t> s_chainLineId[LuxSampler::kMaxEngines];
        const uint32_t line_id =
            s_chainLineId[engine_slot].fetch_add(1, std::memory_order_relaxed);
        if (auto* e = LuxSampler::pinEngine(engine_slot))
        {
            // A DRIVING engine (its own playback / score owns the channel)
            // self-records the modulated output via lux_samplers_record_modulated
            // — skip the positional capture so it never double-records itself.
            // A NON-driving engine on another chain records ITS OWN stream here
            // even while a different engine plays (multi-chain: no cross-bleed).
            if (! e->isDrivingChannel())
                e->recordModulatedFrame(R, G, B, pixel_count, line_id);
        }
        LuxSampler::unpinEngine(engine_slot);
    }

    int lux_sampler_engine_is_driving(int engine)
    {
        // Per-engine driving query (multi-chain split): lets the chain executor
        // gate player-ownership per chain instead of collapsing both engines
        // into the single "first playing engine" (which starved the second
        // simultaneously-playing chain's gates).
        int driving = 0;
        if (auto* e = LuxSampler::pinEngine(engine))
            driving = e->isDrivingChannel() ? 1 : 0;
        LuxSampler::unpinEngine(engine);
        return driving;
    }

    int lux_sampler_is_playing(void)
    {
        int playing = 0;
        for (int i = 0; i < LuxSampler::kMaxEngines && !playing; ++i)
        {
            if (auto* e = LuxSampler::pinEngine(i))
                playing = e->isDrivingChannel() ? 1 : 0;
            LuxSampler::unpinEngine(i);
        }
        return playing;
    }

    int lux_sampler_playing_engine(void)
    {
        // One-plays-at-a-time arbiter ⇒ at most one engine drives the channel.
        // Score relay reports -1: the SCORE path is gated by has_score, never
        // by engine matching.
        for (int i = 0; i < LuxSampler::kMaxEngines; ++i)
        {
            int driving = 0;
            if (auto* e = LuxSampler::pinEngine(i))
                driving = (e->isDrivingChannel() && ! e->isScorePlaying()) ? 1 : 0;
            LuxSampler::unpinEngine(i);
            if (driving)
                return i;
        }
        return -1;
    }

    int lux_sampler_is_recording(void)
    {
        int recording = 0;
        for (int i = 0; i < LuxSampler::kMaxEngines && !recording; ++i)
        {
            if (auto* e = LuxSampler::pinEngine(i))
                recording = e->isAnySlotRecording() ? 1 : 0;
            LuxSampler::unpinEngine(i);
        }
        return recording;
    }

    void lux_sampler_record_input_frame(int            engine,
                                        const uint8_t* R,
                                        const uint8_t* G,
                                        const uint8_t* B,
                                        uint16_t       pixel_count)
    {
        // REC records the module INPUT (user rule 2026-07-13): the chain
        // stream arriving AT the sampler's marker (source → pre-marker
        // processors), captured into the armed slot even while THIS engine
        // is playing — recording never captures the engine's own playback
        // mix. Unlike lux_sampler_record_chain_frame there is deliberately
        // NO isDrivingChannel skip here: the pre-marker span caller
        // targets the marker's own engine.
        if (engine < 0 || engine >= LuxSampler::kMaxEngines)
            return;
        static std::atomic<uint32_t> s_inputLineId[LuxSampler::kMaxEngines];
        const uint32_t line_id =
            s_inputLineId[engine].fetch_add(1, std::memory_order_relaxed);
        if (auto* e = LuxSampler::pinEngine(engine))
            e->recordModulatedFrame(R, G, B, pixel_count, line_id);
        LuxSampler::unpinEngine(engine);
    }

    int lux_sampler_is_passthrough(void)
    {
        // Live should flow only if NO engine is suppressing it (PLAYING/STEP_EMPTY).
        int passthrough = 1;
        for (int i = 0; i < LuxSampler::kMaxEngines && passthrough; ++i)
        {
            if (auto* e = LuxSampler::pinEngine(i))
                if (! e->getAtomicState().passthroughEnabled.load(std::memory_order_relaxed))
                    passthrough = 0;
            LuxSampler::unpinEngine(i);
        }
        return passthrough; // no engine, or all in passthrough → default passthrough
    }

    int lux_sampler_is_seq_live_step(void)
    {
        int liveStep = 0;
        for (int i = 0; i < LuxSampler::kMaxEngines && !liveStep; ++i)
        {
            if (auto* e = LuxSampler::pinEngine(i))
                if (e->getAtomicState().seqLiveStepActive.load(std::memory_order_relaxed))
                    liveStep = 1;
            LuxSampler::unpinEngine(i);
        }
        return liveStep;
    }

    int lux_sampler_is_score_playing(void)
    {
        int playing = 0;
        for (int i = 0; i < LuxSampler::kMaxEngines && !playing; ++i)
        {
            if (auto* e = LuxSampler::pinEngine(i))
                playing = e->isScorePlaying() ? 1 : 0;
            LuxSampler::unpinEngine(i);
        }
        return playing;
    }
}

// ============================================================================
// Per-engine helper: is THIS engine the one driving the modulated channel?
// (Aggregated across engines by lux_sampler_is_playing().)
// ============================================================================
bool LuxSampler::isDrivingChannel() const noexcept
{
    if (isScorePlaying())
        return true;                       // SCORE always takes over the channel
    if (!enabled.load(std::memory_order_relaxed))
        return false;                      // disabled module = passthrough:
                                           // the upstream flow crosses the
                                           // marker (module contract 2026-07-13)
    if (!isAnySlotPlaying())
        return false;
    if (isSeqPlayerHeld())
        return false;                      // player frozen → live passes through
    if (atomicState.seqControlledPlay.load(std::memory_order_relaxed))
        return true;                       // sequencer is the sole writer
    extern sp3ctra_config_t g_sp3ctra_config;
    return (g_sp3ctra_config.sampler_freeze_mode == 0);  // manual transport PLAYING
}

// ============================================================================
// Playback arbiter — SCOPED to shared-chain topologies (multi-chain split).
// Engines on DIFFERENT chains own independent streams: both may play at once,
// so the arbiter is a no-op. Only when A and B sit on ONE chain (same stream)
// does starting one still evict the other. RT-safe (atomic stores only).
// ============================================================================
void LuxSampler::stopOtherEnginesPlayback(int exceptIndex) noexcept
{
    if (! s_enginesShareChain.load(std::memory_order_acquire))
        return;   // cross-chain independence: nothing to evict
    for (int i = 0; i < kMaxEngines; ++i)
    {
        if (i == exceptIndex) continue;
        LuxSampler* e = s_engines[i].load(std::memory_order_acquire);
        if (e == nullptr) continue;
        auto& as = e->atomicState;
        const int cp = as.activePlaySlot.load(std::memory_order_relaxed);
        if (cp >= 0)
        {
            if (cp < LuxSamplerConstants::NUM_SLOTS)
                as.slotState[cp].store(static_cast<int>(SlotState::IDLE),
                                       std::memory_order_release);
            as.stopPlayCmd.store(true, std::memory_order_release);
            as.activePlaySlot.store(-1, std::memory_order_release);
            // Restore the evicted engine's live-passthrough flag: its player
            // tail only restores when slotState is still PLAYING (already
            // IDLE here), so the flag stayed false FOREVER and
            // lux_sampler_is_passthrough() kept the live feed dead after the
            // new owner stopped. The new owner clears its own flag right
            // after this call, so the shared channel stays suppressed while
            // it plays. Sequencer-owned engines keep their flag — the
            // sequencer is the only authority there (rtStop / STEP_LIVE).
            if (! as.seqControlledPlay.load(std::memory_order_relaxed))
                as.passthroughEnabled.store(true, std::memory_order_release);
        }
    }
}

// ============================================================================
// CRC32 — standard polynomial 0xEDB88320 (little-endian / Ethernet)
// ============================================================================
static uint32_t crc32_compute(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

// ============================================================================
// Timing helper (shared between LuxSampler and FramePlayerThread)
// ============================================================================
uint64_t LuxSampler::currentTimeUs() noexcept
{
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(tv.tv_usec);
}

// ============================================================================
// LuxSampler — constructor / destructor
// ============================================================================

LuxSampler::LuxSampler(int engineIndex)
    : engineIndex_(juce::jlimit(0, kMaxEngines - 1, engineIndex))
{
    // Register in the multi-engine registry — first plugin instance wins. A
    // second instance in the same DAW process must NOT clobber the first one's
    // entry (the UDP hooks would silently stop reaching it, and its dtor would
    // null out OUR slot).
    LuxSampler* expected = nullptr;
    registered_ = s_engines[engineIndex_].compare_exchange_strong(
        expected, this, std::memory_order_acq_rel);
    if (!registered_)
        log_warning("FS", "LuxSampler[%c]: registry slot already owned by another "
                          "plugin instance — UDP hooks stay bound to that instance",
                    (char) ('A' + engineIndex_));
    for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
    {
        currentPlayHead[i].store(0,  std::memory_order_relaxed);
        lastPlayHead[i].store(0,     std::memory_order_relaxed);
        lastDirection[i].store(1,    std::memory_order_relaxed); // forward by default
    }
    initFreqCurveDefaults();
    log_info("FS", "LuxSampler[%c] initialised — %d slots, %d frames/slot max, %.1f s/slot max",
             (char) ('A' + engineIndex_),
             LuxSamplerConstants::NUM_SLOTS,
             LuxSamplerConstants::MAX_FRAMES_PER_SLOT,
             static_cast<double>(LuxSamplerConstants::MAX_DURATION_S));
}

LuxSampler::~LuxSampler()
{
    stopPlayerThread();
    if (registered_)
    {
        LuxSampler* expected = this;
        s_engines[engineIndex_].compare_exchange_strong(
            expected, nullptr, std::memory_order_acq_rel);
        // Quiescence: a UDP-thread hook may have pinned this slot and loaded the
        // pointer just before the store above — wait for in-flight calls to
        // drain before the object is freed (hook bodies run in µs; bounded).
        for (int tries = 0;
             s_engineBusy[engineIndex_].load(std::memory_order_acquire) != 0
                 && tries < 100;
             ++tries)
            juce::Thread::sleep(1);
    }
    log_info("FS", "LuxSampler[%c] destroyed", (char) ('A' + engineIndex_));
}

// (processMidi / handleNoteOn / handleNoteOff removed 2026-07-13 — banks are
//  no longer note-addressed. Per-bank PLAY/REC triggering lives in the unified
//  MIDI-Learn targets; several banks may now play simultaneously.)

// ============================================================================
// Non-RT path — onLiveFrameAssembled (phase 1)
//
// Called by udpThread() RIGHT AFTER a full scanline has been reassembled
// from UDP fragments — BEFORE LuxPitch / LuxMask are run on it.
//
// Responsibilities:
//   • Drain pending start/stop record commands posted by processMidi()
//   • Cache the live frame (used later by FramePlayerThread for darken-blend
//     and by getLiveFrame() callers).
//
// MUST NOT capture a frame into the recording slot.  Recording happens in
// onModulatedFrameReady() so the recorded content includes Pitch/Mask.
// MUST NOT update the sampler snapshot.  That snapshot now mirrors the
// post-mask frame (so the Modulated channel stays correct in idle).
// ============================================================================

bool LuxSampler::onLiveFrameAssembled(const uint8_t* R, const uint8_t* G,
                                       const uint8_t* B, uint16_t pixel_count)
{
    if (!enabled.load(std::memory_order_relaxed)) return false;

    // ── Process pending start/stop commands from RT ───────────────────────
    // Stop is drained BEFORE start: during a CIS stream gap the UI can queue
    // "stop (old take)" then "start (new take)"; draining start first armed
    // the slot and the queued stop finalised the NEW take immediately — the
    // UI showed RECORDING while nothing was captured.
    // The whole drain is FROZEN while a chunked save copies frames (see
    // saveInProgress_): a non-overdub START resets frame_count and rewrites
    // frames[0..] on this very thread, corrupting the interleaved chunk
    // copies. Flags stay queued and drain right after the save.
    if (!saveInProgress_.load(std::memory_order_acquire))
    for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
    {
        if (atomicState.stopRecCmd[i].exchange(false, std::memory_order_acq_rel))
        {
            if (activeRecSlot.load(std::memory_order_relaxed) == i)
            {
                std::lock_guard<std::mutex> lk(slotsMutex_);
                slots[i].has_content = (slots[i].frame_count > 0);
                slots[i].duration_us = recBaseUs_ + (currentTimeUs() - recStartTimeUs);
                activeRecSlot.store(-1, std::memory_order_release);
                log_info("FS", "Slot %d: recording stopped — %d frames, %.2f s",
                         i, slots[i].frame_count,
                         static_cast<double>(slots[i].duration_us) / 1e6);
            }
        }

        if (atomicState.startRecCmd[i].exchange(false, std::memory_order_acq_rel))
        {
            {
                std::lock_guard<std::mutex> lk(slotsMutex_);
                // Overdub: append after an existing take instead of erasing it.
                const bool append = overdubMode_.load(std::memory_order_relaxed)
                                    && slots[i].isAllocated()
                                    && slots[i].has_content
                                    && slots[i].frame_count > 0
                                    && slots[i].frame_count < slots[i].capacity;
                if (!slots[i].isAllocated())
                {
                    slots[i].allocate();
                    recBaseUs_ = 0;
                    log_info("FS", "Slot %d: buffer allocated (%d frames × %zu B)",
                             i, LuxSamplerConstants::MAX_FRAMES_PER_SLOT,
                             sizeof(CapturedFrame));
                }
                else if (append)
                {
                    // Keep frame_count / duration_us; new frames continue from the
                    // end, their timestamps offset by the prior duration.
                    recBaseUs_ = slots[i].duration_us;
                    log_info("FS", "Slot %d: overdub — appending after %d frames",
                             i, slots[i].frame_count);
                }
                else
                {
                    slots[i].frame_count = 0;
                    slots[i].play_head   = 0;
                    slots[i].duration_us = 0;
                    slots[i].has_content = false;
                    recBaseUs_ = 0;
                }
            }
            activeRecSlot.store(i, std::memory_order_release);
            recStartTimeUs = currentTimeUs();
            log_info("FS", "Slot %d: recording started", i);
        }
    }

    // ── Cache latest live frame (used by FramePlayerThread for darken-blend) ──
    // Written here (UDP thread, Non-RT) → read by FramePlayerThread (Non-RT).
    // Cached regardless of recording state so blend always has the freshest frame.
    {
        std::lock_guard<std::mutex> lk(liveMutex_);
        livePixelCount_ = std::min(static_cast<int>(pixel_count),
                                   LuxSamplerConstants::MAX_PIXELS);
        std::memcpy(liveR_, R, static_cast<size_t>(livePixelCount_));
        std::memcpy(liveG_, G, static_cast<size_t>(livePixelCount_));
        std::memcpy(liveB_, B, static_cast<size_t>(livePixelCount_));
    }

    return true;
}

// ============================================================================
// recordModulatedFrame — write the (chain-modulated) frame into THIS engine's
// active recording slot. Non-RT (UDP thread). This is the resampling capture:
// when ANOTHER engine is playing into the modulated channel, the frame passed
// here is that playback output, so the recording is the combination ("don't
// bypass the chain"). No snapshot side effect — the playing engine owns it.
// ============================================================================
void LuxSampler::recordModulatedFrame(const uint8_t* R, const uint8_t* G,
                                       const uint8_t* B, uint16_t pixel_count,
                                       uint32_t line_id) noexcept
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    const int recSlot = activeRecSlot.load(std::memory_order_relaxed);
    if (recSlot < 0) return;

    // Sequencer-gated recording: only capture when the current step points here.
    const int gate = seqGateSlot.load(std::memory_order_relaxed);
    if (gate >= 0 && gate != recSlot) return; // gated out — wrong step

    std::lock_guard<std::mutex> lk(slotsMutex_);

    FrameSlot& slot = slots[recSlot];
    if (!slot.isAllocated()) return;

    // Check max duration / buffer overflow. In overdub, recBaseUs_ carries the
    // prior take's duration so both the cap and the stored timestamps stay on a
    // continuous timeline across the appended session.
    const uint64_t elapsed = currentTimeUs() - recStartTimeUs;
    const uint64_t total   = recBaseUs_ + elapsed;
    const uint64_t maxUs   = static_cast<uint64_t>(maxDurationS.load() * 1e6f);
    if (total >= maxUs || slot.frame_count >= slot.capacity)
    {
        slot.has_content = (slot.frame_count > 0);
        slot.duration_us = total;
        activeRecSlot.store(-1, std::memory_order_release);
        atomicState.slotState[recSlot].store(static_cast<int>(SlotState::IDLE),
                                              std::memory_order_release);
        log_info("FS", "Slot %d: overflow — %d frames, %.2f s",
                 recSlot, slot.frame_count, static_cast<double>(total) / 1e6);
        return;
    }

    // Write the modulated frame
    CapturedFrame& frame = slot.frames[slot.frame_count];
    frame.timestamp_us = total;
    frame.line_id      = line_id;
    frame.pixel_count  = pixel_count;
    const int bytes = std::min(static_cast<int>(pixel_count),
                               LuxSamplerConstants::MAX_PIXELS);
    std::memcpy(frame.R, R, static_cast<size_t>(bytes));
    std::memcpy(frame.G, G, static_cast<size_t>(bytes));
    std::memcpy(frame.B, B, static_cast<size_t>(bytes));
    ++slot.frame_count;
}

// ============================================================================
// LuxSampler::getLiveFrame — Non-RT (called by FramePlayerThread)
// ============================================================================
void LuxSampler::getLiveFrame(uint8_t* outR, uint8_t* outG, uint8_t* outB,
                                 int maxPixels, int& outCount) noexcept
{
    std::lock_guard<std::mutex> lk(liveMutex_);
    outCount = std::min(livePixelCount_, maxPixels);
    if (outCount > 0)
    {
        std::memcpy(outR, liveR_, static_cast<size_t>(outCount));
        std::memcpy(outG, liveG_, static_cast<size_t>(outCount));
        std::memcpy(outB, liveB_, static_cast<size_t>(outCount));
    }
}

// ============================================================================
// Non-RT: enable/disable (message thread — deriveChainRouting / enable param)
// ============================================================================

void LuxSampler::setEnabled(bool e) noexcept
{
    const bool was = enabled.exchange(e, std::memory_order_acq_rel);
    if (!was || e)
        return;   // no transition, or turning ON — nothing to tear down

    // Disabled while active (module removed from its chain / LED off): the
    // command drains in onLiveFrameAssembled() no longer run, so an in-flight
    // recording stayed armed forever (STOP REC dead; re-adding the module
    // later resumed capture with a huge elapsed time → take auto-truncated as
    // overflow). Finalise the take and stop playback now.
    const int rec = activeRecSlot.load(std::memory_order_acquire);
    if (rec >= 0 && rec < LuxSamplerConstants::NUM_SLOTS)
    {
        atomicState.startRecCmd[rec].store(false, std::memory_order_release);
        atomicState.stopRecCmd[rec].store(false, std::memory_order_release);
        atomicState.slotState[rec].store(static_cast<int>(SlotState::IDLE),
                                         std::memory_order_release);
        std::lock_guard<std::mutex> lk(slotsMutex_);
        if (activeRecSlot.load(std::memory_order_relaxed) == rec)
        {
            slots[rec].has_content = (slots[rec].frame_count > 0);
            slots[rec].duration_us = recBaseUs_ + (currentTimeUs() - recStartTimeUs);
            activeRecSlot.store(-1, std::memory_order_release);
            log_info("FS", "Slot %d: recording finalised on disable (%d frames)",
                     rec, slots[rec].frame_count);
        }
    }

    // Stop a REAL playing slot (a running SCORE keeps its own lifecycle) and
    // hand the live feed back.
    const int cp = atomicState.activePlaySlot.load(std::memory_order_acquire);
    if (cp >= 0 && cp < LuxSamplerConstants::NUM_SLOTS)
    {
        atomicState.stopPlayCmd.store(true, std::memory_order_release);
        atomicState.slotState[cp].store(static_cast<int>(SlotState::IDLE),
                                        std::memory_order_release);
        atomicState.activePlaySlot.store(-1, std::memory_order_release);
        atomicState.seqControlledPlay.store(false, std::memory_order_release);
        atomicState.passthroughEnabled.store(true, std::memory_order_release);
    }
}

// ============================================================================
// Non-RT: UI-triggered record toggle
// ============================================================================

void LuxSampler::uiToggleRecord(int slotIndex) noexcept
{
    using namespace LuxSamplerConstants;
    if (slotIndex < 0 || slotIndex >= NUM_SLOTS) return;

    const auto cur = static_cast<SlotState>(
        atomicState.slotState[slotIndex].load(std::memory_order_acquire));

    if (cur == SlotState::RECORDING)
    {
        // Toggle off → stop recording. Also clear a queued START that was
        // never drained (CIS stream gap): leaving both flags armed made the
        // eventual drain re-arm the slot and record forever with the UI on
        // IDLE — with start cleared, the drain order no longer matters.
        atomicState.startRecCmd[slotIndex].store(false, std::memory_order_release);
        atomicState.stopRecCmd[slotIndex].store(true, std::memory_order_release);
        atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::IDLE),
                                                std::memory_order_release);
        // Finalise the take NOW too: the UDP drain that normally finalises
        // (has_content / duration_us) only runs when a frame arrives — with
        // the CIS stream stopped, a SAVE SESSION right after this stop
        // silently skipped the take (has_content still false). Idempotent
        // with the drain thanks to the activeRecSlot guard.
        if (activeRecSlot.load(std::memory_order_acquire) == slotIndex)
        {
            std::lock_guard<std::mutex> lk(slotsMutex_);
            if (activeRecSlot.load(std::memory_order_relaxed) == slotIndex)
            {
                slots[slotIndex].has_content = (slots[slotIndex].frame_count > 0);
                slots[slotIndex].duration_us =
                    recBaseUs_ + (currentTimeUs() - recStartTimeUs);
                activeRecSlot.store(-1, std::memory_order_release);
                log_info("FS", "Slot %d: UI stop record — finalised %d frames",
                         slotIndex, slots[slotIndex].frame_count);
            }
        }
        log_info("FS", "Slot %d: UI stop record", slotIndex);
        return;
    }

    // Stop any other ongoing recording first (only one slot records at a time)
    const int curRec = activeRecSlot.load(std::memory_order_relaxed);
    if (curRec >= 0 && curRec != slotIndex)
    {
        atomicState.stopRecCmd[curRec].store(true, std::memory_order_release);
        atomicState.slotState[curRec].store(static_cast<int>(SlotState::IDLE),
                                             std::memory_order_release);
    }

    // Punch-in: stop playback of THIS slot only (a slot cannot play and record
    // at once). Other playing banks — and a running SCORE — keep going: the
    // player self-records its composited output into the armed slot, so
    // recording while other banks play IS the resampling path (multi-chain
    // split, 2026-07-13). When nothing else plays, the player session ends by
    // itself and restores live passthrough for a normal live recording.
    if (static_cast<SlotState>(atomicState.slotState[slotIndex].load(
            std::memory_order_relaxed)) == SlotState::PLAYING)
        atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::IDLE),
                                                std::memory_order_release);

    // Start recording immediately (UI one-click record)
    atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::RECORDING),
                                            std::memory_order_release);
    atomicState.startRecCmd[slotIndex].store(true, std::memory_order_release);
    log_info("FS", "Slot %d: UI start record", slotIndex);
}

// ============================================================================
// Non-RT: UI-triggered play / clear
// ============================================================================

void LuxSampler::uiPlaySlot(int slotIndex) noexcept
{
    if (slotIndex < 0 || slotIndex >= LuxSamplerConstants::NUM_SLOTS) return;

    const auto st = static_cast<SlotState>(
        atomicState.slotState[slotIndex].load(std::memory_order_relaxed));

    if (st == SlotState::PLAYING)
    {
        // Multi-bank play: stop ONLY this bank's voice. The player removes it
        // from its voice set on the next tick and restores live passthrough
        // itself once no voice remains — other banks keep playing.
        atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::IDLE),
                                                std::memory_order_release);
        return;
    }

    if (st == SlotState::RECORDING) return; // busy

    if (!slots[slotIndex].has_content) return; // nothing recorded yet

    // Trigger playback ADDITIVELY (multi-bank): banks already playing keep
    // playing — the player's voice sync picks this slot up from its PLAYING
    // state; the per-bank mixer (level + mix mode) composites them all.
    // UI-driven: FramePlayerThread is allowed to restore live passthrough when
    // playback ends — clear the sequencer ownership flag.
    stopOtherEnginesPlayback(engineIndex_);  // shared-chain eviction only
    atomicState.seqControlledPlay.store(false, std::memory_order_release);
    atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::PLAYING),
                                            std::memory_order_release);
    atomicState.activePlaySlot.store(slotIndex,  std::memory_order_release);
    // Clear any stale stop request BEFORE arming playback — stopPlayerThread()
    // (every prepareToPlay) leaves stopPlayCmd=true that the idle player never
    // consumes; the first PLAY after it was silently swallowed ("press PLAY
    // twice" bug — same fix as uiPlayScore). Sequenced-before startPlayCmd.
    atomicState.stopPlayCmd.store(false,         std::memory_order_release);
    atomicState.startPlayCmd.store(slotIndex,    std::memory_order_release);
    atomicState.passthroughEnabled.store(false,  std::memory_order_release);
}

// ============================================================================
// SCORE module — image → frames + transport
// ============================================================================

void LuxSampler::loadScoreFramesFromImage(const juce::Image& image,
                                         juce::Rectangle<int> band,
                                         double scoreMinHz,
                                         double scoreMaxHz,
                                         bool stereo)
{
    // Always stop any score playback first — FramePlayerThread must not be
    // reading scoreSlot while we reallocate it. The stop is asynchronous, so
    // also wait until the player has actually released the score slot.
    uiStopScore();
    waitForPlayerRelease(LuxSamplerConstants::SCORE_SLOT);
    scorePlayHead.store(0, std::memory_order_relaxed);

    if (!image.isValid() || image.getWidth() <= 0 || image.getHeight() <= 0)
    {
        std::lock_guard<std::mutex> lk(slotsMutex_);
        scoreSlot.clear();
        log_warning("FS", "Score: invalid image, slot cleared");
        return;
    }

    // Extract ONLY the spectrogram band (the part a CIS sensor would scan).
    // Empty band ⇒ fall back to the full image.
    juce::Rectangle<int> b = band.getWidth() > 0 && band.getHeight() > 0
        ? band.getIntersection(image.getBounds())
        : image.getBounds();
    if (b.getWidth() <= 0 || b.getHeight() <= 0)
        b = image.getBounds();

    const int bandX = b.getX();
    const int bandY = b.getY();
    const int bandW = b.getWidth();
    const int bandH = b.getHeight();
    const int frames = std::min(bandW, LuxSamplerConstants::MAX_FRAMES_PER_SLOT);

    // Allocate the score slot to the ACTUAL number of frames (NOT the 180k
    // FrameSlot::allocate() default — that would reserve ~1.87 GB).
    auto buffer = std::make_unique<CapturedFrame[]>(static_cast<size_t>(frames));

    // ── Build the per-output-pixel band row LUT ───────────────────────────────
    // The synthesis maps pixel index px∈[0,kPx) LOGARITHMICALLY to frequency over
    // the instrument's range [synthLo, synthHi]. The score band is ALSO drawn on a
    // LOG axis over [scoreMinHz, scoreMaxHz] (see ScoreGenRenderer — matches
    // PhonoPaper). So this is a log→log match: when the two ranges coincide it is a
    // straight 1:1 row-per-oscillator readout, identical to how the physical CIS
    // scanner reads a printed strip. This keeps the live preview equal to the
    // print+scan result. Rows outside the band map to -1 → white (silence).
    constexpr int kPx = LuxSamplerConstants::MAX_PIXELS;
    const int denom = (kPx > 1) ? (kPx - 1) : 1;

    extern sp3ctra_config_t g_sp3ctra_config;
    const double synthLo = (double) g_sp3ctra_config.low_frequency;
    const double synthHi = (double) g_sp3ctra_config.high_frequency;
    const bool logMap = (scoreMaxHz > scoreMinHz) && (scoreMinHz > 0.0)
                         && (synthHi > synthLo) && (synthLo > 0.0);

    std::vector<int> rowLut(static_cast<size_t>(kPx));
    for (int px = 0; px < kPx; ++px)
    {
        if (logMap)
        {
            const double t = (double) px / (double) denom;             // 0..1 (px → note)
            const double f = synthLo * std::pow(synthHi / synthLo, t); // synth log freq
            // Position of f on the band's LOG frequency axis (matches the image).
            const double pos = std::log(f / scoreMinHz)
                             / std::log(scoreMaxHz / scoreMinHz);
            if (pos < 0.0 || pos > 1.0)
                rowLut[(size_t) px] = -1;                              // outside band → silence
            else
                rowLut[(size_t) px] = (bandY + bandH - 1)
                    - (int) (pos * (double) (bandH - 1) + 0.5);        // flip: low freq → bottom
        }
        else
        {
            // Fallback: plain flipped linear resample of the band.
            rowLut[(size_t) px] = (bandY + bandH - 1)
                - (int) ((static_cast<int64_t>(px) * (bandH - 1)) / denom);
        }
    }

    {
        const juce::Image::BitmapData bmp(image, juce::Image::BitmapData::readOnly);
        for (int x = 0; x < frames; ++x)
        {
            CapturedFrame& f = buffer[static_cast<size_t>(x)];
            f.timestamp_us = static_cast<uint64_t>(x) * 1000ULL; // 1 ms/frame (1000 lps)
            f.line_id      = static_cast<uint32_t>(x);
            f.pixel_count  = static_cast<uint16_t>(kPx);
            const int imgX = bandX + x;
            for (int px = 0; px < kPx; ++px)
            {
                const int srcRow = rowLut[(size_t) px];
                if (srcRow < 0)
                {
                    // Frequency outside the band → white (silence) on every channel.
                    f.R[px] = f.G[px] = f.B[px] = 255;
                    continue;
                }
                const juce::Colour c = bmp.getPixelColour(imgX,
                        juce::jlimit(bandY, bandY + bandH - 1, srcRow));
                if (stereo)
                {
                    // Colour composite (left=red, right=blue): forward R/B unchanged
                    // so LuxStral's colour-temperature panning splits L/R, and
                    // rebalance loudness through GREEN only (green is pan-neutral —
                    // it cancels in both colour-temperature axes). The synth
                    // amplitude is ITU luma, which weights red (0.299) ≠ blue (0.114),
                    // so a left-panned cell would otherwise sit ~2 dB below an equal
                    // right-panned one. A fully-panned cell can be at most
                    // (1-0.299)=0.701 loud (its silent side forces the red channel to
                    // 255), so we drive every cell's inverted-luma to a side-symmetric
                    // target — L and R end up equal, centre stays full. No LuxStral
                    // code is touched; the displayed/exported image is unchanged.
                    const int   R  = c.getRed(), B = c.getBlue();
                    const float rE = 1.0f - R / 255.0f;     // right energy
                    const float lE = 1.0f - B / 255.0f;     // left  energy
                    const float mx = juce::jmax(lE, rE);
                    const float mn = juce::jmin(lE, rE);
                    const float ampTarget = 0.701f * mx + 0.299f * mn;
                    const float fg = 255.0f
                        * (1.0f - (ampTarget - 0.299f * rE - 0.114f * lE) / 0.587f);
                    f.R[px] = (uint8_t) R;
                    f.G[px] = (uint8_t) juce::jlimit(0, 255, (int) (fg + 0.5f));
                    f.B[px] = (uint8_t) B;
                }
                else
                {
                    // Greyscale: red channel copied to R=G=B (centred playback).
                    const uint8_t g = c.getRed();
                    f.R[px] = f.G[px] = f.B[px] = g;
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(slotsMutex_);
        scoreSlot.frames      = std::move(buffer);
        scoreSlot.capacity    = frames;
        scoreSlot.frame_count = frames;
        scoreSlot.play_head   = 0;
        scoreSlot.duration_us = static_cast<uint64_t>(frames) * 1000ULL;
        scoreSlot.has_content = true;
    }

    log_info("FS", "Score: loaded %d frames from band %dx%d (image %dx%d)",
             frames, bandW, bandH, image.getWidth(), image.getHeight());
}

void LuxSampler::uiPlayScore() noexcept
{
    if (scorePlaying.load(std::memory_order_acquire))
    {
        uiStopScore();
        return;
    }

    if (!scoreSlot.has_content) return; // nothing generated yet

    // Relay: if a sampler slot currently owns the shared channel, remember it so it
    // resumes when SCORE stops — SCORE takes over the relay; when it stops, the
    // sampler stream lives on. Halt the sampler's injection (slotState→IDLE) and free the
    // channel for SCORE. A non-slot owner leaves any armed relay untouched, so it
    // survives a live-EQ reload (stop → reallocate → re-play).
    // Encoding: engine * NUM_SLOTS + slot — the channel may be owned by
    // ANOTHER engine (e.g. sampler B playing while SCORE lives on A); the
    // arbiter below would otherwise stop it with no memory and the contract
    // "the sampler resumes when SCORE stops" broke for engine B.
    const int curPlay = atomicState.activePlaySlot.load(std::memory_order_relaxed);
    if (curPlay >= 0)
    {
        atomicState.stopPlayCmd.store(true, std::memory_order_release);
        if (curPlay < LuxSamplerConstants::NUM_SLOTS)
        {
            scoreRelaySlot_.store(engineIndex_ * LuxSamplerConstants::NUM_SLOTS + curPlay,
                                  std::memory_order_relaxed);
            atomicState.slotState[curPlay].store(static_cast<int>(SlotState::IDLE),
                                                 std::memory_order_release);
        }
    }
    else
    {
        // Cross-engine displacement only exists on a SHARED chain (one
        // stream). On split chains the other engine keeps playing — the
        // arbiter below is a no-op there and arming a relay for it would
        // wrongly re-trigger its (still playing) slot when SCORE stops.
        if (enginesShareSameChain())
            for (int i = 0; i < kMaxEngines; ++i)
            {
                if (i == engineIndex_) continue;
                LuxSampler* e = s_engines[i].load(std::memory_order_acquire);
                if (e == nullptr) continue;
                const int cp = e->atomicState.activePlaySlot.load(std::memory_order_relaxed);
                if (cp >= 0 && cp < LuxSamplerConstants::NUM_SLOTS)
                {
                    scoreRelaySlot_.store(i * LuxSamplerConstants::NUM_SLOTS + cp,
                                          std::memory_order_relaxed);
                    break;   // single playback channel → at most one owner
                }
            }
    }

    // Score is UI-driven (never sequencer-controlled): FramePlayerThread is
    // free to restore live passthrough when one-shot playback ends.
    stopOtherEnginesPlayback(engineIndex_);  // shared-chain eviction only
    atomicState.seqControlledPlay.store(false, std::memory_order_release);
    // A real PLAY always advances — clear any leftover scrub-hold flag.
    atomicState.scoreScrubbing.store(false, std::memory_order_release);
    scorePlaying.store(true, std::memory_order_release);
    atomicState.activePlaySlot.store(LuxSamplerConstants::SCORE_SLOT, std::memory_order_release);
    // Clear any stale stop request BEFORE arming playback. loadScoreFramesFromImage()
    // (run on every GENERATE / EQ reload) calls uiStopScore(), which leaves
    // stopPlayCmd=true; because nothing is playing then, the idle FramePlayerThread
    // never consumes it. Without this clear the first PLAY's inner loop would see the
    // leftover flag on its very first iteration and break out immediately — that is
    // the "must press PLAY twice to start" bug. Sequenced-before the startPlayCmd
    // release store below, so the thread is guaranteed to observe stopPlayCmd=false
    // once it picks up this play command.
    atomicState.stopPlayCmd.store(false, std::memory_order_release);
    atomicState.startPlayCmd.store(LuxSamplerConstants::SCORE_SLOT,   std::memory_order_release);
    atomicState.passthroughEnabled.store(false, std::memory_order_release);
}

void LuxSampler::uiStopScore() noexcept
{
    scorePlaying.store(false, std::memory_order_release);
    atomicState.scoreScrubbing.store(false, std::memory_order_release); // also ends any scrub
    scorePlayHead.store(0, std::memory_order_relaxed);
    scoreResumeHead.store(-1, std::memory_order_relaxed); // drop any armed resume

    // Only touch the shared playback channel if SCORE actually owns it. A sampler
    // slot playing on the same channel must keep running: SCORE teardown (manual
    // STOP, GENERATE, live-EQ reload, tab close) must never interrupt the sampler.
    if (atomicState.activePlaySlot.load(std::memory_order_relaxed)
        != LuxSamplerConstants::SCORE_SLOT)
        return;

    // STOP ⇒ silence, like a natural end-of-score (stoppedByNoneMode) and the
    // scrub release: without it a mid-play STOP leaves the last staged column
    // ringing (no staging timeout, no other producer on a sourceless chain).
    // An armed relay still resumes right after — the white frame is overwritten
    // on the resumed sampler's next 1 ms tick.
    atomicState.injectSilenceCmd.store(true, std::memory_order_release);
    atomicState.stopPlayCmd.store(true, std::memory_order_release);
    atomicState.activePlaySlot.store(-1, std::memory_order_release);
    // Leave passthrough disabled when a sampler slot is queued to resume (the
    // player thread re-arms it from its relay slot); restore live only when there
    // is nothing to relay.
    if (scoreRelaySlot_.load(std::memory_order_relaxed) < 0)
        atomicState.passthroughEnabled.store(true, std::memory_order_release);
}

void LuxSampler::uiDiscardScore()
{
    // Stop first: FramePlayerThread must not be reading scoreSlot while we free it
    // (same ordering as loadScoreFramesFromImage's reallocation path). The stop is
    // asynchronous, so wait until the player has actually released the score slot.
    uiStopScore();
    waitForPlayerRelease(LuxSamplerConstants::SCORE_SLOT);
    scorePlayHead.store(0, std::memory_order_relaxed);
    scoreResumeHead.store(-1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(slotsMutex_);
    scoreSlot.clear();   // releases the frame buffer + resets has_content/frame_count
}

bool LuxSampler::uiBeginScoreScrub() noexcept
{
    // Already playing → the live-seek path (uiSeekScore) handles the drag; don't
    // spin up a second transport.
    if (scorePlaying.load(std::memory_order_acquire)) return false;
    if (!scoreSlot.has_content) return false;

    // Take over the synthesis channel from any other player (sampler slot or
    // sequencer), mirroring uiPlayScore(). Relay: remember an overridden sampler
    // slot so it resumes when the scrub ends (uiEndScoreScrub). Same encoding
    // as uiPlayScore: engine * NUM_SLOTS + slot (cross-engine relay).
    const int curPlay = atomicState.activePlaySlot.load(std::memory_order_relaxed);
    if (curPlay >= 0)
    {
        atomicState.stopPlayCmd.store(true, std::memory_order_release);
        if (curPlay < LuxSamplerConstants::NUM_SLOTS)
        {
            scoreRelaySlot_.store(engineIndex_ * LuxSamplerConstants::NUM_SLOTS + curPlay,
                                  std::memory_order_relaxed);
            atomicState.slotState[curPlay].store(static_cast<int>(SlotState::IDLE),
                                                 std::memory_order_release);
        }
    }
    else
    {
        for (int i = 0; i < kMaxEngines; ++i)
        {
            if (i == engineIndex_) continue;
            LuxSampler* e = s_engines[i].load(std::memory_order_acquire);
            if (e == nullptr) continue;
            const int cp = e->atomicState.activePlaySlot.load(std::memory_order_relaxed);
            if (cp >= 0 && cp < LuxSamplerConstants::NUM_SLOTS
                && enginesShareSameChain())   // cross-engine relay: shared chain only
            {
                scoreRelaySlot_.store(i * LuxSamplerConstants::NUM_SLOTS + cp,
                                      std::memory_order_relaxed);
                break;   // single playback channel → at most one owner
            }
        }
    }

    // Scrub is UI-driven and HOLDS position: FramePlayerThread injects the column
    // at the (uiSeekScore-armed) play head every tick but never auto-advances.
    // Note: scorePlaying stays FALSE so the PLAY/STOP button + head-line keep
    // their stopped appearance — this is a transient audition, not playback.
    stopOtherEnginesPlayback(engineIndex_);  // shared-chain eviction only
    atomicState.seqControlledPlay.store(false, std::memory_order_release);
    atomicState.scoreScrubbing.store(true, std::memory_order_release);
    atomicState.activePlaySlot.store(LuxSamplerConstants::SCORE_SLOT, std::memory_order_release);
    // Clear any stale stop request left by a prior uiStopScore()/uiEndScoreScrub()
    // (the idle thread never consumes it) BEFORE arming, else the inner loop would
    // break out on its first iteration. Sequenced-before the startPlayCmd release.
    atomicState.stopPlayCmd.store(false, std::memory_order_release);
    atomicState.startPlayCmd.store(LuxSamplerConstants::SCORE_SLOT, std::memory_order_release);
    atomicState.passthroughEnabled.store(false, std::memory_order_release);
    return true;
}

void LuxSampler::uiEndScoreScrub() noexcept
{
    // Stop injection and relinquish the channel — same channel-aware path as a
    // normal STOP (see uiStopScore): only act if SCORE owns the channel so a
    // sampler playing underneath keeps running. We do NOT reset scorePlayHead, so
    // the scrub head line stays where the drag left it, and we keep the armed
    // resume head so a subsequent PLAY starts from that column.
    atomicState.scoreScrubbing.store(false, std::memory_order_release);
    if (atomicState.activePlaySlot.load(std::memory_order_relaxed)
        != LuxSamplerConstants::SCORE_SLOT)
        return;
    // The audition falls SILENT on release — audio/video symmetry: the probe
    // rings stop receiving frames the instant the session dies, so the held
    // column must stop sounding too (it used to ring forever: the stagings
    // have no timeout and a sourceless score chain has no other producer).
    // Set BEFORE stopPlayCmd so the exiting session is guaranteed to observe
    // it in its end-of-session silence block; the idle drain in run() catches
    // the flag if the session already exited.
    atomicState.injectSilenceCmd.store(true, std::memory_order_release);
    atomicState.stopPlayCmd.store(true, std::memory_order_release);
    atomicState.activePlaySlot.store(-1, std::memory_order_release);
    // Leave passthrough disabled when a sampler slot is queued to resume (the
    // player thread re-arms it); restore live only when there is nothing to relay.
    if (scoreRelaySlot_.load(std::memory_order_relaxed) < 0)
        atomicState.passthroughEnabled.store(true, std::memory_order_release);
}

void LuxSampler::uiClearSlot(int slotIndex) noexcept
{
    if (slotIndex < 0 || slotIndex >= LuxSamplerConstants::NUM_SLOTS) return;

    // Stop recording / playback first
    const auto st = static_cast<SlotState>(
        atomicState.slotState[slotIndex].load(std::memory_order_relaxed));

    if (st == SlotState::RECORDING)
    {
        atomicState.stopRecCmd[slotIndex].store(true, std::memory_order_release);
    }
    // If this slot is playing, dropping its PLAYING state below removes just
    // its voice — other banks keep playing; the player restores passthrough
    // itself when no voice remains (multi-bank play).

    atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::IDLE),
                                            std::memory_order_release);

    // Disarm recording immediately (the stopRecCmd drain runs on the UDP thread
    // and may not have executed yet) so recordModulatedFrame() bails out before
    // writing into the buffer we are about to free.
    if (activeRecSlot.load(std::memory_order_relaxed) == slotIndex)
        activeRecSlot.store(-1, std::memory_order_release);

    // The stop above is asynchronous — wait until the player has released this
    // slot (it dereferences slot.frames WITHOUT slotsMutex_) before freeing.
    waitForPlayerRelease(slotIndex);

    // Clear the slot data under slotsMutex_ so that sampleSpectralForTimeline
    // (message thread) cannot access slot.frames while clear() frees it.
    {
        std::lock_guard<std::mutex> lk(slotsMutex_);
        slots[slotIndex].clear();
    }

    // Wiping the recording also resets its vertical (HF/LF frequency) filter —
    // otherwise a leftover curve would silently shape the NEXT take in this slot.
    resetSlotFreqCurve(slotIndex);
    resetSlotEditHandles(slotIndex);   // start/end/fades/floor back to defaults
}

// ============================================================================
// Thread lifecycle
// ============================================================================

void LuxSampler::startPlayerThread(AudioImageBuffers* audioBuffers,
                                     DoubleBuffer*      doubleBuffer)
{
    stopPlayerThread();
    if (audioBuffers == nullptr)
    {
        log_warning("FS", "startPlayerThread: audioBuffers is null — FramePlayerThread not started");
        return;
    }
    if (doubleBuffer == nullptr)
        log_warning("FS", "startPlayerThread: doubleBuffer is null — preprocessed_data bypass inactive");

    // Store pointers so onFrameAssembled() can write sampler snapshot during recording
    audioBuffers_ = audioBuffers;
    doubleBuffer_ = doubleBuffer;

    playerThread = std::make_unique<FramePlayerThread>(*this, audioBuffers, doubleBuffer);
    playerThread->startThread(juce::Thread::Priority::normal);
    log_info("FS", "FramePlayerThread started");
}

void LuxSampler::stopPlayerThread()
{
    if (playerThread)
    {
        atomicState.stopPlayCmd.store(true, std::memory_order_release);
        atomicState.passthroughEnabled.store(true, std::memory_order_release);
        playerThread->stopThread(2000);
        playerThread.reset();
        log_info("FS", "FramePlayerThread stopped");
    }
}

void LuxSampler::waitForPlayerRelease(int slotIndex, int timeoutMs) const noexcept
{
    // Message thread only. Stop commands are asynchronous: the player may still
    // be inside its current tick dereferencing slot.frames. Wait until it has
    // released the slot before the caller frees/replaces the frame buffer.
    // slotIndex < 0 waits for "no REAL slot busy" (a running SCORE keeps its own
    // scoreSlot and does not conflict with slots[] replacement).
    // Two consecutive idle observations are required: the player publishes
    // playerBusyMask_ a few instructions AFTER consuming startPlayCmd, so a
    // single read could race a playback that is just starting.
    constexpr uint32_t kRealSlotsMask =
        (1u << LuxSamplerConstants::NUM_SLOTS) - 1u;
    int idleStreak = 0;
    for (int elapsed = 0; elapsed <= timeoutMs; ++elapsed)
    {
        const uint32_t busy = playerBusyMask_.load(std::memory_order_acquire);
        const bool clear = (slotIndex >= 0)
            ? ((busy & (1u << slotIndex)) == 0)
            : ((busy & kRealSlotsMask) == 0);
        if (clear)
        {
            if (++idleStreak >= 2) return;
        }
        else
            idleStreak = 0;
        juce::Thread::sleep(1);
    }
    log_warning("FS", "waitForPlayerRelease(%d): timeout after %d ms — proceeding",
                slotIndex, timeoutMs);
}

// ============================================================================
// Slot management
// ============================================================================

void LuxSampler::clearSlot(int i)
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return;

    if (activeRecSlot.load() == i) activeRecSlot.store(-1);

    // Dropping the PLAYING state removes only this slot's voice — other banks
    // keep playing; the player restores passthrough itself when none remain.
    atomicState.slotState[i].store(static_cast<int>(SlotState::IDLE));

    // The stop above is asynchronous — wait until the player has released this
    // slot (it dereferences slot.frames WITHOUT slotsMutex_) before freeing.
    waitForPlayerRelease(i);
    {
        std::lock_guard<std::mutex> lk(slotsMutex_);
        slots[i].clear();
    }
    // Also reset the vertical (HF/LF frequency) filter — see uiClearSlot.
    resetSlotFreqCurve(i);
    resetSlotEditHandles(i);   // start/end/fades/floor back to defaults
    log_info("FS", "Slot %d cleared", i);
}

void LuxSampler::resetSlotEditHandles(int i) noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return;
    setSlotStartFrac(i, 0.0f);
    setSlotEndFrac  (i, 1.0f);
    setSlotAttackLen(i, 0.0f);
    setSlotDecayLen (i, 0.0f);
    setSlotEqFloor  (i, 0.0f);
}

void LuxSampler::resetSlotPlayParams(int i) noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return;
    setSlotSpeed         (i, 1.0f);
    setSlotLoopMode      (i, LoopMode::LOOP);
    setSlotResumeMode    (i, false);
    setSlotBlendAmount   (i, 0.0f);
    setSlotBrightnessLift(i, 0.0f);                  // bank fader back to full
    setSlotMixMode       (i, SlotMixMode::DARKEN);
    setSlotTrebleCut     (i, 0.0f);
    setSlotBassCut       (i, 0.0f);
    setSlotFadeCurveType (i, FadeCurveType::LINEAR); // writes attack+decay too
    setSlotFadeCurvePower(i, 1.0f);
    setSlotLabel         (i, "");
    resetSlotEditHandles (i);   // start/end/attack/decay/floor
    resetSlotFreqCurve   (i);   // flat EQ (clears state + republishes the LUT)
}

void LuxSampler::clearAllSlots()
{
    for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
        clearSlot(i);
    log_info("FS", "All slots cleared");
}

// ============================================================================
// copySlotTo — Non-RT slot duplication (message thread only)
// Deep-copies frame_count frames + play parameters from src to dst.
// Stops any ongoing activity on the destination slot first.
// ============================================================================
void LuxSampler::copySlotTo(int srcIdx, int dstIdx)
{
    if (srcIdx < 0 || srcIdx >= LuxSamplerConstants::NUM_SLOTS) return;
    if (dstIdx < 0 || dstIdx >= LuxSamplerConstants::NUM_SLOTS) return;
    if (srcIdx == dstIdx) return;

    // Stop any ongoing activity on the destination (atomics only — no lock needed)
    atomicState.slotState[dstIdx].store(static_cast<int>(SlotState::IDLE),
                                        std::memory_order_release);
    if (atomicState.activePlaySlot.load(std::memory_order_acquire) == dstIdx)
        atomicState.activePlaySlot.store(-1, std::memory_order_release);

    // The player checks slotState each tick but may still be inside the current
    // one — wait for it to release the destination before rewriting its frames.
    waitForPlayerRelease(dstIdx);

    std::lock_guard<std::mutex> lk(slotsMutex_);

    const FrameSlot& src = slots[srcIdx];
    if (!src.has_content || src.frame_count == 0)
    {
        log_warning("FS", "copySlotTo: source slot %d has no content", srcIdx);
        return;
    }

    FrameSlot& dst = slots[dstIdx];
    dst.allocate(); // resets dst.frame_count to 0, keeps existing heap if already allocated

    // Copy only the recorded frames (not the full capacity)
    const int count = juce::jmin(src.frame_count, dst.capacity);
    std::memcpy(dst.frames.get(), src.frames.get(),
                static_cast<size_t>(count) * sizeof(CapturedFrame));
    dst.frame_count = count;
    dst.duration_us = src.duration_us;
    dst.has_content = true;
    std::strncpy(dst.label, src.label, sizeof(dst.label) - 1);
    dst.label[sizeof(dst.label) - 1] = '\0';

    // Copy play parameters (atomic setters — no lock needed)
    setSlotStartFrac (dstIdx, getSlotStartFrac (srcIdx));
    setSlotEndFrac   (dstIdx, getSlotEndFrac   (srcIdx));
    setSlotSpeed     (dstIdx, getSlotSpeed      (srcIdx));
    setSlotLoopMode  (dstIdx, getSlotLoopMode   (srcIdx));
    setSlotResumeMode    (dstIdx, getSlotResumeMode    (srcIdx));
    setSlotBlendAmount   (dstIdx, getSlotBlendAmount   (srcIdx));
    setSlotAttackLen     (dstIdx, getSlotAttackLen     (srcIdx));
    setSlotDecayLen      (dstIdx, getSlotDecayLen      (srcIdx));
    setSlotBrightnessLift(dstIdx, getSlotBrightnessLift(srcIdx));
    setSlotTrebleCut     (dstIdx, getSlotTrebleCut     (srcIdx));
    setSlotBassCut       (dstIdx, getSlotBassCut       (srcIdx));
    setSlotFadeCurveType (dstIdx, getSlotFadeCurveType (srcIdx));
    setSlotFadeCurvePower(dstIdx, getSlotFadeCurvePower(srcIdx));
    setSlotAttackCurveType (dstIdx, getSlotAttackCurveType (srcIdx));
    setSlotAttackCurvePower(dstIdx, getSlotAttackCurvePower(srcIdx));
    setSlotDecayCurveType  (dstIdx, getSlotDecayCurveType  (srcIdx));
    setSlotDecayCurvePower (dstIdx, getSlotDecayCurvePower (srcIdx));
    setSlotEq            (dstIdx, getSlotEq            (srcIdx));
    setSlotEqFloor       (dstIdx, getSlotEqFloor       (srcIdx));

    log_info("FS", "copySlotTo: slot %d → %d (%d frames)", srcIdx, dstIdx, count);
}

// ============================================================================
// cropSlotToBounds — destructive trim to the current [startFrac, endFrac].
// Non-RT (message thread only). Moves the kept frames to the front of the
// buffer, rebases their timestamps to t₀=0, and resets the bounds to full.
// ============================================================================
void LuxSampler::cropSlotToBounds(int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= LuxSamplerConstants::NUM_SLOTS) return;

    // Stop any ongoing recording / playback on this slot first. Clearing
    // activeRecSlot makes recordModulatedFrame() bail before it takes the lock,
    // so no UDP-thread write races with the memmove below.
    const auto st = static_cast<SlotState>(
        atomicState.slotState[slotIndex].load(std::memory_order_relaxed));
    if (st == SlotState::RECORDING)
    {
        atomicState.stopRecCmd[slotIndex].store(true, std::memory_order_release);
        if (activeRecSlot.load(std::memory_order_relaxed) == slotIndex)
            activeRecSlot.store(-1, std::memory_order_release);
    }
    if (st == SlotState::PLAYING)
    {
        atomicState.stopPlayCmd.store(true, std::memory_order_release);
        atomicState.activePlaySlot.store(-1, std::memory_order_release);
        atomicState.passthroughEnabled.store(true, std::memory_order_release);
    }
    atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::IDLE),
                                            std::memory_order_release);

    // The stops above are asynchronous — wait until the player has released
    // this slot before the memmove below reorders its frames.
    waitForPlayerRelease(slotIndex);

    std::lock_guard<std::mutex> lk(slotsMutex_);

    FrameSlot& slot = slots[slotIndex];
    if (!slot.isAllocated() || !slot.has_content || slot.frame_count <= 0)
        return;

    const int   fc = slot.frame_count;
    const float sf = getSlotStartFrac(slotIndex);
    const float ef = getSlotEndFrac(slotIndex);
    const int startFrame = juce::jlimit(0, fc - 1,
                                        static_cast<int>(sf * static_cast<float>(fc)));
    const int endFrame   = juce::jlimit(startFrame + 1, fc,
                                        static_cast<int>(ef * static_cast<float>(fc)));
    const int count = endFrame - startFrame;

    if (count <= 0 || (startFrame == 0 && endFrame == fc))
        return; // bounds already cover the whole take — nothing to trim

    if (startFrame > 0)
        std::memmove(slot.frames.get(),
                     slot.frames.get() + startFrame,
                     static_cast<size_t>(count) * sizeof(CapturedFrame));

    // Rebase timestamps so the cropped take starts at t₀ = 0.
    const uint64_t base = slot.frames[0].timestamp_us;
    for (int i = 0; i < count; ++i)
        slot.frames[i].timestamp_us -= base;

    slot.frame_count = count;
    slot.play_head   = 0;
    slot.duration_us = slot.frames[count - 1].timestamp_us;
    slot.has_content = true;

    // The kept region is now the entire buffer.
    setSlotStartFrac(slotIndex, 0.0f);
    setSlotEndFrac  (slotIndex, 1.0f);

    log_info("FS", "Slot %d: cropped [%d..%d] → %d frames, %.2f s",
             slotIndex, startFrame, endFrame, count,
             static_cast<double>(slot.duration_us) / 1e6);
}

// ============================================================================
// Image EQ (SCORE-style ±dB, boost + cut) — LUT build + publish (message thread)
//
// The per-slot EQ is stored as an encoded string in the SAME format as
// ScoreEqComponent::encodeState():  "minF|maxF|g0;g1;…"  where the gains sit on
// octave-boundary nodes. rebuildFreqLut() parses it and fills the double-buffered
// freqLut_ with a GAIN IN dB per normalised pixel position (left=bass … right=
// treble); the RT loop turns that into a darkness shift (see FramePlayerThread).
// freqCurveActive_ stays false while the curve is flat so the RT loop skips it.
// ============================================================================
namespace {
/** Parse the gains list out of an "minF|maxF|g0;g1;…" EQ string.
 *  @return the number of gains written to @p out (0 → treat as flat). */
int parseEqGains(const juce::String& s, float* out, int maxN) noexcept
{
    if (s.isEmpty()) return 0;
    const int bar2 = s.lastIndexOfChar('|');
    if (bar2 < 0) return 0;
    const juce::String gainsStr = s.substring(bar2 + 1);
    juce::StringArray toks;
    toks.addTokens(gainsStr, ";", "");
    int n = 0;
    for (const auto& t : toks)
    {
        if (n >= maxN) break;
        out[n++] = juce::jlimit(-24.0f, 24.0f, t.getFloatValue());
    }
    return n;
}
} // namespace

void LuxSampler::initFreqCurveDefaults() noexcept
{
    for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
    {
        eqState_[i].clear();
        for (int b = 0; b < 2; ++b)
            for (int j = 0; j < LuxSamplerConstants::FREQ_LUT_N; ++j)
                freqLut_[i][b][j] = 0.0f;                 // 0 dB everywhere
        freqLutActive_[i].store(0, std::memory_order_relaxed);
        freqCurveActive_[i].store(false, std::memory_order_relaxed);
    }
}

void LuxSampler::resetSlotFreqCurve(int i) noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return;
    eqState_[i].clear();
    rebuildFreqLut(i);   // publishes a flat (0 dB) LUT + clears freqCurveActive_
}

void LuxSampler::rebuildFreqLut(int i) noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return;

    float gains[LuxSamplerConstants::MAX_EQ_NODES];
    const int ng = parseEqGains(eqState_[i], gains, LuxSamplerConstants::MAX_EQ_NODES);

    // Fill the inactive buffer with a per-position GAIN IN dB, then publish.
    // Nodes sit on octave boundaries → position xn maps linearly onto the node
    // index axis (idx = xn·(ng-1)); we linear-interpolate the dB between nodes.
    const int cur    = freqLutActive_[i].load(std::memory_order_relaxed);
    const int target = 1 - cur;
    float*    lut    = freqLut_[i][target];

    bool active = false;
    if (ng >= 2)
    {
        for (int j = 0; j < LuxSamplerConstants::FREQ_LUT_N; ++j)
        {
            const float xn  = (float) j / (float) (LuxSamplerConstants::FREQ_LUT_N - 1);
            const float idx = xn * (float) (ng - 1);
            int   i0 = (int) idx;
            if (i0 > ng - 2) i0 = ng - 2;
            const float frac = idx - (float) i0;
            const float g    = gains[i0] + frac * (gains[i0 + 1] - gains[i0]);
            lut[j] = g;
            if (std::abs(g) > 0.01f) active = true;
        }
    }
    else
    {
        for (int j = 0; j < LuxSamplerConstants::FREQ_LUT_N; ++j) lut[j] = 0.0f;
    }

    freqLutActive_[i].store(target, std::memory_order_release);
    freqCurveActive_[i].store(active, std::memory_order_release);
}

void LuxSampler::setSlotEq(int i, const juce::String& encoded) noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return;
    eqState_[i] = encoded;
    rebuildFreqLut(i);
}

float LuxSampler::getSlotEqBandGain(int slot, int band) const noexcept
{
    if (slot < 0 || slot >= LuxSamplerConstants::NUM_SLOTS) return 0.0f;
    if (band < 0 || band >= kEqBands) return 0.0f;
    float g[LuxSamplerConstants::MAX_EQ_NODES];
    const int n = parseEqGains(eqState_[slot], g, LuxSamplerConstants::MAX_EQ_NODES);
    return (band < n) ? g[band] : 0.0f;
}

void LuxSampler::setSlotEqBandGain(int slot, int band, float gainDb) noexcept
{
    if (slot < 0 || slot >= LuxSamplerConstants::NUM_SLOTS) return;
    if (band < 0 || band >= kEqBands) return;

    // Start from the current gains (missing/empty → flat), overwrite one band,
    // and re-encode the fixed 9-node grid in ScoreEqComponent's string format.
    float g[kEqBands] = { 0.0f };
    float parsed[LuxSamplerConstants::MAX_EQ_NODES];
    const int n = parseEqGains(eqState_[slot], parsed, LuxSamplerConstants::MAX_EQ_NODES);
    for (int i = 0; i < kEqBands; ++i)
        g[i] = (i < n) ? parsed[i] : 0.0f;
    g[band] = juce::jlimit(-24.0f, 24.0f, gainDb);

    juce::String enc;
    enc << juce::String(kEqMinHz, 3) << '|' << juce::String(kEqMaxHz, 3) << '|';
    for (int i = 0; i < kEqBands; ++i)
    {
        if (i) enc << ';';
        enc << juce::String(g[i], 2);
    }
    setSlotEq(slot, enc);   // stores + rebuildFreqLut()
}

juce::String LuxSampler::getSlotEq(int i) const
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return {};
    return eqState_[i];
}

// ============================================================================
// Slot info queries
// ============================================================================

SlotState LuxSampler::getSlotState(int i) const noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return SlotState::IDLE;
    return static_cast<SlotState>(
        atomicState.slotState[i].load(std::memory_order_relaxed));
}

int LuxSampler::getSlotFrameCount(int i) const noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0;
    return slots[i].frame_count;
}

uint64_t LuxSampler::getSlotDurationUs(int i) const noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return 0;
    return slots[i].duration_us;
}

bool LuxSampler::slotHasContent(int i) const noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return false;
    return slots[i].has_content;
}

const char* LuxSampler::getSlotLabel(int i) const noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return "";
    return slots[i].label;
}

void LuxSampler::setSlotLabel(int i, const char* label) noexcept
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS || label == nullptr) return;
    std::strncpy(slots[i].label, label, 63);
    slots[i].label[63] = '\0';
}

// ============================================================================
// File I/O — .fsmp binary format (§11 of the spec)
// ============================================================================

// Binary structures (packed — little-endian on all modern platforms)
#pragma pack(push, 1)

struct FsmpFileHeader  // 64 bytes
{
    char     magic[4];        // 0x00 "FSMP"
    uint16_t format_version;  // 0x04
    uint16_t flags;           // 0x06 reserved = 0
    uint32_t num_slots;       // 0x08
    uint8_t  midi_channel;    // 0x0C
    int8_t   octave_offset;   // 0x0D
    float    max_duration_s;  // 0x0E
    uint64_t created_at;      // 0x12
    uint8_t  reserved[34];    // 0x1A
    uint32_t header_crc32;    // 0x3C
};
static_assert(sizeof(FsmpFileHeader) == 64, "FsmpFileHeader size mismatch");

struct FsmpSlotHeader  // 82 bytes
{
    uint8_t  slot_index;   // 0x00
    uint8_t  has_content;  // 0x01
    uint32_t frame_count;  // 0x02
    uint64_t duration_us;  // 0x06
    char     label[64];    // 0x0E
    uint32_t slot_crc32;   // 0x4E
};
static_assert(sizeof(FsmpSlotHeader) == 82, "FsmpSlotHeader size mismatch");

struct FsmpFrameHeader  // 12 bytes
{
    uint64_t timestamp_us;
    uint32_t payload_size;  // line_id(4) + pixel_count(2) + R + G + B
};

#pragma pack(pop)

bool LuxSampler::saveToFile(const juce::File& file) const
{
    using namespace LuxSamplerConstants;

    juce::FileOutputStream out(file);
    if (!out.openedOk())
    {
        log_error("FS", "saveToFile: cannot open '%s'",
                  file.getFullPathName().toRawUTF8());
        return false;
    }
    out.setPosition(0);
    out.truncate();

    // ── File header ──────────────────────────────────────────────────────
    FsmpFileHeader hdr {};
    hdr.magic[0] = 'F'; hdr.magic[1] = 'S'; hdr.magic[2] = 'M'; hdr.magic[3] = 'P';
    hdr.format_version = FSMP_VERSION;
    hdr.flags          = 0;
    hdr.num_slots      = static_cast<uint32_t>(NUM_SLOTS);
    hdr.midi_channel   = static_cast<uint8_t>(midiChannel.load());
    hdr.octave_offset  = static_cast<int8_t>(octaveOffset.load());
    hdr.max_duration_s = maxDurationS.load();
    hdr.created_at     = static_cast<uint64_t>(std::time(nullptr));
    std::memset(hdr.reserved, 0, sizeof(hdr.reserved));
    hdr.header_crc32   = crc32_compute(reinterpret_cast<const uint8_t*>(&hdr),
                                        offsetof(FsmpFileHeader, header_crc32));
    out.write(&hdr, sizeof(hdr));

    // ── Slot blocks ──────────────────────────────────────────────────────
    // slotsMutex_ is taken per CHUNK, not across the whole file write: the UDP
    // thread takes it for every recorded frame, and holding it for the full
    // save froze live capture/recording for seconds. Frames below the
    // snapshotted frame_count are stable for the duration of the save —
    // recording only appends, and every path that frees or reorders frames
    // runs on THIS (message) thread.
    constexpr int kChunkFrames = 256;   // ≈ 2.6 MB staging
    // Freeze rec-command drains for the whole chunked copy (see saveInProgress_).
    saveInProgress_.store(true, std::memory_order_release);
    struct SaveFlagReset
    {
        std::atomic<bool>& f;
        ~SaveFlagReset() { f.store(false, std::memory_order_release); }
    } saveFlagReset { saveInProgress_ };   // mutable member — writable from const
    std::vector<CapturedFrame> staging;
    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        FsmpSlotHeader shdr {};
        int fc = 0;
        {
            std::lock_guard<std::mutex> lk(slotsMutex_);
            const FrameSlot& slot = slots[s];
            shdr.slot_index  = static_cast<uint8_t>(s);
            shdr.has_content = slot.has_content ? 0x01u : 0x00u;
            shdr.frame_count = static_cast<uint32_t>(slot.frame_count);
            shdr.duration_us = slot.duration_us;
            std::strncpy(shdr.label, slot.label, 63);
            shdr.label[63]   = '\0';
            if (slot.has_content && slot.frame_count > 0 && slot.isAllocated())
                fc = slot.frame_count;
        }
        shdr.slot_crc32 = crc32_compute(reinterpret_cast<const uint8_t*>(&shdr),
                                         offsetof(FsmpSlotHeader, slot_crc32));
        out.write(&shdr, sizeof(shdr));

        for (int base = 0; base < fc; base += kChunkFrames)
        {
            const int n = std::min(kChunkFrames, fc - base);
            staging.resize(static_cast<size_t>(n));
            {
                std::lock_guard<std::mutex> lk(slotsMutex_);
                std::memcpy(staging.data(), slots[s].frames.get() + base,
                            static_cast<size_t>(n) * sizeof(CapturedFrame));
            }
            for (int f = 0; f < n; ++f)
            {
                const CapturedFrame& fr = staging[static_cast<size_t>(f)];
                const uint32_t psize    = sizeof(fr.line_id) + sizeof(fr.pixel_count)
                                          + 3u * static_cast<uint32_t>(fr.pixel_count);
                FsmpFrameHeader fhdr { fr.timestamp_us, psize };
                out.write(&fhdr,          sizeof(fhdr));
                out.write(&fr.line_id,    sizeof(fr.line_id));
                out.write(&fr.pixel_count,sizeof(fr.pixel_count));
                out.write(fr.R, fr.pixel_count);
                out.write(fr.G, fr.pixel_count);
                out.write(fr.B, fr.pixel_count);
            }
        }
    }

    // ── EOF marker ───────────────────────────────────────────────────────
    const uint32_t eof = FSMP_EOF_MARKER;
    out.write(&eof, sizeof(eof));

    log_info("FS", "Saved to '%s' (%d slots)",
             file.getFullPathName().toRawUTF8(), NUM_SLOTS);
    return true;
}

// ============================================================================
// Image export — Non-RT only
// Each slot's recorded frames are rendered as a 2D RGB image:
//   X axis = pixel column (0 .. pixel_count-1)
//   Y axis = frame index  (0 .. frame_count-1)
// PNG = lossless, JPEG = quality 90.
// ============================================================================

juce::Image LuxSampler::renderSlotImage(int slotIndex,
                                        int maxW,
                                        int maxH,
                                        bool timeHorizontal) const
{
    using namespace LuxSamplerConstants;

    if (slotIndex < 0 || slotIndex >= NUM_SLOTS)
        return {};

    // Snapshot slot dimensions under mutex (metadata + pixel-count of first frame)
    int frameCount = 0;
    int pixelCount = 0;
    {
        std::lock_guard<std::mutex> lk(slotsMutex_);
        const FrameSlot& slot = slots[slotIndex];
        if (!slot.has_content || slot.frame_count <= 0 || !slot.isAllocated())
            return {};
        frameCount = slot.frame_count;
        pixelCount = slot.frames[0].pixel_count;
        if (pixelCount <= 0)
            return {};
    }

    // Native orientation: X = pixel (frequency), Y = frame (time, earliest on top).
    // timeHorizontal transposes for the UI backdrop: X = frame (time, left→right),
    // Y = pixel mapped so treble (high index) is on top and bass (low index) bottom.
    const int srcW = timeHorizontal ? frameCount : pixelCount; // source columns
    const int srcH = timeHorizontal ? pixelCount : frameCount; // source rows
    const int outW = (maxW > 0) ? juce::jmin(srcW, maxW) : srcW;
    const int outH = (maxH > 0) ? juce::jmin(srcH, maxH) : srcH;
    if (outW <= 0 || outH <= 0)
        return {};

    // Nearest-neighbour striding when downsampled (0 caps = full resolution).
    const auto srcIndex = [](int o, int outN, int srcN) noexcept
    {
        if (outN <= 1) return 0;
        return juce::jlimit(0, srcN - 1,
                            (int) ((long long) o * (srcN - 1) / (outN - 1)));
    };

    juce::Image img(juce::Image::RGB, outW, outH, true);
    {
        juce::Image::BitmapData bmp(img, juce::Image::BitmapData::writeOnly);
        std::lock_guard<std::mutex> lk(slotsMutex_);
        const FrameSlot& slot = slots[slotIndex];
        const int fc = juce::jmin(frameCount, slot.frame_count); // re-check after lock

        // Use setPixelColour() to be platform-independent: the in-memory byte
        // order of juce::Image::RGB (PixelRGB) is platform-dependent (BGR on
        // some targets, RGB on others). Writing raw bytes with a hard-coded
        // order produced incorrect colours (export looked monochrome).
        for (int y = 0; y < outH; ++y)
        {
            for (int x = 0; x < outW; ++x)
            {
                int frameIdx, pixIdx;
                if (timeHorizontal)
                {
                    frameIdx = srcIndex(x, outW, fc);
                    // Flip Y so the top row is the highest pixel index (treble).
                    pixIdx   = srcIndex(outH - 1 - y, outH, pixelCount);
                }
                else
                {
                    pixIdx   = srcIndex(x, outW, pixelCount);
                    frameIdx = srcIndex(y, outH, fc);
                }
                if (frameIdx >= fc) continue;
                const CapturedFrame& fr = slot.frames[frameIdx];
                const int px = juce::jmin(pixelCount, static_cast<int>(fr.pixel_count));
                const int pi = juce::jmin(pixIdx, px - 1);
                if (pi < 0) continue;
                bmp.setPixelColour(x, y, juce::Colour(fr.R[pi], fr.G[pi], fr.B[pi]));
            }
        }
    }
    return img;
}

bool LuxSampler::exportSlotImage(int slotIndex,
                                  const juce::File& file,
                                  bool asPng) const
{
    // Full-resolution, native orientation (X = pixel, Y = frame).
    juce::Image img = renderSlotImage(slotIndex, 0, 0, /*timeHorizontal=*/false);
    if (!img.isValid())
    {
        log_error("FS", "exportSlotImage: slot %d empty or invalid", slotIndex);
        return false;
    }

    // Write to file (overwrite if it exists)
    file.deleteFile();
    juce::FileOutputStream out(file);
    if (out.failedToOpen())
    {
        log_error("FS", "exportSlotImage: cannot open '%s'",
                  file.getFullPathName().toRawUTF8());
        return false;
    }

    bool ok = false;
    if (asPng)
    {
        juce::PNGImageFormat fmt;
        ok = fmt.writeImageToStream(img, out);
    }
    else
    {
        juce::JPEGImageFormat fmt;
        fmt.setQuality(0.9f);
        ok = fmt.writeImageToStream(img, out);
    }

    if (!ok)
    {
        log_error("FS", "exportSlotImage: encoder failed for '%s'",
                  file.getFullPathName().toRawUTF8());
        out.flush();
        file.deleteFile();
        return false;
    }

    log_info("FS", "Exported slot %d image to '%s' (%dx%d, %s)",
             slotIndex, file.getFullPathName().toRawUTF8(),
             img.getWidth(), img.getHeight(), asPng ? "PNG" : "JPEG");
    return true;
}

int LuxSampler::exportAllSlotsImages(const juce::File& destDirectory,
                                       const juce::String& baseName,
                                       bool asPng) const
{
    using namespace LuxSamplerConstants;

    if (!destDirectory.createDirectory().wasOk())
    {
        log_error("FS", "exportAllSlotsImages: cannot create directory '%s'",
                  destDirectory.getFullPathName().toRawUTF8());
        return 0;
    }

    const juce::String ext = asPng ? ".png" : ".jpg";
    int exported = 0;

    for (int s = 0; s < NUM_SLOTS; ++s)
    {
        if (!slotHasContent(s))
            continue;

        // Build safe filename: "<baseName>_slotNN_<label>.<ext>"
        juce::String slotLabel(getSlotLabel(s));
        slotLabel = slotLabel.retainCharacters(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");

        juce::String fname = baseName + "_slot"
                              + juce::String(s).paddedLeft('0', 2);
        if (slotLabel.isNotEmpty())
            fname += "_" + slotLabel;
        fname += ext;

        const juce::File target = destDirectory.getChildFile(fname);
        if (exportSlotImage(s, target, asPng))
            ++exported;
    }

    return exported;
}

bool LuxSampler::loadFromFile(const juce::File& file)

{
    using namespace LuxSamplerConstants;

    juce::FileInputStream in(file);
    if (!in.openedOk())
    {
        log_error("FS", "loadFromFile: cannot open '%s'",
                  file.getFullPathName().toRawUTF8());
        return false;
    }

    // ── File header ──────────────────────────────────────────────────────
    FsmpFileHeader hdr {};
    if (in.read(&hdr, sizeof(hdr)) != sizeof(hdr))
    {
        log_error("FS", "loadFromFile: truncated file header");
        return false;
    }
    if (hdr.magic[0]!='F'||hdr.magic[1]!='S'||hdr.magic[2]!='M'||hdr.magic[3]!='P')
    {
        log_error("FS", "loadFromFile: invalid magic bytes");
        return false;
    }
    if (hdr.format_version > FSMP_VERSION)
    {
        log_error("FS", "loadFromFile: unsupported version %u (max %u)",
                  hdr.format_version, FSMP_VERSION);
        return false;
    }
    const uint32_t expected_crc = crc32_compute(
        reinterpret_cast<const uint8_t*>(&hdr),
        offsetof(FsmpFileHeader, header_crc32));
    if (expected_crc != hdr.header_crc32)
    {
        log_error("FS", "loadFromFile: header CRC mismatch (file=%08X calc=%08X)",
                  hdr.header_crc32, expected_crc);
        return false;
    }

    // NOTE: the header's midi_channel / octave_offset / max_duration are
    // deliberately NOT applied — those live in the APVTS (restored by the
    // host before any session auto-load) and applying stale file values here
    // silently overrode the user's saved settings on every session reload.

    const int numSlotsInFile = static_cast<int>(
        std::min(hdr.num_slots, static_cast<uint32_t>(NUM_SLOTS)));

    // ── Stop any ongoing activity before replacing slot contents ─────────
    // (mirrors loadSlotFromFile). The player dereferences slot.frames WITHOUT
    // slotsMutex_, so freeing a playing slot's buffer below would be a
    // use-after-free. Stop playback of real slots (a running SCORE keeps its
    // own scoreSlot — untouched here), disarm recording, suspend command
    // pickup for the whole load, and wait for the player to release.
    playbackSuspended_.store(true, std::memory_order_release);
    struct SuspendReset
    {
        std::atomic<bool>& f;
        ~SuspendReset() { f.store(false, std::memory_order_release); }
    } suspendReset { playbackSuspended_ };

    activeRecSlot.store(-1, std::memory_order_release);
    {
        const int cp = atomicState.activePlaySlot.load(std::memory_order_acquire);
        if (cp >= 0 && cp < NUM_SLOTS)
        {
            atomicState.stopPlayCmd.store(true, std::memory_order_release);
            atomicState.activePlaySlot.store(-1, std::memory_order_release);
            atomicState.passthroughEnabled.store(true, std::memory_order_release);
        }
    }
    for (int i = 0; i < NUM_SLOTS; ++i)
    {
        atomicState.startRecCmd[i].store(false, std::memory_order_release);
        atomicState.stopRecCmd[i].store(false, std::memory_order_release);
        atomicState.slotState[i].store(static_cast<int>(SlotState::IDLE),
                                       std::memory_order_release);
    }
    waitForPlayerRelease(-1);

    // ── Slot blocks ──────────────────────────────────────────────────────
    for (int s = 0; s < numSlotsInFile; ++s)
    {
        FsmpSlotHeader shdr {};
        if (in.read(&shdr, sizeof(shdr)) != sizeof(shdr)) break;

        const uint32_t calc_crc = crc32_compute(
            reinterpret_cast<const uint8_t*>(&shdr),
            offsetof(FsmpSlotHeader, slot_crc32));
        if (calc_crc != shdr.slot_crc32)
        {
            log_warning("FS", "loadFromFile: slot %d CRC mismatch — skipped", s);
            continue;
        }

        const int idx = static_cast<int>(shdr.slot_index);
        if (idx < 0 || idx >= NUM_SLOTS) continue;

        // Read frame data into a local staging buffer so that slotsMutex_ is
        // held only during the final copy into slots[], not during I/O.
        const int toLoad = static_cast<int>(
            std::min(shdr.frame_count, static_cast<uint32_t>(MAX_FRAMES_PER_SLOT)));

        // Allocate staging buffer on the heap to avoid large stack frames.
        std::vector<CapturedFrame> staging;
        bool loadOk = false;

        if (shdr.has_content && shdr.frame_count > 0)
        {
            staging.resize(static_cast<size_t>(toLoad));
            int loaded = 0;
            for (int f = 0; f < toLoad; ++f)
            {
                FsmpFrameHeader fhdr {};
                if (in.read(&fhdr, sizeof(fhdr)) != sizeof(fhdr)) break;

                CapturedFrame& fr = staging[static_cast<size_t>(f)];
                fr.timestamp_us = fhdr.timestamp_us;

                uint32_t lid = 0; uint16_t pc = 0;
                in.read(&lid, sizeof(lid));
                in.read(&pc,  sizeof(pc));
                fr.line_id     = lid;
                fr.pixel_count = pc;

                const int bytes = std::min(static_cast<int>(pc), MAX_PIXELS);
                in.read(fr.R, bytes);
                in.read(fr.G, bytes);
                in.read(fr.B, bytes);
                ++loaded;
            }
            staging.resize(static_cast<size_t>(loaded));
            loadOk = (loaded > 0);
        }

        // Commit to the slot under slotsMutex_ so sampleSpectralForTimeline
        // (message thread) cannot read a partially-initialised slot.
        {
            std::lock_guard<std::mutex> lk(slotsMutex_);
            slots[idx].clear();
            if (!loadOk) continue;

            slots[idx].allocate();
            slots[idx].has_content = true;
            slots[idx].duration_us = shdr.duration_us;
            std::strncpy(slots[idx].label, shdr.label, 63);
            slots[idx].label[63] = '\0';

            const int count = juce::jmin(static_cast<int>(staging.size()),
                                         slots[idx].capacity);
            std::memcpy(slots[idx].frames.get(), staging.data(),
                        static_cast<size_t>(count) * sizeof(CapturedFrame));
            slots[idx].frame_count = count;
        }
    }

    log_info("FS", "Loaded from '%s' (%d slots)",
             file.getFullPathName().toRawUTF8(), numSlotsInFile);
    return true;
}

// ============================================================================
// Per-slot file I/O — .fslot binary format
//
// Single-slot, self-contained format. Carries one slot's frames PLUS its
// per-slot play parameters (speed, loop mode, fades, etc.) as embedded XML
// so the file is fully portable across sessions.
//
// Layout:
//   [4]  magic       "FSLT" (0x46534C54, little-endian = 0x544C5346)
//   [2]  version     0x0001
//   [2]  flags       reserved = 0
//   [2]  slot_index  source slot index (0..NUM_SLOTS-1) — informational
//   [82] FsmpSlotHeader  (reused from .fsmp format)
//   [N]  frames      N × (FsmpFrameHeader + line_id + pixel_count + R + G + B)
//   [4]  paramsXmlLen
//   [*]  paramsXml   UTF-8 XML <SlotParams .../>
//   [4]  EOF marker  0xDEADBEEF
// ============================================================================

namespace
{
    constexpr uint32_t FSLOT_MAGIC   = 0x46534C54u; // "FSLT"
    constexpr uint16_t FSLOT_VERSION = 0x0001u;
}

// ============================================================================
// Per-slot play-parameter XML — the single serialisation shared by .fslot,
// the .sp3s session and the DAW state blob.
// ============================================================================

void LuxSampler::slotParamsToXml(int slotIndex, juce::XmlElement& xml) const
{
    if (slotIndex < 0 || slotIndex >= LuxSamplerConstants::NUM_SLOTS)
        return;

    xml.setAttribute("idx",            slotIndex);
    xml.setAttribute("startFrac",      static_cast<double>(getSlotStartFrac(slotIndex)));
    xml.setAttribute("endFrac",        static_cast<double>(getSlotEndFrac(slotIndex)));
    xml.setAttribute("speed",          static_cast<double>(getSlotSpeed(slotIndex)));
    xml.setAttribute("loopMode",       static_cast<int>(getSlotLoopMode(slotIndex)));
    xml.setAttribute("resumeMode",     static_cast<int>(getSlotResumeMode(slotIndex)));
    xml.setAttribute("blendAmount",    static_cast<double>(getSlotBlendAmount(slotIndex)));
    xml.setAttribute("attackLen",      static_cast<double>(getSlotAttackLen(slotIndex)));
    xml.setAttribute("decayLen",       static_cast<double>(getSlotDecayLen(slotIndex)));
    xml.setAttribute("brightnessLift", static_cast<double>(getSlotBrightnessLift(slotIndex)));
    xml.setAttribute("mixMode",        static_cast<int>(getSlotMixMode(slotIndex)));
    xml.setAttribute("trebleCut",      static_cast<double>(getSlotTrebleCut(slotIndex)));
    xml.setAttribute("bassCut",        static_cast<double>(getSlotBassCut(slotIndex)));
    // Image EQ (SCORE-style ±dB) → encoded "minF|maxF|g0;g1;…" + pre-EQ floor.
    xml.setAttribute("imageEq",  getSlotEq(slotIndex));
    xml.setAttribute("eqFloor",  static_cast<double>(getSlotEqFloor(slotIndex)));
    xml.setAttribute("fadeCurveType",  static_cast<int>(getSlotFadeCurveType(slotIndex)));
    xml.setAttribute("fadeCurvePower", static_cast<double>(getSlotFadeCurvePower(slotIndex)));
    // Independent attack / decay fade shaping.
    xml.setAttribute("attackCurveType",  static_cast<int>(getSlotAttackCurveType(slotIndex)));
    xml.setAttribute("attackCurvePower", static_cast<double>(getSlotAttackCurvePower(slotIndex)));
    xml.setAttribute("decayCurveType",   static_cast<int>(getSlotDecayCurveType(slotIndex)));
    xml.setAttribute("decayCurvePower",  static_cast<double>(getSlotDecayCurvePower(slotIndex)));
    xml.setAttribute("label",          juce::String(getSlotLabel(slotIndex)));
}

void LuxSampler::slotParamsFromXml(int slotIndex, const juce::XmlElement& xml)
{
    if (slotIndex < 0 || slotIndex >= LuxSamplerConstants::NUM_SLOTS)
        return;

    setSlotStartFrac     (slotIndex, static_cast<float>(xml.getDoubleAttribute("startFrac",      0.0)));
    setSlotEndFrac       (slotIndex, static_cast<float>(xml.getDoubleAttribute("endFrac",        1.0)));
    setSlotSpeed         (slotIndex, static_cast<float>(xml.getDoubleAttribute("speed",          1.0)));
    setSlotLoopMode      (slotIndex, static_cast<LoopMode>(xml.getIntAttribute("loopMode",       1)));
    setSlotResumeMode    (slotIndex, xml.getIntAttribute("resumeMode", 0) != 0);
    setSlotBlendAmount   (slotIndex, static_cast<float>(xml.getDoubleAttribute("blendAmount",    0.0)));
    setSlotAttackLen     (slotIndex, static_cast<float>(xml.getDoubleAttribute("attackLen",      0.0)));
    setSlotDecayLen      (slotIndex, static_cast<float>(xml.getDoubleAttribute("decayLen",       0.0)));
    setSlotBrightnessLift(slotIndex, static_cast<float>(xml.getDoubleAttribute("brightnessLift", 0.0)));
    // Bank mix mode (multi-bank mixer). Absent in legacy files → DARKEN, which
    // is output-identical for a single playing bank (material union).
    setSlotMixMode       (slotIndex, static_cast<SlotMixMode>(xml.getIntAttribute(
                              "mixMode", static_cast<int>(SlotMixMode::DARKEN))));
    setSlotTrebleCut     (slotIndex, static_cast<float>(xml.getDoubleAttribute("trebleCut",      0.0)));
    setSlotBassCut       (slotIndex, static_cast<float>(xml.getDoubleAttribute("bassCut",        0.0)));
    // Legacy shared fade curve first (writes both attack+decay), then per-fade
    // overrides when present (newer sessions).
    setSlotFadeCurveType (slotIndex, static_cast<FadeCurveType>(xml.getIntAttribute("fadeCurveType", 0)));
    setSlotFadeCurvePower(slotIndex, static_cast<float>(xml.getDoubleAttribute("fadeCurvePower", 1.0)));
    if (xml.hasAttribute("attackCurveType"))
        setSlotAttackCurveType(slotIndex, static_cast<FadeCurveType>(xml.getIntAttribute("attackCurveType", 0)));
    if (xml.hasAttribute("attackCurvePower"))
        setSlotAttackCurvePower(slotIndex, static_cast<float>(xml.getDoubleAttribute("attackCurvePower", 1.0)));
    if (xml.hasAttribute("decayCurveType"))
        setSlotDecayCurveType(slotIndex, static_cast<FadeCurveType>(xml.getIntAttribute("decayCurveType", 0)));
    if (xml.hasAttribute("decayCurvePower"))
        setSlotDecayCurvePower(slotIndex, static_cast<float>(xml.getDoubleAttribute("decayCurvePower", 1.0)));
    // Legacy "loopOverlap" (Loop XF, feature removed) is intentionally ignored.
    // Image EQ (SCORE-style ±dB). Legacy freqCurveLF/HF / trebleCut / bassCut are
    // no longer restored (the EQ was redesigned); a flat EQ is the safe default.
    setSlotEq(slotIndex, xml.getStringAttribute("imageEq", ""));
    setSlotEqFloor(slotIndex, static_cast<float>(xml.getDoubleAttribute("eqFloor", 0.0)));
    // Apply the label whenever the attribute is PRESENT — an empty value is a
    // deliberate clear and must round-trip (slotParamsToXml always writes it).
    // Only a truly absent attribute (legacy file) keeps the header's label.
    if (xml.hasAttribute("label"))
        setSlotLabel(slotIndex,
                     xml.getStringAttribute("label", "").toRawUTF8());
}

bool LuxSampler::saveSlotToFile(int slotIndex, const juce::File& file) const
{
    using namespace LuxSamplerConstants;

    if (slotIndex < 0 || slotIndex >= NUM_SLOTS)
    {
        log_error("FS", "saveSlotToFile: invalid slot index %d", slotIndex);
        return false;
    }

    // Reject empty slots — nothing to save.
    if (!slots[slotIndex].has_content || slots[slotIndex].frame_count <= 0)
    {
        log_warning("FS", "saveSlotToFile: slot %d is empty", slotIndex);
        return false;
    }

    file.deleteFile();
    juce::FileOutputStream out(file);
    if (!out.openedOk())
    {
        log_error("FS", "saveSlotToFile: cannot open '%s'",
                  file.getFullPathName().toRawUTF8());
        return false;
    }
    out.setPosition(0);
    out.truncate();

    // ── File header (10 bytes) ───────────────────────────────────────────────
    const uint32_t magic    = FSLOT_MAGIC;
    const uint16_t version  = FSLOT_VERSION;
    const uint16_t flags    = 0;
    const uint16_t slotIdx  = static_cast<uint16_t>(slotIndex);
    out.write(&magic,   sizeof(magic));
    out.write(&version, sizeof(version));
    out.write(&flags,   sizeof(flags));
    out.write(&slotIdx, sizeof(slotIdx));

    // ── Slot header + frames ─────────────────────────────────────────────────
    // Chunked like saveToFile(): slotsMutex_ is shared with the UDP thread's
    // per-frame recording path — never hold it across disk I/O.
    {
        constexpr int kChunkFrames = 256;   // ≈ 2.6 MB staging
        // Freeze rec-command drains for the whole chunked copy (see saveInProgress_).
        saveInProgress_.store(true, std::memory_order_release);
        struct SaveFlagReset
        {
            std::atomic<bool>& f;
            ~SaveFlagReset() { f.store(false, std::memory_order_release); }
        } saveFlagReset { saveInProgress_ };   // mutable member — writable from const
        FsmpSlotHeader shdr {};
        int fc = 0;
        {
            std::lock_guard<std::mutex> lk(slotsMutex_);
            const FrameSlot& slot = slots[slotIndex];
            shdr.slot_index  = static_cast<uint8_t>(slotIndex);
            shdr.has_content = slot.has_content ? 0x01u : 0x00u;
            shdr.frame_count = static_cast<uint32_t>(slot.frame_count);
            shdr.duration_us = slot.duration_us;
            std::strncpy(shdr.label, slot.label, 63);
            shdr.label[63]   = '\0';
            if (slot.has_content && slot.frame_count > 0 && slot.isAllocated())
                fc = slot.frame_count;
        }
        shdr.slot_crc32 = crc32_compute(reinterpret_cast<const uint8_t*>(&shdr),
                                         offsetof(FsmpSlotHeader, slot_crc32));
        out.write(&shdr, sizeof(shdr));

        std::vector<CapturedFrame> staging;
        for (int base = 0; base < fc; base += kChunkFrames)
        {
            const int n = std::min(kChunkFrames, fc - base);
            staging.resize(static_cast<size_t>(n));
            {
                std::lock_guard<std::mutex> lk(slotsMutex_);
                std::memcpy(staging.data(), slots[slotIndex].frames.get() + base,
                            static_cast<size_t>(n) * sizeof(CapturedFrame));
            }
            for (int f = 0; f < n; ++f)
            {
                const CapturedFrame& fr = staging[static_cast<size_t>(f)];
                const uint32_t psize    = sizeof(fr.line_id) + sizeof(fr.pixel_count)
                                          + 3u * static_cast<uint32_t>(fr.pixel_count);
                FsmpFrameHeader fhdr { fr.timestamp_us, psize };
                out.write(&fhdr,           sizeof(fhdr));
                out.write(&fr.line_id,     sizeof(fr.line_id));
                out.write(&fr.pixel_count, sizeof(fr.pixel_count));
                out.write(fr.R, fr.pixel_count);
                out.write(fr.G, fr.pixel_count);
                out.write(fr.B, fr.pixel_count);
            }
        }
    }

    // ── Per-slot play parameters as embedded XML ─────────────────────────────
    juce::XmlElement paramsXml("SlotParams");
    slotParamsToXml(slotIndex, paramsXml);

    const juce::String xmlStr = paramsXml.toString();
    const uint32_t xmlLen     = static_cast<uint32_t>(xmlStr.getNumBytesAsUTF8());
    out.write(&xmlLen, sizeof(xmlLen));
    if (xmlLen > 0)
        out.write(xmlStr.toRawUTF8(), xmlLen);

    // ── EOF marker ───────────────────────────────────────────────────────────
    const uint32_t eof = FSMP_EOF_MARKER;
    out.write(&eof, sizeof(eof));

    if (!out.getStatus().wasOk())
    {
        log_error("FS", "saveSlotToFile: write error for '%s'",
                  file.getFullPathName().toRawUTF8());
        return false;
    }

    log_info("FS", "Saved slot %d to '%s'",
             slotIndex, file.getFullPathName().toRawUTF8());
    return true;
}

bool LuxSampler::loadSlotFromFile(int slotIndex, const juce::File& file)
{
    using namespace LuxSamplerConstants;

    if (slotIndex < 0 || slotIndex >= NUM_SLOTS)
    {
        log_error("FS", "loadSlotFromFile: invalid slot index %d", slotIndex);
        return false;
    }

    juce::FileInputStream in(file);
    if (!in.openedOk())
    {
        log_error("FS", "loadSlotFromFile: cannot open '%s'",
                  file.getFullPathName().toRawUTF8());
        return false;
    }

    // ── File header (10 bytes) ───────────────────────────────────────────────
    uint32_t magic   = 0;
    uint16_t version = 0;
    uint16_t flags   = 0;
    uint16_t fileSlotIdx = 0;
    if (in.read(&magic,       sizeof(magic))       != sizeof(magic)       ||
        in.read(&version,     sizeof(version))     != sizeof(version)     ||
        in.read(&flags,       sizeof(flags))       != sizeof(flags)       ||
        in.read(&fileSlotIdx, sizeof(fileSlotIdx)) != sizeof(fileSlotIdx))
    {
        log_error("FS", "loadSlotFromFile: truncated file header");
        return false;
    }
    if (magic != FSLOT_MAGIC)
    {
        log_error("FS", "loadSlotFromFile: invalid magic bytes (%08X)", magic);
        return false;
    }
    if (version > FSLOT_VERSION)
    {
        log_error("FS", "loadSlotFromFile: unsupported version %u", version);
        return false;
    }
    juce::ignoreUnused(flags, fileSlotIdx);

    // ── Slot header ──────────────────────────────────────────────────────────
    FsmpSlotHeader shdr {};
    if (in.read(&shdr, sizeof(shdr)) != sizeof(shdr))
    {
        log_error("FS", "loadSlotFromFile: truncated slot header");
        return false;
    }
    const uint32_t calc_crc = crc32_compute(
        reinterpret_cast<const uint8_t*>(&shdr),
        offsetof(FsmpSlotHeader, slot_crc32));
    if (calc_crc != shdr.slot_crc32)
    {
        log_error("FS", "loadSlotFromFile: slot CRC mismatch (file=%08X calc=%08X)",
                  shdr.slot_crc32, calc_crc);
        return false;
    }

    // ── Frame data into a staging buffer (no lock during I/O) ────────────────
    const int toLoad = static_cast<int>(
        std::min(shdr.frame_count, static_cast<uint32_t>(MAX_FRAMES_PER_SLOT)));

    std::vector<CapturedFrame> staging;
    bool hasFrames = false;
    if (shdr.has_content && shdr.frame_count > 0)
    {
        staging.resize(static_cast<size_t>(toLoad));
        int loaded = 0;
        for (int f = 0; f < toLoad; ++f)
        {
            FsmpFrameHeader fhdr {};
            if (in.read(&fhdr, sizeof(fhdr)) != sizeof(fhdr)) break;

            CapturedFrame& fr = staging[static_cast<size_t>(f)];
            fr.timestamp_us = fhdr.timestamp_us;

            uint32_t lid = 0; uint16_t pc = 0;
            in.read(&lid, sizeof(lid));
            in.read(&pc,  sizeof(pc));
            fr.line_id     = lid;
            fr.pixel_count = pc;

            const int bytes = std::min(static_cast<int>(pc), MAX_PIXELS);
            in.read(fr.R, bytes);
            in.read(fr.G, bytes);
            in.read(fr.B, bytes);
            ++loaded;
        }
        staging.resize(static_cast<size_t>(loaded));
        hasFrames = (loaded > 0);
    }

    // ── Embedded params XML (optional — older files may not have it) ─────────
    juce::String paramsXmlStr;
    uint32_t     xmlLen = 0;
    if (in.read(&xmlLen, sizeof(xmlLen)) == sizeof(xmlLen)
        && xmlLen > 0 && xmlLen < 1u * 1024u * 1024u)
    {
        juce::MemoryBlock xmlBlock;
        xmlBlock.setSize(static_cast<size_t>(xmlLen));
        if (in.read(xmlBlock.getData(), static_cast<int>(xmlLen))
            == static_cast<int>(xmlLen))
        {
            paramsXmlStr = juce::String(
                static_cast<const char*>(xmlBlock.getData()),
                static_cast<size_t>(xmlLen));
        }
    }

    // ── Stop any ongoing activity on the destination slot ────────────────────
    if (activeRecSlot.load() == slotIndex) activeRecSlot.store(-1);
    if (atomicState.activePlaySlot.load(std::memory_order_acquire) == slotIndex)
    {
        atomicState.stopPlayCmd.store(true);
        atomicState.activePlaySlot.store(-1);
        atomicState.passthroughEnabled.store(true);
    }
    atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::IDLE));

    // The stop above is asynchronous — wait until the player has released this
    // slot before clear()/allocate() below free or replace its frame buffer.
    waitForPlayerRelease(slotIndex);

    // ── Commit frames under slotsMutex_ ──────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(slotsMutex_);
        slots[slotIndex].clear();

        if (hasFrames)
        {
            slots[slotIndex].allocate();
            slots[slotIndex].has_content = true;
            slots[slotIndex].duration_us = shdr.duration_us;
            std::strncpy(slots[slotIndex].label, shdr.label, 63);
            slots[slotIndex].label[63] = '\0';

            const int count = juce::jmin(static_cast<int>(staging.size()),
                                         slots[slotIndex].capacity);
            std::memcpy(slots[slotIndex].frames.get(), staging.data(),
                        static_cast<size_t>(count) * sizeof(CapturedFrame));
            slots[slotIndex].frame_count = count;
        }
    }

    // ── Apply embedded params (if present) ───────────────────────────────────
    if (paramsXmlStr.isNotEmpty())
    {
        if (auto xml = juce::parseXML(paramsXmlStr))
        {
            if (xml->getTagName() == "SlotParams")
                slotParamsFromXml(slotIndex, *xml);
        }
    }

    log_info("FS", "Loaded slot %d from '%s' (%d frames)",
             slotIndex, file.getFullPathName().toRawUTF8(),
             static_cast<int>(staging.size()));
    return true;
}

// ============================================================================
// FramePlayerThread
// ============================================================================

FramePlayerThread::FramePlayerThread(LuxSampler& sampler,
                                     AudioImageBuffers* audioBuffers,
                                     DoubleBuffer*      doubleBuffer)
    : juce::Thread("Sp3ctraFramePlayer"),
      sampler(sampler),
      audioBuffers(audioBuffers),
      doubleBuffer(doubleBuffer)
{}

FramePlayerThread::~FramePlayerThread()
{
    stopThread(2000);
}

uint64_t FramePlayerThread::currentTimeUs() noexcept
{
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(tv.tv_usec);
}

// ============================================================================
// FramePlayerThread::injectWhiteFrame — inject silence into all output paths
// Non-RT only (FramePlayerThread context).
//
// Writes a full-white (255) frame to:
//   1. AudioImageBuffers sampler snapshot (sampler_R/G/B)
//   2. AudioImageBuffers main read/write buffer (visual mix bus)
//   3. DoubleBuffer.preprocessed_data (audio synthesis, Source=S only)
//
// Called when playback stops to prevent the last sampler frame from being
// heard/seen as a frozen artefact (STEP_EMPTY, rtStop, LoopMode::NONE end).
// ============================================================================
void FramePlayerThread::injectWhiteFrame() noexcept
{
    if (audioBuffers == nullptr) return;

    // White = 255 on all channels = silence in Sp3ctra's image-to-sound mapping.
    uint8_t whiteR[LuxSamplerConstants::MAX_PIXELS];
    uint8_t whiteG[LuxSamplerConstants::MAX_PIXELS];
    uint8_t whiteB[LuxSamplerConstants::MAX_PIXELS];
    std::memset(whiteR, 255, sizeof(whiteR));
    std::memset(whiteG, 255, sizeof(whiteG));
    std::memset(whiteB, 255, sizeof(whiteB));
    const int nbPx = LuxSamplerConstants::MAX_PIXELS;

    // 1. Clear sampler snapshot so the visualizer shows white immediately.
    audio_image_buffers_snapshot_sampler(audioBuffers, whiteR, whiteG, whiteB, nbPx);

    // 2. Clear the main AudioImageBuffers (visual mix bus / LuxStral source).
    {
        uint8_t* wR = nullptr;
        uint8_t* wG = nullptr;
        uint8_t* wB = nullptr;
        if (audio_image_buffers_start_write(audioBuffers, &wR, &wG, &wB) == 0)
        {
            std::memset(wR, 255, static_cast<size_t>(nbPx));
            std::memset(wG, 255, static_cast<size_t>(nbPx));
            std::memset(wB, 255, static_cast<size_t>(nbPx));
            audio_image_buffers_complete_write(audioBuffers);
        }
    }

    // 3. Silence ONLY the LuxStral additive section of preprocessed_data.
    //
    // CRITICAL — do NOT use pipeline_process_frame(white) here:
    //   pipeline_process_frame() fills ALL sections of PreprocessedImageData,
    //   including polyphonic.grayscale (LuxSynth) with white = 1.0 (max signal).
    //   Because the UDP thread's silence branch (multithreading.c, src=0 /
    //   not playing) only zeroes additive.* and never restores polyphonic.*,
    //   polyphonic.grayscale would stay frozen at 1.0 permanently, causing
    //   LuxSynth (Source=L) to produce sound from a flat-white spectrum instead
    //   of the live signal — the "LuxSynth gray freeze" regression.
    //
    // Instead, mirror exactly the UDP thread's silence injection:
    //   zero additive.{grayscale, notes, contrast_factor} only.
    //   polyphonic (LuxSynth), photowave (LuxWave) and stereo sections are
    //   owned by their respective source-routing paths and must not be touched
    //   here.
    //
    // This covers Source=S (LuxStral on Sampler).  Source=L and Source=M are
    // handled exclusively by the UDP thread (preprocessed_data = preprocessed_temp,
    // dataReady = 1) and do not require any action from injectWhiteFrame().
    // Synth-split P3: with "→ LUXSTRAL"/"→ LUXSYNTH"/"→ LUXWAVE" OUTs the
    // audio-thread mixer owns the synth feeds and blends the STAGED sends —
    // which have no freshness timeout. Deactivate the stagings of every chain
    // this player relayed, or the last staged column keeps ringing forever
    // (sourceless score/sampler chains have no producer to replace it).
    chain_player_stagings_set_inactive(sampler.getEngineIndex());

    if (doubleBuffer != nullptr)
    {
        // M7 — plan-driven gates: the additive/pathB sections may only be
        // silenced here when the player actually relays those paths. The
        // white frame runs when THIS engine's player goes idle — silence the
        // paths this engine's chain (or the score relay it hosts) fed.
        const int  engineIdx = sampler.getEngineIndex();
        const bool addOwned  =
            chain_additive_player_candidate(0, engineIdx) != 0
            || chain_additive_player_candidate(1, engineIdx) != 0;
        const bool pbOwned   =
            chain_pathb_player_candidate(0, engineIdx) != 0
            || chain_pathb_player_candidate(1, engineIdx) != 0;
        if (addOwned)
        {
            pthread_mutex_lock(&doubleBuffer->mutex);
            // Zero additive synthesis input — identical to multithreading.c silence branch.
            std::memset(doubleBuffer->preprocessed_data.additive.grayscale, 0,
                        sizeof(doubleBuffer->preprocessed_data.additive.grayscale));
            std::memset(doubleBuffer->preprocessed_data.additive.notes, 0,
                        sizeof(doubleBuffer->preprocessed_data.additive.notes));
            doubleBuffer->preprocessed_data.additive.contrast_factor = 0.0f;
            // FIX(silence): Also zero polyphonic.* when its chain is
            // sampler-relayed. Without this, LuxSynth keeps generating audio
            // from the last played frame during STEP_EMPTY / rtStop / end.
            if (pbOwned)
            {
                std::memset(doubleBuffer->preprocessed_data.polyphonic.grayscale, 0,
                            sizeof(doubleBuffer->preprocessed_data.polyphonic.grayscale));
                std::memset(doubleBuffer->preprocessed_data.polyphonic.magnitudes, 0,
                            sizeof(doubleBuffer->preprocessed_data.polyphonic.magnitudes));
                doubleBuffer->preprocessed_data.polyphonic.valid = 0;
            }
            doubleBuffer->dataReady = 1;
            pthread_mutex_unlock(&doubleBuffer->mutex);

            // Per-engine input taps (per-chain display): mirror the silence
            // injection above so the head panels show "unfed" (white) instead
            // of the last playback frame.
            audio_image_buffers_publish_engine_input(
                audioBuffers, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL_A,
                nullptr, nullptr, nullptr, nbPx);
            if (pbOwned)
                audio_image_buffers_publish_engine_input(
                    audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
                    nullptr, nullptr, nullptr, nbPx);
        }
    }
}

// ============================================================================
// FramePlayerThread — multi-voice playback (multi-bank simultaneous play)
//
// Architecture (2026-07-13 multi-bank mixer):
//   run()               outer loop — waits for a startPlayCmd wake-up, then
//                       dispatches to a SCORE session or a sampler session.
//   runSamplerSession() the voice set FOLLOWS slotState[]: every slot whose
//                       state is PLAYING is a voice; each tick every voice is
//                       advanced (tickVoice) and composited into ONE master
//                       frame via its bank mixer (level = 1 − brightnessLift,
//                       per-bank SlotMixMode), then injected (outputFrame).
//   runScoreSession()   single-voice session on the dedicated score slot —
//                       same tick/output path, no bank mixer.
// ============================================================================

// ── SCORE relay ──────────────────────────────────────────────────────────────
// When SCORE relinquishes the shared channel, hand it back to the sampler slot
// it overrode (armed by uiPlayScore()/uiBeginScoreScrub()) so the sampler
// stream resumes underneath instead of going silent. The slot replays under
// its own Resume mode. Multi-bank limitation: only the PRIMARY voice (lowest
// slot, mirrored in activePlaySlot) is relayed — additional voices that were
// playing when SCORE took over do not resume automatically.
// Returns true when a slot was re-armed (caller must then NOT restore live
// passthrough).
bool FramePlayerThread::resumeScoreRelaySlot()
{
    auto& state = sampler.getAtomicState();

    // Encoded engine * NUM_SLOTS + slot (see uiPlayScore) — the channel
    // may have been owned by ANOTHER engine (sampler B under SCORE on A).
    const int enc = sampler.consumeScoreRelaySlot();
    if (enc < 0) return false;
    const int engIdx = enc / LuxSamplerConstants::NUM_SLOTS;
    const int relay  = enc % LuxSamplerConstants::NUM_SLOTS;
    // Pin the target engine: this runs on A's player thread while the
    // processor may be destroying engine B — an unpinned dereference
    // could race the dtor's CAS-unregister (same protocol as the C hooks).
    LuxSampler* target = LuxSampler::pinEngine(engIdx);
    struct RelayPinReset
    {
        int idx;
        ~RelayPinReset() { LuxSampler::unpinEngine(idx); }
    } pinReset { engIdx };
    if (target == nullptr) return false;
    if (!target->getSlot(relay).has_content) return false;

    auto& ts = target->getAtomicState();
    ts.slotState[relay].store(static_cast<int>(SlotState::PLAYING),
                              std::memory_order_release);
    ts.activePlaySlot.store(relay, std::memory_order_release);
    ts.stopPlayCmd.store(false, std::memory_order_release);   // clear stale stop
    ts.startPlayCmd.store(relay, std::memory_order_release);
    ts.passthroughEnabled.store(false, std::memory_order_release);

    if (&ts != &state)
    {
        // Cross-engine resume: release OUR channel bookkeeping — the score
        // sentinel must not linger in activePlaySlot (it would poison the
        // next arbiter pass), and our passthrough flag must not keep the
        // live feed suppressed (the target engine owns that now).
        if (state.activePlaySlot.load(std::memory_order_relaxed)
            == LuxSamplerConstants::SCORE_SLOT)
            state.activePlaySlot.store(-1, std::memory_order_release);
        state.passthroughEnabled.store(true, std::memory_order_release);
    }
    log_info("FS", "SCORE stopped — relaying back to sampler %d slot %d",
             engIdx, relay);
    return true;
}

// ── tickVoice ────────────────────────────────────────────────────────────────
// Advance ONE voice by one 1 ms tick and render its processed frame into
// outR/G/B: on-the-fly param re-read, loop/direction/range handling,
// attack/decay fades, pre-EQ floor and image EQ. The bank mixer
// (level + mix mode) is NOT applied here — the caller composites.
// Returns false when the voice ended this tick (LoopMode::NONE reached its
// boundary); the overshot play head is left unclamped so a Resume-mode restart
// falls back to the region start (historical behaviour).
bool FramePlayerThread::tickVoice(VoiceCtx& v, FrameSlot& slot, bool isScore,
                                  uint8_t* outR, uint8_t* outG, uint8_t* outB,
                                  int& outNb)
{
    auto& state = sampler.getAtomicState();
    const int slotIdx = v.slot;
    outNb = 0;

    // ── Re-read play params every tick (on-the-fly) ──────────────────────────
    const float    p_start = sampler.getSlotStartFrac(slotIdx);
    const float    p_end   = sampler.getSlotEndFrac(slotIdx);
    const float    p_speed = juce::jlimit(0.01f, 32.0f,
                                 sampler.getSlotSpeed(slotIdx));
    const LoopMode p_loop  = sampler.getSlotLoopMode(slotIdx);

    const int startFrame = juce::jlimit(0, slot.frame_count - 1,
        static_cast<int>(p_start * static_cast<float>(slot.frame_count)));
    const int endFrame   = juce::jlimit(startFrame + 1, slot.frame_count,
        static_cast<int>(p_end * static_cast<float>(slot.frame_count)));
    const int zoneLen    = endFrame - startFrame;

    // ── Loop-mode change → update direction ──────────────────────────────────
    if (p_loop != v.prevLoopMode)
    {
        v.prevLoopMode = p_loop;
        switch (p_loop)
        {
            case LoopMode::LOOP:
            case LoopMode::NONE:
                v.direction = 1;
                break;
            case LoopMode::INVERSE:
                v.direction = -1;
                break;
            case LoopMode::PINGPONG:
                break; // keep current direction
        }
    }

    // ── Range change → clamp play_head, reset accumulator ────────────────────
    if (startFrame != v.prevStartFrame || endFrame != v.prevEndFrame)
    {
        const bool wasFirst = v.firstRangeInit;
        v.firstRangeInit = false;
        v.prevStartFrame = startFrame;
        v.prevEndFrame   = endFrame;

        if (wasFirst && sampler.getSlotResumeMode(slotIdx))
        {
            const int saved = sampler.getLastPlayHead(slotIdx);
            if (saved >= startFrame && saved < endFrame)
            {
                slot.play_head = saved;
                if (v.prevLoopMode == LoopMode::PINGPONG)
                    v.direction = sampler.getLastDirection(slotIdx);
            }
            else
            {
                slot.play_head = (v.direction > 0) ? startFrame : endFrame - 1;
            }
        }
        else if (slot.play_head < startFrame || slot.play_head >= endFrame)
        {
            slot.play_head = (v.direction > 0) ? startFrame : endFrame - 1;
        }

        v.frameAcc = 0.0f;
    }

    // ── Manual scrub: UI dragged the score play head elsewhere ───────────────
    if (isScore)
    {
        const int seek = state.scoreSeekHead.exchange(-1, std::memory_order_acq_rel);
        if (seek >= 0)
        {
            slot.play_head = juce::jlimit(0, slot.frame_count - 1, seek);
            v.frameAcc     = 0.0f;
        }
    }

    // ── Advance play_head by fractional speed per tick ───────────────────────
    // Integer step: 0 is allowed for speed<1 (repeats current frame).
    // Scrub-audition (held position): NEVER auto-advance — the play head moves
    // only via the seek above, so the column under the cursor is re-injected
    // unchanged every tick → a sustained tone.
    const bool scrubHold = isScore
        && state.scoreScrubbing.load(std::memory_order_relaxed);
    if (!scrubHold)
    {
        v.frameAcc    += p_speed;
        const int step = static_cast<int>(v.frameAcc);
        v.frameAcc    -= static_cast<float>(step);

        if (step > 0 && zoneLen > 0)
        {
            slot.play_head += step * v.direction;

            // ── Boundary / loop-mode handling ─────────────────────────────
            const bool fwdBound = (v.direction > 0 && slot.play_head >= endFrame);
            const bool bwdBound = (v.direction < 0 && slot.play_head <  startFrame);
            if (fwdBound || bwdBound)
            {
                switch (p_loop)
                {
                    case LoopMode::NONE:
                        // Voice ended — leave the overshot head unclamped so a
                        // Resume-mode restart falls back to the region start.
                        return false;
                    case LoopMode::LOOP:
                        v.direction    = 1;
                        // Wrap within zone, accounting for possible overshoot
                        slot.play_head = startFrame
                            + ((slot.play_head - startFrame) % zoneLen
                               + zoneLen) % zoneLen;
                        v.frameAcc = 0.0f;
                        log_debug("FS", "Slot %d: loop", slotIdx);
                        break;
                    case LoopMode::INVERSE:
                        v.direction    = -1;
                        slot.play_head = (endFrame - 1)
                            - (((endFrame - 1) - slot.play_head) % zoneLen
                               + zoneLen) % zoneLen;
                        v.frameAcc = 0.0f;
                        break;
                    case LoopMode::PINGPONG:
                        v.direction    = -v.direction;
                        slot.play_head = juce::jlimit(startFrame,
                                                      endFrame - 1,
                                                      slot.play_head);
                        v.frameAcc = 0.0f;
                        break;
                }
            }
        }
    }

    // Safety clamp — prevents out-of-bounds access
    slot.play_head = juce::jlimit(startFrame, endFrame - 1, slot.play_head);

    const CapturedFrame& frame = slot.frames[slot.play_head];

    // ── Working buffers — fades/EQ applied before the caller's composite ─────
    // Zero-filled so pixels beyond pixel_count carry no stale data (the caller
    // never composites beyond outNb).
    const int nb = std::min(static_cast<int>(frame.pixel_count),
                            LuxSamplerConstants::MAX_PIXELS);
    std::memset(outR, 0, LuxSamplerConstants::MAX_PIXELS);
    std::memset(outG, 0, LuxSamplerConstants::MAX_PIXELS);
    std::memset(outB, 0, LuxSamplerConstants::MAX_PIXELS);

    std::memcpy(outR, frame.R, static_cast<size_t>(nb));
    std::memcpy(outG, frame.G, static_cast<size_t>(nb));
    std::memcpy(outB, frame.B, static_cast<size_t>(nb));

    // ── Read per-fade curve params once per frame ────────────────────────────
    const auto   p_atkCurveType  = sampler.getSlotAttackCurveType(slotIdx);
    const float  p_atkCurvePower = sampler.getSlotAttackCurvePower(slotIdx);
    const auto   p_decCurveType  = sampler.getSlotDecayCurveType(slotIdx);
    const float  p_decCurvePower = sampler.getSlotDecayCurvePower(slotIdx);

    // ── Attack fade-in (exposure ramp): white at start → normal ──────────────
    // attackLen is normalised over [startFrame, endFrame).
    // At headOffset=0 ramp=1 (fully white/silent); at attackLen ramp=0.
    {
        const float p_attack = sampler.getSlotAttackLen(slotIdx);
        if (p_attack > 0.001f)
        {
            const int   totalFrames  = endFrame - startFrame;
            const float attackFrames = p_attack * static_cast<float>(totalFrames);
            const int   headOffset   = (v.direction > 0)
                ? (slot.play_head - startFrame)
                : (endFrame - 1 - slot.play_head);
            if (attackFrames > 0.5f &&
                headOffset < static_cast<int>(attackFrames + 0.5f))
            {
                // t: 0 at start bound → 1 at end of attack zone
                const float t = static_cast<float>(headOffset) / attackFrames;
                // Curve-shaped ramp: 1 (white) at t=0, 0 (normal) at t=1
                const float ramp = 1.0f - applyFadeCurve(t, p_atkCurveType, p_atkCurvePower);
                for (int px = 0; px < nb; ++px)
                {
                    outR[px] = static_cast<uint8_t>(
                        outR[px] + ramp * (255.0f - (float)outR[px]));
                    outG[px] = static_cast<uint8_t>(
                        outG[px] + ramp * (255.0f - (float)outG[px]));
                    outB[px] = static_cast<uint8_t>(
                        outB[px] + ramp * (255.0f - (float)outB[px]));
                }
            }
        }
    }

    // ── Decay fade-out (exposure ramp): normal → white at end ────────────────
    // Mirrors attack but measured from the end bound.
    // At tailOffset=0 (end bound) ramp=1 (white); at decayLen ramp=0 (normal).
    {
        const float p_decay = sampler.getSlotDecayLen(slotIdx);
        if (p_decay > 0.001f)
        {
            const int   totalFrames = endFrame - startFrame;
            const float decayFrames = p_decay * static_cast<float>(totalFrames);
            // tailOffset: distance from the active end bound (direction-aware)
            const int   tailOffset  = (v.direction > 0)
                ? (endFrame - 1 - slot.play_head)
                : (slot.play_head - startFrame);
            if (decayFrames > 0.5f &&
                tailOffset < static_cast<int>(decayFrames + 0.5f))
            {
                // t: 0 at end bound → 1 at start of decay zone
                const float t = static_cast<float>(tailOffset) / decayFrames;
                // Curve-shaped ramp: 1 (white) at t=0, 0 (normal) at t=1
                const float ramp = 1.0f - applyFadeCurve(t, p_decCurveType, p_decCurvePower);
                for (int px = 0; px < nb; ++px)
                {
                    outR[px] = static_cast<uint8_t>(
                        outR[px] + ramp * (255.0f - (float)outR[px]));
                    outG[px] = static_cast<uint8_t>(
                        outG[px] + ramp * (255.0f - (float)outG[px]));
                    outB[px] = static_cast<uint8_t>(
                        outB[px] + ramp * (255.0f - (float)outB[px]));
                }
            }
        }
    }

    // (The former per-slot brightness-lift stage moved into the bank mixer:
    //  the caller pre-fades this frame by level = 1 − brightnessLift when
    //  compositing — see compositeVoice.)

    // ── Pre-EQ material floor ────────────────────────────────────────────────
    // Remove everything below a darkness threshold (push to white) BEFORE
    // the EQ, so a boost cannot resurrect the near-white noise floor into
    // black bands. floor=1 → total white mask (silence). Per channel.
    {
        const float p_floor = sampler.getSlotEqFloor(slotIdx);
        if (p_floor > 0.001f)
        {
            const float thr = p_floor * 255.0f; // darkness threshold in 0..255
            for (int px = 0; px < nb; ++px)
            {
                if ((255.0f - (float) outR[px]) < thr) outR[px] = 255;
                if ((255.0f - (float) outG[px]) < thr) outG[px] = 255;
                if ((255.0f - (float) outB[px]) < thr) outB[px] = 255;
            }
        }
    }

    // ── Image EQ (SCORE-style ±dB, boost + cut) ──────────────────────────────
    // A per-slot graphic EQ over the pixel/frequency axis (left = bass/low,
    // right = treble/high). The LUT holds a GAIN IN dB per normalised
    // position; we convert it to a darkness shift (dShift = gain/dynRange)
    // and apply it exactly like SCORE: darker = louder (boost), whiter =
    // quieter (cut); silence (255) stays silent under boost.
    if (sampler.isFreqCurveActive(slotIdx))
    {
        const int    a   = sampler.getFreqLutActive(slotIdx);
        const float* lut = sampler.getFreqLut(slotIdx, a);
        if (lut != nullptr)
        {
            const float scale = static_cast<float>(LuxSamplerConstants::FREQ_LUT_N - 1);
            const float range = LuxSamplerConstants::EQ_DYN_RANGE_DB;
            const auto  shift = [](uint8_t vpx, float dShift) -> uint8_t
            {
                if (vpx >= 255 && dShift > 0.0f) return 255; // silence stays silent
                const float dk = juce::jlimit(0.0f, 1.0f,
                                      (1.0f - (float) vpx / 255.0f) + dShift);
                return static_cast<uint8_t>(
                    juce::jlimit(0, 255, (int) std::lround((1.0f - dk) * 255.0f)));
            };
            for (int px = 0; px < nb; ++px)
            {
                const float xn  = (nb > 1) ? static_cast<float>(px)
                                              / static_cast<float>(nb - 1)
                                           : 0.0f;
                const float gdb = lut[static_cast<int>(xn * scale)];   // dB
                if (std::abs(gdb) < 0.01f) continue;
                const float dShift = gdb / range;
                outR[px] = shift(outR[px], dShift);
                outG[px] = shift(outG[px], dShift);
                outB[px] = shift(outB[px], dShift);
            }
        }
    }

    // Update UI playhead cursor (atomic write — Non-RT safe)
    if (isScore)
        sampler.notifyScorePlayHead(slot.play_head);
    else
        sampler.notifyPlayHead(slotIdx, slot.play_head);

    outNb = nb;
    return true;
}

// ── compositeVoice ───────────────────────────────────────────────────────────
// Composite one voice frame into the master frame. Darkness domain: 255 =
// white = silence = identity. A single voice at level 1 is output-identical
// in every mode (the master starts white), so legacy single-bank playback is
// bit-exact regardless of the chosen mix mode.
void FramePlayerThread::compositeVoice(uint8_t* mR, uint8_t* mG, uint8_t* mB,
                                       const uint8_t* vR, const uint8_t* vG,
                                       const uint8_t* vB, int vNb,
                                       float level, SlotMixMode mode) noexcept
{
    const float L = juce::jlimit(0.0f, 1.0f, level);
    if (vNb <= 0 || L <= 0.001f) return;   // silent bank — white identity

    switch (mode)
    {
        case SlotMixMode::MIX:
            // Normal blend: the bank fader is the blend opacity.
            for (int px = 0; px < vNb; ++px)
            {
                mR[px] = static_cast<uint8_t>(mR[px] + L * ((float) vR[px] - mR[px]));
                mG[px] = static_cast<uint8_t>(mG[px] + L * ((float) vG[px] - mG[px]));
                mB[px] = static_cast<uint8_t>(mB[px] + L * ((float) vB[px] - mB[px]));
            }
            break;

        case SlotMixMode::ADD:
            // Energy add (linear burn): darkness_master += level · darkness_voice.
            for (int px = 0; px < vNb; ++px)
            {
                mR[px] = static_cast<uint8_t>(juce::jmax(0,
                    (int) mR[px] - (int) (L * (255.0f - (float) vR[px]) + 0.5f)));
                mG[px] = static_cast<uint8_t>(juce::jmax(0,
                    (int) mG[px] - (int) (L * (255.0f - (float) vG[px]) + 0.5f)));
                mB[px] = static_cast<uint8_t>(juce::jmax(0,
                    (int) mB[px] - (int) (L * (255.0f - (float) vB[px]) + 0.5f)));
            }
            break;

        case SlotMixMode::DARKEN:
        default:
            // Material union: prefade the voice toward white by the fader,
            // then darkest pixel wins (the house blend rule — same as the
            // sampler↔live darken mix).
            for (int px = 0; px < vNb; ++px)
            {
                const int pfR = 255 - (int) (L * (255.0f - (float) vR[px]) + 0.5f);
                const int pfG = 255 - (int) (L * (255.0f - (float) vG[px]) + 0.5f);
                const int pfB = 255 - (int) (L * (255.0f - (float) vB[px]) + 0.5f);
                mR[px] = static_cast<uint8_t>(juce::jmin((int) mR[px], pfR));
                mG[px] = static_cast<uint8_t>(juce::jmin((int) mG[px], pfG));
                mB[px] = static_cast<uint8_t>(juce::jmin((int) mB[px], pfB));
            }
            break;
    }
}

// ── outputFrame ──────────────────────────────────────────────────────────────
// Shared injection tail for the composited master frame: sampler snapshot,
// self-resampling record, live darken-blend, transport fade + opacity, sends
// staging, engine-A chain inserts, visual mix bus and preprocessed commit.
// Runs once per 1 ms tick regardless of how many voices were composited.
void FramePlayerThread::outputFrame(uint8_t* workR, uint8_t* workG,
                                    uint8_t* workB, int nb,
                                    bool isScore, float liveBlendAmount)
{
    auto& state = sampler.getAtomicState();

    // ── Snapshot pure sampler frame BEFORE live blend ─────────────────────
    // So the visualizer can show the sampler output in isolation.
    // Multi-chain split: the shared sampler snapshot is a SINGLE display
    // bus — with two engines playing simultaneously only the display
    // owner writes it (score owns it outright; otherwise the first
    // driving engine). The other engine's playback still reaches the
    // synths through its own per-chain staging.
    {
        const bool displayOwner =
            lux_sampler_is_score_playing()
                ? isScore
                : (lux_sampler_playing_engine()
                   == sampler.getEngineIndex());
        if (displayOwner)
        {
            // (P4-M3) The zone-1 view is the SELECTION TAP, published by the
            // player's own chain walk at the exact position — no more global
            // modulated publish. The sampler snapshot write remains (its
            // readers died with the bus; purge slated for M5).
            audio_image_buffers_snapshot_sampler(audioBuffers,
                                                 workR, workG, workB, nb);
        }
    }

    // (P4-M2) Downstream capture moved INTO the unified chain walk below
    // (chain_player_execute_owned): a downstream SAMPLER marker records its
    // input AT ITS POSITION in the chain — post-blend, post-upstream-FX —
    // exactly the stream a module below the player receives.

    // ── Live darken-blend: min(sample, live) weighted by blendAmount ─────
    // blendAmount=0 → pure playback; blendAmount=1 → full darken blend.
    // Multi-voice: the strongest per-bank blendAmount of the active voices
    // drives the whole composite. Applied AFTER fades/EQ so the blend sees
    // the fully processed frame.
    {
        const float p_blend = liveBlendAmount;
        if (p_blend > 0.001f)
        {
            uint8_t lvR[LuxSamplerConstants::MAX_PIXELS] {};
            uint8_t lvG[LuxSamplerConstants::MAX_PIXELS] {};
            uint8_t lvB[LuxSamplerConstants::MAX_PIXELS] {};
            int liveN = 0;
            sampler.getLiveFrame(lvR, lvG, lvB, nb, liveN);
            const int blendN = std::min(liveN, nb);
            for (int px = 0; px < blendN; ++px)
            {
                const auto dR = (uint8_t)std::min((int)workR[px], (int)lvR[px]);
                const auto dG = (uint8_t)std::min((int)workG[px], (int)lvG[px]);
                const auto dB = (uint8_t)std::min((int)workB[px], (int)lvB[px]);
                workR[px] = static_cast<uint8_t>(
                    (float)workR[px] + p_blend * ((float)dR - (float)workR[px]));
                workG[px] = static_cast<uint8_t>(
                    (float)workG[px] + p_blend * ((float)dG - (float)workG[px]));
                workB[px] = static_cast<uint8_t>(
                    (float)workB[px] + p_blend * ((float)dB - (float)workB[px]));
            }
        }
    }

    // ── Mix sampler + live before visual injection ────────────────────────────
    // Blend rule: darken (min per channel). White (255) is the identity element
    // so sources with opacity=0 become white and do not affect the result.
    //
    // When sampler_freeze_mode == 2 (STOP transport): skip injection entirely
    // so the live UDP thread regains exclusive control of AudioImageBuffers.
    {
        extern sp3ctra_config_t g_sp3ctra_config;
        // FIX(routing): When the sequencer drives playback, treat the transport
        // as PLAY (0) regardless of the sampler_freeze_mode UI state.
        // The sequencer (FrameSequencer::playing) is the authoritative transport;
        // sampler_freeze_mode only controls the manual Play/Hold/Stop Transport UI
        // and must not gate injection into AudioImageBuffers during sequencer play.
        const bool seqDriven  = state.seqControlledPlay.load(std::memory_order_relaxed);
        // The SCORE module has its own transport — the sampler Transport
        // UI (sampler_freeze_mode) must never gate or fade it.
        const int   smpFreeze  = (seqDriven || isScore) ? 0 : g_sp3ctra_config.sampler_freeze_mode;
        const int   liveFreeze = g_sp3ctra_config.image_freeze_mode;
        const float liveOp     = g_sp3ctra_config.image_live_opacity;
        const float smpOp      = g_sp3ctra_config.image_sampler_opacity;
        // ── Transport fade-in on Play: REMOVED (2026-07-13) ───────────────
        // The sampler now starts at full opacity immediately on PLAY — there
        // is no HOLD/STOP → PLAY volume/brightness ramp. The `sampler_fade_in_ms`
        // parameter is inert for the sampler chain.

        // Effective sampler opacity = user crossfader opacity (no fade ramp).
        // At effectiveSmpOp=0 → frame=white (silence); =smpOp → normal brightness.
        //
        // Plan-aware (M7): when the additive path is sampler-relayed, bypass the
        // MIX crossfader opacity (relay/Score always full opacity). The crossfader
        // balance (smpOp) is only meaningful for the legacy MIX blending.
        const bool addRelayed = chain_additive_player_candidate(
            0, sampler.getEngineIndex()) != 0;
        // ── Module bypass ───────────────────────────────────────────────
        // When the SAMPLER module is DISABLED in the chain rack, its player
        // output must be IGNORED — but the players are NOT stopped. Forcing
        // the opacity to 0 turns the frame white (= silence / darken-blend
        // identity) before it reaches the visual mix bus AND the audio
        // pipeline (the preprocessed_data step below reads these same
        // whitened pixels), so the sampler stops contributing while the
        // FramePlayerThread keeps advancing the play head. The SCORE relay
        // (isScore) owns a separate enable and is never gated here.
        const bool moduleIgnored = (! isScore && ! sampler.isEnabled());
        const float effectiveSmpOp = moduleIgnored
            ? 0.0f
            : (addRelayed || isScore)
                ? 1.0f      // relay/Score: full opacity
                : smpOp;    // legacy MIX: crossfader balance

        if (smpFreeze != 2) // Do not inject when sampler transport is STOP
        {
            // 1. Apply sampler opacity (crossfader balance)
            if (effectiveSmpOp < 0.999f)
            {
                const float inv = 1.0f - effectiveSmpOp;
                for (int px = 0; px < nb; ++px)
                {
                    workR[px] = static_cast<uint8_t>(workR[px] * effectiveSmpOp + 255.f * inv);
                    workG[px] = static_cast<uint8_t>(workG[px] * effectiveSmpOp + 255.f * inv);
                    workB[px] = static_cast<uint8_t>(workB[px] * effectiveSmpOp + 255.f * inv);
                }
            }

            // 2. Darken-blend with live when live transport is active
            //    Skip entirely when the additive path is sampler-
            //    relayed and for the Score (no live contribution).
            if (!addRelayed && !isScore
                && liveFreeze != 2 && liveOp > 0.001f)
            {
                uint8_t lvR[LuxSamplerConstants::MAX_PIXELS] {};
                uint8_t lvG[LuxSamplerConstants::MAX_PIXELS] {};
                uint8_t lvB[LuxSamplerConstants::MAX_PIXELS] {};
                int liveN = 0;
                sampler.getLiveFrame(lvR, lvG, lvB, nb, liveN);
                const int blendN = std::min(liveN, nb);
                const float liveInv = 1.0f - liveOp;
                for (int px = 0; px < blendN; ++px)
                {
                    // Apply live opacity: fade live frame toward white
                    const auto lR = static_cast<uint8_t>(
                        lvR[px] * liveOp + 255.f * liveInv);
                    const auto lG = static_cast<uint8_t>(
                        lvG[px] * liveOp + 255.f * liveInv);
                    const auto lB = static_cast<uint8_t>(
                        lvB[px] * liveOp + 255.f * liveInv);
                    // Darken blend: darkest pixel wins
                    workR[px] = std::min(workR[px], lR);
                    workG[px] = std::min(workG[px], lG);
                    workB[px] = std::min(workB[px], lB);
                }
            }

            // 2b. P4-M2 — ONE positional walk per player-owned chain (the
            //     same executor as udpThread/feeder): span BELOW the owning
            //     marker on the blended frame. Stages every OUT at its exact
            //     position (LuxStral/LuxSynth/LuxWave), runs post-marker
            //     FX/probes exactly once, records downstream markers
            //     (bounce/resampling) and publishes the exact selection tap.
            //     Writes the stream at the first owned LS OUT back into
            //     work* so the mix bus + audio commits below see post-FX.
            (void) chain_player_execute_owned(
                isScore ? 1 : 0,
                sampler.getEngineIndex(),
                (state.seqControlledPlay.load(std::memory_order_relaxed)
                 || isScore) ? 1 : 0,
                audioBuffers, workR, workG, workB, nb);

            // 3. Write mixed frame to AudioImageBuffers (the visual mix
            //    bus). Single display bus — with two engines playing
            //    simultaneously only the display owner writes it (the
            //    other playback stays audio-only; per-engine viz is a
            //    follow-up). Same owner rule as the sampler snapshot.
            const bool mixBusOwner =
                lux_sampler_is_score_playing()
                    ? isScore
                    : (lux_sampler_playing_engine()
                       == sampler.getEngineIndex());
            uint8_t* wR = nullptr;
            uint8_t* wG = nullptr;
            uint8_t* wB = nullptr;
            if (mixBusOwner
                && audio_image_buffers_start_write(audioBuffers,
                                                   &wR, &wG, &wB) == 0)
            {
                std::memcpy(wR, workR, static_cast<size_t>(nb));
                std::memcpy(wG, workG, static_cast<size_t>(nb));
                std::memcpy(wB, workB, static_cast<size_t>(nb));
                audio_image_buffers_complete_write(audioBuffers);
            }
        }
    }

    // ---------------------------------------------------------------
    // CRITICAL: update db->preprocessed_data from the (modified) frame.
    // synth_AudioProcess uses db->preprocessed_data for audio generation
    // (not the raw RGB buffers). fades + blend are applied to workR/G/B
    // before preprocessing so they affect the synthesised sound.
    //
    // Source routing: sampler thread only writes preprocessed_data
    // when Source=S (IMAGE_SOURCE_SAMPLER=0).
    // Source=L(1) or M(2): the live UDP thread is the sole writer.
    //
    // NOTE: sampler_freeze_mode is NOT checked here.  The pipeline's
    // envelope (ENVELOPE_SAMPLER) already handles PLAY/HOLD/STOP via
    // config->freeze_mode.  For Source=S the UDP thread never writes
    // preprocessed_data during playback, so there is no ownership
    // conflict.  The previous guard (freeze_mode != 2) prevented
    // preprocessed_data updates when the transport was in STOP,
    // causing stale audio while the visual animated correctly.
    // ---------------------------------------------------------------
    {
        // (P4-M4) The audio-thread MIXER is the additive sections' SOLE
        // writer in every topology — the player never commits Path A any
        // more (its LuxStral contribution goes through the staged sends of
        // its chain walk). Only the polyphonic (pb) views remain player-
        // committed while the pb chain is relayed.
        const bool pbOwned  = chain_pathb_player_candidate(
            isScore ? 1 : 0, sampler.getEngineIndex()) != 0;

        if (doubleBuffer != nullptr && pbOwned)
        {
            // One instance per player thread (engines A/B each run their
            // own FramePlayerThread): the old stack `ppData {}` value-
            // initialized ~100 KB per 1 ms tick — pure memset cost.
            // Sections are only committed under the same flag that just
            // computed them, so no stale section can ever be committed.
            static thread_local PreprocessedImageData ppData;
            PipelineConfig sampler_cfg = pipeline_build_config_sampler();
            // FIX(routing): Sequencer-driven playback must not be silenced
            // by the pipeline envelope when sampler_freeze_mode=STOP (2).
            // Override freeze_mode to PLAY so ENVELOPE_SAMPLER processes
            // the frame normally regardless of the Transport UI state.
            if (state.seqControlledPlay.load(std::memory_order_relaxed) || isScore)
                sampler_cfg.freeze_mode = 0; /* force PLAY — sequencer / score active */

            pipeline_path_luxsynth_luxwave(workR, workG, workB,
                                           &sampler_cfg, &ppData);
            ppData.timestamp_us = static_cast<uint64_t>(currentTimeUs());

            pthread_mutex_lock(&doubleBuffer->mutex);
            /* Polyphonic (views) only — the mixer owns everything else. */
            doubleBuffer->preprocessed_data.polyphonic = ppData.polyphonic;
            pthread_mutex_unlock(&doubleBuffer->mutex);

            // Per-engine input tap Path-B (per-chain display): the exact
            // frame fed to the pipeline above. (Tap A is published by the
            // chain walk at its first owned LS OUT.)
            {
                audio_image_buffers_publish_engine_input(
                    audioBuffers, AUDIO_IMAGE_ENGINE_TAP_PATHB,
                    workR, workG, workB, nb);
                // (P4-M2) The "→ LUXSYNTH"/"→ LUXWAVE" staging happens in
                // the unified chain walk above, at each OUT's exact position
                // in its own chain (no more first-OUT-only, cross-chain-FX
                // approximation).
            }
        }
    }
}

// ── runScoreSession ──────────────────────────────────────────────────────────
// One SCORE playback session: a single voice on the dedicated score slot,
// exclusive owner of the shared channel (sentinel activePlaySlot==SCORE_SLOT).
void FramePlayerThread::runScoreSession()
{
    auto& state = sampler.getAtomicState();
    using namespace LuxSamplerConstants;

    // Publish the busy bit BEFORE touching the slot's fields — message-thread
    // code that frees/replaces the score frames stops playback then
    // waitForPlayerRelease()s on this mask. Cleared on every exit path (RAII).
    sampler.addPlayerBusySlot(SCORE_SLOT);
    struct BusyReset
    {
        LuxSampler& s;
        ~BusyReset() { s.clearPlayerBusyMask(); }
    } busyReset { sampler };

    FrameSlot& slot = sampler.getScoreSlot();

    if (!slot.has_content || slot.frame_count == 0 || !slot.isAllocated())
    {
        log_warning("FS", "Score: play requested but no content");
        sampler.notifyScoreStopped();
        // SCORE took the channel but has nothing to play — hand it back to
        // the sampler slot it overrode (if any) rather than silencing.
        if (resumeScoreRelaySlot())
            return;
        state.activePlaySlot.store(-1, std::memory_order_release);
        if (!state.seqControlledPlay.load(std::memory_order_relaxed))
            state.passthroughEnabled.store(true, std::memory_order_release);
        return;
    }

    log_info("FS", "Score: playback start — %d frames, %.2f s",
             slot.frame_count, static_cast<double>(slot.duration_us) / 1e6);

    VoiceCtx v;
    v.slot         = SCORE_SLOT;
    v.active       = true;
    v.prevLoopMode = sampler.getSlotLoopMode(SCORE_SLOT);
    v.direction    = (v.prevLoopMode == LoopMode::INVERSE) ? -1 : 1;
    slot.play_head = 0; // set on first range init below
    // SCORE resume: an armed resume frame (e.g. from a live EQ re-apply that
    // reloaded the frames) takes over the initial head so playback continues
    // where it left off instead of snapping back to 0. One-shot — consumed
    // here, so a fresh PLAY still starts from the beginning.
    {
        const int resume = sampler.consumeScoreResumeHead();
        if (resume > 0 && slot.frame_count > 0)
            slot.play_head = juce::jlimit(0, slot.frame_count - 1, resume);
    }

    constexpr uint64_t kPeriodUs = 1000; // 1 ms = 1000 lines/sec
    uint64_t lastInjectUs      = currentTimeUs();
    bool     stoppedByNoneMode = false;

    uint8_t workR[MAX_PIXELS];
    uint8_t workG[MAX_PIXELS];
    uint8_t workB[MAX_PIXELS];

    while (!threadShouldExit())
    {
        // External stop commands
        if (state.stopPlayCmd.exchange(false, std::memory_order_acq_rel))
            break;
        // The score has no slotState entry — it stops only via stopPlayCmd or
        // a competing startPlayCmd.
        {
            const int pending = state.startPlayCmd.load(std::memory_order_relaxed);
            if (pending >= 0 && pending != SCORE_SLOT)
                break;
        }

        // ── Pause / hold: freeze play_head, re-anchor injection timer ─────
        if (sampler.isSeqPlayerHeld())
        {
            lastInjectUs = currentTimeUs(); // no burst on resume
            Thread::sleep(2);
            continue;
        }

        // ── Wait for next 1ms injection tick ──────────────────────────────
        const uint64_t now             = currentTimeUs();
        const uint64_t sinceLastInject = now - lastInjectUs;
        if (sinceLastInject < kPeriodUs)
        {
            const uint64_t remaining = kPeriodUs - sinceLastInject;
            if (remaining > 2000) Thread::sleep(1);
            else                  Thread::yield();
            continue;
        }
        // Lock-step advance — avoid drift accumulation
        lastInjectUs += kPeriodUs;
        if (lastInjectUs > now) lastInjectUs = now; // catch-up safety

        int nb = 0;
        const bool alive = tickVoice(v, slot, true, workR, workG, workB, nb);
        if (nb > 0)
            outputFrame(workR, workG, workB, nb, true, 0.0f);
        if (!alive)
        {
            stoppedByNoneMode = true;
            break;
        }
    }

    // ── Inject white (silence) frame when playback ends ───────────────────
    {
        const bool doSilence =
            stoppedByNoneMode
            || sampler.isSeqSilentStepActive()
            || state.injectSilenceCmd.exchange(false, std::memory_order_acq_rel);
        const bool liveStep = state.seqLiveStepActive.load(std::memory_order_relaxed);
        if (doSilence && !liveStep)
        {
            log_info("FS", "Score: playback end — injecting white frame (silence)");
            injectWhiteFrame();
        }
        else if (doSilence && liveStep)
            log_info("FS", "Score: playback end — seqLiveStep active, skipping white frame");
    }

    log_info("FS", "Score: playback stopped (head=%d/%d)",
             slot.play_head, slot.frame_count);

    // Relinquish the channel unless a fresh score play is already queued
    // (rapid stop+play).
    {
        const int pendingCmd = state.startPlayCmd.load(std::memory_order_acquire);
        if (pendingCmd != SCORE_SLOT)
        {
            sampler.notifyScoreStopped();
            // Relay first: if SCORE overrode a playing sampler slot, resume
            // it. Otherwise relinquish the channel and restore live.
            if (!resumeScoreRelaySlot())
            {
                if (state.activePlaySlot.load(std::memory_order_relaxed)
                    == SCORE_SLOT)
                    state.activePlaySlot.store(-1, std::memory_order_release);
                state.passthroughEnabled.store(true, std::memory_order_release);
            }
        }
    }
}

// ── runSamplerSession ────────────────────────────────────────────────────────
// One sampler playback session with an N-voice set that FOLLOWS slotState[]:
// every slot whose state is PLAYING is a voice (uiPlaySlot / uiToggleRecord /
// uiClearSlot / the sequencer and the cross-engine arbiter all flip these
// states). Each 1 ms tick advances every voice and composites them into ONE
// master frame via the per-bank mixer (level + mix mode), which then runs the
// legacy injection tail exactly once.
void FramePlayerThread::runSamplerSession()
{
    auto& state = sampler.getAtomicState();
    using namespace LuxSamplerConstants;

    // Whatever the exit path, no busy bit may leak past the session.
    struct BusyReset
    {
        LuxSampler& s;
        ~BusyReset() { s.clearPlayerBusyMask(); }
    } busyReset { sampler };

    VoiceCtx voices[NUM_SLOTS];
    int  numActive      = 0;
    bool sessionSilence = false;   // last voice ended on LoopMode::NONE

    auto activateVoice = [&](int i)
    {
        // Publish the busy bit BEFORE touching the slot's fields — message-
        // thread code that frees/replaces the frames stops playback then
        // waitForPlayerRelease()s on this mask.
        sampler.addPlayerBusySlot(i);
        FrameSlot& sl = sampler.getSlot(i);
        if (!sl.has_content || sl.frame_count == 0 || !sl.isAllocated())
        {
            log_warning("FS", "Slot %d: play requested but no content", i);
            state.slotState[i].store(static_cast<int>(SlotState::IDLE),
                                     std::memory_order_release);
            sampler.removePlayerBusySlot(i);
            return;
        }
        VoiceCtx& v = voices[i];
        v = VoiceCtx {};
        v.slot         = i;
        v.active       = true;
        v.prevLoopMode = sampler.getSlotLoopMode(i);
        v.direction    = (v.prevLoopMode == LoopMode::INVERSE) ? -1 : 1;
        sl.play_head   = 0; // real head set on the voice's first range init
        ++numActive;
        log_info("FS", "Slot %d: playback start — %d frames, %.2f s",
                 i, sl.frame_count, static_cast<double>(sl.duration_us) / 1e6);
    };

    auto deactivateVoice = [&](int i, bool toIdle)
    {
        VoiceCtx& v = voices[i];
        if (!v.active) return;
        FrameSlot& sl = sampler.getSlot(i);
        // Save last position and direction for Resume mode (the direction
        // restores the PINGPONG sense).
        sampler.saveLastPlayHead(i, sl.play_head);
        sampler.saveLastDirection(i, v.direction);
        if (toIdle)
            state.slotState[i].store(static_cast<int>(SlotState::IDLE),
                                     std::memory_order_release);
        v.active = false;
        --numActive;
        sampler.removePlayerBusySlot(i);
        log_info("FS", "Slot %d: playback stopped (head=%d/%d)",
                 i, sl.play_head, sl.frame_count);
    };

    constexpr uint64_t kPeriodUs = 1000; // 1 ms = 1000 lines/sec
    uint64_t lastInjectUs = currentTimeUs();

    // Per-voice scratch (reused voice by voice) + master composite frame.
    uint8_t vR[MAX_PIXELS],    vG[MAX_PIXELS],    vB[MAX_PIXELS];
    uint8_t workR[MAX_PIXELS], workG[MAX_PIXELS], workB[MAX_PIXELS];

    while (!threadShouldExit())
    {
        // ── Session-level commands ─────────────────────────────────────────
        if (state.stopPlayCmd.exchange(false, std::memory_order_acq_rel))
            break;
        {
            const int pending = state.startPlayCmd.load(std::memory_order_acquire);
            if (pending == SCORE_SLOT)
                break;   // SCORE takes over — the outer loop starts its session
            if (pending >= 0)
            {
                state.startPlayCmd.exchange(-1, std::memory_order_acq_rel);
                // Sequencer steps stay MONOPHONIC: a sequencer-triggered play
                // replaces every other running voice. Manual play (uiPlaySlot)
                // is additive — seqControlledPlay distinguishes the two.
                if (state.seqControlledPlay.load(std::memory_order_relaxed))
                    for (int i = 0; i < NUM_SLOTS; ++i)
                        if (i != pending && voices[i].active)
                            deactivateVoice(i, true);
                // The pending slot itself joins through the state sync below
                // (its slotState is already PLAYING).
            }
        }

        // ── Sync the voice set with slotState[] — PLAYING is the truth ────
        for (int i = 0; i < NUM_SLOTS; ++i)
        {
            const bool shouldPlay = static_cast<SlotState>(
                state.slotState[i].load(std::memory_order_relaxed))
                == SlotState::PLAYING;
            if (shouldPlay && !voices[i].active)
                activateVoice(i);
            else if (!shouldPlay && voices[i].active)
                deactivateVoice(i, false);
        }
        if (numActive == 0)
            break;

        // Publish the primary voice (lowest active slot): UI underline, SCORE
        // relay arming, display-owner arbitration and the isAnySlotPlaying()
        // gates all key off activePlaySlot.
        for (int i = 0; i < NUM_SLOTS; ++i)
            if (voices[i].active)
            {
                if (state.activePlaySlot.load(std::memory_order_relaxed) != i)
                    state.activePlaySlot.store(i, std::memory_order_release);
                break;
            }

        // ── Pause / hold: freeze play heads, re-anchor injection timer ────
        if (sampler.isSeqPlayerHeld())
        {
            lastInjectUs = currentTimeUs(); // no burst on resume
            Thread::sleep(2);
            continue;
        }

        // ── Wait for next 1ms injection tick ───────────────────────────────
        const uint64_t now             = currentTimeUs();
        const uint64_t sinceLastInject = now - lastInjectUs;
        if (sinceLastInject < kPeriodUs)
        {
            const uint64_t remaining = kPeriodUs - sinceLastInject;
            if (remaining > 2000) Thread::sleep(1);
            else                  Thread::yield();
            continue;
        }
        // Lock-step advance — avoid drift accumulation
        lastInjectUs += kPeriodUs;
        if (lastInjectUs > now) lastInjectUs = now; // catch-up safety

        // ── Tick every voice, composite into the master frame ──────────────
        // The master starts white (255 = silence): the identity element of
        // every mix mode, so a single bank at level 1 is bit-exact with the
        // legacy single-voice output.
        std::memset(workR, 255, sizeof(workR));
        std::memset(workG, 255, sizeof(workG));
        std::memset(workB, 255, sizeof(workB));
        int   masterNb  = 0;
        float liveBlend = 0.0f;

        for (int i = 0; i < NUM_SLOTS; ++i)
        {
            if (!voices[i].active) continue;
            int nb = 0;
            const bool alive = tickVoice(voices[i], sampler.getSlot(i), false,
                                         vR, vG, vB, nb);
            // Bank mixer: fader = 1 − brightnessLift, per-bank mix mode.
            compositeVoice(workR, workG, workB, vR, vG, vB, nb,
                           1.0f - sampler.getSlotBrightnessLift(i),
                           sampler.getSlotMixMode(i));
            masterNb  = juce::jmax(masterNb, nb);
            liveBlend = juce::jmax(liveBlend, sampler.getSlotBlendAmount(i));
            if (!alive)
            {
                deactivateVoice(i, true);
                if (numActive == 0)
                    sessionSilence = true; // last voice ended on NONE → silence
            }
        }

        if (masterNb > 0)
            outputFrame(workR, workG, workB, masterNb, false, liveBlend);

        if (numActive == 0)
            break;
    }

    // ── Session teardown ────────────────────────────────────────────────────
    // Voices still active were interrupted by a session-level break (stop
    // command / SCORE takeover / thread exit): save their heads and drop the
    // PLAYING state — unless a fresh start for that same slot is already
    // queued (rapid stop+play must keep its armed state; see the historical
    // "resume cursor stays but play does not start" bug).
    {
        const int pendingCmd = state.startPlayCmd.load(std::memory_order_acquire);
        for (int i = 0; i < NUM_SLOTS; ++i)
            if (voices[i].active)
                deactivateVoice(i, pendingCmd != i);
    }

    // ── Inject white (silence) frame when playback ends ────────────────────
    // Triggered by any of:
    //   • LoopMode::NONE reached the end on the LAST voice   (sessionSilence)
    //   • A STEP_EMPTY sequencer step is now active          (seqSilentStepActive)
    //   • An explicit silence command from rtStop() or
    //     triggerStep(STEP_EMPTY)                            (injectSilenceCmd)
    // Normal bank→bank transitions satisfy none of these conditions, so the
    // cut between consecutive banks remains seamless (no white-frame gap).
    {
        const bool doSilence =
            sessionSilence
            || sampler.isSeqSilentStepActive()
            || state.injectSilenceCmd.exchange(false, std::memory_order_acq_rel);

        // FIX(live): When the sequencer's STEP_LIVE is active, the live UDP
        // stream is the intended content — NEVER overwrite with white.
        // seqLiveStepActive (not passthroughEnabled!) distinguishes
        // sequencer-STEP_LIVE from normal idle/stop where silence IS needed.
        const bool liveStep = state.seqLiveStepActive.load(std::memory_order_relaxed);

        if (doSilence && !liveStep)
        {
            log_info("FS", "Sampler session end — injecting white frame (silence)");
            injectWhiteFrame();
        }
        else if (doSilence && liveStep)
        {
            log_info("FS", "Sampler session end — seqLiveStep active, skipping white frame");
        }
    }

    // ── Release the shared channel unless a new play is already queued ─────
    // (rapid stop+play, or a SCORE takeover about to start its session).
    // activePlaySlot may already hold the SCORE sentinel when uiPlayScore()
    // displaced this session mid-flight — never clobber it with -1.
    {
        const int pendingCmd = state.startPlayCmd.load(std::memory_order_acquire);
        if (pendingCmd < 0)
        {
            if (state.activePlaySlot.load(std::memory_order_relaxed)
                < NUM_SLOTS)
                state.activePlaySlot.store(-1, std::memory_order_release);
            // When the sequencer owns the play session (seqControlledPlay),
            // do NOT restore live passthrough here — the sequencer (STEP_LIVE
            // or rtStop) is the only authority that re-enables the live UDP
            // stream.
            if (!state.seqControlledPlay.load(std::memory_order_relaxed))
                state.passthroughEnabled.store(true, std::memory_order_release);
        }
    }
}

// ── run ──────────────────────────────────────────────────────────────────────
void FramePlayerThread::run()
{
    auto& state = sampler.getAtomicState();
    log_info("FS", "FramePlayerThread running");

    while (!threadShouldExit())
    {
        // Bulk slot replacement in progress (loadFromFile): leave any queued
        // command untouched and stay idle until the message thread finishes.
        if (sampler.isPlaybackSuspended())
        {
            Thread::sleep(1);
            continue;
        }

        // Wait for a startPlay command. It only WAKES the player: inside a
        // sampler session the voice set itself is derived from slotState[].
        const int slotToPlay = state.startPlayCmd.exchange(-1,
                                                            std::memory_order_acq_rel);
        if (slotToPlay < 0)
        {
            // No play command pending.  Check for a pending silence-injection
            // request (posted by triggerStep(STEP_EMPTY) or rtStop() from the
            // sequencer).  This handles the case where no slot was playing when
            // the silence command was issued (e.g. STEP_EMPTY after STEP_LIVE,
            // or rtStop() when the sequencer was idle).
            if (state.injectSilenceCmd.exchange(false, std::memory_order_acq_rel))
            {
                // FIX(live): When the sequencer's STEP_LIVE is active,
                // the live UDP stream is the sole authority — NEVER inject white.
                // Consume the flag but skip the white frame injection.
                if (state.seqLiveStepActive.load(std::memory_order_relaxed))
                {
                    log_info("FS", "FramePlayerThread: idle — seqLiveStep active, skipping white frame");
                }
                else
                {
                    log_info("FS", "FramePlayerThread: idle — injecting white frame (silence)");
                    injectWhiteFrame();
                }
            }
            Thread::sleep(1);
            continue;
        }

        if (slotToPlay == LuxSamplerConstants::SCORE_SLOT)
            runScoreSession();
        else
            runSamplerSession();
    }

    // Final safety: restore passthrough on thread exit only when not under
    // sequencer control.  stopPlayerThread() already writes passthroughEnabled=true
    // before signalling threadShouldExit(), so this is a belt-and-suspenders
    // guard for edge cases (non-sequencer-driven exit paths).
    if (!state.seqControlledPlay.load(std::memory_order_relaxed))
        state.passthroughEnabled.store(true, std::memory_order_release);
    log_info("FS", "FramePlayerThread exiting");
}

// ============================================================================
// LuxSampler::sampleBrightnessForTimeline — Non-RT only
// ============================================================================

void LuxSampler::sampleBrightnessForTimeline(int    slotIdx,
                                                float* outBrightness,
                                                int    count) const noexcept
{
    if (slotIdx < 0 || slotIdx >= LuxSamplerConstants::NUM_SLOTS
        || outBrightness == nullptr || count <= 0)
        return;

    // slotsMutex_ prevents a concurrent clear() / allocate() from freeing or
    // reallocating slot.frames while this function reads from it.
    std::lock_guard<std::mutex> lk(slotsMutex_);

    const FrameSlot& slot = slots[slotIdx];
    if (!slot.has_content || slot.frame_count == 0 || !slot.isAllocated())
    {
        for (int k = 0; k < count; ++k)
            outBrightness[k] = 0.0f;
        return;
    }

    // Snapshot frame_count once so the upper bound is consistent throughout
    // the loop even if the UDP thread increments it concurrently.
    const int fc = slot.frame_count;

    // For each timeline column, pick one frame and average 8 evenly-spaced
    // pixels.  Total cost: O(8 * count) — safe from the message thread.
    for (int k = 0; k < count; ++k)
    {
        const int frameIdx = juce::jlimit(0, fc - 1, k * fc / count);
        const CapturedFrame& f = slot.frames[frameIdx];

        const int pc   = juce::jlimit(1, LuxSamplerConstants::MAX_PIXELS,
                                      static_cast<int>(f.pixel_count));
        const int step = std::max(1, pc / 8);

        uint32_t sum      = 0;
        int      nSamples = 0;
        for (int p = 0; p < pc; p += step, ++nSamples)
            sum += static_cast<uint32_t>(f.R[p])
                 + static_cast<uint32_t>(f.G[p])
                 + static_cast<uint32_t>(f.B[p]);

        // Invert: more black (low luminance) → higher bar on the timeline.
        const float rawBri = (nSamples > 0)
            ? juce::jlimit(0.0f, 1.0f,
                           static_cast<float>(sum) / (nSamples * 3.0f * 255.0f))
            : 0.0f;
        outBrightness[k] = 1.0f - rawBri;
    }
}

// ============================================================================
// LuxSampler::sampleSpectralForTimeline — Non-RT only
// ============================================================================
void LuxSampler::sampleSpectralForTimeline(int    slotIdx,
                                              float* outBass,
                                              float* outTreble,
                                              int    count) const noexcept
{
    if (slotIdx < 0 || slotIdx >= LuxSamplerConstants::NUM_SLOTS
        || outBass == nullptr || outTreble == nullptr || count <= 0)
        return;

    // slotsMutex_ prevents a concurrent clear() / allocate() from freeing or
    // reallocating slot.frames while this function reads from it.
    // Root cause of the EXC_BAD_ACCESS crash: frames.reset() was called on
    // another thread (UDP start-rec or UI clear) while this function held a
    // stale register-cached pointer to the freed CapturedFrame[] allocation.
    std::lock_guard<std::mutex> lk(slotsMutex_);

    const FrameSlot& slot = slots[slotIdx];
    if (!slot.has_content || slot.frame_count == 0 || !slot.isAllocated())
    {
        for (int k = 0; k < count; ++k)
            outBass[k] = outTreble[k] = 0.0f;
        return;
    }

    // Snapshot frame_count once so every frameIdx computation in the loop uses
    // a consistent upper bound even if the UDP thread increments it concurrently.
    const int fc = slot.frame_count;

    // For each column pick one representative frame and sample pixels from
    // each half.  O(~512 * 8) — safe on the message thread.
    for (int k = 0; k < count; ++k)
    {
        const int frameIdx = juce::jlimit(0, fc - 1, k * fc / count);
        const CapturedFrame& f = slot.frames[frameIdx];
        const int pc   = juce::jlimit(2, LuxSamplerConstants::MAX_PIXELS,
                                      static_cast<int>(f.pixel_count));
        const int half = pc / 2;

        // Bass: dense sampling of left half (low frequencies → gravity → down).
        // Step ≤ 4 px ensures narrow features (e.g. a 20-px black line) are
        // reliably captured and discontinuities are avoided.
        const int bassStep = std::max(1, half / 256);
        uint32_t bassSum = 0; int nBass = 0;
        for (int p = 0; p < half; p += bassStep, ++nBass)
            bassSum += (uint32_t)f.R[p] + f.G[p] + f.B[p];

        // Treble: dense sampling of right half (high frequencies → air → up).
        const int treStep = std::max(1, (pc - half) / 256);
        uint32_t treSum = 0; int nTre = 0;
        for (int p = half; p < pc; p += treStep, ++nTre)
            treSum += (uint32_t)f.R[p] + f.G[p] + f.B[p];

        // Darkness = 1 − brightness  (dark pixels = spectral energy)
        outBass[k] = (nBass > 0) ? juce::jlimit(0.0f, 1.0f,
            1.0f - (float)bassSum / (nBass * 3.0f * 255.0f)) : 0.0f;
        outTreble[k] = (nTre > 0) ? juce::jlimit(0.0f, 1.0f,
            1.0f - (float)treSum  / (nTre  * 3.0f * 255.0f)) : 0.0f;
    }
}

// ============================================================================
// LuxSampler::sampleFreqProfileForCurve — Non-RT only
// Average spectral energy over the frequency (pixel) axis, averaged over time.
// ============================================================================
void LuxSampler::sampleFreqProfileForCurve(int    slotIdx,
                                            float* outProfile,
                                            int    count) const noexcept
{
    if (slotIdx < 0 || slotIdx >= LuxSamplerConstants::NUM_SLOTS
        || outProfile == nullptr || count <= 0)
        return;

    std::lock_guard<std::mutex> lk(slotsMutex_);

    const FrameSlot& slot = slots[slotIdx];
    if (!slot.has_content || slot.frame_count == 0 || !slot.isAllocated())
    {
        for (int k = 0; k < count; ++k) outProfile[k] = 0.0f;
        return;
    }

    const int fc      = slot.frame_count;
    const int pc      = juce::jlimit(2, LuxSamplerConstants::MAX_PIXELS,
                                     static_cast<int>(slot.frames[0].pixel_count));
    const int nFrames = juce::jmin(fc, 64); // time-average over a spread of frames

    for (int b = 0; b < count; ++b)
    {
        const int px0  = juce::jlimit(0, pc - 1, b * pc / count);
        const int px1  = juce::jlimit(px0 + 1, pc, (b + 1) * pc / count);
        const int step = std::max(1, (px1 - px0) / 8);

        double sum = 0.0; int n = 0;
        for (int fi = 0; fi < nFrames; ++fi)
        {
            const int frameIdx = juce::jlimit(0, fc - 1, fi * fc / nFrames);
            const CapturedFrame& f = slot.frames[frameIdx];
            for (int p = px0; p < px1; p += step)
            {
                sum += (double) f.R[p] + f.G[p] + f.B[p];
                ++n;
            }
        }
        // Darkness = 1 − brightness (dark pixels = spectral energy).
        outProfile[b] = (n > 0)
            ? juce::jlimit(0.0f, 1.0f, 1.0f - (float) (sum / (n * 3.0 * 255.0)))
            : 0.0f;
    }
}
