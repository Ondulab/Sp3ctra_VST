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
// LoopMode — per-slot playback loop behaviour
// ============================================================================
enum class LoopMode : int
{
    NONE     = 0, // Play once, then stop and restore passthrough
    LOOP     = 1, // Loop forward: wrap play_head back to startFrame on overflow
    INVERSE  = 2, // Loop backward: play in reverse, wrap back to endFrame
    PINGPONG = 3  // Alternate forward / backward each time a boundary is reached
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
    /** Index of the slot currently active in playback (-1 = none). Non-RT safe. */
    int getActivePlaySlot() const noexcept
    {
        return atomicState.activePlaySlot.load(std::memory_order_acquire);
    }

    // =========================================================================
    // Configuration (message thread / APVTS listener)
    // =========================================================================
    void setEnabled(bool e)          noexcept { enabled.store(e); }
    void setMidiChannel(int ch)      noexcept { midiChannel.store(ch); }   // 1–16
    void setOctaveOffset(int off)    noexcept { octaveOffset.store(off); } // -2..+2
    void setMaxDuration(float secs)  noexcept { maxDurationS.store(secs); }// 1..10

    /** Sequencer-gated recording.
     *  Called from processBlock (RT) every audio block.
     *  - gateSlot >= 0 : only capture frames when activeRecSlot == gateSlot
     *                    (sequencer is enabled + playing + step points at that bank)
     *  - gateSlot == -1 : no gating — frames are always captured (sequencer disabled
     *                     or current step is a passthrough/empty step)
     *  RT-safe: single atomic store, read only in onFrameAssembled (Non-RT). */
    void setSeqGateSlot(int gateSlot) noexcept
    {
        seqGateSlot.store(gateSlot, std::memory_order_relaxed);
    }

    int   getMidiChannel()  const noexcept { return midiChannel.load(); }
    int   getOctaveOffset() const noexcept { return octaveOffset.load(); }
    float getMaxDuration()  const noexcept { return maxDurationS.load(); }

    // =========================================================================
    // Per-slot play parameters (message/timer thread — Non-RT)
    // Written by UI controls; read by FramePlayerThread for playback behaviour.
    // =========================================================================
    void setSlotStartFrac(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].startFrac.store(juce::jlimit(0.0f, 1.0f, v),
                                          std::memory_order_relaxed);
    }
    void setSlotEndFrac(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].endFrac.store(juce::jlimit(0.0f, 1.0f, v),
                                        std::memory_order_relaxed);
    }
    void setSlotSpeed(int i, float v) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].speed.store(juce::jlimit(0.01f, 32.0f, v),
                                      std::memory_order_relaxed);
    }
    void setSlotLoopMode(int i, LoopMode m) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].loopMode.store(static_cast<int>(m),
                                         std::memory_order_relaxed);
    }
    void setSlotPriority(int i, bool p) noexcept
    {
        if (i >= 0 && i < FrameSamplerConstants::NUM_SLOTS)
            slotParams[i].priority.store(p, std::memory_order_relaxed);
    }

    float    getSlotStartFrac(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 0.0f;
        return slotParams[i].startFrac.load(std::memory_order_relaxed);
    }
    float    getSlotEndFrac(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 1.0f;
        return slotParams[i].endFrac.load(std::memory_order_relaxed);
    }
    float    getSlotSpeed(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return 1.0f;
        return slotParams[i].speed.load(std::memory_order_relaxed);
    }
    LoopMode getSlotLoopMode(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return LoopMode::LOOP;
        return static_cast<LoopMode>(slotParams[i].loopMode.load(std::memory_order_relaxed));
    }
    bool     getSlotPriority(int i) const noexcept
    {
        if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) return false;
        return slotParams[i].priority.load(std::memory_order_relaxed);
    }

    // =========================================================================
    // Non-RT: UI-triggered commands (message/timer thread — atomics only)
    // =========================================================================
    /** Toggle record for slotIndex from the UI.
     *  - If slot is RECORDING   → stop recording.
     *  - If slot is IDLE/ARMED  → start recording immediately (bypass ARMED).
     *  - If slot is PLAYING     → punch-in (stop playback, start recording).
     *  Any ongoing recording on another slot is stopped first (only one at a time). */
    void uiToggleRecord(int slotIndex) noexcept;

    /** Play a slot if it has content and is IDLE.
     *  If the slot is already PLAYING, stop it (restore passthrough).
     *  No-op if the slot is empty or currently recording. */
    void uiPlaySlot(int slotIndex) noexcept;

    /** Clear all recorded frames from a slot and reset it to IDLE.
     *  Stops any ongoing recording or playback on that slot first. */
    void uiClearSlot(int slotIndex) noexcept;

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
    // -1 = no gating; 0-11 = only record frames while sequencer step == this bank
    std::atomic<int>   seqGateSlot { -1 };

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
    // Per-slot play parameters — parallel to slots[], owned by FrameSampler.
    // Written by UI (Non-RT); read by FramePlayerThread (Non-RT).
    // -------------------------------------------------------------------------
    struct SlotPlayParams
    {
        std::atomic<float> startFrac { 0.0f }; // Normalised playback start [0..1]
        std::atomic<float> endFrac   { 1.0f }; // Normalised playback end   [0..1]
        std::atomic<float> speed     { 1.0f }; // Playback speed multiplier [0.1..8]
        std::atomic<int>   loopMode  { static_cast<int>(LoopMode::LOOP) };
        std::atomic<bool>  priority  { false }; // Priority for late-read handling

        SlotPlayParams() = default;
        SlotPlayParams(const SlotPlayParams&)            = delete;
        SlotPlayParams& operator=(const SlotPlayParams&) = delete;
    };

    SlotPlayParams slotParams[FrameSamplerConstants::NUM_SLOTS];

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static uint64_t currentTimeUs() noexcept;

    // RT handlers — atomics only
    void handleNoteOn (int note, int velocity) noexcept;
    void handleNoteOff(int note)               noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FrameSampler)
};
