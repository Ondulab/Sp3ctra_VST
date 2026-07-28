#pragma once

#include <juce_core/juce_core.h>
#include "Sp3ctraCore.h"

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <avrt.h>
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
            log_startup_detail("LUXSYNTH", "QoS set to USER_INTERACTIVE");
        else
            log_warning("LUXSYNTH", "Failed to set QoS class");
#elif defined(_WIN32)
        {
            DWORD mmcssTaskIndex = 0;
            if (AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex) != nullptr)
                log_startup_detail("LUXSYNTH", "MMCSS Pro Audio joined");
            else
                log_warning("LUXSYNTH", "MMCSS join failed");
            // HIGHEST, not TIME_CRITICAL: only the audio pacer thread gets
            // TIME_CRITICAL — see synth_luxstral_threading_rt.c rationale.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        }
#endif

        ctx->luxsynth_thread_running = 1;
        log_info("LUXSYNTH", "Calling luxsynth_processing_loop()...");

        // Pass a direct pointer to the running flag — avoids the old MinContext
        // struct-layout mismatch that caused the loop to read the wrong offset
        // on 64-bit systems (offset 8 = socket instead of offset 72 = flag).
        luxsynth_processing_loop(&ctx->luxsynth_thread_running);

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
