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


// Note: vst_adapters.h already includes everything we need
// No need to include vst_adapters_c.h here (would cause redefinitions)

// VST-specific audio buffers for LuxStral (RENAMED to avoid conflicts)
// extern "C": the C engine (synth_luxstral.c) references these by their plain
// name; without C linkage MSVC would mangle the C++ definition and the link
// would fail (macOS/Itanium leaves namespace-scope variables unmangled, so it
// only bites on Windows). vst_adapters_c.h declares the matching extern "C".
// Single-producer/single-consumer handoff; the producer sleeps on a semaphore.
extern "C" {
AudioImageBuffer luxstral_buffers_L[2] = {{nullptr, 0, 0}, {nullptr, 0, 0}};
AudioImageBuffer luxstral_buffers_R[2] = {{nullptr, 0, 0}, {nullptr, 0, 0}};
volatile int luxstral_buffer_index = 0;  // 🔧 Access ONLY via __atomic_*_n for ARM64
volatile int g_vst_callback_consumed_buffer = 1;  // 🔧 Access ONLY via __atomic_*_n
}

// Flag to track buffer initialization
static bool luxstral_audio_buffers_initialized = false;
static int luxstral_audio_buffer_size = 0;  // Track current buffer size for reallocation

// NOTE: wavesGeneratorParams, waves, and unitary_waveform are defined in wave_generation.c
// Don't redefine them here to avoid duplicate symbols

// RT Profiler type (the LuxStral threading now records mutex-contention timing
// into the REAL g_vst_rt_profiler defined in PluginProcessor.cpp — the former
// separate g_rt_profiler instance here was zero-initialised and never flushed,
// so its measurements were dead. Removed.)
extern "C" {
#include "../utils/rt_profiler.h"
}

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
    
    log_startup_detail("SYNTH", "Initializing audio buffers (size=%d samples)", buffer_size);
    
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
 * @brief Currently allocated per-channel output buffer size, in samples.
 * 0 until luxstral_init_audio_buffers() has run. Consumers must clamp their
 * reads to this value — the host block size can change after allocation.
 */
int luxstral_get_audio_buffer_size(void) {
    return luxstral_audio_buffer_size;
}


} // extern "C"
