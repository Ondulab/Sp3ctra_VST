#pragma once

#include <juce_core/juce_core.h>
#include "Sp3ctraCore.h"

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

extern "C" {
    #include "synthesis/luxsynth/luxsynth_vst_adapter.h"
    #include "core/context.h"
    #include "utils/logger.h"
}

/**
 * @brief JUCE thread wrapper for LuxSynth additive synthesis processing.
 *
 * Same pattern as AudioProcessingThread (LuxStral):
 * - Runs luxsynth_processing_loop() in a dedicated thread
 * - processBlock consumes the double-buffer output
 * - Uses its own stop flag (ctx->luxsynth_thread_running)
 */
class LuxSynthProcessingThread : public juce::Thread
{
public:
    explicit LuxSynthProcessingThread(Sp3ctraCore* core)
        : Thread("Sp3ctraLuxSynth"), core(core)
    {
        log_info("LUXSYNTH", "LuxSynthProcessingThread: Constructor");
    }

    ~LuxSynthProcessingThread() override
    {
        log_info("LUXSYNTH", "LuxSynthProcessingThread: Destructor");
        if (isThreadRunning())
        {
            requestStop();
            stopThread(2000);
        }
    }

    void run() override
    {
        log_info("LUXSYNTH", "LuxSynthProcessingThread starting...");

        if (!core)
        {
            log_error("LUXSYNTH", "LuxSynthProcessingThread: core is null!");
            return;
        }

        Context* ctx = core->getContext();
        if (!ctx)
        {
            log_error("LUXSYNTH", "LuxSynthProcessingThread: Context is null!");
            return;
        }

#ifdef __APPLE__
        if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0)
            log_info("LUXSYNTH", "QoS set to USER_INTERACTIVE");
        else
            log_warning("LUXSYNTH", "Failed to set QoS class");
#endif

        ctx->luxsynth_thread_running = 1;
        log_info("LUXSYNTH", "Calling luxsynth_processing_loop()...");

        luxsynth_processing_loop((void*)ctx);

        log_info("LUXSYNTH", "luxsynth_processing_loop() returned");
    }

    void requestStop()
    {
        log_info("LUXSYNTH", "Requesting thread stop");
        if (core)
        {
            Context* ctx = core->getContext();
            if (ctx)
                ctx->luxsynth_thread_running = 0;
        }
        luxsynth_signal_consumed();
        signalThreadShouldExit();
    }

private:
    Sp3ctraCore* core;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSynthProcessingThread)
};
