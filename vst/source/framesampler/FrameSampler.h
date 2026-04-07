#pragma once

/*
 * FrameSampler.h
 *
 * Main controller for the FrameSampler subsystem.
 * Records, stores and replays the Sp3ctra CIS image stream via MIDI commands.
 *
 * RT safety contract (enforced throughout):
 *   - processMidi() runs on the audio thread → atomics ONLY, no alloc, no lock, no I/O
 *   - onFrameAssembled() runs on udpThread (Non-RT) → alloc allowed on first use
 *   - FramePlayerThread runs Non-RT → alloc/lock/I/O allowed
 *
 * Architecture: see docs/SPEC_FrameSampler.html §9
 */

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
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
namespace FrameSamplerConstants
{
    constexpr int     NUM_SLOTS           = 12;
    constexpr int     MAX_FRAMES_PER_SLOT = 30000; // 200 DPI, 10 s, ×1.5 margin
    constexpr int     MAX_PIXELS          = 3456;  // Fixed 400 DPI (FIXED_BUFFER_PIXELS)
    constexpr float   MAX_DURATION_S      = 10.0f;

    // MIDI note bases (C0 = MIDI 12 — Ableton/GM convention)
    constexpr int MIDI_REC_NOTE_BASE  = 12; // C0..B0 → slots 0..11
    constexpr int MIDI_PLAY_NOTE_BASE = 24; // C1..B1 → slots 0..11

    // .fsmp binary file format
    constexpr uint32_t FSMP_MAGIC      = 0x46534D50u; // "FSMP"
    constexpr uint16_t FSMP_VERSION    = 0x0001u;
    constexpr uint32_t FSMP_EOF_MARKER = 0xDEADBEEFu;
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
    uint8_t  R[FrameSamplerConstants::MAX_PIXELS] {};
    uint8_t  G[FrameSamplerConstants::MAX_PIXELS] {};
    uint8_t  B[FrameSamplerConstants::MAX_PIXELS] {};
};

// ============================================================================
// SlotState — per-slot state machine states
// ============================================================================
enum class SlotState : int
{
    IDLE      = 0,
    ARMED     = 1,
    RECORDING = 2,
    PLAYING   = 3
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
                           FrameSamplerConstants::MAX_FRAMES_PER_SLOT);
            capacity = FrameSamplerConstants::MAX_FRAMES_PER_SLOT;
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
// FrameSamplerAtomicState — lock-free RT ↔ Non-RT interface
// RT path (processBlock) writes these; Non-RT threads read them.
// ============================================================================
struct FrameSamplerAtomicState
{
    std::atomic<int>  slotState[FrameSamplerConstants::NUM_SLOTS];
    std::atomic<int>  activePlaySlot    { -1 };   // -1 = none playing
    std::atomic<bool> passthroughEnabled { true }; // false during PLAYING

    // Command pulses: set by RT, cleared (exchange) by Non-RT threads
    std::atomic<bool> startRecCmd[FrameSamplerConstants::NUM_SLOTS];
    std::atomic<bool> stopRecCmd[FrameSamplerConstants::NUM_SLOTS];
    std::atomic<int>  startPlayCmd { -1 };   // slot index, -1 = no command
    std::atomic<bool> stopPlayCmd  { false };

    FrameSamplerAtomicState() noexcept
    {
        for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
        {
            slotState[i].store(static_cast<int>(SlotState::IDLE),
                               std::memory_order_relaxed);
            startRecCmd[i].store(false, std::memory_order_relaxed);
            stopRecCmd[i].store(false, std::memory_order_relaxed);
        }
    }

    FrameSamplerAtomicState(const FrameSamplerAtomicState&) = delete;
    FrameSamplerAtomicState& operator=(const FrameSamplerAtomicState&) = delete;
};

// ============================================================================
// Forward declarations
// ============================================================================
class FrameSampler;

// ============================================================================
// FramePlayerThread — Non-RT thread that injects recorded frames into synthesis
// ============================================================================
class FramePlayerThread final : public juce::Thread
{
public:
    FramePlayerThread(FrameSampler& sampler,
                      AudioImageBuffers* audioBuffers,
                      DoubleBuffer*      doubleBuffer);
    ~FramePlayerThread() override;
    void run() override;

private:
    FrameSampler&      sampler;
    AudioImageBuffers* audioBuffers;
    DoubleBuffer*      doubleBuffer; // for updating preprocessed_data during playback

    static uint64_t currentTimeUs() noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FramePlayerThread)
};

// ============================================================================
// FrameSampler — main controller
// ============================================================================
class FrameSampler
{
public:
    FrameSampler();
    ~FrameSampler();

    // =========================================================================
    // RT path — processBlock  (atomics ONLY — no alloc, no lock, no I/O)
    // =========================================================================
    void processMidi(const juce::MidiBuffer& midiBuffer);

    // =========================================================================
    // Non-RT path — called by udpThread hook after complete line assembled
    // Returns true if the frame was captured for recording.
    // =========================================================================
    bool onFrameAssembled(const uint8_t* R, const uint8_t* G, const uint8_t* B,
                          uint16_t pixel_count, uint32_t line_id);

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
    bool isEnabled() const noexcept
    {
        return enabled.load(std::memory_order_relaxed);
    }

    // =========================================================================
    // Configuration (message thread / APVTS listener)
    // =========================================================================
    void setEnabled(bool e)          noexcept { enabled.store(e); }
    void setMidiChannel(int ch)      noexcept { midiChannel.store(ch); }   // 1–16
    void setOctaveOffset(int off)    noexcept { octaveOffset.store(off); } // -2..+2
    void setMaxDuration(float secs)  noexcept { maxDurationS.store(secs); }// 1..10

    int   getMidiChannel()  const noexcept { return midiChannel.load(); }
    int   getOctaveOffset() const noexcept { return octaveOffset.load(); }
    float getMaxDuration()  const noexcept { return maxDurationS.load(); }

    // =========================================================================
    // Slot management (Non-RT)
    // =========================================================================
    void clearSlot(int slotIndex);
    void clearAllSlots();

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
    // Internal access for FramePlayerThread
    // =========================================================================
    FrameSlot&               getSlot(int i)  noexcept { return slots[i]; }
    FrameSamplerAtomicState& getAtomicState() noexcept { return atomicState; }

    // =========================================================================
    // Static singleton pointer — set in ctor, cleared in dtor.
    // Used by C hook functions in FrameSampler.cpp.
    // Limitation: only one VST instance supported per process.
    // =========================================================================
    static FrameSampler* s_instance;

private:
    // -------------------------------------------------------------------------
    // RT state (atomics only)
    // -------------------------------------------------------------------------
    FrameSamplerAtomicState atomicState;

    std::atomic<bool>  enabled     { false };
    std::atomic<int>   midiChannel { 1 };
    std::atomic<int>   octaveOffset{ 0 };
    std::atomic<float> maxDurationS{ 10.0f };

    // -------------------------------------------------------------------------
    // Non-RT state
    // -------------------------------------------------------------------------
    FrameSlot slots[FrameSamplerConstants::NUM_SLOTS];

    std::atomic<int> activeRecSlot { -1 }; // -1 = not recording
    uint64_t         recStartTimeUs = 0;   // set when recording starts

    // -------------------------------------------------------------------------
    // Player thread
    // -------------------------------------------------------------------------
    std::unique_ptr<FramePlayerThread> playerThread;

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static uint64_t currentTimeUs() noexcept;

    // RT handlers — atomics only
    void handleNoteOn (int note, int velocity) noexcept;
    void handleNoteOff(int note)               noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FrameSampler)
};
