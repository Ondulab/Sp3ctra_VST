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
        if (FrameSampler::s_instance != nullptr)
            return FrameSampler::s_instance->isAnySlotPlaying() ? 1 : 0;
        return 0;
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
            // IDLE (or other) + NoteOn PLAY → PLAYING
            // FramePlayerThread checks has_content; reverts to IDLE if empty
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

    // ── Write frame if recording is active ───────────────────────────────
    const int recSlot = activeRecSlot.load(std::memory_order_relaxed);
    if (recSlot < 0) return false;

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

    // Trigger playback
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
            state.passthroughEnabled.store(true, std::memory_order_release);
            continue;
        }

        log_info("FS", "Slot %d: playback start — %d frames, %.2f s",
                 slotToPlay, slot.frame_count,
                 static_cast<double>(slot.duration_us) / 1e6);

        slot.play_head       = 0;
        uint64_t loopStartUs = currentTimeUs();

        // ── Inner playback loop ───────────────────────────────────────────
        while (!threadShouldExit())
        {
            // Stop command?
            if (state.stopPlayCmd.exchange(false, std::memory_order_acq_rel))
                break;

            // State changed externally?
            if (static_cast<SlotState>(state.slotState[slotToPlay].load(
                    std::memory_order_relaxed)) != SlotState::PLAYING)
                break;

            // Higher-priority slot wants to play?
            const int pending = state.startPlayCmd.load(std::memory_order_relaxed);
            if (pending >= 0 && pending != slotToPlay)
                break;

            // Seamless loop: reset head when buffer ends
            if (slot.play_head >= slot.frame_count)
            {
                slot.play_head = 0;
                loopStartUs    = currentTimeUs();
                log_debug("FS", "Slot %d: loop", slotToPlay);
                continue;
            }

            const CapturedFrame& frame   = slot.frames[slot.play_head];
            const uint64_t       elapsed = currentTimeUs() - loopStartUs;

            // Wait until it is time to inject this frame
            if (elapsed < frame.timestamp_us)
            {
                const uint64_t wait = frame.timestamp_us - elapsed;
                if (wait > 2000)
                    Thread::sleep(1);
                else
                    Thread::yield();
                continue;
            }

            // Inject frame into AudioImageBuffers (replaces live UDP feed)
            uint8_t* wR = nullptr;
            uint8_t* wG = nullptr;
            uint8_t* wB = nullptr;

            if (audio_image_buffers_start_write(audioBuffers, &wR, &wG, &wB) == 0)
            {
                const int n = std::min(static_cast<int>(frame.pixel_count),
                                       FrameSamplerConstants::MAX_PIXELS);
                std::memcpy(wR, frame.R, static_cast<size_t>(n));
                std::memcpy(wG, frame.G, static_cast<size_t>(n));
                std::memcpy(wB, frame.B, static_cast<size_t>(n));
                audio_image_buffers_complete_write(audioBuffers);
            }

            // ---------------------------------------------------------------
            // CRITICAL: update db->preprocessed_data from the playback frame.
            // synth_AudioProcess uses db->preprocessed_data for audio generation
            // (not the raw RGB buffers). Without this, the audio engine keeps
            // using the live-stream preprocessing computed by the UDP thread.
            // ---------------------------------------------------------------
            if (doubleBuffer != nullptr)
            {
                // Prepare full-size buffers (zero-fill if pixel_count < MAX_PIXELS)
                uint8_t tmpR[FrameSamplerConstants::MAX_PIXELS] {};
                uint8_t tmpG[FrameSamplerConstants::MAX_PIXELS] {};
                uint8_t tmpB[FrameSamplerConstants::MAX_PIXELS] {};

                const int nb = std::min(static_cast<int>(frame.pixel_count),
                                        FrameSamplerConstants::MAX_PIXELS);
                std::memcpy(tmpR, frame.R, static_cast<size_t>(nb));
                std::memcpy(tmpG, frame.G, static_cast<size_t>(nb));
                std::memcpy(tmpB, frame.B, static_cast<size_t>(nb));

                PreprocessedImageData ppData {};
                if (image_preprocess_frame(tmpR, tmpG, tmpB, &ppData) == 0)
                {
                    // Ensure timestamp is non-zero so synth_AudioProcess
                    // takes the has_preprocessed branch.
                    ppData.timestamp_us = static_cast<uint64_t>(currentTimeUs());

                    pthread_mutex_lock(&doubleBuffer->mutex);
                    doubleBuffer->preprocessed_data = ppData;
                    doubleBuffer->dataReady = 1;
                    pthread_mutex_unlock(&doubleBuffer->mutex);
                }
            }

            ++slot.play_head;
        }

        log_info("FS", "Slot %d: playback stopped (head=%d/%d)",
                 slotToPlay, slot.play_head, slot.frame_count);

        // Restore state (in case RT did not do it already)
        if (static_cast<SlotState>(state.slotState[slotToPlay].load()) ==
            SlotState::PLAYING)
        {
            state.slotState[slotToPlay].store(static_cast<int>(SlotState::IDLE),
                                              std::memory_order_release);
            state.activePlaySlot.store(-1, std::memory_order_release);
            state.passthroughEnabled.store(true, std::memory_order_release);
        }
    }

    // Final safety: always restore passthrough on thread exit
    state.passthroughEnabled.store(true, std::memory_order_release);
    log_info("FS", "FramePlayerThread exiting");
}
