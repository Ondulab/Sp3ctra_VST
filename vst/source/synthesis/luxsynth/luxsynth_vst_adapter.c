/*
 * luxsynth_vst_adapter.c
 *
 * VST integration: double-buffer, MIDI ring, spectral bridge.
 * Same pattern as LuxStral's vst_adapters.cpp.
 */

#include "luxsynth_vst_adapter.h"
#include "image_pipeline_types.h"
#include <stdlib.h>
#include <string.h>
#include <sched.h>  /* sched_yield() */

/* ============================================================================
 * GLOBALS
 * ========================================================================== */

LuxSynthAudioBuffer luxsynth_buffers_L[2] = {{NULL, 0}, {NULL, 0}};
LuxSynthAudioBuffer luxsynth_buffers_R[2] = {{NULL, 0}, {NULL, 0}};
volatile int luxsynth_buffer_index = 0;
volatile int luxsynth_buffer_consumed_flag = 0;

LuxSynthMidiRing g_luxsynth_midi_ring = {{{0}}, 0, 0};
LuxSynthEngine g_luxsynth_engine = {0};

static int luxsynth_buffer_size = 0;

/* ============================================================================
 * DOUBLE-BUFFER MANAGEMENT
 * ========================================================================== */

int luxsynth_init_audio_buffers(int buffer_size)
{
    if (buffer_size <= 0 || buffer_size > LUXSYNTH_MAX_BUFFER_SIZE)
        return -1;

    /* Free any previous buffers */
    luxsynth_free_audio_buffers();

    luxsynth_buffer_size = buffer_size;

    for (int i = 0; i < 2; i++)
    {
        luxsynth_buffers_L[i].data = (float *)calloc((size_t)buffer_size, sizeof(float));
        luxsynth_buffers_R[i].data = (float *)calloc((size_t)buffer_size, sizeof(float));
        luxsynth_buffers_L[i].ready = 0;
        luxsynth_buffers_R[i].ready = 0;

        if (!luxsynth_buffers_L[i].data || !luxsynth_buffers_R[i].data)
        {
            luxsynth_free_audio_buffers();
            return -1;
        }
    }

    luxsynth_buffer_index = 0;
    luxsynth_buffer_consumed_flag = 0;

    /* Initialize MIDI ring */
    atomic_store(&g_luxsynth_midi_ring.write_pos, 0);
    atomic_store(&g_luxsynth_midi_ring.read_pos, 0);

    return 0;
}

void luxsynth_free_audio_buffers(void)
{
    for (int i = 0; i < 2; i++)
    {
        if (luxsynth_buffers_L[i].data) { free(luxsynth_buffers_L[i].data); luxsynth_buffers_L[i].data = NULL; }
        if (luxsynth_buffers_R[i].data) { free(luxsynth_buffers_R[i].data); luxsynth_buffers_R[i].data = NULL; }
        luxsynth_buffers_L[i].ready = 0;
        luxsynth_buffers_R[i].ready = 0;
    }
    luxsynth_buffer_size = 0;
}

int luxsynth_are_buffers_ready(void)
{
    return (luxsynth_buffers_L[0].data != NULL && luxsynth_buffers_R[0].data != NULL &&
            luxsynth_buffers_L[1].data != NULL && luxsynth_buffers_R[1].data != NULL);
}

/* ============================================================================
 * THREAD SYNCHRONIZATION
 * ========================================================================== */

void luxsynth_signal_consumed(void)
{
    __atomic_store_n(&luxsynth_buffer_consumed_flag, 1, __ATOMIC_RELEASE);
}

void luxsynth_wait_for_consumed(void)
{
    /* Spin-wait with yield — same pattern as LuxStral */
    int timeout = 5000; /* ~50ms at 10us per iteration */
    while (!__atomic_load_n(&luxsynth_buffer_consumed_flag, __ATOMIC_ACQUIRE))
    {
        sched_yield();
        if (--timeout <= 0) break;
    }
    __atomic_store_n(&luxsynth_buffer_consumed_flag, 0, __ATOMIC_RELEASE);
}

/* ============================================================================
 * LOCK-FREE MIDI RING BUFFER
 * ========================================================================== */

void luxsynth_push_midi_event(uint8_t type, uint8_t note, uint8_t velocity)
{
    int wp = atomic_load_explicit(&g_luxsynth_midi_ring.write_pos, memory_order_relaxed);
    int next_wp = (wp + 1) % LUXSYNTH_MIDI_RING_SIZE;
    int rp = atomic_load_explicit(&g_luxsynth_midi_ring.read_pos, memory_order_acquire);

    if (next_wp == rp) return; /* Ring full — drop event */

    g_luxsynth_midi_ring.events[wp].type = type;
    g_luxsynth_midi_ring.events[wp].note = note;
    g_luxsynth_midi_ring.events[wp].velocity = velocity;

    atomic_store_explicit(&g_luxsynth_midi_ring.write_pos, next_wp, memory_order_release);
}

void luxsynth_process_pending_midi(void)
{
    int rp = atomic_load_explicit(&g_luxsynth_midi_ring.read_pos, memory_order_relaxed);
    int wp = atomic_load_explicit(&g_luxsynth_midi_ring.write_pos, memory_order_acquire);

    while (rp != wp)
    {
        LuxSynthMidiEvent *ev = &g_luxsynth_midi_ring.events[rp];
        if (ev->type == 0x90 && ev->velocity > 0)
            luxsynth_engine_note_on(&g_luxsynth_engine, ev->note, ev->velocity);
        else
            luxsynth_engine_note_off(&g_luxsynth_engine, ev->note);

        rp = (rp + 1) % LUXSYNTH_MIDI_RING_SIZE;
    }

    atomic_store_explicit(&g_luxsynth_midi_ring.read_pos, rp, memory_order_release);
}

/* ============================================================================
 * MAIN PROCESSING LOOP (called from LuxSynthProcessingThread)
 * ========================================================================== */

void luxsynth_processing_loop(volatile int *running_flag)
{
    if (!running_flag) return;

    while (__atomic_load_n(running_flag, __ATOMIC_ACQUIRE))
    {
        /* 1. Process pending MIDI events */
        luxsynth_process_pending_midi();

        /* 2. Generate audio into current write buffer */
        int write_idx = __atomic_load_n(&luxsynth_buffer_index, __ATOMIC_ACQUIRE);

        if (luxsynth_buffers_L[write_idx].data && luxsynth_buffers_R[write_idx].data)
        {
            luxsynth_engine_process(&g_luxsynth_engine,
                                    luxsynth_buffer_size,
                                    luxsynth_buffers_L[write_idx].data,
                                    luxsynth_buffers_R[write_idx].data);

            /* Mark buffer as ready */
            __atomic_store_n(&luxsynth_buffers_L[write_idx].ready, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&luxsynth_buffers_R[write_idx].ready, 1, __ATOMIC_RELEASE);

            /* Flip buffer index */
            int next_idx = 1 - write_idx;
            __atomic_store_n(&luxsynth_buffer_index, next_idx, __ATOMIC_RELEASE);

            /* Wait for consumer to read before overwriting */
            luxsynth_wait_for_consumed();
        }
        else
        {
            /* Buffers not allocated — yield and retry */
            sched_yield();
        }
    }
}
