/*
 * FrameSampler.cpp
 *
 * Implementation of the FrameSampler subsystem.
 * See FrameSampler.h for architecture notes.
 */

#include "FrameSampler.h"

extern "C" {
    #include "audio_image_buffers.h"
    #include "multithreading.h"         // DoubleBuffer + pthread mutex
    #include "../processing/image_preprocessor.h" // image_preprocess_frame, PreprocessedImageData
    #include "logger.h"
}

#include <sys/time.h>
#include <algorithm>
#include <cstring>

// ============================================================================
// Static instance (singleton for C hook access)
// ============================================================================
FrameSampler* FrameSampler::s_instance = nullptr;

// ============================================================================
// C-linkage hook functions — called from udpThread() in multithreading.c
// ============================================================================
extern "C"
{
    void frame_sampler_on_frame_assembled(const uint8_t* R,
                                           const uint8_t* G,
                                           const uint8_t* B,
                                           uint16_t       pixel_count,
                                           uint32_t       line_id)
    {
        if (FrameSampler::s_instance != nullptr)
            FrameSampler::s_instance->onFrameAssembled(R, G, B, pixel_count, line_id);
    }

    int frame_sampler_is_playing(void)
    {
        if (FrameSampler::s_instance == nullptr)
            return 0;
        if (!FrameSampler::s_instance->isAnySlotPlaying())
            return 0;
        // When sampler transport is STOP (sampler_freeze_mode==2), the sampler
        // must NOT gate the live audio/visual path even if a slot is scheduled.
        // Returning 0 allows the UDP thread to keep writing to AudioImageBuffers
        // and updating db->preprocessed_data with live data uninterrupted.
        extern sp3ctra_config_t g_sp3ctra_config;
        return (g_sp3ctra_config.sampler_freeze_mode != 2) ? 1 : 0;
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
// Timing helper (shared between FrameSampler and FramePlayerThread)
// ============================================================================
uint64_t FrameSampler::currentTimeUs() noexcept
{
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000ULL +
           static_cast<uint64_t>(tv.tv_usec);
}

// ============================================================================
// FrameSampler — constructor / destructor
// ============================================================================

FrameSampler::FrameSampler()
{
    s_instance = this;
    for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
    {
        currentPlayHead[i].store(0,  std::memory_order_relaxed);
        lastPlayHead[i].store(0,     std::memory_order_relaxed);
        lastDirection[i].store(1,    std::memory_order_relaxed); // forward by default
    }
    log_info("FS", "FrameSampler initialised — %d slots, %d frames/slot max, %.1f s/slot max",
             FrameSamplerConstants::NUM_SLOTS,
             FrameSamplerConstants::MAX_FRAMES_PER_SLOT,
             static_cast<double>(FrameSamplerConstants::MAX_DURATION_S));
}

FrameSampler::~FrameSampler()
{
    stopPlayerThread();
    s_instance = nullptr;
    log_info("FS", "FrameSampler destroyed");
}

// ============================================================================
// RT path — processMidi
// HARD CONSTRAINT: atomics ONLY. No alloc, no mutex, no I/O, no logging.
// ============================================================================

void FrameSampler::processMidi(const juce::MidiBuffer& midiBuffer)
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
void FrameSampler::handleNoteOn(int note, int velocity) noexcept
{
    juce::ignoreUnused(velocity);
    using namespace FrameSamplerConstants;

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
void FrameSampler::handleNoteOff(int note) noexcept
{
    using namespace FrameSamplerConstants;

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

bool FrameSampler::onFrameAssembled(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                                     uint16_t pixel_count, uint32_t line_id)
{
    if (!enabled.load(std::memory_order_relaxed)) return false;

    // ── Process pending start/stop commands from RT ───────────────────────
    for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
    {
        if (atomicState.startRecCmd[i].exchange(false, std::memory_order_acq_rel))
        {
            if (!slots[i].isAllocated())
            {
                slots[i].allocate();
                log_info("FS", "Slot %d: buffer allocated (%d frames × %zu B)",
                         i, FrameSamplerConstants::MAX_FRAMES_PER_SLOT,
                         sizeof(CapturedFrame));
            }
            else
            {
                slots[i].frame_count = 0;
                slots[i].play_head   = 0;
                slots[i].duration_us = 0;
                slots[i].has_content = false;
            }
            activeRecSlot.store(i, std::memory_order_release);
            recStartTimeUs = currentTimeUs();
            log_info("FS", "Slot %d: recording started", i);
        }

        if (atomicState.stopRecCmd[i].exchange(false, std::memory_order_acq_rel))
        {
            if (activeRecSlot.load(std::memory_order_relaxed) == i)
            {
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
                                   FrameSamplerConstants::MAX_PIXELS);
        std::memcpy(liveR_, R, static_cast<size_t>(livePixelCount_));
        std::memcpy(liveG_, G, static_cast<size_t>(livePixelCount_));
        std::memcpy(liveB_, B, static_cast<size_t>(livePixelCount_));
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

    const int bytes = std::min(static_cast<int>(pixel_count),
                               FrameSamplerConstants::MAX_PIXELS);
    std::memcpy(frame.R, R, static_cast<size_t>(bytes));
    std::memcpy(frame.G, G, static_cast<size_t>(bytes));
    std::memcpy(frame.B, B, static_cast<size_t>(bytes));

    ++slot.frame_count;
    return true;
}

// ============================================================================
// FrameSampler::getLiveFrame — Non-RT (called by FramePlayerThread)
// ============================================================================
void FrameSampler::getLiveFrame(uint8_t* outR, uint8_t* outG, uint8_t* outB,
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

void FrameSampler::uiToggleRecord(int slotIndex) noexcept
{
    using namespace FrameSamplerConstants;
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

void FrameSampler::uiPlaySlot(int slotIndex) noexcept
{
    if (slotIndex < 0 || slotIndex >= FrameSamplerConstants::NUM_SLOTS) return;

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

void FrameSampler::uiClearSlot(int slotIndex) noexcept
{
    if (slotIndex < 0 || slotIndex >= FrameSamplerConstants::NUM_SLOTS) return;

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

    // Clear the slot data (Non-RT: heap free allowed here)
    slots[slotIndex].clear();
}

// ============================================================================
// Thread lifecycle
// ============================================================================

void FrameSampler::startPlayerThread(AudioImageBuffers* audioBuffers,
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

    playerThread = std::make_unique<FramePlayerThread>(*this, audioBuffers, doubleBuffer);
    playerThread->startThread(juce::Thread::Priority::normal);
    log_info("FS", "FramePlayerThread started");
}

void FrameSampler::stopPlayerThread()
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

void FrameSampler::clearSlot(int i)
{
    if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return;

    if (activeRecSlot.load() == i) activeRecSlot.store(-1);

    const int curPlay = atomicState.activePlaySlot.load();
    if (curPlay == i)
    {
        atomicState.stopPlayCmd.store(true);
        atomicState.activePlaySlot.store(-1);
        atomicState.passthroughEnabled.store(true);
    }

    atomicState.slotState[i].store(static_cast<int>(SlotState::IDLE));
    slots[i].clear();
    log_info("FS", "Slot %d cleared", i);
}

void FrameSampler::clearAllSlots()
{
    for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
        clearSlot(i);
    log_info("FS", "All slots cleared");
}

// ============================================================================
// copySlotTo — Non-RT slot duplication (message thread only)
// Deep-copies frame_count frames + play parameters from src to dst.
// Stops any ongoing activity on the destination slot first.
// ============================================================================
void FrameSampler::copySlotTo(int srcIdx, int dstIdx)
{
    if (srcIdx < 0 || srcIdx >= FrameSamplerConstants::NUM_SLOTS) return;
    if (dstIdx < 0 || dstIdx >= FrameSamplerConstants::NUM_SLOTS) return;
    if (srcIdx == dstIdx) return;

    const FrameSlot& src = slots[srcIdx];
    if (!src.has_content || src.frame_count == 0)
    {
        log_warning("FS", "copySlotTo: source slot %d has no content", srcIdx);
        return;
    }

    // Stop any ongoing activity on the destination
    atomicState.slotState[dstIdx].store(static_cast<int>(SlotState::IDLE),
                                        std::memory_order_release);
    if (atomicState.activePlaySlot.load(std::memory_order_acquire) == dstIdx)
        atomicState.activePlaySlot.store(-1, std::memory_order_release);

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

    // Copy play parameters
    setSlotStartFrac (dstIdx, getSlotStartFrac (srcIdx));
    setSlotEndFrac   (dstIdx, getSlotEndFrac   (srcIdx));
    setSlotSpeed     (dstIdx, getSlotSpeed      (srcIdx));
    setSlotLoopMode  (dstIdx, getSlotLoopMode   (srcIdx));
    setSlotResumeMode(dstIdx, getSlotResumeMode (srcIdx));

    log_info("FS", "copySlotTo: slot %d → %d (%d frames)", srcIdx, dstIdx, count);
}

// ============================================================================
// Slot info queries
// ============================================================================

SlotState FrameSampler::getSlotState(int i) const noexcept
{
    if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return SlotState::IDLE;
    return static_cast<SlotState>(
        atomicState.slotState[i].load(std::memory_order_relaxed));
}

int FrameSampler::getSlotFrameCount(int i) const noexcept
{
    if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0;
    return slots[i].frame_count;
}

uint64_t FrameSampler::getSlotDurationUs(int i) const noexcept
{
    if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0;
    return slots[i].duration_us;
}

bool FrameSampler::slotHasContent(int i) const noexcept
{
    if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return false;
    return slots[i].has_content;
}

const char* FrameSampler::getSlotLabel(int i) const noexcept
{
    if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return "";
    return slots[i].label;
}

void FrameSampler::setSlotLabel(int i, const char* label) noexcept
{
    if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS || label == nullptr) return;
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

bool FrameSampler::saveToFile(const juce::File& file) const
{
    using namespace FrameSamplerConstants;

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

        for (int f = 0; f < slot.frame_count; ++f)
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

    // ── EOF marker ───────────────────────────────────────────────────────
    const uint32_t eof = FSMP_EOF_MARKER;
    out.write(&eof, sizeof(eof));

    log_info("FS", "Saved to '%s' (%d slots)",
             file.getFullPathName().toRawUTF8(), NUM_SLOTS);
    return true;
}

bool FrameSampler::loadFromFile(const juce::File& file)
{
    using namespace FrameSamplerConstants;

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

        slots[idx].clear();
        if (!shdr.has_content || shdr.frame_count == 0) continue;

        slots[idx].allocate();
        slots[idx].has_content = true;
        slots[idx].duration_us = shdr.duration_us;
        std::strncpy(slots[idx].label, shdr.label, 63);
        slots[idx].label[63] = '\0';

        const int toLoad = static_cast<int>(
            std::min(shdr.frame_count, static_cast<uint32_t>(MAX_FRAMES_PER_SLOT)));

        for (int f = 0; f < toLoad; ++f)
        {
            FsmpFrameHeader fhdr {};
            if (in.read(&fhdr, sizeof(fhdr)) != sizeof(fhdr)) break;

            CapturedFrame& fr = slots[idx].frames[f];
            fr.timestamp_us = fhdr.timestamp_us;

            uint32_t lid = 0; uint16_t pc = 0;
            in.read(&lid, sizeof(lid));
            in.read(&pc,  sizeof(pc));
            fr.line_id    = lid;
            fr.pixel_count = pc;

            const int bytes = std::min(static_cast<int>(pc), MAX_PIXELS);
            in.read(fr.R, bytes);
            in.read(fr.G, bytes);
            in.read(fr.B, bytes);
            ++slots[idx].frame_count;
        }
    }

    log_info("FS", "Loaded from '%s' (%d slots)",
             file.getFullPathName().toRawUTF8(), numSlotsInFile);
    return true;
}

// ============================================================================
// FramePlayerThread
// ============================================================================

FramePlayerThread::FramePlayerThread(FrameSampler& sampler,
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

        // Per-slot play parameters are re-read every iteration so changes
        // made via the UI sliders take effect immediately (on-the-fly).
        // We track prevStartFrame/prevEndFrame to detect range changes and
        // re-anchor the reference timestamps + loopStartUs accordingly.
        int      prevStartFrame  = -1;
        int      prevEndFrame    = -1;
        bool     firstRangeInit  = true; // true until first range is established
        // Initialise prevLoopMode from current atomic so the first iteration
        // does NOT trigger a spurious mode-change reset.
        LoopMode prevLoopMode = sampler.getSlotLoopMode(slotToPlay);
        uint64_t fwdRefTs     = 0;
        uint64_t bwdRefTs     = 0;

        // Initial direction derives from the starting loop mode.
        int direction = (prevLoopMode == LoopMode::INVERSE) ? -1 : 1;
        slot.play_head       = 0; // set on first range initialisation below
        uint64_t loopStartUs = currentTimeUs();

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

            // ── Pause / hold: freeze play_head, keep current frame visible ─
            // Re-anchor loopStartUs each held iteration so that resuming does
            // not produce a burst of back-to-back injections (no time debt).
            if (sampler.isSeqPlayerHeld())
            {
                loopStartUs = currentTimeUs();
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

            // ── Detect loop-mode change → update direction + re-anchor timing ──
            // Two bugs fixed here:
            //  1. INV→LOOP left direction=-1 → play_head would oscillate at
            //     startFrame-1 (tight boundary loop).
            //  2. LOOP→INV (or any mode change) reset loopStartUs to now() but
            //     did NOT account for where play_head currently is.  When
            //     play_head is near startFrame, rawTs(backward) ≈ full_duration,
            //     so elapsed(0) < scaledTs(huge) → thread keeps sleeping for
            //     the full duration → appears frozen.
            //  Fix: set loopStartUs so that the current frame's scheduled offset
            //  has already elapsed, making the transition seamless.
            if (p_loop != prevLoopMode)
            {
                prevLoopMode = p_loop;
                switch (p_loop)
                {
                    case LoopMode::LOOP:
                    case LoopMode::NONE:
                        direction = 1;                          // always forward
                        break;
                    case LoopMode::INVERSE:
                        direction = -1;                         // always backward
                        break;
                    case LoopMode::PINGPONG:
                        break;                                  // keep current direction
                }

                // Re-anchor loopStartUs relative to the current play_head in
                // the new direction so the first frame plays without delay.
                // Guard: prevStartFrame >= 0 ensures fwdRefTs/bwdRefTs are valid.
                if (prevStartFrame >= 0 &&
                    slot.play_head >= 0 && slot.play_head < slot.frame_count)
                {
                    const uint64_t curTs  = slot.frames[slot.play_head].timestamp_us;
                    const uint64_t rawTs  = (direction > 0)
                        ? (curTs > fwdRefTs ? curTs - fwdRefTs : 0ULL)
                        : (bwdRefTs > curTs ? bwdRefTs - curTs : 0ULL);
                    const float    spd    = juce::jlimit(0.01f, 32.0f,
                                               sampler.getSlotSpeed(slotToPlay));
                    const uint64_t scaled = static_cast<uint64_t>(
                                               static_cast<float>(rawTs) / spd);
                    loopStartUs = currentTimeUs() - scaled; // current frame is "due now"
                }
                else
                {
                    loopStartUs = currentTimeUs();
                }
            }

            // Detect range change → re-anchor ref timestamps, clamp play_head
            if (startFrame != prevStartFrame || endFrame != prevEndFrame)
            {
                const bool wasFirst = firstRangeInit;
                firstRangeInit = false;
                prevStartFrame = startFrame;
                prevEndFrame   = endFrame;
                fwdRefTs       = slot.frames[startFrame].timestamp_us;
                bwdRefTs       = slot.frames[endFrame - 1].timestamp_us;

                if (wasFirst && sampler.getSlotResumeMode(slotToPlay))
                {
                    // Resume mode: restore last stopped position if it is within
                    // the current [startFrame, endFrame) range.
                    const int saved = sampler.getLastPlayHead(slotToPlay);
                    if (saved >= startFrame && saved < endFrame)
                    {
                        slot.play_head = saved;
                        // For PINGPONG, also restore the last direction so the
                        // playback resumes in the same sense it was going when
                        // it stopped — not always restarting forward.
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

                // Re-anchor loopStartUs relative to the current play_head
                // timestamp so the first frame at the new position is due
                // immediately.
                // Without this, starting at a mid-recording position (e.g.
                // Resume mode with play_head at T=5 s) would set elapsed=0
                // while scaledTs=5 s/speed → the thread would wait that full
                // duration before injecting the first frame (= audible latency).
                {
                    const uint64_t curTs  = slot.frames[slot.play_head].timestamp_us;
                    const uint64_t rawTs  = (direction > 0)
                        ? (curTs > fwdRefTs ? curTs - fwdRefTs : 0ULL)
                        : (bwdRefTs > curTs ? bwdRefTs - curTs : 0ULL);
                    const float    spd    = juce::jlimit(0.01f, 32.0f,
                                               sampler.getSlotSpeed(slotToPlay));
                    const uint64_t scaled = static_cast<uint64_t>(
                                               static_cast<float>(rawTs) / spd);
                    loopStartUs = currentTimeUs() - scaled; // frame is "due now"
                }
            }

            // ── Boundary / loop-mode handling ─────────────────────────────
            const bool fwdBound = (direction > 0 && slot.play_head >= endFrame);
            const bool bwdBound = (direction < 0 && slot.play_head <  startFrame);
            if (fwdBound || bwdBound)
            {
                bool stop = false;
                switch (p_loop)
                {
                    case LoopMode::NONE:
                        stop = true;
                        break;
                    case LoopMode::LOOP:
                        direction      = 1;          // guard: ensure forward after INV/PING
                        slot.play_head = startFrame;
                        loopStartUs    = currentTimeUs();
                        log_debug("FS", "Slot %d: loop", slotToPlay);
                        break;
                    case LoopMode::INVERSE:
                        direction      = -1;
                        slot.play_head = endFrame - 1;
                        loopStartUs    = currentTimeUs();
                        break;
                    case LoopMode::PINGPONG:
                        direction      = -direction;
                        slot.play_head = (direction > 0) ? startFrame : endFrame - 1;
                        loopStartUs    = currentTimeUs();
                        break;
                }
                if (stop) break;
                continue;
            }

            const CapturedFrame& frame = slot.frames[slot.play_head];

            // ── Timestamp scheduling with speed + direction ───────────────
            const uint64_t rawTs = (direction > 0)
                ? (frame.timestamp_us > fwdRefTs ? frame.timestamp_us - fwdRefTs : 0ULL)
                : (bwdRefTs > frame.timestamp_us ? bwdRefTs - frame.timestamp_us : 0ULL);
            const uint64_t scaledTs = static_cast<uint64_t>(
                static_cast<float>(rawTs) / p_speed);
            const uint64_t elapsed = currentTimeUs() - loopStartUs;

            if (elapsed < scaledTs)
            {
                const uint64_t wait = scaledTs - elapsed;
                if (wait > 2000) Thread::sleep(1);
                else             Thread::yield();
                continue;
            }

            // ── Working buffers — attack + blend applied before BOTH outputs ──────
            // Zero-filled so pixels beyond pixel_count are silent (black = 0).
            const int nb = std::min(static_cast<int>(frame.pixel_count),
                                    FrameSamplerConstants::MAX_PIXELS);
            uint8_t workR[FrameSamplerConstants::MAX_PIXELS] {};
            uint8_t workG[FrameSamplerConstants::MAX_PIXELS] {};
            uint8_t workB[FrameSamplerConstants::MAX_PIXELS] {};
            std::memcpy(workR, frame.R, static_cast<size_t>(nb));
            std::memcpy(workG, frame.G, static_cast<size_t>(nb));
            std::memcpy(workB, frame.B, static_cast<size_t>(nb));

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
                        const float ramp = 1.0f -
                            static_cast<float>(headOffset) / attackFrames;
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
                        const float ramp = 1.0f -
                            static_cast<float>(tailOffset) / decayFrames;
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

            // ── Treble cut: fade right-half pixels toward white ─────────────────────
            // Right-half pixels = high-frequency content (closer to the far edge
            // of the illuminated strip).
            {
                const float p_tc = sampler.getSlotTrebleCut(slotToPlay);
                if (p_tc > 0.001f)
                {
                    const int halfPx = nb / 2;
                    for (int px = halfPx; px < nb; ++px)
                    {
                        // Linear taper: no lift at halfPx, full lift at nb-1
                        const float t = p_tc * static_cast<float>(px - halfPx)
                                        / static_cast<float>(std::max(1, nb - 1 - halfPx));
                        workR[px] = static_cast<uint8_t>(
                            workR[px] + t * (255.0f - (float)workR[px]));
                        workG[px] = static_cast<uint8_t>(
                            workG[px] + t * (255.0f - (float)workG[px]));
                        workB[px] = static_cast<uint8_t>(
                            workB[px] + t * (255.0f - (float)workB[px]));
                    }
                }
            }

            // ── Bass cut: fade left-half pixels toward white ──────────────────────────
            // Left-half pixels = low-frequency content (closer to the sensor edge).
            {
                const float p_bc = sampler.getSlotBassCut(slotToPlay);
                if (p_bc > 0.001f)
                {
                    const int halfPx = nb / 2;
                    for (int px = 0; px < halfPx; ++px)
                    {
                        // Linear taper: full lift at px=0, no lift at halfPx-1
                        const float t = p_bc * static_cast<float>(halfPx - 1 - px)
                                        / static_cast<float>(std::max(1, halfPx - 1));
                        workR[px] = static_cast<uint8_t>(
                            workR[px] + t * (255.0f - (float)workR[px]));
                        workG[px] = static_cast<uint8_t>(
                            workG[px] + t * (255.0f - (float)workG[px]));
                        workB[px] = static_cast<uint8_t>(
                            workB[px] + t * (255.0f - (float)workB[px]));
                    }
                }
            }

            // ── Live darken-blend: min(sample, live) weighted by blendAmount ─────
            // blendAmount=0 → pure playback; blendAmount=1 → full darken blend.
            // Applied AFTER attack/decay/lift so the blend sees the fully
            // processed sample rather than the raw captured frame.
            {
                const float p_blend = sampler.getSlotBlendAmount(slotToPlay);
                if (p_blend > 0.001f)
                {
                    uint8_t lvR[FrameSamplerConstants::MAX_PIXELS] {};
                    uint8_t lvG[FrameSamplerConstants::MAX_PIXELS] {};
                    uint8_t lvB[FrameSamplerConstants::MAX_PIXELS] {};
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
                const int   smpFreeze  = g_sp3ctra_config.sampler_freeze_mode;
                const int   liveFreeze = g_sp3ctra_config.image_freeze_mode;
                const float liveOp     = g_sp3ctra_config.image_live_opacity;
                const float smpOp      = g_sp3ctra_config.image_sampler_opacity;
                const int   fadeInMs   = g_sp3ctra_config.sampler_fade_in_ms;

                // ── Transport fade-in: HOLD/STOP → PLAY ───────────────────────────
                // Linear ramp [0→1] over sampler_fade_in_ms ms.
                // Uses FramePlayerThread member state for cross-iteration tracking.
                {
                    const uint64_t nowUs = currentTimeUs();
                    if (smpFreeze == 0)  // PLAY
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
                const float effectiveSmpOp = smpOp * transportFadeRamp_;

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
                    if (liveFreeze != 2 && liveOp > 0.001f)
                    {
                        uint8_t lvR[FrameSamplerConstants::MAX_PIXELS] {};
                        uint8_t lvG[FrameSamplerConstants::MAX_PIXELS] {};
                        uint8_t lvB[FrameSamplerConstants::MAX_PIXELS] {};
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
            // Guard: skip when sampler transport is STOP (sampler_freeze_mode==2).
            // In that state the UDP thread owns db->preprocessed_data (live
            // preprocessing).  Writing here with notes=0 would silence the live
            // audio in bursts synchronised with the step sequencer — the audible
            // "rhythmic impact in the live" bug.
            // ---------------------------------------------------------------
            {
                extern sp3ctra_config_t g_sp3ctra_config;
                if (doubleBuffer != nullptr
                    && g_sp3ctra_config.sampler_freeze_mode != 2)
                {
                    PreprocessedImageData ppData {};
                    if (image_preprocess_frame_sampler(workR, workG, workB, &ppData) == 0)
                    {
                        ppData.timestamp_us = static_cast<uint64_t>(currentTimeUs());
                        pthread_mutex_lock(&doubleBuffer->mutex);
                        doubleBuffer->preprocessed_data = ppData;
                        doubleBuffer->dataReady = 1;
                        pthread_mutex_unlock(&doubleBuffer->mutex);
                    }
                }
            }

            // Update UI playhead cursor (atomic write — Non-RT safe)
            sampler.notifyPlayHead(slotToPlay, slot.play_head);

            slot.play_head += direction;
        }

        // Save last play position and direction for resume mode.
        // Saving direction allows PINGPONG to resume in the correct sense.
        sampler.saveLastPlayHead(slotToPlay, slot.play_head);
        sampler.saveLastDirection(slotToPlay, direction);

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
// FrameSampler::sampleBrightnessForTimeline — Non-RT only
// ============================================================================

void FrameSampler::sampleBrightnessForTimeline(int    slotIdx,
                                                float* outBrightness,
                                                int    count) const noexcept
{
    if (slotIdx < 0 || slotIdx >= FrameSamplerConstants::NUM_SLOTS
        || outBrightness == nullptr || count <= 0)
        return;

    const FrameSlot& slot = slots[slotIdx];
    if (!slot.has_content || slot.frame_count == 0 || !slot.isAllocated())
    {
        for (int k = 0; k < count; ++k)
            outBrightness[k] = 0.0f;
        return;
    }

    // For each timeline column, pick one frame and average 8 evenly-spaced
    // pixels.  Total cost: O(8 * count) — safe from the message thread.
    for (int k = 0; k < count; ++k)
    {
        const int frameIdx = juce::jlimit(0, slot.frame_count - 1,
                                          k * slot.frame_count / count);
        const CapturedFrame& f = slot.frames[frameIdx];

        const int pc   = juce::jlimit(1, FrameSamplerConstants::MAX_PIXELS,
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
// FrameSampler::sampleSpectralForTimeline — Non-RT only
// ============================================================================
void FrameSampler::sampleSpectralForTimeline(int    slotIdx,
                                              float* outBass,
                                              float* outTreble,
                                              int    count) const noexcept
{
    if (slotIdx < 0 || slotIdx >= FrameSamplerConstants::NUM_SLOTS
        || outBass == nullptr || outTreble == nullptr || count <= 0)
        return;

    const FrameSlot& slot = slots[slotIdx];
    if (!slot.has_content || slot.frame_count == 0 || !slot.isAllocated())
    {
        for (int k = 0; k < count; ++k)
            outBass[k] = outTreble[k] = 0.0f;
        return;
    }

    // For each column pick one representative frame and sample 4 pixels
    // from each half.  O(8 * count) — safe on the message thread.
    for (int k = 0; k < count; ++k)
    {
        const int frameIdx = juce::jlimit(0, slot.frame_count - 1,
                                          k * slot.frame_count / count);
        const CapturedFrame& f = slot.frames[frameIdx];
        const int pc   = juce::jlimit(2, FrameSamplerConstants::MAX_PIXELS,
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
