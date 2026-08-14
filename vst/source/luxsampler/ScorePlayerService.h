/**
 * @file ScorePlayerService.h
 * @brief P5-M4 — per-instance score playback: 8 slots, ONE multiplexed thread.
 *
 * Replaces the legacy shared score channel (LuxSampler engine A's single
 * scoreSlot + runScoreSession): every SCORE-family module instance
 * (SCORE / TIMBRE / MIDI SCORE / VOICE) owns a pool slot {frames, transport,
 * play-head}, and one service thread ticks every playing slot at 1 kHz.
 * N scores play SIMULTANEOUSLY, each feeding only the chains whose
 * SCORE-family marker carries its slot (insert_state_idx — see
 * chain_player_owned / chain_hosts_driving_score in multithreading.c).
 *
 * Each slot tick is the score subset of the legacy FramePlayerThread path:
 * advance the head (speed / loop / scrub-hold / seek), then run the unified
 * P4 chain walk (chain_player_execute_owned) on the frame — OUT staging at
 * exact positions, post-marker FX/probes, downstream SAMPLER records,
 * selection taps — plus the polyphonic Path-B commit and the visual mix bus
 * (single display owner: the lowest slot playing/scrubbing/running-out; the
 * sampler engines + producers defer via score_player_owns_display(). A
 * PARKED hold (P8, VOICE) feeds audio but never claims the bus).
 *
 * There is NO relay and NO cross-player eviction here: score slots are
 * independent players (chain doctrine). The sampler-resume relay existed
 * only because the score rode the sampler's player thread.
 *
 * ScoreChannel mirrors LuxSampler's historical score API name-for-name so
 * the four generator tabs rebind by swapping their accessor only.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include "LuxSampler.h"   // CapturedFrame, LoopMode, LuxSamplerConstants

struct AudioImageBuffers;
struct DoubleBuffer;

class ScorePlayerService;

/** Per-slot facade over ScorePlayerService, method-compatible with the
 *  legacy LuxSampler score API (the tabs' `fs->` call sites compile as-is). */
class ScoreChannel
{
public:
    bool isScorePlaying() const noexcept;
    bool scoreHasContent() const noexcept;
    int  getScoreFrameCount() const noexcept;
    int  getScorePlayHead() const noexcept;

    void uiPlayScore() noexcept;
    void uiStopScore() noexcept;
    void uiDiscardScore();

    /** Module ACTIVE state (rack LED), decoupled from the transport:
     *  deactivating stops playback but remembers the head + that it was
     *  playing; reactivating resumes it (no need to re-press PLAY). */
    bool isScoreActive() const noexcept;
    void setScoreActive(bool active) noexcept;
    /** True while this slot writes its chains (playing, scrubbing, parked
     *  hold or a tail runout) — the rack LED reads the real flux, not the
     *  transport (same semantics as the media sources' "loaded" LED). */
    bool isScoreFeeding() const noexcept;
    bool uiBeginScoreScrub() noexcept;
    void uiEndScoreScrub() noexcept;
    void uiSetScorePaused(bool paused) noexcept;
    void uiSeekScore(int frame) noexcept;
    void setScoreResumeHead(int frame) noexcept;
    void setScoreSpeed(float v) noexcept;
    void setScoreLoopMode(LoopMode m) noexcept;

    void loadScoreFramesFromImage(const juce::Image& image,
                                  juce::Rectangle<int> band = {},
                                  double scoreMinHz = 0.0,
                                  double scoreMaxHz = 0.0,
                                  bool stereo = false);

    /** Live swap: replaces the frames WITHOUT stopping the transport — the
     *  head is remapped proportionally and the next 1 ms tick simply reads
     *  the new content (no gap, no click, pause/scrub holds survive). Build
     *  the frames off-thread with ScorePlayerService::buildFramesFromImage. */
    void uiHotSwapScoreFrames(std::vector<CapturedFrame>&& frames) noexcept;

    int slot() const noexcept { return slot_; }

private:
    friend class ScorePlayerService;
    ScorePlayerService* svc_ = nullptr;
    int slot_ = 0;
};

class ScorePlayerService : public juce::Thread
{
public:
    static constexpr int kMaxSlots = 8;

    ScorePlayerService();
    ~ScorePlayerService() override;

    /** Must be set (message thread) before startThread() — same wiring as
     *  LuxSampler::startPlayerThread (visual mix bus + synth double buffer). */
    void setBuffers(AudioImageBuffers* audioBuffers,
                    DoubleBuffer* doubleBuffer) noexcept
    {
        audioBuffers_ = audioBuffers;
        doubleBuffer_ = doubleBuffer;
    }

    /** Slot facade for UI/processor call sites (never null for 0..7). */
    ScoreChannel* channel(int slot) noexcept
    {
        return (slot >= 0 && slot < kMaxSlots) ? &channels_[slot] : nullptr;
    }

    /** Fired (message thread) after a successful loadFramesFromImage with the
     *  exact image + calibration the frames were built from — the processor
     *  persists it as the slot's session take (see captureScoreTake). Hot
     *  swaps (uiHotSwapScoreFrames) do NOT fire: they carry prebuilt frames
     *  without the source image, and their callers reload through this path
     *  on the next full (re)generate anyway. */
    std::function<void(int slot, const juce::Image& image,
                       juce::Rectangle<int> band, double scoreMinHz,
                       double scoreMaxHz, bool stereo)> onFramesLoaded;

    // ── Transport / content (message thread) ────────────────────────────────
    bool isPlaying(int slot) const noexcept;      // UI transport state (not scrub)
    bool hasContent(int slot) const noexcept;
    int  frameCount(int slot) const noexcept;
    int  playHead(int slot) const noexcept;

    void play(int slot) noexcept;                 // toggle: stops if playing
    void stop(int slot) noexcept;
    bool isActive(int slot) const noexcept;       // module enable (rack LED)
    void setActive(int slot, bool active) noexcept;
    void discard(int slot);                       // stop + free the slot's frames
    bool beginScrub(int slot) noexcept;
    void endScrub(int slot) noexcept;
    void setPaused(int slot, bool paused) noexcept; // freeze a RUNNING transport
    void seek(int slot, int frame) noexcept;
    void setResumeHead(int slot, int frame) noexcept;
    void setSpeed(int slot, float v) noexcept;
    void setLoopMode(int slot, LoopMode m) noexcept;

    void loadFramesFromImage(int slot, const juce::Image& image,
                             juce::Rectangle<int> band,
                             double scoreMinHz, double scoreMaxHz, bool stereo);

    /** Pure image→frames conversion (the expensive part of a load) — safe on
     *  any thread, no slot touched. Empty result = invalid/degenerate image. */
    static std::vector<CapturedFrame> buildFramesFromImage(
        const juce::Image& image, juce::Rectangle<int> band,
        double scoreMinHz, double scoreMaxHz, bool stereo);

    /** Swap a slot's frames in place, transport untouched (see
     *  ScoreChannel::uiHotSwapScoreFrames). Empty input is ignored. */
    void hotSwapFrames(int slot, std::vector<CapturedFrame>&& frames) noexcept;

    /** P8 — "feed like a media source" (VOICE): with content loaded and the
     *  module ACTIVE the session stays alive when the transport stops, the
     *  column under the head re-injected every tick (a parked drone — the
     *  exact analogue of a loaded IMAGE's frozen line). PLAY animates the
     *  head; STOP parks it in place instead of rewinding. Set per slot from
     *  the chain-plan derivation (slot type map). */
    void setHoldWhenStopped(int slot, bool hold) noexcept;

    /** C-hook backend: slot may still write its chains (play, scrub, parked
     *  hold or a session winding down). Any-thread safe. */
    bool slotIsFeeding(int slot) const noexcept;
    bool anyFeeding() const noexcept;

    /** Display arbitration ONLY (single visual mix bus): true while a slot
     *  plays, scrubs or runs out a tail. A PARKED hold (P8) keeps feeding its
     *  chains but never claims the bus — a parked VOICE must not freeze the
     *  live view for hours. Any-thread safe. */
    bool ownsDisplay() const noexcept;

    void run() override;

private:
    struct ScoreSlot
    {
        // Frame store — swap/free under frameMutex; the player copies ONE
        // frame per tick under the same mutex (≈10 KB, Non-RT thread).
        std::mutex                 frameMutex;
        std::vector<CapturedFrame> frames;
        std::atomic<int>  frameCount    { 0 };
        std::atomic<bool> hasContent    { false };

        // Transport (message thread writes, player + gates read)
        std::atomic<bool> playRequested { false };
        std::atomic<bool> scrubbing     { false };
        std::atomic<int>  seekHead      { -1 };
        std::atomic<int>  resumeHead    { -1 };
        std::atomic<int>  playHead      { 0 };
        std::atomic<float> speed        { 1.0f };
        std::atomic<int>  loopMode      { static_cast<int>(LoopMode::NONE) };

        // Player-side session flag (published for the ownership gates)
        std::atomic<bool> sessionActive { false };
        // Player-side runout mirror (published for the DISPLAY arbitration:
        // a tail runout still writes blank paper to the bus, a parked hold
        // does not — see ownsDisplay()).
        std::atomic<bool> runoutActive  { false };

        // Module ACTIVE state (rack LED enable), independent of the transport.
        // Deactivating stops the run and latches resumeOnReactivate so the
        // next re-activate replays from resumeHead. Default ON.
        std::atomic<bool> active             { true };
        std::atomic<bool> resumeOnReactivate { false };

        // P8 — VOICE-hosted slots: active + content ⇒ the session persists
        // with the transport stopped (parked column drone, IMAGE parity).
        std::atomic<bool> holdWhenStopped    { false };
    };

    /** Player-thread-private session state (one per slot). */
    struct Session
    {
        bool     active        = false;
        int      direction     = 1;
        float    frameAcc      = 0.0f;
        LoopMode prevLoopMode  = LoopMode::NONE;

        // FX tail runout: the transport is already STOPped but the chain's
        // Reverb/Echo still have something to print, so the session stays
        // alive injecting blank paper at the same 1 kHz until they are spent
        // (see runout() / chain_player_fx_tail_alive).
        bool     runout        = false;
        int      runoutTicks   = 0;
        int      lastNb        = 0;   // pixel count of the last injected frame
    };

    /** Hard ceiling on a tail runout (1 ms ticks). The tail query is the real
     *  terminator; this only bounds a pathological setting (max decay + near-
     *  unity echo feedback) so a stopped score can never hold its chains
     *  forever. 60 s is well past any musically useful tail. */
    static constexpr int kRunoutMaxTicks = 60000;

    /** P8 hold predicate: the slot wants a live session even with the
     *  transport stopped (VOICE parity with a loaded IMAGE). */
    bool slotWantsHold(int slot) const noexcept
    {
        const ScoreSlot& s = slots_[slot];
        return s.holdWhenStopped.load(std::memory_order_acquire)
            && s.active.load(std::memory_order_acquire)
            && s.hasContent.load(std::memory_order_acquire);
    }

    void beginSession(int slot) noexcept;
    void endSession(int slot, bool wasDisplayOwner) noexcept;
    /** Enter the tail runout if anything downstream still rings; otherwise
     *  tear the session down now. Returns true when the runout took over. */
    bool beginRunoutOrEnd(int slot, bool isDisplayOwner) noexcept;
    /** One runout tick: blank paper through the same chain walk as a played
     *  frame, so the FX see a silent line and decay exactly as they would on
     *  a silent passage of the score. */
    void injectRunout(int slot, bool displayOwner) noexcept;
    /** Advance the head one 1 ms tick and copy the current frame into the
     *  work buffers. Returns false when a LoopMode::NONE run reached its
     *  end (nb stays 0 — nothing injected that tick, like the legacy path). */
    bool advanceAndFetch(int slot, int& nb) noexcept;
    /** The score subset of the legacy outputFrame(): unified chain walk,
     *  polyphonic Path-B commit, visual mix bus (display owner only). */
    void inject(int slot, int nb, bool displayOwner) noexcept;
    void writeWhiteMixBus() noexcept;

    ScoreSlot   slots_[kMaxSlots];
    Session     sessions_[kMaxSlots];
    ScoreChannel channels_[kMaxSlots];

    AudioImageBuffers* audioBuffers_ = nullptr;
    DoubleBuffer*      doubleBuffer_ = nullptr;

    // Work buffers — single player thread, reused every tick.
    uint8_t workR_[LuxSamplerConstants::MAX_PIXELS] {};
    uint8_t workG_[LuxSamplerConstants::MAX_PIXELS] {};
    uint8_t workB_[LuxSamplerConstants::MAX_PIXELS] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScorePlayerService)
};
