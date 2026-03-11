#pragma once

#include <juce_core/juce_core.h>
#include "Sp3ctraCore.h"

// macOS QoS support for RT priority
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

// Forward declaration of C functions
extern "C" {
    void* audioProcessingThread(void* arg);
    void luxstral_signal_buffer_consumed(void);  // Unblock spin-wait on shutdown
    #include "core/context.h"
    #include "utils/logger.h"
}

/**
 * @brief JUCE thread wrapper for Audio Processing
 * 
 * This class wraps the existing C audioProcessingThread() function from multithreading.c
 * into a JUCE Thread for clean integration with the VST plugin lifecycle.
 * 
 * CRITICAL: In standalone mode, audioProcessingThread calls synth_AudioProcess() in a loop.
 * The RtAudio callback only READS the generated audio buffers.
 * 
 * In VST mode, this thread performs the same role:
 * - Continuously calls synth_AudioProcess() to generate audio
 * - processBlock() only READS the generated buffers (no synthesis in callback!)
 * 
 * 🔧 CRITICAL FIX: This thread uses its OWN stop flag (audioThreadShouldRun)
 * instead of sharing ctx->running with UdpReceiverThread. This prevents
 * buffer size changes from killing the UDP thread!
 */
class AudioProcessingThread : public juce::Thread {
public:
    /**
     * @brief Constructor
     * @param core Pointer to Sp3ctraCore (must remain valid for thread lifetime)
     */
    explicit AudioProcessingThread(Sp3ctraCore* core)
        : Thread("Sp3ctraAudioProcessing"), core(core) {
        log_info("SYNTH", "AudioProcessingThread: Constructor called");
    }
    
    /**
     * @brief Destructor
     * @note Automatically stops thread if still running
     */
    ~AudioProcessingThread() override {
        log_info("SYNTH", "AudioProcessingThread: Destructor called");
        
        // Ensure thread is stopped (JUCE best practice)
        if (isThreadRunning()) {
            requestStop();
            stopThread(2000);  // 2 second timeout
        }
    }
    
    /**
     * @brief Thread execution function
     * 
     * Calls the existing C audioProcessingThread() function which handles:
     * - Continuous synth_AudioProcess() calls
     * - Audio buffer generation
     * - Context->running flag for shutdown
     */
    void run() override {
        log_info("SYNTH", "AudioProcessingThread starting with RT priority...");
        
        if (!core) {
            log_error("SYNTH", "AudioProcessingThread: core is null!");
            return;
        }
        
        Context* ctx = core->getContext();
        if (!ctx) {
            log_error("SYNTH", "AudioProcessingThread: Context is null!");
            return;
        }
        
        // 🔧 RT PRIORITY BOOST: Set macOS QoS to highest user-interactive level
        // This ensures the audio processing thread gets CPU time before other processes
#ifdef __APPLE__
        if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0) {
            log_info("SYNTH", "AudioProcessingThread: QoS set to USER_INTERACTIVE");
        } else {
            log_warning("SYNTH", "AudioProcessingThread: Failed to set QoS class");
        }
#endif
        
        // 🔧 CRITICAL FIX: Set audio_thread_running, NOT running!
        // This allows stopping ONLY the audio thread during buffer size changes
        // without killing the UDP thread (which uses ctx->running)
        ctx->audio_thread_running = 1;
        
        log_info("SYNTH", "Calling C audioProcessingThread() function...");
        
        // Call existing C function (blocks until Context->audio_thread_running = 0)
        audioProcessingThread((void*)ctx);
        
        log_info("SYNTH", "audioProcessingThread() returned, thread exiting");
    }
    
    /**
     * @brief Request thread stop (custom method)
     * 
     * 🔧 FIX: Sets audio_thread_running=0 AND signals the consumed-buffer flag
     * so luxstral_wait_for_buffer_consumed() returns immediately instead of
     * spinning for 50ms. Without this, stopThread(2000) could timeout on
     * slow machines, and the subsequent .reset() would destroy the Thread
     * object while the thread is still in JUCE cleanup → PAC failure crash.
     */
    void requestStop() {
        log_info("SYNTH", "AudioProcessingThread: Requesting thread stop");
        
        // 🔧 Step 1: Set audio_thread_running = 0 (NOT running!)
        // This stops ONLY the audio thread, UDP thread keeps running
        if (core) {
            Context* ctx = core->getContext();
            if (ctx) {
                ctx->audio_thread_running = 0;
            }
        }
        
        // 🔧 Step 2: Signal the consumed flag to unblock the spin-wait immediately
        // Without this, the thread sits in luxstral_wait_for_buffer_consumed()
        // for up to 50ms before checking audio_thread_running
        luxstral_signal_buffer_consumed();
        
        // 🔧 Step 3: Also use JUCE's built-in thread exit mechanism
        signalThreadShouldExit();
    }
    
private:
    Sp3ctraCore* core;  // Non-owning pointer (owned by PluginProcessor)
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioProcessingThread)
};
