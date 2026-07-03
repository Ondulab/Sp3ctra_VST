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
#include "MediaSourceEngines.h"

extern "C" {
#include "../core/context.h"
#include "../threading/multithreading.h"
}

class MediaSourceService : public juce::Thread
{
public:
    MediaSourceService(ImageSourceEngine& img,
                       VideoSourceEngine& vid,
                       CameraSourceEngine& cam)
        : Thread("Sp3ctraMediaSrc"), img_(img), vid_(vid), cam_(cam) {}

    ~MediaSourceService() override { stopThread(2000); }

    /** Must be set (message thread) before startThread(). */
    void setContext(Context* ctx) noexcept { ctx_.store(ctx, std::memory_order_release); }

    void run() override
    {
        while (! threadShouldExit())
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            img_.tick(now);
            vid_.tick(now);
            cam_.tick(now);

            if (internal_source_any_active())
            {
                if (auto* ctx = ctx_.load(std::memory_order_acquire))
                    internal_sources_process_tick(ctx);
                wait(2);      // ~500 Hz — plenty for 30/60 fps media + inertia-free line moves
            }
            else
            {
                wait(50);     // idle: no source active, just poll for activation
            }
        }
    }

private:
    ImageSourceEngine&  img_;
    VideoSourceEngine&  vid_;
    CameraSourceEngine& cam_;
    std::atomic<Context*> ctx_ { nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MediaSourceService)
};
