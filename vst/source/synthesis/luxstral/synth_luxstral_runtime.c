/*
 * synth_luxstral_runtime.c
 *
 * Runtime configuration implementation for additive synthesis
 *
 * Author: zhonx
 */

#include "vst_adapters_c.h"
#include "synth_luxstral_runtime.h"
#include "logger.h"
#include "wave_generation.h"
#include <stdio.h>
#include <stdlib.h>

/* Global runtime configuration */
synth_runtime_config_t g_synth_runtime = {0};

/* Dynamic buffer pointers (will be allocated at runtime) */
static struct wave *g_waves_dynamic = NULL;
/* NOTE: g_unitary_waveform_dynamic removed — replaced by the global shared
 * g_sine_table[SINE_TABLE_SIZE] (4 KB) defined in wave_generation.c.
 * The former 40 MB allocation (10M × 4 bytes) is no longer needed.        */

int synth_runtime_init(int max_pixels, int pixels_per_note) {
    if (pixels_per_note < 1) {
        log_error("SYNTH", "pixels_per_note must be >= 1");
        return -1;
    }
    
    if (max_pixels % pixels_per_note != 0) {
        log_error("SYNTH", "max_pixels (%d) must be divisible by pixels_per_note (%d)",
                max_pixels, pixels_per_note);
        return -1;
    }
    
    g_synth_runtime.max_pixels = max_pixels;
    g_synth_runtime.pixels_per_note = pixels_per_note;
    g_synth_runtime.num_notes = max_pixels / pixels_per_note;
    
    log_startup_detail("RUNTIME", "Initialized runtime: %d notes (px=%d, px/note=%d)",
             g_synth_runtime.num_notes,
             g_synth_runtime.max_pixels,
             g_synth_runtime.pixels_per_note);
    
    return 0;
}

int synth_runtime_allocate_buffers(void) {
    if (g_synth_runtime.num_notes <= 0) {
        log_error("SYNTH", "Runtime config not initialized");
        return -1;
    }
    
    // Allocate waves array dynamically
    size_t waves_size = g_synth_runtime.num_notes * sizeof(struct wave);
    g_waves_dynamic = (struct wave*)calloc(g_synth_runtime.num_notes, sizeof(struct wave));
    if (!g_waves_dynamic) {
        log_error("SYNTH", "Failed to allocate waves array (%zu bytes)", waves_size);
        return -1;
    }
    
    /* The shared sine table (g_sine_table[SINE_TABLE_SIZE] = 4 KB) is a static
     * global array in wave_generation.c — no heap allocation needed here.    */
    log_startup_detail("RUNTIME", "Allocated waves array: %d notes, %d bytes/note",
             g_synth_runtime.num_notes, (int)sizeof(struct wave));
    log_startup_detail("RUNTIME", "Shared sine table: %d entries (4 KB) — no heap alloc",
             SINE_TABLE_SIZE);
    
    return 0;
}

void synth_runtime_free_buffers(void) {
    if (g_waves_dynamic) {
        free(g_waves_dynamic);
        g_waves_dynamic = NULL;
    }
    
    log_info("RUNTIME", "Freed dynamic buffers");
}

/* Accessor functions for dynamic buffers */
struct wave* synth_runtime_get_waves(void) {
    return g_waves_dynamic;
}

/* synth_runtime_get_unitary_waveform() removed — the shared sine table
 * g_sine_table[] in wave_generation.c replaces the former per-comma buffer.
 * Callers should use g_sine_table directly via wave_generation.h.           */
