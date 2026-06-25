/**
 * @file ScoreGenThread.h
 * @brief Worker thread that runs scoregen::renderScore() off the message thread.
 *
 * The render is heavy (64K FFT × N windows + per-pixel sampling). Callbacks fire
 * from the worker thread; the owner is responsible for marshalling them back to
 * the message thread (see ScoreGenTabComponent, juce::MessageManager::callAsync).
 */
#pragma once

#include <juce_core/juce_core.h>
#include "ScoreGenRenderer.h"

class ScoreGenJob : public juce::Thread
{
public:
    ScoreGenJob() : juce::Thread("Sp3ctraScoreGen") {}
    ~ScoreGenJob() override { stopThread(3000); }

    /** Called from the worker thread with progress in [0,1]. */
    std::function<void(float)>                 onProgress;
    /** Called from the worker thread when the render finishes (or fails). */
    std::function<void(scoregen::RenderResult)> onDone;

    /** Starts a render; cancels any in-flight render first. */
    void start(juce::File wav, ScoreSettings settings)
    {
        stopThread(3000);
        wav_      = std::move(wav);
        settings_ = settings;
        startThread();
    }

    void run() override
    {
        auto res = scoregen::renderScore(
            wav_, settings_,
            [this](float p) { if (onProgress) onProgress(p); },
            [this]()        { return threadShouldExit(); });

        if (! threadShouldExit() && onDone)
            onDone(std::move(res));
    }

private:
    juce::File    wav_;
    ScoreSettings settings_ {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreGenJob)
};
