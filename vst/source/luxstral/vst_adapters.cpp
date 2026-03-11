/*
 * vst_adapters.cpp
 *
 * C++ implementation of VST adapter functions for LuxStral engine
 *
 * Author: zhonx
 * Created: January 2026
 */

#include <juce_core/juce_core.h>
#include "vst_adapters.h"
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>   // For mlock() - prevent page faults in RT threads
#include <sched.h>      // For sched_yield() - lock-free spin-wait
#include <sys/time.h>   // For gettimeofday() - spin-wait timeout

// Note: vst_adapters.h already includes everything we need
// No need to include vst_adapters_c.h here (would cause redefinitions)

// Note: shared_var is already defined in context.h and instantiated in multithreading.c

// VST-specific audio buffers for LuxStral (RENAMED to avoid conflicts)
AudioImageBuffer luxstral_buffers_L[2] = {{nullptr, 0, 0}, {nullptr, 0, 0}};
AudioImageBuffer luxstral_buffers_R[2] = {{nullptr, 0, 0}, {nullptr, 0, 0}};
volatile int luxstral_buffer_index = 0;  // 🔧 Access ONLY via __atomic_*_n for ARM64

// VST callback synchronization (producer/consumer handoff)
// 🔧 LOCK-FREE: Replaced pthread_cond with atomic flag polling
// pthread_cond_signal() without mutex caused lost signals → 200ms audio gaps
volatile int g_vst_callback_consumed_buffer = 1;  // 🔧 Access ONLY via __atomic_*_n

// Flag to track buffer initialization
static bool luxstral_audio_buffers_initialized = false;
static int luxstral_audio_buffer_size = 0;  // Track current buffer size for reallocation

// NOTE: wavesGeneratorParams, waves, and unitary_waveform are defined in wave_generation.c
// Don't redefine them here to avoid duplicate symbols

// RT Profiler (disabled in VST)
// Define g_rt_profiler here with proper type from rt_profiler.h
extern "C" {
#include "../utils/rt_profiler.h"
}

// Global instance (disabled)
RTProfiler g_rt_profiler = {0};

/* Logging Functions Implementation ------------------------------*/

// Include the unified C logger
extern "C" {
#include "../utils/logger.h"
}

extern "C" {

/**
 * @brief Log info message through unified C logger
 * 
 * This function routes VST logging to the unified C logger system
 * which outputs in format: [HH:MM:SS] [LEVEL] [MODULE] message
 * 
 * @param message Message to log (may include [MODULE] prefix)
 */
void vst_log_info(const char* message) {
    // Route to unified C logger with SYNTH module (for LuxStral code)
    log_info("SYNTH", "%s", message);
}

/**
 * @brief Log warning message through unified C logger
 */
void vst_log_warning(const char* message) {
    log_warning("SYNTH", "%s", message);
}

/**
 * @brief Log error message through unified C logger
 */
void vst_log_error(const char* message) {
    log_error("SYNTH", "%s", message);
    DBG("ERROR: " << message);  // Also output to IDE debugger
}

/**
 * @brief Initialize LuxStral audio buffers for VST
 * 
 * This function allocates the audio buffers that synth_luxstral.c writes to
 * and processBlock() reads from. Must be called before synthesis starts.
 * 
 * If buffers are already initialized with a different size, they are
 * reallocated to the new size (required when DAW changes buffer size).
 * 
 * @param buffer_size Size of each audio buffer in samples
 * @return 0 on success, -1 on error
 */
int luxstral_init_audio_buffers(int buffer_size) {
    // Check if reallocation is needed (different size)
    if (luxstral_audio_buffers_initialized) {
        if (luxstral_audio_buffer_size == buffer_size) {
            log_info("SYNTH", "Audio buffers already initialized with correct size");
            return 0;
        }
        
        // Buffer size changed - need to reallocate
        log_info("SYNTH", "Buffer size changed (%d -> %d), reallocating...",
                 luxstral_audio_buffer_size, buffer_size);
        luxstral_cleanup_audio_buffers();
    }
    
    if (buffer_size <= 0) {
        log_error("SYNTH", "Invalid buffer size");
        return -1;
    }
    
    log_info("SYNTH", "Initializing audio buffers (size=%d samples)", buffer_size);
    
    // Allocate buffers for both double-buffer slots
    for (int i = 0; i < 2; i++) {
        // Left channel
        luxstral_buffers_L[i].data = (float*)calloc(buffer_size, sizeof(float));
        if (!luxstral_buffers_L[i].data) {
            log_error("SYNTH", "Failed to allocate left buffer");
            luxstral_cleanup_audio_buffers();
            return -1;
        }
        luxstral_buffers_L[i].ready = 0;
        luxstral_buffers_L[i].write_timestamp_us = 0;
        
        // Right channel
        luxstral_buffers_R[i].data = (float*)calloc(buffer_size, sizeof(float));
        if (!luxstral_buffers_R[i].data) {
            log_error("SYNTH", "Failed to allocate right buffer");
            luxstral_cleanup_audio_buffers();
            return -1;
        }
        luxstral_buffers_R[i].ready = 0;
        luxstral_buffers_R[i].write_timestamp_us = 0;
    }
    
    luxstral_buffer_index = 0;
    luxstral_audio_buffer_size = buffer_size;  // Store current size
    luxstral_audio_buffers_initialized = true;
    
    // 🔧 CRITICAL FIX: Reset producer/consumer synchronization state
    // Without this, after buffer reallocation (DAW buffer size change),
    // audioProcessingThread blocks waiting for g_vst_callback_consumed_buffer=1
    // but processBlock() never signals because ready=0 → DEADLOCK!
    __atomic_store_n(&g_vst_callback_consumed_buffer, 1, __ATOMIC_RELEASE);
    
    // ========================================================================
    // 🔧 RT OPTIMIZATION: Lock audio buffers in memory to prevent page faults
    // Page faults during RT audio processing can cause latency spikes of 50ms+!
    // mlock() ensures the buffers stay in physical RAM and are never swapped.
    // ========================================================================
    size_t buffer_bytes = (size_t)buffer_size * sizeof(float);
    int mlock_success = 0;
    int mlock_total = 4;  // 2 channels × 2 double-buffer slots
    
    for (int i = 0; i < 2; i++) {
        if (mlock(luxstral_buffers_L[i].data, buffer_bytes) == 0) {
            mlock_success++;
        }
        if (mlock(luxstral_buffers_R[i].data, buffer_bytes) == 0) {
            mlock_success++;
        }
    }
    
    if (mlock_success == mlock_total) {
        log_info("SYNTH", "Audio buffers locked in memory (mlock) - page faults prevented");
    } else if (mlock_success > 0) {
        log_warning("SYNTH", "Partial mlock: %d/%d buffers locked (may need elevated privileges)", 
                    mlock_success, mlock_total);
    } else {
        log_info("SYNTH", "mlock unavailable - continuing without memory locking");
    }
    
    log_info("SYNTH", "Audio buffers initialized successfully");
    return 0;
}

/**
 * @brief Cleanup LuxStral audio buffers
 */
void luxstral_cleanup_audio_buffers(void) {
    for (int i = 0; i < 2; i++) {
        if (luxstral_buffers_L[i].data) {
            free(luxstral_buffers_L[i].data);
            luxstral_buffers_L[i].data = nullptr;
        }
        luxstral_buffers_L[i].ready = 0;
        
        if (luxstral_buffers_R[i].data) {
            free(luxstral_buffers_R[i].data);
            luxstral_buffers_R[i].data = nullptr;
        }
        luxstral_buffers_R[i].ready = 0;
    }
    
    luxstral_audio_buffers_initialized = false;
    log_info("SYNTH", "Audio buffers cleaned up");
}

/**
 * @brief Check if audio buffers are initialized
 * @return true if initialized, false otherwise
 */
bool luxstral_are_audio_buffers_ready(void) {
    return luxstral_audio_buffers_initialized;
}

/**
 * @brief Initialize callback synchronization system
 * 
 * Called once during plugin initialization. The mutex and condition variable
 * are statically initialized, so this is mostly a placeholder for future
 * dynamic initialization if needed.
 */
void luxstral_init_callback_sync(void) {
    g_vst_callback_consumed_buffer = 1;  // Start ready for first synthesis
    log_info("SYNTH", "Callback synchronization initialized");
}

/**
 * @brief Cleanup callback synchronization system
 */
void luxstral_cleanup_callback_sync(void) {
    // 🔧 LOCK-FREE: Just set flag to unblock any waiting thread
    __atomic_store_n(&g_vst_callback_consumed_buffer, 1, __ATOMIC_RELEASE);
    log_info("SYNTH", "Callback synchronization cleaned up (lock-free)");
}

/**
 * @brief Signal that processBlock() has consumed a buffer
 * 
 * This is called by the VST's processBlock() after reading audio data.
 * It wakes up the audioProcessingThread so it can generate the next buffer.
 * 
 * RT-SAFE: This function is called from the audio thread.
 * LOCK-FREE: Single atomic store, no mutex, no pthread_cond_signal.
 * This eliminates the lost-signal race condition that caused 200ms audio gaps.
 */
void luxstral_signal_buffer_consumed(void) {
    // 🔧 LOCK-FREE: Single atomic store with release semantics
    // No pthread_cond_signal needed - producer polls the flag directly
    __atomic_store_n(&g_vst_callback_consumed_buffer, 1, __ATOMIC_RELEASE);
}

/**
 * @brief Wait for processBlock() to consume the current buffer
 * 
 * This is called by audioProcessingThread before generating a new buffer.
 * It polls the atomic flag until processBlock() signals consumption.
 * 
 * NON-RT: This runs in the synthesis thread, yielding is acceptable.
 * 
 * 🔧 LOCK-FREE REWRITE: Replaced pthread_cond_timedwait (200ms timeout,
 * signal loss race) with atomic polling + sched_yield (~microsecond latency).
 * This eliminates the fundamental race condition where pthread_cond_signal
 * without mutex caused lost signals → 200ms audio gaps → crackling.
 */
void luxstral_wait_for_buffer_consumed(void) {
    // Fast path: check if already consumed (common case after first buffer)
    if (__atomic_load_n(&g_vst_callback_consumed_buffer, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_vst_callback_consumed_buffer, 0, __ATOMIC_RELEASE);
        return;
    }

    // Adaptive spin-wait with timeout
    // Phase 1: Tight spin (nanosecond response for immediate availability)
    // Phase 2: Yield-based polling (microsecond response, CPU-friendly)
    // Phase 3: Timeout (prevents deadlock when audio stops)
    struct timeval start_time;
    gettimeofday(&start_time, NULL);
    
    // Timeout: 2× buffer duration or 50ms minimum (prevents deadlock)
    // g_sp3ctra_config is declared in config_loader.h (included via vst_adapters.h)
    int sample_rate = g_sp3ctra_config.sampling_frequency > 0 ? 
                      g_sp3ctra_config.sampling_frequency : 48000;
    int buffer_size = g_sp3ctra_config.audio_buffer_size > 0 ? 
                      g_sp3ctra_config.audio_buffer_size : 512;
    int64_t timeout_us = (int64_t)buffer_size * 2000000LL / (int64_t)sample_rate;
    if (timeout_us < 50000) timeout_us = 50000;  // 50ms minimum

    int spin_count = 0;
    while (!__atomic_load_n(&g_vst_callback_consumed_buffer, __ATOMIC_ACQUIRE)) {
        spin_count++;
        
        if (spin_count < 100) {
            // Phase 1: Tight spin (first ~100 iterations ≈ microseconds)
            #if defined(__aarch64__)
            __asm__ volatile("yield");  // ARM64 hint: release pipeline
            #elif defined(__x86_64__)
            __asm__ volatile("pause");  // x86 hint: reduce power in spin
            #endif
        } else {
            // Phase 2: Yield CPU (every 100 spins)
            if (spin_count % 100 == 0) {
                sched_yield();
            }
            
            // Phase 3: Check timeout (every 1000 spins)
            if (spin_count % 1000 == 0) {
                struct timeval now;
                gettimeofday(&now, NULL);
                int64_t elapsed_us = (int64_t)(now.tv_sec - start_time.tv_sec) * 1000000LL +
                                     (int64_t)(now.tv_usec - start_time.tv_usec);
                if (elapsed_us > timeout_us) {
                    // Timeout: audio probably stopped, don't block forever
                    return;
                }
            }
        }
    }

    // Buffer was consumed, reset flag so we wait next time
    __atomic_store_n(&g_vst_callback_consumed_buffer, 0, __ATOMIC_RELEASE);
}

} // extern "C"
