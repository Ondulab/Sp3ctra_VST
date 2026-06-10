/*
 * LuxSampler.cpp
 *
 * Implementation of the LuxSampler subsystem.
 * See LuxSampler.h for architecture notes.
 */

#include "LuxSampler.h"

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

// ============================================================================
// Static instance (singleton for C hook access)
// ============================================================================
LuxSampler* LuxSampler::s_instance = nullptr;

// ============================================================================
// C-linkage hook functions — called from udpThread() in multithreading.c
// ============================================================================
extern "C"
{
    void lux_sampler_on_frame_assembled(const uint8_t* R,
                                           const uint8_t* G,
                                           const uint8_t* B,
                                           uint16_t       pixel_count,
                                           uint32_t       line_id)
    {
        if (LuxSampler::s_instance != nullptr)
            LuxSampler::s_instance->onFrameAssembled(R, G, B, pixel_count, line_id);
    }

    int lux_sampler_is_playing(void)
    {
        if (LuxSampler::s_instance == nullptr)
            return 0;
        if (!LuxSampler::s_instance->isAnySlotPlaying())
            return 0;
        // When the sequencer holds the player (seqPlayerHeld), the
        // FramePlayerThread sleeps and does not inject frames.  The RAW
        // live stream must pass through so audio/visual remain alive.
        if (LuxSampler::s_instance->isSeqPlayerHeld())
            return 0;
        // FIX(routing): When the sequencer drives playback (seqControlledPlay=true),
        // FramePlayerThread is always the sole writer of AudioImageBuffers regardless
        // of the sampler Transport UI state (sampler_freeze_mode).
        // sampler_freeze_mode controls the manual Play/Hold/Stop transport ONLY —
        // it must not gate AudioImageBuffers access when the sequencer is running.
        if (LuxSampler::s_instance->getAtomicState().seqControlledPlay.load(
                std::memory_order_relaxed))
            return 1;

        // Non-sequencer playback: only block the live UDP stream when the sampler
        // transport is actively playing (sampler_freeze_mode==0).
        extern sp3ctra_config_t g_sp3ctra_config;
        return (g_sp3ctra_config.sampler_freeze_mode == 0) ? 1 : 0;
    }

    int lux_sampler_is_recording(void)
    {
        if (LuxSampler::s_instance == nullptr)
            return 0;
        return LuxSampler::s_instance->isAnySlotRecording() ? 1 : 0;
    }

    int lux_sampler_is_passthrough(void)
    {
        if (LuxSampler::s_instance == nullptr)
            return 1; // No sampler → default passthrough
        return LuxSampler::s_instance->getAtomicState()
                   .passthroughEnabled.load(std::memory_order_relaxed) ? 1 : 0;
    }

    int lux_sampler_is_seq_live_step(void)
    {
        if (LuxSampler::s_instance == nullptr)
            return 0; // No sampler → no sequencer STEP_LIVE
        return LuxSampler::s_instance->getAtomicState()
                   .seqLiveStepActive.load(std::memory_order_relaxed) ? 1 : 0;
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

LuxSampler::LuxSampler()
{
    s_instance = this;
    for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
    {
        currentPlayHead[i].store(0,  std::memory_order_relaxed);
        lastPlayHead[i].store(0,     std::memory_order_relaxed);
        lastDirection[i].store(1,    std::memory_order_relaxed); // forward by default
    }
    log_info("FS", "LuxSampler initialised — %d slots, %d frames/slot max, %.1f s/slot max",
             LuxSamplerConstants::NUM_SLOTS,
             LuxSamplerConstants::MAX_FRAMES_PER_SLOT,
             static_cast<double>(LuxSamplerConstants::MAX_DURATION_S));
}

LuxSampler::~LuxSampler()
{
    stopPlayerThread();
    s_instance = nullptr;
    log_info("FS", "LuxSampler destroyed");
}

// ============================================================================
// RT path — processMidi
// HARD CONSTRAINT: atomics ONLY. No alloc, no mutex, no I/O, no logging.
// ============================================================================

void LuxSampler::processMidi(const juce::MidiBuffer& midiBuffer)
{
    if (!enabled.load(std::memory_order_relaxed)) return;

    const int ch  = midiChannel.load(std::memory_order_relaxed);
    const int off = octaveOffset.load(std::memory_order_relaxed) * 12;

    for (const auto metadata : midiBuffer)
    {
        const auto& msg = metadata.getMessage();
        if (msg.getChannel() != ch) continue;

        const int note = msg.getNoteNumber() + off;

        if (msg.isNoteOn())
            handleNoteOn(note, msg.getVelocity());
        else if (msg.isNoteOff(true)) // true = treat NoteOn velocity=0 as NoteOff
            handleNoteOff(note);
    }
}

// RT NoteOn handler — atomics only
void LuxSampler::handleNoteOn(int note, int velocity) noexcept
{
    juce::ignoreUnused(velocity);
    using namespace LuxSamplerConstants;

    if (note >= MIDI_REC_NOTE_BASE && note < MIDI_REC_NOTE_BASE + NUM_SLOTS)
    {
        // ── REC note (C0..B0) ─────────────────────────────────────────────
        const int i   = note - MIDI_REC_NOTE_BASE;
        const auto cur = static_cast<SlotState>(
            atomicState.slotState[i].load(std::memory_order_relaxed));

        if (cur == SlotState::IDLE || cur == SlotState::ARMED)
        {
            // → ARMED
            atomicState.slotState[i].store(static_cast<int>(SlotState::ARMED),
                                            std::memory_order_release);
        }
        else if (cur == SlotState::PLAYING)
        {
            // Punch-in: stop playback, restart recording from beginning (FS-107 simplified)
            atomicState.stopPlayCmd.store(true, std::memory_order_release);
            atomicState.activePlaySlot.store(-1, std::memory_order_release);
            atomicState.passthroughEnabled.store(true, std::memory_order_release);
            atomicState.slotState[i].store(static_cast<int>(SlotState::RECORDING),
                                            std::memory_order_release);
            atomicState.startRecCmd[i].store(true, std::memory_order_release);
        }
        // RECORDING → ignore (already recording)
    }
    else if (note >= MIDI_PLAY_NOTE_BASE && note < MIDI_PLAY_NOTE_BASE + NUM_SLOTS)
    {
        // ── PLAY note (C1..B1) ────────────────────────────────────────────
        const int i   = note - MIDI_PLAY_NOTE_BASE;
        const auto cur = static_cast<SlotState>(
            atomicState.slotState[i].load(std::memory_order_relaxed));

        // Priority rule: highest slot index (highest note) wins
        const int curPlay = atomicState.activePlaySlot.load(std::memory_order_relaxed);
        if (curPlay >= 0 && i < curPlay)
            return; // Lower-priority note — ignore

        // Stop current player if a different slot was playing
        if (curPlay >= 0 && curPlay != i)
        {
            atomicState.stopPlayCmd.store(true, std::memory_order_release);
            atomicState.slotState[curPlay].store(static_cast<int>(SlotState::IDLE),
                                                  std::memory_order_release);
        }

        if (cur == SlotState::ARMED)
        {
            // ARMED + NoteOn PLAY → RECORDING (passthrough stays on during rec)
            atomicState.slotState[i].store(static_cast<int>(SlotState::RECORDING),
                                            std::memory_order_release);
            atomicState.startRecCmd[i].store(true, std::memory_order_release);
        }
        else
        {
            // IDLE (or other) + NoteOn PLAY → PLAYING (MIDI/UI-driven, not sequencer).
            // FramePlayerThread checks has_content; reverts to IDLE if empty.
            // Clear seqControlledPlay so the player thread is allowed to
            // restore live passthrough when playback ends.
            atomicState.seqControlledPlay.store(false, std::memory_order_release);
            atomicState.slotState[i].store(static_cast<int>(SlotState::PLAYING),
                                            std::memory_order_release);
            atomicState.activePlaySlot.store(i, std::memory_order_release);
            atomicState.startPlayCmd.store(i, std::memory_order_release);
            atomicState.passthroughEnabled.store(false, std::memory_order_release);
        }
    }
}

// RT NoteOff handler — atomics only
void LuxSampler::handleNoteOff(int note) noexcept
{
    using namespace LuxSamplerConstants;

    if (note >= MIDI_REC_NOTE_BASE && note < MIDI_REC_NOTE_BASE + NUM_SLOTS)
    {
        // ── REC note off ──────────────────────────────────────────────────
        const int i   = note - MIDI_REC_NOTE_BASE;
        const auto cur = static_cast<SlotState>(
            atomicState.slotState[i].load(std::memory_order_relaxed));

        if (cur == SlotState::ARMED)
        {
            atomicState.slotState[i].store(static_cast<int>(SlotState::IDLE),
                                            std::memory_order_release);
        }
        else if (cur == SlotState::RECORDING)
        {
            // Stop recording
            atomicState.stopRecCmd[i].store(true, std::memory_order_release);
            atomicState.slotState[i].store(static_cast<int>(SlotState::IDLE),
                                            std::memory_order_release);
        }
    }
    else if (note >= MIDI_PLAY_NOTE_BASE && note < MIDI_PLAY_NOTE_BASE + NUM_SLOTS)
    {
        // ── PLAY note off ─────────────────────────────────────────────────
        const int i   = note - MIDI_PLAY_NOTE_BASE;
        const auto cur = static_cast<SlotState>(
            atomicState.slotState[i].load(std::memory_order_relaxed));

        if (cur == SlotState::RECORDING)
        {
            // NoteOff PLAY while recording → stop recording
            atomicState.stopRecCmd[i].store(true, std::memory_order_release);
            atomicState.slotState[i].store(static_cast<int>(SlotState::IDLE),
                                            std::memory_order_release);
        }
        else if (cur == SlotState::PLAYING)
        {
            // Stop playback, restore passthrough
            atomicState.stopPlayCmd.store(true, std::memory_order_release);
            atomicState.slotState[i].store(static_cast<int>(SlotState::IDLE),
                                            std::memory_order_release);
            atomicState.activePlaySlot.store(-1, std::memory_order_release);
            atomicState.passthroughEnabled.store(true, std::memory_order_release);
        }
    }
}

// ============================================================================
// Non-RT path — onFrameAssembled (called from udpThread via C hook)
// ============================================================================

bool LuxSampler::onFrameAssembled(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                                     uint16_t pixel_count, uint32_t line_id)
{
    if (!enabled.load(std::memory_order_relaxed)) return false;

    // ── Process pending start/stop commands from RT ───────────────────────
    for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
    {
        if (atomicState.startRecCmd[i].exchange(false, std::memory_order_acq_rel))
        {
            {
                std::lock_guard<std::mutex> lk(slotsMutex_);
                if (!slots[i].isAllocated())
                {
                    slots[i].allocate();
                    log_info("FS", "Slot %d: buffer allocated (%d frames × %zu B)",
                             i, LuxSamplerConstants::MAX_FRAMES_PER_SLOT,
                             sizeof(CapturedFrame));
                }
                else
                {
                    slots[i].frame_count = 0;
                    slots[i].play_head   = 0;
                    slots[i].duration_us = 0;
                    slots[i].has_content = false;
                }
            }
            activeRecSlot.store(i, std::memory_order_release);
            recStartTimeUs = currentTimeUs();
            log_info("FS", "Slot %d: recording started", i);
        }

        if (atomicState.stopRecCmd[i].exchange(false, std::memory_order_acq_rel))
        {
            if (activeRecSlot.load(std::memory_order_relaxed) == i)
            {
                std::lock_guard<std::mutex> lk(slotsMutex_);
                slots[i].has_content = (slots[i].frame_count > 0);
                slots[i].duration_us = currentTimeUs() - recStartTimeUs;
                activeRecSlot.store(-1, std::memory_order_release);
                log_info("FS", "Slot %d: recording stopped — %d frames, %.2f s",
                         i, slots[i].frame_count,
                         static_cast<double>(slots[i].duration_us) / 1e6);
            }
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

    // ── Continuous RAW pass-through into the sampler snapshot ────────────
    // Mirror the live UDP frame into AudioImageBuffers' sampler snapshot
    // whenever no slot is playing.  This keeps Source=Sampler / the sampler
    // visualizer fed by the live stream during IDLE, ARMED and RECORDING
    // states (previously only RECORDING produced a snapshot).
    //
    // During PLAYING: FramePlayerThread is the sole writer of the sampler
    // snapshot — we must not race with it from the UDP thread.
    // isAnySlotPlaying() also covers the seqPlayerHeld case (slotState stays
    // PLAYING while held), so a paused playback keeps its frozen frame.
    if (audioBuffers_ != nullptr && !isAnySlotPlaying())
    {
        const int liveBytes = std::min(static_cast<int>(pixel_count),
                                        LuxSamplerConstants::MAX_PIXELS);
        if (liveBytes > 0)
            audio_image_buffers_snapshot_sampler(audioBuffers_,
                                                  R, G, B, liveBytes);
    }

    // ── Write frame if recording is active ───────────────────────────────

    const int recSlot = activeRecSlot.load(std::memory_order_relaxed);
    if (recSlot < 0) return false;

    // ── Sequencer-gated recording ─────────────────────────────────────────
    // If seqGateSlot >= 0, the sequencer is enabled + playing and the frame
    // must only be captured when the sequencer's current step points at recSlot.
    // seqGateSlot == -1 means no gating (sequencer off or passthrough step).
    const int gate = seqGateSlot.load(std::memory_order_relaxed);
    if (gate >= 0 && gate != recSlot) return false; // gated out — wrong step

    // ── Write frame data under slotsMutex_ ───────────────────────────────
    // slotsMutex_ prevents sampleSpectralForTimeline (message thread) from
    // reading slot.frames while this function frees/reallocates or appends
    // to the CapturedFrame[] array.  The lock is held only for the duration
    // of the frame-write itself to minimise contention at high frame rates.
    int bytes = 0;
    {
        std::lock_guard<std::mutex> lk(slotsMutex_);

        FrameSlot& slot = slots[recSlot];
        if (!slot.isAllocated()) return false;

        // Check max duration / buffer overflow
        const uint64_t elapsed = currentTimeUs() - recStartTimeUs;
        const uint64_t maxUs   = static_cast<uint64_t>(maxDurationS.load() * 1e6f);

        if (elapsed >= maxUs || slot.frame_count >= slot.capacity)
        {
            slot.has_content = (slot.frame_count > 0);
            slot.duration_us = elapsed;
            activeRecSlot.store(-1, std::memory_order_release);
            // Transition to IDLE (overflow — Non-RT write to atomic is safe)
            atomicState.slotState[recSlot].store(static_cast<int>(SlotState::IDLE),
                                                  std::memory_order_release);
            log_info("FS", "Slot %d: overflow — %d frames, %.2f s",
                     recSlot, slot.frame_count,
                     static_cast<double>(elapsed) / 1e6);
            return false;
        }

        // Write the frame
        CapturedFrame& frame = slot.frames[slot.frame_count];
        frame.timestamp_us = elapsed;
        frame.line_id      = line_id;
        frame.pixel_count  = pixel_count;

        bytes = std::min(static_cast<int>(pixel_count),
                         LuxSamplerConstants::MAX_PIXELS);
        std::memcpy(frame.R, R, static_cast<size_t>(bytes));
        std::memcpy(frame.G, G, static_cast<size_t>(bytes));
        std::memcpy(frame.B, B, static_cast<size_t>(bytes));

        ++slot.frame_count;
    }

    // ── Write RAW incoming frame to sampler snapshot during recording ─────
    // This ensures Source=Sampler mode can read the live incoming data while
    // a slot is recording (the sampler output reflects the incoming stream).
    if (audioBuffers_ != nullptr && bytes > 0)
    {
        audio_image_buffers_snapshot_sampler(audioBuffers_, R, G, B, bytes);
    }

    return true;
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
        // Toggle off → stop recording
        atomicState.stopRecCmd[slotIndex].store(true, std::memory_order_release);
        atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::IDLE),
                                                std::memory_order_release);
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

    // Stop playback if active (punch-in or silent start)
    const int curPlay = atomicState.activePlaySlot.load(std::memory_order_relaxed);
    if (curPlay >= 0)
    {
        atomicState.stopPlayCmd.store(true, std::memory_order_release);
        atomicState.activePlaySlot.store(-1, std::memory_order_release);
        atomicState.passthroughEnabled.store(true, std::memory_order_release);
        if (curPlay != slotIndex)
            atomicState.slotState[curPlay].store(static_cast<int>(SlotState::IDLE),
                                                  std::memory_order_release);
    }

    // Start recording immediately (bypass ARMED state — UI one-click record)
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
        // Stop playback → restore passthrough
        atomicState.stopPlayCmd.store(true, std::memory_order_release);
        atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::IDLE),
                                                std::memory_order_release);
        atomicState.activePlaySlot.store(-1,   std::memory_order_release);
        atomicState.passthroughEnabled.store(true, std::memory_order_release);
        return;
    }

    if (st == SlotState::RECORDING || st == SlotState::ARMED) return; // busy

    if (!slots[slotIndex].has_content) return; // nothing recorded yet

    // Stop any other slot that is currently playing
    const int curPlay = atomicState.activePlaySlot.load(std::memory_order_relaxed);
    if (curPlay >= 0 && curPlay != slotIndex)
    {
        atomicState.stopPlayCmd.store(true, std::memory_order_release);
        atomicState.slotState[curPlay].store(static_cast<int>(SlotState::IDLE),
                                              std::memory_order_release);
    }

    // Trigger playback (UI-driven: FramePlayerThread is allowed to restore
    // live passthrough when playback ends — clear sequencer ownership flag).
    atomicState.seqControlledPlay.store(false, std::memory_order_release);
    atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::PLAYING),
                                            std::memory_order_release);
    atomicState.activePlaySlot.store(slotIndex,  std::memory_order_release);
    atomicState.startPlayCmd.store(slotIndex,    std::memory_order_release);
    atomicState.passthroughEnabled.store(false,  std::memory_order_release);
}

void LuxSampler::uiClearSlot(int slotIndex) noexcept
{
    if (slotIndex < 0 || slotIndex >= LuxSamplerConstants::NUM_SLOTS) return;

    // Stop recording / playback first
    const auto st = static_cast<SlotState>(
        atomicState.slotState[slotIndex].load(std::memory_order_relaxed));

    if (st == SlotState::RECORDING || st == SlotState::ARMED)
    {
        atomicState.stopRecCmd[slotIndex].store(true, std::memory_order_release);
    }
    if (st == SlotState::PLAYING)
    {
        atomicState.stopPlayCmd.store(true, std::memory_order_release);
        atomicState.activePlaySlot.store(-1, std::memory_order_release);
        atomicState.passthroughEnabled.store(true, std::memory_order_release);
    }

    atomicState.slotState[slotIndex].store(static_cast<int>(SlotState::IDLE),
                                            std::memory_order_release);

    // Clear the slot data under slotsMutex_ so that sampleSpectralForTimeline
    // (message thread) cannot access slot.frames while clear() frees it.
    std::lock_guard<std::mutex> lk(slotsMutex_);
    slots[slotIndex].clear();
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

// ============================================================================
// Slot management
// ============================================================================

void LuxSampler::clearSlot(int i)
{
    if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) return;

    if (activeRecSlot.load() == i) activeRecSlot.store(-1);

    const int curPlay = atomicState.activePlaySlot.load();
    if (curPlay == i)
    {
        atomicState.stopPlayCmd.store(true);
        atomicState.activePlaySlot.store(-1);
        atomicState.passthroughEnabled.store(true);
    }

    atomicState.slotState[i].store(static_cast<int>(SlotState::IDLE));
    {
        std::lock_guard<std::mutex> lk(slotsMutex_);
        slots[i].clear();
    }
    log_info("FS", "Slot %d cleared", i);
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

    log_info("FS", "copySlotTo: slot %d → %d (%d frames)", srcIdx, dstIdx, count);
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
    {
        std::lock_guard<std::mutex> lk(slotsMutex_);
        for (int s = 0; s < NUM_SLOTS; ++s)
        {
            const FrameSlot& slot = slots[s];

            FsmpSlotHeader shdr {};
            shdr.slot_index  = static_cast<uint8_t>(s);
            shdr.has_content = slot.has_content ? 0x01u : 0x00u;
            shdr.frame_count = static_cast<uint32_t>(slot.frame_count);
            shdr.duration_us = slot.duration_us;
            std::strncpy(shdr.label, slot.label, 63);
            shdr.label[63]   = '\0';
            shdr.slot_crc32  = crc32_compute(reinterpret_cast<const uint8_t*>(&shdr),
                                              offsetof(FsmpSlotHeader, slot_crc32));
            out.write(&shdr, sizeof(shdr));

            if (!slot.has_content || slot.frame_count == 0 || !slot.isAllocated())
                continue;

            const int fc = slot.frame_count; // snapshot under lock
            for (int f = 0; f < fc; ++f)
            {
                const CapturedFrame& fr = slot.frames[f];
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

bool LuxSampler::exportSlotImage(int slotIndex,
                                  const juce::File& file,
                                  bool asPng) const
{
    using namespace LuxSamplerConstants;

    if (slotIndex < 0 || slotIndex >= NUM_SLOTS)
    {
        log_error("FS", "exportSlotImage: invalid slot index %d", slotIndex);
        return false;
    }

    // Snapshot slot under mutex (only metadata + pixel-count of first frame)
    int      width  = 0;
    int      height = 0;
    {
        std::lock_guard<std::mutex> lk(slotsMutex_);
        const FrameSlot& slot = slots[slotIndex];
        if (!slot.has_content || slot.frame_count <= 0 || !slot.isAllocated())
            return false;
        height = slot.frame_count;
        width  = slot.frames[0].pixel_count;
        if (width <= 0)
            return false;
    }

    // Allocate destination JUCE image (RGB, no alpha)
    juce::Image img(juce::Image::RGB, width, height, true);

    {
        juce::Image::BitmapData bmp(img, juce::Image::BitmapData::writeOnly);
        std::lock_guard<std::mutex> lk(slotsMutex_);
        const FrameSlot& slot = slots[slotIndex];

        // Re-check after lock (frame_count could have changed)
        const int rowCount = juce::jmin(height, slot.frame_count);
        for (int y = 0; y < rowCount; ++y)
        {
            const CapturedFrame& fr = slot.frames[y];
            const int            px = juce::jmin(width, static_cast<int>(fr.pixel_count));

            // Use setPixelColour() to be platform-independent: the in-memory byte
            // order of juce::Image::RGB (PixelRGB) is platform-dependent (BGR on
            // some targets, RGB on others). Writing raw bytes with a hard-coded
            // order produced incorrect colours (export looked monochrome).
            for (int x = 0; x < px; ++x)
            {
                bmp.setPixelColour(x, y,
                                   juce::Colour(fr.R[x], fr.G[x], fr.B[x]));
            }
        }
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
             width, height, asPng ? "PNG" : "JPEG");
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

    setMidiChannel(static_cast<int>(hdr.midi_channel));
    setOctaveOffset(static_cast<int>(hdr.octave_offset));
    setMaxDuration(hdr.max_duration_s);

    const int numSlotsInFile = static_cast<int>(
        std::min(hdr.num_slots, static_cast<uint32_t>(NUM_SLOTS)));

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

    // ── Slot header + frames (under lock) ────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(slotsMutex_);
        const FrameSlot& slot = slots[slotIndex];

        FsmpSlotHeader shdr {};
        shdr.slot_index  = static_cast<uint8_t>(slotIndex);
        shdr.has_content = slot.has_content ? 0x01u : 0x00u;
        shdr.frame_count = static_cast<uint32_t>(slot.frame_count);
        shdr.duration_us = slot.duration_us;
        std::strncpy(shdr.label, slot.label, 63);
        shdr.label[63]   = '\0';
        shdr.slot_crc32  = crc32_compute(reinterpret_cast<const uint8_t*>(&shdr),
                                          offsetof(FsmpSlotHeader, slot_crc32));
        out.write(&shdr, sizeof(shdr));

        if (slot.has_content && slot.frame_count > 0 && slot.isAllocated())
        {
            const int fc = slot.frame_count;
            for (int f = 0; f < fc; ++f)
            {
                const CapturedFrame& fr = slot.frames[f];
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
    paramsXml.setAttribute("startFrac",      static_cast<double>(getSlotStartFrac(slotIndex)));
    paramsXml.setAttribute("endFrac",        static_cast<double>(getSlotEndFrac(slotIndex)));
    paramsXml.setAttribute("speed",          static_cast<double>(getSlotSpeed(slotIndex)));
    paramsXml.setAttribute("loopMode",       static_cast<int>(getSlotLoopMode(slotIndex)));
    paramsXml.setAttribute("resumeMode",     static_cast<int>(getSlotResumeMode(slotIndex)));
    paramsXml.setAttribute("blendAmount",    static_cast<double>(getSlotBlendAmount(slotIndex)));
    paramsXml.setAttribute("attackLen",      static_cast<double>(getSlotAttackLen(slotIndex)));
    paramsXml.setAttribute("decayLen",       static_cast<double>(getSlotDecayLen(slotIndex)));
    paramsXml.setAttribute("brightnessLift", static_cast<double>(getSlotBrightnessLift(slotIndex)));
    paramsXml.setAttribute("trebleCut",      static_cast<double>(getSlotTrebleCut(slotIndex)));
    paramsXml.setAttribute("bassCut",        static_cast<double>(getSlotBassCut(slotIndex)));
    paramsXml.setAttribute("fadeCurveType",  static_cast<int>(getSlotFadeCurveType(slotIndex)));
    paramsXml.setAttribute("fadeCurvePower", static_cast<double>(getSlotFadeCurvePower(slotIndex)));
    paramsXml.setAttribute("label",          juce::String(getSlotLabel(slotIndex)));

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
            {
                setSlotStartFrac     (slotIndex, static_cast<float>(xml->getDoubleAttribute("startFrac",      0.0)));
                setSlotEndFrac       (slotIndex, static_cast<float>(xml->getDoubleAttribute("endFrac",        1.0)));
                setSlotSpeed         (slotIndex, static_cast<float>(xml->getDoubleAttribute("speed",          1.0)));
                setSlotLoopMode      (slotIndex, static_cast<LoopMode>(xml->getIntAttribute("loopMode",       1)));
                setSlotResumeMode    (slotIndex, xml->getIntAttribute("resumeMode", 0) != 0);
                setSlotBlendAmount   (slotIndex, static_cast<float>(xml->getDoubleAttribute("blendAmount",    0.0)));
                setSlotAttackLen     (slotIndex, static_cast<float>(xml->getDoubleAttribute("attackLen",      0.0)));
                setSlotDecayLen      (slotIndex, static_cast<float>(xml->getDoubleAttribute("decayLen",       0.0)));
                setSlotBrightnessLift(slotIndex, static_cast<float>(xml->getDoubleAttribute("brightnessLift", 0.0)));
                setSlotTrebleCut     (slotIndex, static_cast<float>(xml->getDoubleAttribute("trebleCut",      0.0)));
                setSlotBassCut       (slotIndex, static_cast<float>(xml->getDoubleAttribute("bassCut",        0.0)));
                setSlotFadeCurveType (slotIndex, static_cast<FadeCurveType>(xml->getIntAttribute("fadeCurveType", 0)));
                setSlotFadeCurvePower(slotIndex, static_cast<float>(xml->getDoubleAttribute("fadeCurvePower", 1.0)));
                const juce::String lbl = xml->getStringAttribute("label", "");
                if (lbl.isNotEmpty())
                    setSlotLabel(slotIndex, lbl.toRawUTF8());
            }
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
    if (doubleBuffer != nullptr)
    {
        extern sp3ctra_config_t g_sp3ctra_config;
        if (g_sp3ctra_config.luxstral_source_type == 0 /* IMAGE_SOURCE_SAMPLER */)
        {
            pthread_mutex_lock(&doubleBuffer->mutex);
            // Zero additive synthesis input — identical to multithreading.c silence branch.
            std::memset(doubleBuffer->preprocessed_data.additive.grayscale, 0,
                        sizeof(doubleBuffer->preprocessed_data.additive.grayscale));
            std::memset(doubleBuffer->preprocessed_data.additive.notes, 0,
                        sizeof(doubleBuffer->preprocessed_data.additive.notes));
            doubleBuffer->preprocessed_data.additive.contrast_factor = 0.0f;
            // FIX(silence): Also zero polyphonic.* when LuxSynth source is SAMPLER.
            // Without this, LuxSynth keeps generating audio from the last played
            // frame during STEP_EMPTY / rtStop / LoopMode::NONE end.
            if (g_sp3ctra_config.luxsynth_source_type == 0 /* IMAGE_SOURCE_SAMPLER */)
            {
                std::memset(doubleBuffer->preprocessed_data.polyphonic.grayscale, 0,
                            sizeof(doubleBuffer->preprocessed_data.polyphonic.grayscale));
                std::memset(doubleBuffer->preprocessed_data.polyphonic.magnitudes, 0,
                            sizeof(doubleBuffer->preprocessed_data.polyphonic.magnitudes));
                doubleBuffer->preprocessed_data.polyphonic.valid = 0;
            }
            doubleBuffer->dataReady = 2; /* sampler source tag — consumer gating intact */
            pthread_mutex_unlock(&doubleBuffer->mutex);
        }
    }
}

void FramePlayerThread::run()
{
    auto& state = sampler.getAtomicState();
    log_info("FS", "FramePlayerThread running");

    while (!threadShouldExit())
    {
        // Wait for a startPlay command
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

        FrameSlot& slot = sampler.getSlot(slotToPlay);

        if (!slot.has_content || slot.frame_count == 0 || !slot.isAllocated())
        {
            log_warning("FS", "Slot %d: play requested but no content", slotToPlay);
            state.slotState[slotToPlay].store(static_cast<int>(SlotState::IDLE),
                                              std::memory_order_release);
            state.activePlaySlot.store(-1, std::memory_order_release);
            // Only restore live passthrough when the play was NOT triggered by
            // the sequencer.  When the sequencer drives playback and the slot
            // is empty, the live UDP stream must stay suppressed — only an
            // explicit STEP_LIVE step or rtStop() may re-enable it.
            if (!state.seqControlledPlay.load(std::memory_order_relaxed))
                state.passthroughEnabled.store(true, std::memory_order_release);
            continue;
        }

        log_info("FS", "Slot %d: playback start — %d frames, %.2f s",
                 slotToPlay, slot.frame_count,
                 static_cast<double>(slot.duration_us) / 1e6);

        // ── Fixed-rate injection: 1000 lines/sec regardless of speed ─────
        // speed = frame-skip factor per 1ms tick:
        //   x1   → advance play_head by 1  every 1ms → 1000 lps
        //   x32  → advance play_head by 32 every 1ms → still 1000 lps output
        //           but timeline traversed 32× faster
        //   x0.5 → advance play_head by 0 or 1 alternating → 500 lps effective,
        //           1000 lps output (frame repetition = slow-motion)
        constexpr uint64_t kPeriodUs = 1000; // 1 ms = 1000 lines/sec
        uint64_t lastInjectUs = currentTimeUs();
        float    frameAcc     = 0.0f;       // sub-frame accumulator

        int      prevStartFrame = -1;
        int      prevEndFrame   = -1;
        bool     firstRangeInit = true;
        LoopMode prevLoopMode   = sampler.getSlotLoopMode(slotToPlay);
        int      direction      = (prevLoopMode == LoopMode::INVERSE) ? -1 : 1;
        slot.play_head          = 0; // set on first range init below
        bool stoppedByNoneMode  = false; // tracks if NONE loop reached end

        // ── Inner playback loop ───────────────────────────────────────────
        while (!threadShouldExit())
        {
            // External stop commands
            if (state.stopPlayCmd.exchange(false, std::memory_order_acq_rel))
                break;
            if (static_cast<SlotState>(state.slotState[slotToPlay].load(
                    std::memory_order_relaxed)) != SlotState::PLAYING)
                break;
            const int pending = state.startPlayCmd.load(std::memory_order_relaxed);
            if (pending >= 0 && pending != slotToPlay)
                break;

            // ── Pause / hold: freeze play_head, re-anchor injection timer ─
            if (sampler.isSeqPlayerHeld())
            {
                lastInjectUs = currentTimeUs(); // no burst on resume
                Thread::sleep(2);
                continue;
            }

            // ── Re-read play params every iteration (on-the-fly) ─────────
            const float    p_start = sampler.getSlotStartFrac(slotToPlay);
            const float    p_end   = sampler.getSlotEndFrac(slotToPlay);
            const float    p_speed = juce::jlimit(0.01f, 32.0f,
                                         sampler.getSlotSpeed(slotToPlay));
            const LoopMode p_loop  = sampler.getSlotLoopMode(slotToPlay);

            const int startFrame = juce::jlimit(0, slot.frame_count - 1,
                static_cast<int>(p_start * static_cast<float>(slot.frame_count)));
            const int endFrame   = juce::jlimit(startFrame + 1, slot.frame_count,
                static_cast<int>(p_end * static_cast<float>(slot.frame_count)));
            const int zoneLen    = endFrame - startFrame;

            // ── Loop-mode change → update direction ───────────────────────
            if (p_loop != prevLoopMode)
            {
                prevLoopMode = p_loop;
                switch (p_loop)
                {
                    case LoopMode::LOOP:
                    case LoopMode::NONE:
                        direction = 1;
                        break;
                    case LoopMode::INVERSE:
                        direction = -1;
                        break;
                    case LoopMode::PINGPONG:
                        break; // keep current direction
                }
            }

            // ── Range change → clamp play_head, reset accumulator ─────────
            if (startFrame != prevStartFrame || endFrame != prevEndFrame)
            {
                const bool wasFirst = firstRangeInit;
                firstRangeInit = false;
                prevStartFrame = startFrame;
                prevEndFrame   = endFrame;

                if (wasFirst && sampler.getSlotResumeMode(slotToPlay))
                {
                    const int saved = sampler.getLastPlayHead(slotToPlay);
                    if (saved >= startFrame && saved < endFrame)
                    {
                        slot.play_head = saved;
                        if (prevLoopMode == LoopMode::PINGPONG)
                            direction = sampler.getLastDirection(slotToPlay);
                    }
                    else
                    {
                        slot.play_head = (direction > 0) ? startFrame : endFrame - 1;
                    }
                }
                else if (slot.play_head < startFrame || slot.play_head >= endFrame)
                {
                    slot.play_head = (direction > 0) ? startFrame : endFrame - 1;
                }

                // First frame of new range is due immediately
                lastInjectUs = currentTimeUs();
                frameAcc     = 0.0f;
            }

            // ── Wait for next 1ms injection tick ─────────────────────────
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

            // ── Advance play_head by fractional speed per tick ────────────
            // Integer step: 0 is allowed for speed<1 (repeats current frame).
            frameAcc    += p_speed;
            const int step = static_cast<int>(frameAcc);
            frameAcc    -= static_cast<float>(step);

            if (step > 0 && zoneLen > 0)
            {
                slot.play_head += step * direction;

                // ── Boundary / loop-mode handling ─────────────────────────
                const bool fwdBound = (direction > 0 && slot.play_head >= endFrame);
                const bool bwdBound = (direction < 0 && slot.play_head <  startFrame);
                if (fwdBound || bwdBound)
                {
                    bool stop = false;
                    switch (p_loop)
                    {
                        case LoopMode::NONE:
                            stop = true;
                            stoppedByNoneMode = true;
                            break;
                        case LoopMode::LOOP:
                            direction      = 1;
                            // Wrap within zone, accounting for possible overshoot
                            slot.play_head = startFrame
                                + ((slot.play_head - startFrame) % zoneLen
                                   + zoneLen) % zoneLen;
                            frameAcc = 0.0f;
                            log_debug("FS", "Slot %d: loop", slotToPlay);
                            break;
                        case LoopMode::INVERSE:
                            direction      = -1;
                            slot.play_head = (endFrame - 1)
                                - (((endFrame - 1) - slot.play_head) % zoneLen
                                   + zoneLen) % zoneLen;
                            frameAcc = 0.0f;
                            break;
                        case LoopMode::PINGPONG:
                            direction      = -direction;
                            slot.play_head = juce::jlimit(startFrame,
                                                          endFrame - 1,
                                                          slot.play_head);
                            frameAcc = 0.0f;
                            break;
                    }
                    if (stop) break;
                }
            }

            // Safety clamp — prevents out-of-bounds access
            slot.play_head = juce::jlimit(startFrame, endFrame - 1, slot.play_head);

            const CapturedFrame& frame = slot.frames[slot.play_head];

            // ── Working buffers — attack + blend applied before BOTH outputs ──────
            // Zero-filled so pixels beyond pixel_count are silent (black = 0).
            const int nb = std::min(static_cast<int>(frame.pixel_count),
                                    LuxSamplerConstants::MAX_PIXELS);
            uint8_t workR[LuxSamplerConstants::MAX_PIXELS] {};
            uint8_t workG[LuxSamplerConstants::MAX_PIXELS] {};
            uint8_t workB[LuxSamplerConstants::MAX_PIXELS] {};
            std::memcpy(workR, frame.R, static_cast<size_t>(nb));
            std::memcpy(workG, frame.G, static_cast<size_t>(nb));
            std::memcpy(workB, frame.B, static_cast<size_t>(nb));

            // ── Read fade curve params once per frame (shared by all fades) ─────
            const auto   p_curveType  = sampler.getSlotFadeCurveType(slotToPlay);
            const float  p_curvePower = sampler.getSlotFadeCurvePower(slotToPlay);

            // ── Attack fade-in (exposure ramp): white at start → normal ─────────
            // attackLen is normalised over [startFrame, endFrame).
            // At headOffset=0 ramp=1 (fully white/silent); at attackLen ramp=0.
            {
                const float p_attack = sampler.getSlotAttackLen(slotToPlay);
                if (p_attack > 0.001f)
                {
                    const int   totalFrames  = endFrame - startFrame;
                    const float attackFrames = p_attack * static_cast<float>(totalFrames);
                    const int   headOffset   = (direction > 0)
                        ? (slot.play_head - startFrame)
                        : (endFrame - 1 - slot.play_head);
                    if (attackFrames > 0.5f &&
                        headOffset < static_cast<int>(attackFrames + 0.5f))
                    {
                        // t: 0 at start bound → 1 at end of attack zone
                        const float t = static_cast<float>(headOffset) / attackFrames;
                        // Curve-shaped ramp: 1 (white) at t=0, 0 (normal) at t=1
                        const float ramp = 1.0f - applyFadeCurve(t, p_curveType, p_curvePower);
                        for (int px = 0; px < nb; ++px)
                        {
                            workR[px] = static_cast<uint8_t>(
                                workR[px] + ramp * (255.0f - (float)workR[px]));
                            workG[px] = static_cast<uint8_t>(
                                workG[px] + ramp * (255.0f - (float)workG[px]));
                            workB[px] = static_cast<uint8_t>(
                                workB[px] + ramp * (255.0f - (float)workB[px]));
                        }
                    }
                }
            }

            // ── Decay fade-out (exposure ramp): normal → white at end ─────────────
            // Mirrors attack but measured from the end bound.
            // At tailOffset=0 (end bound) ramp=1 (white); at decayLen ramp=0 (normal).
            {
                const float p_decay = sampler.getSlotDecayLen(slotToPlay);
                if (p_decay > 0.001f)
                {
                    const int   totalFrames = endFrame - startFrame;
                    const float decayFrames = p_decay * static_cast<float>(totalFrames);
                    // tailOffset: distance from the active end bound (direction-aware)
                    const int   tailOffset  = (direction > 0)
                        ? (endFrame - 1 - slot.play_head)
                        : (slot.play_head - startFrame);
                    if (decayFrames > 0.5f &&
                        tailOffset < static_cast<int>(decayFrames + 0.5f))
                    {
                        // t: 0 at end bound → 1 at start of decay zone
                        const float t = static_cast<float>(tailOffset) / decayFrames;
                        // Curve-shaped ramp: 1 (white) at t=0, 0 (normal) at t=1
                        const float ramp = 1.0f - applyFadeCurve(t, p_curveType, p_curvePower);
                        for (int px = 0; px < nb; ++px)
                        {
                            workR[px] = static_cast<uint8_t>(
                                workR[px] + ramp * (255.0f - (float)workR[px]));
                            workG[px] = static_cast<uint8_t>(
                                workG[px] + ramp * (255.0f - (float)workG[px]));
                            workB[px] = static_cast<uint8_t>(
                                workB[px] + ramp * (255.0f - (float)workB[px]));
                        }
                    }
                }
            }

            // ── Global brightness lift (uniform exposure) ─────────────────────────────
            // brightnessLift=0 → no change; brightnessLift=1 → all pixels → white.
            {
                const float p_lift = sampler.getSlotBrightnessLift(slotToPlay);
                if (p_lift > 0.001f)
                {
                    for (int px = 0; px < nb; ++px)
                    {
                        workR[px] = static_cast<uint8_t>(
                            workR[px] + p_lift * (255.0f - (float)workR[px]));
                        workG[px] = static_cast<uint8_t>(
                            workG[px] + p_lift * (255.0f - (float)workG[px]));
                        workB[px] = static_cast<uint8_t>(
                            workB[px] + p_lift * (255.0f - (float)workB[px]));
                    }
                }
            }

            // ── Treble cut: hard cutoff on right-half pixels ────────────────────────
            // Right-half pixels = high-frequency content.
            // The slider controls the cutoff position:
            //   p_tc=0 → no masking; p_tc=1 → entire right half → white.
            // A short transition zone (~16 px) softens the edge.
            {
                const float p_tc = sampler.getSlotTrebleCut(slotToPlay);
                if (p_tc > 0.001f)
                {
                    const int halfPx    = nb / 2;
                    const int halfWidth = nb - halfPx;
                    // Cutoff: everything at or beyond this pixel → white
                    const int cutoffPx  = halfPx + static_cast<int>(
                        (1.0f - p_tc) * static_cast<float>(halfWidth));
                    // Short transition zone for smooth edge (≤16 px)
                    const int transW    = std::max(1, std::min(16, halfWidth / 8));
                    const int transStart = std::max(halfPx, cutoffPx - transW);
                    for (int px = transStart; px < nb; ++px)
                    {
                        const float tLin = (px >= cutoffPx) ? 1.0f
                            : static_cast<float>(px - transStart)
                              / static_cast<float>(std::max(1, cutoffPx - transStart));
                        const float t = applyFadeCurve(tLin, p_curveType, p_curvePower);
                        workR[px] = static_cast<uint8_t>(
                            workR[px] + t * (255.0f - (float)workR[px]));
                        workG[px] = static_cast<uint8_t>(
                            workG[px] + t * (255.0f - (float)workG[px]));
                        workB[px] = static_cast<uint8_t>(
                            workB[px] + t * (255.0f - (float)workB[px]));
                    }
                }
            }

            // ── Bass cut: hard cutoff on left-half pixels ───────────────────────────
            // Left-half pixels = low-frequency content.
            // The slider controls the cutoff position (mirrored):
            //   p_bc=0 → no masking; p_bc=1 → entire left half → white.
            // A short transition zone (~16 px) softens the edge.
            {
                const float p_bc = sampler.getSlotBassCut(slotToPlay);
                if (p_bc > 0.001f)
                {
                    const int halfPx    = nb / 2;
                    // Cutoff: everything at or below this pixel → white
                    // p_bc=0 → cutoffPx=-1 (nothing); p_bc=1 → cutoffPx=halfPx-1
                    const int cutoffPx  = static_cast<int>(
                        p_bc * static_cast<float>(halfPx)) - 1;
                    // Short transition zone for smooth edge (≤16 px)
                    const int transW    = std::max(1, std::min(16, halfPx / 8));
                    const int transEnd  = std::min(halfPx - 1, cutoffPx + transW);
                    for (int px = 0; px <= transEnd; ++px)
                    {
                        const float tLin = (px <= cutoffPx) ? 1.0f
                            : 1.0f - static_cast<float>(px - cutoffPx)
                                     / static_cast<float>(std::max(1, transEnd - cutoffPx));
                        const float t = applyFadeCurve(tLin, p_curveType, p_curvePower);
                        workR[px] = static_cast<uint8_t>(
                            workR[px] + t * (255.0f - (float)workR[px]));
                        workG[px] = static_cast<uint8_t>(
                            workG[px] + t * (255.0f - (float)workG[px]));
                        workB[px] = static_cast<uint8_t>(
                            workB[px] + t * (255.0f - (float)workB[px]));
                    }
                }
            }

            // ── Snapshot pure sampler frame BEFORE live blend ─────────────────
            // So the visualizer can show the sampler output in isolation.
            audio_image_buffers_snapshot_sampler(audioBuffers, workR, workG, workB, nb);

            // ── Live darken-blend: min(sample, live) weighted by blendAmount ─────
            // blendAmount=0 → pure playback; blendAmount=1 → full darken blend.
            // Applied AFTER attack/decay/lift so the blend sees the fully
            // processed sample rather than the raw captured frame.
            {
                const float p_blend = sampler.getSlotBlendAmount(slotToPlay);
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
                const int   smpFreeze  = seqDriven ? 0 : g_sp3ctra_config.sampler_freeze_mode;
                const int   liveFreeze = g_sp3ctra_config.image_freeze_mode;
                const float liveOp     = g_sp3ctra_config.image_live_opacity;
                const float smpOp      = g_sp3ctra_config.image_sampler_opacity;
                const int   fadeInMs   = g_sp3ctra_config.sampler_fade_in_ms;

                // ── Transport fade-in: HOLD/STOP → PLAY ───────────────────────────
                // Linear ramp [0→1] over sampler_fade_in_ms ms.
                // Uses FramePlayerThread member state for cross-iteration tracking.
                {
                    const uint64_t nowUs = currentTimeUs();
                    if (seqDriven)
                    {
                        // Sequencer-driven: always full gain, no UI transport fade management.
                        // The sequencer step advance is the only gating authority; the
                        // sampler Transport UI must not attenuate the injected frames.
                        transportFadeRamp_   = 1.0f;
                        transportPrevFreeze_ = 0; /* record as PLAY for next iteration */
                    }
                    else if (smpFreeze == 0)  // PLAY
                    {
                        if (transportPrevFreeze_ != 0 && fadeInMs > 0)
                        {
                            // Transition detected (HOLD/STOP → PLAY): reset to silence.
                            transportFadeStartUs_ = nowUs;
                            transportFadeRamp_    = 0.0f;
                        }
                        if (fadeInMs > 0 && transportFadeRamp_ < 1.0f)
                        {
                            const float elapsedMs = static_cast<float>(
                                nowUs - transportFadeStartUs_) / 1000.0f;
                            transportFadeRamp_ = juce::jlimit(
                                0.0f, 1.0f,
                                elapsedMs / static_cast<float>(fadeInMs));
                        }
                        else if (fadeInMs == 0)
                        {
                            transportFadeRamp_ = 1.0f; // no fade configured
                        }
                    }
                    else
                    {
                        // HOLD (1) or STOP (2): reset ramp so the next PLAY triggers fade.
                        transportFadeRamp_ = 0.0f;
                    }
                    transportPrevFreeze_ = smpFreeze;
                }

                // Effective sampler opacity = user opacity × fade ramp.
                // At ramp=0 → effectiveSmpOp=0 → frame=white (silence).
                // At ramp=1 → effectiveSmpOp=smpOp → normal brightness.
                //
                // Source-aware: when Source=S (pure sampler), bypass the MIX
                // crossfader opacity — only the transport fade ramp applies.
                // The crossfader balance (smpOp) is only meaningful in MIX mode.
                const int srcType = g_sp3ctra_config.luxstral_source_type;
                const float effectiveSmpOp = (srcType == 0 /* IMAGE_SOURCE_SAMPLER */)
                    ? transportFadeRamp_               // Source=S: full opacity, fade only
                    : smpOp * transportFadeRamp_;      // Source=M: crossfader × fade

                if (smpFreeze != 2) // Do not inject when sampler transport is STOP
                {
                    // 1. Apply sampler opacity + transport fade-in ramp
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
                    //    Skip entirely for Source=S (pure sampler — no live contribution).
                    if (srcType != 0 /* not IMAGE_SOURCE_SAMPLER */
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

                    // 3. Write mixed frame to AudioImageBuffers (the visual mix bus)
                    uint8_t* wR = nullptr;
                    uint8_t* wG = nullptr;
                    uint8_t* wB = nullptr;
                    if (audio_image_buffers_start_write(audioBuffers, &wR, &wG, &wB) == 0)
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
            // (not the raw RGB buffers). attack + blend are applied to workR/G/B
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
                extern sp3ctra_config_t g_sp3ctra_config;
                const int src = g_sp3ctra_config.luxstral_source_type;
                if (doubleBuffer != nullptr
                    && src == 0 /* IMAGE_SOURCE_SAMPLER */)
                {
                    PreprocessedImageData ppData {};
                    PipelineConfig sampler_cfg = pipeline_build_config_sampler();
                    // FIX(routing): Sequencer-driven playback must not be silenced
                    // by the pipeline envelope when sampler_freeze_mode=STOP (2).
                    // Override freeze_mode to PLAY so ENVELOPE_SAMPLER processes
                    // the frame normally regardless of the Transport UI state.
                    if (state.seqControlledPlay.load(std::memory_order_relaxed))
                        sampler_cfg.freeze_mode = 0; /* force PLAY — sequencer active */
                    if (pipeline_process_frame(workR, workG, workB, &sampler_cfg, &ppData) == 0)
                    {
                        ppData.timestamp_us = static_cast<uint64_t>(currentTimeUs());
                        pthread_mutex_lock(&doubleBuffer->mutex);
                        // FIX(routing): Do NOT overwrite the entire preprocessed_data struct.
                        // polyphonic.* (LuxSynth) may be fed by a different source (e.g. Live)
                        // and must not be clobbered with sampler-derived data.
                        // Only copy sections owned by the sampler/LuxStral path.
                        doubleBuffer->preprocessed_data.additive    = ppData.additive;
                        doubleBuffer->preprocessed_data.photowave   = ppData.photowave;
                        doubleBuffer->preprocessed_data.stereo      = ppData.stereo;
                        doubleBuffer->preprocessed_data.strokeforge = ppData.strokeforge;
                        doubleBuffer->preprocessed_data.timestamp_us = ppData.timestamp_us;
                        // Only update polyphonic if LuxSynth source is also SAMPLER.
                        if (g_sp3ctra_config.luxsynth_source_type == 0 /* IMAGE_SOURCE_SAMPLER */)
                            doubleBuffer->preprocessed_data.polyphonic = ppData.polyphonic;
                        doubleBuffer->dataReady = 2; /* 2 = sampler source tag */
                        pthread_mutex_unlock(&doubleBuffer->mutex);
                    }
                }
            }

            // Update UI playhead cursor (atomic write — Non-RT safe)
            sampler.notifyPlayHead(slotToPlay, slot.play_head);
        }

        // Save last play position and direction for resume mode.
        // Saving direction allows PINGPONG to resume in the correct sense.
        sampler.saveLastPlayHead(slotToPlay, slot.play_head);
        sampler.saveLastDirection(slotToPlay, direction);

        // ── Inject white (silence) frame when playback ends ───────────────
        // Triggered by any of:
        //   • LoopMode::NONE reached end of play zone          (stoppedByNoneMode)
        //   • A STEP_EMPTY sequencer step is now active        (seqSilentStepActive)
        //   • An explicit silence command from rtStop() or
        //     triggerStep(STEP_EMPTY) when no slot was playing (injectSilenceCmd)
        // Normal bank→bank transitions satisfy none of these conditions, so the
        // cut between consecutive banks remains seamless (no white-frame gap).
        {
            const bool doSilence =
                stoppedByNoneMode
                || sampler.isSeqSilentStepActive()
                || state.injectSilenceCmd.exchange(false, std::memory_order_acq_rel);

            // FIX(live): When the sequencer's STEP_LIVE is active, the live UDP
            // stream is the intended content — NEVER overwrite with white.
            // seqLiveStepActive (not passthroughEnabled!) distinguishes
            // sequencer-STEP_LIVE from normal idle/stop where silence IS needed.
            const bool liveStep = state.seqLiveStepActive.load(std::memory_order_relaxed);

            if (doSilence && !liveStep)
            {
                if (stoppedByNoneMode)
                    log_info("FS", "Slot %d: NONE mode end — injecting white frame (silence)",
                             slotToPlay);
                else
                    log_info("FS", "Slot %d: stop/empty step — injecting white frame (silence)",
                             slotToPlay);
                injectWhiteFrame();
            }
            else if (doSilence && liveStep)
            {
                log_info("FS", "Slot %d: seqLiveStep active — skipping white frame",
                         slotToPlay);
            }
        }

        log_info("FS", "Slot %d: playback stopped (head=%d/%d)",
                 slotToPlay, slot.play_head, slot.frame_count);

        // Restore state only if no new play command is already pending for this
        // slot.  Without this guard, a rapid stop+play from uiPlaySlot() can
        // set slotState=PLAYING and startPlayCmd=slotToPlay while this thread is
        // still in the tail of the inner loop; the thread would then overwrite
        // the PLAYING state with IDLE and the subsequent startPlayCmd would be
        // consumed with no active playback — the "resume cursor stays but play
        // does not start" bug.
        {
            const int pendingCmd = state.startPlayCmd.load(std::memory_order_acquire);
            if (pendingCmd != slotToPlay &&
                static_cast<SlotState>(state.slotState[slotToPlay].load(
                    std::memory_order_acquire)) == SlotState::PLAYING)
            {
                state.slotState[slotToPlay].store(static_cast<int>(SlotState::IDLE),
                                                  std::memory_order_release);
                state.activePlaySlot.store(-1, std::memory_order_release);
                // When the sequencer owns the play session (seqControlledPlay),
                // do NOT restore live passthrough here — the sequencer (STEP_LIVE
                // or rtStop) is the only authority that re-enables the live UDP
                // stream.  This prevents LoopMode::NONE from leaking live audio
                // between the sample end and the next sequencer step boundary.
                if (!state.seqControlledPlay.load(std::memory_order_relaxed))
                    state.passthroughEnabled.store(true, std::memory_order_release);
            }
        }
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
