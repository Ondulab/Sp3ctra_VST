/*
 * luxsynth_vst_adapter.h
 *
 * VST integration layer for the LuxSynth additive synthesis engine.
 * Provides:
 *   - Lock-free double-buffer for audio output
 *   - Lock-free MIDI event ring buffer
 *   - Spectral data bridge from image pipeline
 *   - Thread synchronization primitives
 *
 * RT-safety: All functions called from processBlock are lock-free.
 */

#ifndef LUXSYNTH_VST_ADAPTER_H
#define LUXSYNTH_VST_ADAPTER_H

#include "synth_luxsynth_engine.h"
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * DOUBLE-BUFFER for audio output (same pattern as LuxStral)
 * ========================================================================== */

typedef struct {
    float *data;
    volatile int ready;
} LuxSynthAudioBuffer;

/* Global double-buffer (matches LuxStral pattern) */
extern LuxSynthAudioBuffer luxsynth_buffers_L[2];
extern LuxSynthAudioBuffer luxsynth_buffers_R[2];
extern volatile int luxsynth_buffer_index;

/* ============================================================================
 * LOCK-FREE MIDI EVENT RING BUFFER
 * ========================================================================== */

#define LUXSYNTH_MIDI_RING_SIZE 256

typedef struct {
    uint8_t type;    /* 0x90 = Note On, 0x80 = Note Off */
    uint8_t note;
    uint8_t velocity;
} LuxSynthMidiEvent;

typedef struct {
    LuxSynthMidiEvent events[LUXSYNTH_MIDI_RING_SIZE];
    atomic_int write_pos;
    atomic_int read_pos;
} LuxSynthMidiRing;

/* Global MIDI ring buffer */
extern LuxSynthMidiRing g_luxsynth_midi_ring;

/* ============================================================================
 * GLOBAL ENGINE INSTANCE
 * ========================================================================== */

extern LuxSynthEngine g_luxsynth_engine;

/* ============================================================================
 * CONSUMED FLAG (for thread synchronization)
 * ========================================================================== */

extern volatile int luxsynth_buffer_consumed_flag;

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

/**
 * @brief Initialize LuxSynth audio double-buffers.
 * @param buffer_size  Samples per buffer (from DAW).
 * @return 0 on success, -1 on failure.
 */
int luxsynth_init_audio_buffers(int buffer_size);

/**
 * @brief Free LuxSynth audio double-buffers.
 */
void luxsynth_free_audio_buffers(void);

/**
 * @brief Check if LuxSynth audio buffers are allocated and ready.
 */
int luxsynth_are_buffers_ready(void);

/**
 * @brief Signal that processBlock has consumed a buffer.
 * Unblocks the processing thread spin-wait.
 */
void luxsynth_signal_consumed(void);

/**
 * @brief Wait for processBlock to consume the current buffer.
 * Called by the processing thread. Spin-waits with yield.
 */
void luxsynth_wait_for_consumed(void);

/**
 * @brief Push a MIDI event into the lock-free ring buffer.
 * Called from processBlock (RT thread).
 */
void luxsynth_push_midi_event(uint8_t type, uint8_t note, uint8_t velocity);

/**
 * @brief Process all pending MIDI events in the ring buffer.
 * Called from the LuxSynth processing thread.
 */
void luxsynth_process_pending_midi(void);

/**
 * @brief Main processing function called by the LuxSynth thread.
 * Reads spectral data, processes MIDI, generates audio, writes to double-buffer.
 * @param ctx  Context pointer (for thread running flag).
 */
void luxsynth_processing_loop(volatile int *running_flag);

#ifdef __cplusplus
}
#endif

#endif /* LUXSYNTH_VST_ADAPTER_H */
