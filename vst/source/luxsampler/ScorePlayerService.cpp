/**
 * @file ScorePlayerService.cpp
 * @brief P5-M4 — per-instance score playback (8 slots, one 1 kHz thread).
 */
#include "ScorePlayerService.h"
#include "score_player_hooks.h"

extern "C" {
    #include "audio_image_buffers.h"
    #include "multithreading.h"         // DoubleBuffer + pthread mutex + chain_player_execute_owned
    #include "../processing/image_preprocessor.h" // PreprocessedImageData
#include "../processing/image_pipeline.h"     // pipeline_build_config_sampler, pipeline_path_luxsynth_luxwave
    #include "logger.h"
    #include "rt_profiler.h"                   // per-family perf timing (Score)
}

#include <sys/time.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// The shared RT profiler (defined in PluginProcessor.cpp) — the score player
// thread reports its per-tick processing time into the Score family slot.
extern RTProfiler g_vst_rt_profiler;

namespace
{
    uint64_t nowUs() noexcept
    {
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        return (uint64_t) tv.tv_sec * 1000000ULL + (uint64_t) tv.tv_usec;
    }

    std::atomic<ScorePlayerService*> s_scoreService { nullptr };
    /* Read-side pin for the C hooks: the destructor unregisters then drains
     * this counter before the object dies — a udp/feeder-thread hook that
     * already loaded the pointer finishes its call safely (same reason as
     * LuxSampler's s_engineBusy pin). */
    std::atomic<int> s_hookBusy { 0 };
}

// ═════════════════════════════════════════════════════════════════════════════
// ScoreChannel — thin forwards
// ═════════════════════════════════════════════════════════════════════════════
bool ScoreChannel::isScorePlaying()  const noexcept { return svc_->isPlaying(slot_); }
bool ScoreChannel::scoreHasContent() const noexcept { return svc_->hasContent(slot_); }
int  ScoreChannel::getScoreFrameCount() const noexcept { return svc_->frameCount(slot_); }
int  ScoreChannel::getScorePlayHead()   const noexcept { return svc_->playHead(slot_); }
void ScoreChannel::uiPlayScore() noexcept       { svc_->play(slot_); }
void ScoreChannel::uiStopScore() noexcept       { svc_->stop(slot_); }
bool ScoreChannel::isScoreActive() const noexcept { return svc_->isActive(slot_); }
void ScoreChannel::setScoreActive(bool a) noexcept { svc_->setActive(slot_, a); }
void ScoreChannel::uiDiscardScore()             { svc_->discard(slot_); }
bool ScoreChannel::uiBeginScoreScrub() noexcept { return svc_->beginScrub(slot_); }
void ScoreChannel::uiEndScoreScrub() noexcept   { svc_->endScrub(slot_); }
void ScoreChannel::uiSetScorePaused(bool paused) noexcept { svc_->setPaused(slot_, paused); }
void ScoreChannel::uiHotSwapScoreFrames(std::vector<CapturedFrame>&& frames) noexcept
{
    svc_->hotSwapFrames(slot_, std::move(frames));
}
void ScoreChannel::uiSeekScore(int frame) noexcept { svc_->seek(slot_, frame); }
void ScoreChannel::setScoreResumeHead(int frame) noexcept { svc_->setResumeHead(slot_, frame); }
void ScoreChannel::setScoreSpeed(float v) noexcept { svc_->setSpeed(slot_, v); }
void ScoreChannel::setScoreLoopMode(LoopMode m) noexcept { svc_->setLoopMode(slot_, m); }
void ScoreChannel::loadScoreFramesFromImage(const juce::Image& image,
                                            juce::Rectangle<int> band,
                                            double scoreMinHz, double scoreMaxHz,
                                            bool stereo)
{
    svc_->loadFramesFromImage(slot_, image, band, scoreMinHz, scoreMaxHz, stereo);
}

// ═════════════════════════════════════════════════════════════════════════════
// Service — lifecycle
// ═════════════════════════════════════════════════════════════════════════════
ScorePlayerService::ScorePlayerService() : juce::Thread("Sp3ctraScorePlayer")
{
    for (int i = 0; i < kMaxSlots; ++i)
    {
        channels_[i].svc_  = this;
        channels_[i].slot_ = i;
    }
    // Multi-instance DAW process: FIRST service wins the hook registration
    // (CAS — an unconditional store would let instance B clobber A, then A's
    // destructor would unbind B mid-play). Same protocol as the LuxSampler
    // engine registry.
    ScorePlayerService* expected = nullptr;
    s_scoreService.compare_exchange_strong(expected, this,
                                           std::memory_order_acq_rel);
}

ScorePlayerService::~ScorePlayerService()
{
    stopThread(2000);
    // Unregister only OUR OWN registration, then drain in-flight C hooks
    // (udp/feeder threads of the shared core may outlive this instance).
    ScorePlayerService* expected = this;
    if (s_scoreService.compare_exchange_strong(expected, nullptr,
                                               std::memory_order_acq_rel))
        while (s_hookBusy.load(std::memory_order_acquire) != 0)
            juce::Thread::yield();
}

// ═════════════════════════════════════════════════════════════════════════════
// Transport / content — message-thread API
// ═════════════════════════════════════════════════════════════════════════════
bool ScorePlayerService::isPlaying(int slot) const noexcept
{
    return slot >= 0 && slot < kMaxSlots
        && slots_[slot].playRequested.load(std::memory_order_acquire);
}

bool ScorePlayerService::hasContent(int slot) const noexcept
{
    return slot >= 0 && slot < kMaxSlots
        && slots_[slot].hasContent.load(std::memory_order_acquire);
}

int ScorePlayerService::frameCount(int slot) const noexcept
{
    return (slot >= 0 && slot < kMaxSlots)
        ? slots_[slot].frameCount.load(std::memory_order_relaxed) : 0;
}

int ScorePlayerService::playHead(int slot) const noexcept
{
    return (slot >= 0 && slot < kMaxSlots)
        ? slots_[slot].playHead.load(std::memory_order_relaxed) : 0;
}

void ScorePlayerService::play(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    ScoreSlot& s = slots_[slot];
    // Toggle parity with the legacy uiPlayScore(): PLAY while playing = STOP.
    if (s.playRequested.load(std::memory_order_acquire)) { stop(slot); return; }
    if (!s.hasContent.load(std::memory_order_acquire)) return;
    // A real PLAY always advances — never inherits a leftover scrub hold.
    s.scrubbing.store(false, std::memory_order_release);
    s.playRequested.store(true, std::memory_order_release);
    notify();
}

void ScorePlayerService::stop(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    ScoreSlot& s = slots_[slot];
    s.playRequested.store(false, std::memory_order_release);
    s.scrubbing.store(false, std::memory_order_release);   // also ends any scrub
    s.resumeHead.store(-1, std::memory_order_relaxed);     // drop any armed resume
    // Also disarm a live seek raced just before the stop: the session may
    // tear down without a final tick, and a leftover seekHead would yank the
    // NEXT session's first tick to the old drag column (or into new frames
    // after a GENERATE reload) after beginSession chose head 0.
    s.seekHead.store(-1, std::memory_order_relaxed);
    s.playHead.store(0, std::memory_order_relaxed);
}

bool ScorePlayerService::isActive(int slot) const noexcept
{
    return slot >= 0 && slot < kMaxSlots
        && slots_[slot].active.load(std::memory_order_acquire);
}

void ScorePlayerService::setActive(int slot, bool active) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    ScoreSlot& s = slots_[slot];
    // Only act on a real transition — the enable param re-applies idempotently
    // on restore / bulk apply, and we must not clobber a live resume latch.
    if (s.active.exchange(active, std::memory_order_acq_rel) == active) return;

    if (!active)
    {
        // Deactivate: stop the reading, but REMEMBER the head and that it was
        // playing so re-activate resumes exactly there (no re-press of PLAY).
        if (s.playRequested.load(std::memory_order_acquire))
        {
            const int head = s.playHead.load(std::memory_order_relaxed);
            stop(slot);                                          // silence
            s.resumeHead.store(head, std::memory_order_relaxed); // continue here
            s.resumeOnReactivate.store(true, std::memory_order_release);
        }
        else
            s.resumeOnReactivate.store(false, std::memory_order_release);
    }
    else if (s.resumeOnReactivate.exchange(false, std::memory_order_acq_rel))
    {
        // Re-activate: it was playing when deactivated → resume from the
        // remembered head (beginSession consumes the armed resumeHead).
        play(slot);
    }
}

void ScorePlayerService::discard(int slot)
{
    if (slot < 0 || slot >= kMaxSlots) return;
    ScoreSlot& s = slots_[slot];
    stop(slot);
    {
        std::lock_guard<std::mutex> lk(s.frameMutex);
        std::vector<CapturedFrame>().swap(s.frames);   // actually release the memory
        s.frameCount.store(0, std::memory_order_relaxed);
        s.hasContent.store(false, std::memory_order_release);
    }
    s.playHead.store(0, std::memory_order_relaxed);
    // Module removal calls this BEFORE the new plan (without our marker) is
    // published: deactivate the stagings NOW, against the still-current plan.
    // The session's own teardown (≤1 ms later) may resolve the NEW plan and
    // miss them — and it can't re-stage either (the frames are gone).
    score_player_stagings_set_inactive(slot);
}

bool ScorePlayerService::beginScrub(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    ScoreSlot& s = slots_[slot];
    // Already playing → the live-seek path handles the drag (legacy contract).
    if (s.playRequested.load(std::memory_order_acquire)) return false;
    if (!s.hasContent.load(std::memory_order_acquire)) return false;
    // playRequested stays FALSE: the PLAY button + head line keep their
    // stopped appearance — this is a transient audition, not playback.
    s.scrubbing.store(true, std::memory_order_release);
    notify();
    return true;
}

void ScorePlayerService::endScrub(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    // Keep the play head AND the armed resume head where the drag left them
    // so a subsequent PLAY starts from that column. The session teardown
    // (white + stagings) runs on the player thread's next tick.
    slots_[slot].scrubbing.store(false, std::memory_order_release);
}

void ScorePlayerService::setPaused(int slot, bool paused) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    ScoreSlot& s = slots_[slot];
    // Pause of a RUNNING transport only: rides the scrub-hold path — the
    // session stays alive (playRequested true) and the tick re-injects the
    // column under the head every 1 ms, a sustained instant. seek() moves
    // the held column live. The stopped-transport hold is beginScrub().
    if (!s.playRequested.load(std::memory_order_acquire)) return;
    s.scrubbing.store(paused, std::memory_order_release);
}

void ScorePlayerService::seek(int slot, int frame) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    ScoreSlot& s = slots_[slot];
    const int n = s.frameCount.load(std::memory_order_relaxed);
    if (n <= 0) return;
    const int f = juce::jlimit(0, n - 1, frame);
    s.seekHead.store(f, std::memory_order_release);   // live seek if playing
    s.resumeHead.store(f, std::memory_order_relaxed); // start here on next PLAY
    s.playHead.store(f, std::memory_order_relaxed);   // reflect immediately in UI
}

void ScorePlayerService::setResumeHead(int slot, int frame) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[slot].resumeHead.store(frame, std::memory_order_relaxed);
}

void ScorePlayerService::setSpeed(int slot, float v) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[slot].speed.store(juce::jlimit(0.01f, 32.0f, v),
                             std::memory_order_relaxed);
}

void ScorePlayerService::setLoopMode(int slot, LoopMode m) noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return;
    slots_[slot].loopMode.store(static_cast<int>(m), std::memory_order_relaxed);
}

bool ScorePlayerService::slotIsFeeding(int slot) const noexcept
{
    if (slot < 0 || slot >= kMaxSlots) return false;
    const ScoreSlot& s = slots_[slot];
    return s.playRequested.load(std::memory_order_acquire)
        || s.scrubbing.load(std::memory_order_acquire)
        || s.sessionActive.load(std::memory_order_acquire);
}

bool ScorePlayerService::anyFeeding() const noexcept
{
    for (int i = 0; i < kMaxSlots; ++i)
        if (slotIsFeeding(i))
            return true;
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Frame loading — port of LuxSampler::loadScoreFramesFromImage (per slot)
// ═════════════════════════════════════════════════════════════════════════════
void ScorePlayerService::loadFramesFromImage(int slot, const juce::Image& image,
                                             juce::Rectangle<int> band,
                                             double scoreMinHz, double scoreMaxHz,
                                             bool stereo)
{
    if (slot < 0 || slot >= kMaxSlots) return;
    ScoreSlot& s = slots_[slot];

    // Stop first (legacy ordering) — the frame swap below is mutex-safe even
    // mid-session, but a reload must never keep the old take sounding.
    stop(slot);

    auto buffer = buildFramesFromImage(image, band, scoreMinHz, scoreMaxHz, stereo);
    if (buffer.empty())
    {
        std::lock_guard<std::mutex> lk(s.frameMutex);
        std::vector<CapturedFrame>().swap(s.frames);
        s.frameCount.store(0, std::memory_order_relaxed);
        s.hasContent.store(false, std::memory_order_release);
        log_warning("SCP", "Score[%d]: invalid image, slot cleared", slot);
        return;
    }
    const int frames = (int) buffer.size();

    {
        std::lock_guard<std::mutex> lk(s.frameMutex);
        s.frames = std::move(buffer);
        s.frameCount.store(frames, std::memory_order_relaxed);
        s.hasContent.store(frames > 0, std::memory_order_release);
    }
    s.playHead.store(0, std::memory_order_relaxed);

    log_info("SCP", "Score[%d]: loaded %d frames (image %dx%d)",
             slot, frames, image.getWidth(), image.getHeight());
}

void ScorePlayerService::hotSwapFrames(int slot,
                                       std::vector<CapturedFrame>&& frames) noexcept
{
    if (slot < 0 || slot >= kMaxSlots || frames.empty()) return;
    ScoreSlot& s = slots_[slot];
    const int newN = (int) frames.size();

    std::lock_guard<std::mutex> lk(s.frameMutex);
    const int oldN = (int) s.frames.size();
    // Same musical position in the new time grid (the frame count moves with
    // the writing speed) — the next tick reads the new content right there.
    const int head = s.playHead.load(std::memory_order_relaxed);
    const int mapped = oldN > 0
        ? juce::jlimit(0, newN - 1,
                       juce::roundToInt((double) head * newN / oldN))
        : 0;
    s.frames = std::move(frames);
    s.frameCount.store(newN, std::memory_order_relaxed);
    s.hasContent.store(true, std::memory_order_release);
    s.playHead.store(mapped, std::memory_order_relaxed);
}

std::vector<CapturedFrame> ScorePlayerService::buildFramesFromImage(
    const juce::Image& image, juce::Rectangle<int> band,
    double scoreMinHz, double scoreMaxHz, bool stereo)
{
    if (!image.isValid() || image.getWidth() <= 0 || image.getHeight() <= 0)
        return {};

    // Extract ONLY the spectrogram band (the part a CIS sensor would scan).
    // Empty band ⇒ fall back to the full image.
    juce::Rectangle<int> b = band.getWidth() > 0 && band.getHeight() > 0
        ? band.getIntersection(image.getBounds())
        : image.getBounds();
    if (b.getWidth() <= 0 || b.getHeight() <= 0)
        b = image.getBounds();

    const int bandX  = b.getX();
    const int bandY  = b.getY();
    const int bandH  = b.getHeight();
    const int frames = std::min(b.getWidth(),
                                LuxSamplerConstants::MAX_FRAMES_PER_SLOT);

    std::vector<CapturedFrame> buffer(static_cast<size_t>(frames));

    // ── Per-output-pixel band row LUT ────────────────────────────────────────
    // The synthesis maps pixel index px∈[0,kPx) LOGARITHMICALLY to frequency
    // over the instrument's range; the score band is ALSO drawn on a LOG axis
    // over [scoreMinHz, scoreMaxHz] — a log→log match (1:1 row-per-oscillator
    // readout when the ranges coincide, like the physical CIS scan of a
    // printed strip). Rows outside the band map to -1 → white (silence).
    constexpr int kPx  = LuxSamplerConstants::MAX_PIXELS;
    const int     denom = (kPx > 1) ? (kPx - 1) : 1;

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
            const double t = (double) px / (double) denom;
            const double f = synthLo * std::pow(synthHi / synthLo, t);
            const double pos = std::log(f / scoreMinHz)
                             / std::log(scoreMaxHz / scoreMinHz);
            if (pos < 0.0 || pos > 1.0)
                rowLut[(size_t) px] = -1;                       // outside band → silence
            else
                rowLut[(size_t) px] = (bandY + bandH - 1)
                    - (int) (pos * (double) (bandH - 1) + 0.5); // flip: low freq → bottom
        }
        else
        {
            rowLut[(size_t) px] = (bandY + bandH - 1)
                - (int) ((static_cast<int64_t>(px) * (bandH - 1)) / denom);
        }
    }

    {
        const juce::Image::BitmapData bmp(image, juce::Image::BitmapData::readOnly);
        for (int x = 0; x < frames; ++x)
        {
            CapturedFrame& f = buffer[static_cast<size_t>(x)];
            f.timestamp_us = static_cast<uint64_t>(x) * 1000ULL; // 1 ms/frame
            f.line_id      = static_cast<uint32_t>(x);
            f.pixel_count  = static_cast<uint16_t>(kPx);
            const int imgX = bandX + x;
            for (int px = 0; px < kPx; ++px)
            {
                const int srcRow = rowLut[(size_t) px];
                if (srcRow < 0)
                {
                    f.R[px] = f.G[px] = f.B[px] = 255;
                    continue;
                }
                const juce::Colour c = bmp.getPixelColour(imgX,
                        juce::jlimit(bandY, bandY + bandH - 1, srcRow));
                if (stereo)
                {
                    // Colour composite (left=red, right=blue): forward R/B
                    // unchanged for LuxStral's colour-temperature panning and
                    // rebalance loudness through GREEN only (pan-neutral) so
                    // fully-panned cells sit at equal L/R loudness — see the
                    // SCORE stereo-mode derivation.
                    const int   R  = c.getRed(), B = c.getBlue();
                    const float rE = 1.0f - R / 255.0f;
                    const float lE = 1.0f - B / 255.0f;
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
                    const uint8_t g = c.getRed();
                    f.R[px] = f.G[px] = f.B[px] = g;
                }
            }
        }
    }

    return buffer;
}

// ═════════════════════════════════════════════════════════════════════════════
// Player thread — one 1 kHz clock, every feeding slot ticked per period
// ═════════════════════════════════════════════════════════════════════════════
void ScorePlayerService::run()
{
    uint64_t lastTickUs = nowUs();

    while (!threadShouldExit())
    {
        // Idle: nothing wants to run and no session to wind down.
        bool anyWork = false;
        for (int i = 0; i < kMaxSlots && !anyWork; ++i)
            anyWork = sessions_[i].active
                   || slots_[i].playRequested.load(std::memory_order_acquire)
                   || slots_[i].scrubbing.load(std::memory_order_acquire);
        if (!anyWork)
        {
            wait(5);
            lastTickUs = nowUs();
            continue;
        }

        // 1 ms lock-step (drift-free, catch-up-safe) — same pacing as the
        // legacy runScoreSession.
        const uint64_t now   = nowUs();
        const uint64_t since = now - lastTickUs;
        if (since < 1000)
        {
            // Busy-yield between ticks — deliberate legacy parity with the
            // FramePlayerThread sessions: sleep(1) can overshoot by
            // milliseconds and would jitter the 1 kHz line cadence (audible
            // texture). Costs one core while ANY score plays, like before.
            juce::Thread::yield();
            continue;
        }
        lastTickUs += 1000;
        if (lastTickUs > now) lastTickUs = now;

        const uint64_t scoreTickT0 = nowUs();   // per-family perf timing

        // Display owner: the LOWEST feeding slot writes the visual mix bus
        // (single display bus; the sampler engines defer while any score
        // plays — per-slot viz is the same follow-up as the samplers').
        int displayOwner = -1;
        for (int i = 0; i < kMaxSlots; ++i)
            if (sessions_[i].active
                || slots_[i].playRequested.load(std::memory_order_acquire)
                || slots_[i].scrubbing.load(std::memory_order_acquire))
            { displayOwner = i; break; }

        for (int i = 0; i < kMaxSlots; ++i)
        {
            ScoreSlot& s = slots_[i];
            Session&   v = sessions_[i];
            const bool want = s.playRequested.load(std::memory_order_acquire)
                           || s.scrubbing.load(std::memory_order_acquire);

            if (!v.active)
            {
                if (!want) continue;
                beginSession(i);
                if (!v.active) continue;   // no content — request dropped
            }
            else if (!want)
            {
                endSession(i, i == displayOwner);
                continue;
            }

            int nb = 0;
            const bool alive = advanceAndFetch(i, nb);
            if (nb > 0)
                inject(i, nb, i == displayOwner);
            if (!alive)
            {
                // Natural end (LoopMode::NONE): transport snaps back to
                // STOP; the head stays where the run ended (legacy parity).
                s.playRequested.store(false, std::memory_order_release);
                s.scrubbing.store(false, std::memory_order_release);
                endSession(i, i == displayOwner);
            }
        }

        rt_profiler_engine_report(&g_vst_rt_profiler, RT_ENGINE_SCORE,
                                  nowUs() - scoreTickT0);
    }

    // Thread teardown: silence everything we still own.
    for (int i = 0; i < kMaxSlots; ++i)
        if (sessions_[i].active)
            endSession(i, false);
}

void ScorePlayerService::beginSession(int slot) noexcept
{
    ScoreSlot& s = slots_[slot];
    Session&   v = sessions_[slot];

    std::lock_guard<std::mutex> lk(s.frameMutex);
    const int n = (int) s.frames.size();
    if (n <= 0)
    {
        log_warning("SCP", "Score[%d]: play requested but no content", slot);
        s.playRequested.store(false, std::memory_order_release);
        s.scrubbing.store(false, std::memory_order_release);
        return;
    }

    v = Session {};
    v.prevLoopMode = static_cast<LoopMode>(s.loopMode.load(std::memory_order_relaxed));
    v.direction    = (v.prevLoopMode == LoopMode::INVERSE
                      || v.prevLoopMode == LoopMode::ONCE_BACKWARD) ? -1 : 1;

    // An armed resume frame (live EQ re-apply, scrub release) takes over the
    // initial head — one-shot, so a fresh PLAY starts from the beginning.
    const int resume = s.resumeHead.exchange(-1, std::memory_order_relaxed);
    const int head   = (resume > 0) ? juce::jlimit(0, n - 1, resume)
                                    : (v.direction > 0 ? 0 : n - 1);
    s.playHead.store(head, std::memory_order_relaxed);

    v.active = true;
    s.sessionActive.store(true, std::memory_order_release);
    log_info("SCP", "Score[%d]: playback start — %d frames, %.2f s",
             slot, n, (double) n / 1000.0);
}

void ScorePlayerService::endSession(int slot, bool wasDisplayOwner) noexcept
{
    ScoreSlot& s = slots_[slot];
    Session&   v = sessions_[slot];
    if (!v.active) return;
    v.active = false;

    // Audio silence: the stagings have no timeout — deactivate every OUT this
    // slot's chains staged (the mixers then commit silence). Sourced chains
    // are re-staged by their own producer on its next line; sourceless chains
    // fall to the feeder's no-signal publication (white taps + probes).
    score_player_stagings_set_inactive(slot);

    // Path-B teardown (legacy injectWhiteFrame parity): while this slot owned
    // the pb chain, inject() was the polyphonic sections' SOLE writer — zero
    // them and whiten the tap, or a SOURCELESS pb chain freezes on the last
    // played column forever (no producer ever runs that chain again; the
    // feeder's no-signal path only whitens the taps it owns).
    if (doubleBuffer_ != nullptr
        && chain_pathb_player_candidate(/*is_score*/ 1, slot) != 0)
    {
        pthread_mutex_lock(&doubleBuffer_->mutex);
        std::memset(doubleBuffer_->preprocessed_data.polyphonic.grayscale, 0,
                    sizeof(doubleBuffer_->preprocessed_data.polyphonic.grayscale));
        std::memset(doubleBuffer_->preprocessed_data.polyphonic.magnitudes, 0,
                    sizeof(doubleBuffer_->preprocessed_data.polyphonic.magnitudes));
        doubleBuffer_->preprocessed_data.polyphonic.valid = 0;
        doubleBuffer_->dataReady = 1;
        pthread_mutex_unlock(&doubleBuffer_->mutex);

        audio_image_buffers_publish_engine_input(
            audioBuffers_, AUDIO_IMAGE_ENGINE_TAP_PATHB,
            NULL, NULL, NULL, get_cis_pixels_nb());   // NULL = white
    }
    // Same for the LuxStral head-panel tap when this slot's chain was the
    // first "→ LUXSTRAL" send: show "unfed" (white) instead of freezing.
    if (audioBuffers_ != nullptr
        && chain_additive_player_candidate(/*is_score*/ 1, slot) != 0)
        audio_image_buffers_publish_engine_input(
            audioBuffers_, AUDIO_IMAGE_ENGINE_TAP_LUXSTRAL,
            NULL, NULL, NULL, get_cis_pixels_nb());

    if (wasDisplayOwner)
        writeWhiteMixBus();   // blank paper on the visual mix bus

    s.sessionActive.store(false, std::memory_order_release);
    log_info("SCP", "Score[%d]: playback stopped (head=%d/%d)",
             slot, s.playHead.load(std::memory_order_relaxed),
             s.frameCount.load(std::memory_order_relaxed));
}

bool ScorePlayerService::advanceAndFetch(int slot, int& nb) noexcept
{
    ScoreSlot& s = slots_[slot];
    Session&   v = sessions_[slot];
    nb = 0;

    std::lock_guard<std::mutex> lk(s.frameMutex);
    const int n = (int) s.frames.size();
    if (n <= 0) return false;   // content vanished → natural-end teardown

    // Loop-mode change → direction update (legacy tickVoice contract)
    const LoopMode lm = static_cast<LoopMode>(s.loopMode.load(std::memory_order_relaxed));
    if (lm != v.prevLoopMode)
    {
        v.prevLoopMode = lm;
        switch (lm)
        {
            case LoopMode::LOOP:
            case LoopMode::NONE:           v.direction = 1;  break;
            case LoopMode::INVERSE:
            case LoopMode::ONCE_BACKWARD:  v.direction = -1; break;
            case LoopMode::PINGPONG:
            case LoopMode::ONCE_ROUNDTRIP: break; // keep current direction
        }
    }

    int head = juce::jlimit(0, n - 1, s.playHead.load(std::memory_order_relaxed));

    // Manual scrub / live seek: the UI dragged the head elsewhere.
    const int seekTo = s.seekHead.exchange(-1, std::memory_order_acq_rel);
    if (seekTo >= 0)
    {
        head       = juce::jlimit(0, n - 1, seekTo);
        v.frameAcc = 0.0f;
    }

    // Scrub-audition holds position: the column under the cursor is
    // re-injected unchanged every tick → a sustained tone.
    bool alive = true;
    if (!s.scrubbing.load(std::memory_order_relaxed))
    {
        v.frameAcc    += s.speed.load(std::memory_order_relaxed);
        const int step = (int) v.frameAcc;
        v.frameAcc    -= (float) step;

        if (step > 0)
        {
            head += step * v.direction;
            const bool fwdBound = (v.direction > 0 && head >= n);
            const bool bwdBound = (v.direction < 0 && head < 0);
            if (fwdBound || bwdBound)
            {
                switch (lm)
                {
                    case LoopMode::NONE:
                    case LoopMode::ONCE_BACKWARD:
                        alive = false;
                        break;
                    case LoopMode::LOOP:
                        v.direction = 1;
                        head        = ((head % n) + n) % n;
                        v.frameAcc  = 0.0f;
                        break;
                    case LoopMode::INVERSE:
                        v.direction = -1;
                        head        = (n - 1) - ((((n - 1) - head) % n + n) % n);
                        v.frameAcc  = 0.0f;
                        break;
                    case LoopMode::PINGPONG:
                        v.direction = -v.direction;
                        head        = juce::jlimit(0, n - 1, head);
                        v.frameAcc  = 0.0f;
                        break;
                    case LoopMode::ONCE_ROUNDTRIP:
                        // One bounce: forward edge turns around, backward ends.
                        if (bwdBound) { alive = false; break; }
                        v.direction = -1;
                        head        = juce::jlimit(0, n - 1, head);
                        v.frameAcc  = 0.0f;
                        break;
                }
            }
        }
    }

    head = juce::jlimit(0, n - 1, head);
    s.playHead.store(head, std::memory_order_relaxed);
    if (!alive)
        return false;   // end tick injects nothing (legacy parity)

    const CapturedFrame& frame = s.frames[(size_t) head];
    nb = std::min((int) frame.pixel_count, LuxSamplerConstants::MAX_PIXELS);
    std::memcpy(workR_, frame.R, (size_t) nb);
    std::memcpy(workG_, frame.G, (size_t) nb);
    std::memcpy(workB_, frame.B, (size_t) nb);
    return true;
}

void ScorePlayerService::inject(int slot, int nb, bool displayOwner) noexcept
{
    // ── The unified P4 chain walk — stages every OUT of every chain whose
    //    SCORE-family marker carries THIS slot, runs post-marker FX/probes,
    //    records downstream SAMPLER markers, publishes the selection taps.
    //    force_play=1: the score owns its own transport (never gated by the
    //    sampler Transport UI). Post-FX stream is written back into work*.
    (void) chain_player_execute_owned(/*is_score*/ 1, slot, /*force_play*/ 1,
                                      audioBuffers_, workR_, workG_, workB_, nb);

    // ── Visual mix bus (single display bus, one owner) ───────────────────────
    if (displayOwner && audioBuffers_ != nullptr)
    {
        uint8_t *wR = nullptr, *wG = nullptr, *wB = nullptr;
        if (audio_image_buffers_start_write(audioBuffers_, &wR, &wG, &wB) == 0)
        {
            std::memcpy(wR, workR_, (size_t) nb);
            std::memcpy(wG, workG_, (size_t) nb);
            std::memcpy(wB, workB_, (size_t) nb);
            audio_image_buffers_complete_write(audioBuffers_);
        }
    }

    // ── Polyphonic Path-B commit while THIS slot owns the pb chain ──────────
    if (doubleBuffer_ != nullptr
        && chain_pathb_player_candidate(/*is_score*/ 1, slot) != 0)
    {
        static thread_local PreprocessedImageData ppData;
        PipelineConfig cfg = pipeline_build_config_sampler();
        cfg.freeze_mode = 0;   // force PLAY — the score has its own transport
        pipeline_path_luxsynth_luxwave(workR_, workG_, workB_, &cfg, &ppData);
        ppData.timestamp_us = nowUs();

        pthread_mutex_lock(&doubleBuffer_->mutex);
        /* Polyphonic (views) only — the audio-thread mixer owns everything
         * else (P4-M4). */
        doubleBuffer_->preprocessed_data.polyphonic = ppData.polyphonic;
        pthread_mutex_unlock(&doubleBuffer_->mutex);

        // Per-engine Path-B input tap (per-chain display): the exact frame
        // fed to the pipeline above.
        audio_image_buffers_publish_engine_input(
            audioBuffers_, AUDIO_IMAGE_ENGINE_TAP_PATHB,
            workR_, workG_, workB_, nb);
    }
}

void ScorePlayerService::writeWhiteMixBus() noexcept
{
    if (audioBuffers_ == nullptr) return;
    uint8_t *wR = nullptr, *wG = nullptr, *wB = nullptr;
    if (audio_image_buffers_start_write(audioBuffers_, &wR, &wG, &wB) == 0)
    {
        std::memset(wR, 255, LuxSamplerConstants::MAX_PIXELS);
        std::memset(wG, 255, LuxSamplerConstants::MAX_PIXELS);
        std::memset(wB, 255, LuxSamplerConstants::MAX_PIXELS);
        audio_image_buffers_complete_write(audioBuffers_);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// C hooks — chain-executor gates (multithreading.c)
// ═════════════════════════════════════════════════════════════════════════════
extern "C" int score_player_slot_is_playing(int slot)
{
    s_hookBusy.fetch_add(1, std::memory_order_acq_rel);
    auto* svc = s_scoreService.load(std::memory_order_acquire);
    const int r = (svc != nullptr && svc->slotIsFeeding(slot)) ? 1 : 0;
    s_hookBusy.fetch_sub(1, std::memory_order_acq_rel);
    return r;
}

extern "C" int score_player_any_playing(void)
{
    s_hookBusy.fetch_add(1, std::memory_order_acq_rel);
    auto* svc = s_scoreService.load(std::memory_order_acquire);
    const int r = (svc != nullptr && svc->anyFeeding()) ? 1 : 0;
    s_hookBusy.fetch_sub(1, std::memory_order_acq_rel);
    return r;
}
