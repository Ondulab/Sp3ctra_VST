/**
 * @file MediaSourceService.h
 * @brief M9 — Single Non-RT thread driving the IMAGE / VIDEO / CAMERA sources.
 *
 * Each tick (~500 Hz while a source is active):
 *   1. engines produce their current line (transport advance, video/camera
 *      frame pull) and publish it into the internal_source pool;
 *   2. internal_sources_process_tick() feeds the per-synth chains from those
 *      lines when the SP3CTRA device is not streaming (see multithreading.c).
 *
 * Started by PluginProcessor::prepareToPlay once the shared core is up
 * (mirrors the LuxSampler startPlayerThread pattern); stopped with it.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <memory>
#include "MediaSourceEngines.h"

extern "C" {
#include "../core/context.h"
#include "../threading/multithreading.h"
}

class MediaSourceService : public juce::Thread
{
public:
    MediaSourceService(std::array<std::unique_ptr<ImageSourceEngine>, 8>& imgs,
                       VideoSourceEngine& vid,
                       CameraSourceEngine& cam)
        : Thread("Sp3ctraMediaSrc"), imgs_(imgs), vid_(vid), cam_(cam) {}

    ~MediaSourceService() override { stopThread(2000); }

    /** Must be set (message thread) before startThread(). */
    void setContext(Context* ctx) noexcept { ctx_.store(ctx, std::memory_order_release); }

    void run() override
    {
        while (! threadShouldExit())
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            for (auto& eng : imgs_)
                if (eng != nullptr)
                    eng->tick(now);
            vid_.tick(now);
            cam_.tick(now);

            if (auto* ctx = ctx_.load(std::memory_order_acquire))
                internal_sources_process_tick(ctx);

            // The tick also runs with no active source: it drains the sampler
            // start/stop record commands (their only drain site while the
            // SP3CTRA device is silent — see multithreading.c), so REC
            // arm/stop must stay alive at the idle poll rate.
            if (internal_source_any_active())
                wait(2);      // ~500 Hz — plenty for 30/60 fps media + inertia-free line moves
            else
                wait(50);     // idle: poll for activation + keep the REC drain alive
        }
    }

private:
    std::array<std::unique_ptr<ImageSourceEngine>, 8>& imgs_;
    VideoSourceEngine&  vid_;
    CameraSourceEngine& cam_;
    std::atomic<Context*> ctx_ { nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MediaSourceService)
};
