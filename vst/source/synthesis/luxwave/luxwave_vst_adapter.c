/*
 * luxwave_vst_adapter.c
 *
 * VST integration for LuxWave engine.
 * Provides global engine instance and lock-free MIDI ring buffer.
 * Simpler than LuxSynth adapter — no double-buffer, LuxWave runs inline
 * in processBlock using the same image line as LuxSynth.
 */

#include "luxwave_vst_adapter.h"
#include <stdatomic.h>
#include <string.h>

/* ============================================================================
 * GLOBALS
 * ========================================================================== */

LuxWaveEngine g_luxwave_engine = {0};

/* ============================================================================
 * LOCK-FREE MIDI RING BUFFER
 * ========================================================================== */

static LuxWaveMidiEvent lw_midi_ring[LUXWAVE_MIDI_RING_SIZE];
static atomic_int lw_midi_write_pos = 0;
static atomic_int lw_midi_read_pos  = 0;

void luxwave_push_midi_event(uint8_t status, uint8_t data1, uint8_t data2)
{
    int wp = atomic_load_explicit(&lw_midi_write_pos, memory_order_relaxed);
    int next_wp = (wp + 1) % LUXWAVE_MIDI_RING_SIZE;
    int rp = atomic_load_explicit(&lw_midi_read_pos, memory_order_acquire);

    if (next_wp == rp) return; /* Ring full — drop event */

    lw_midi_ring[wp].status = status;
    lw_midi_ring[wp].data1  = data1;
    lw_midi_ring[wp].data2  = data2;

    atomic_store_explicit(&lw_midi_write_pos, next_wp, memory_order_release);
}

void luxwave_process_pending_midi(void)
{
    int rp = atomic_load_explicit(&lw_midi_read_pos, memory_order_relaxed);
    int wp = atomic_load_explicit(&lw_midi_write_pos, memory_order_acquire);

    while (rp != wp)
    {
        LuxWaveMidiEvent *ev = &lw_midi_ring[rp];
        if (ev->status == 0x90 && ev->data2 > 0)
            luxwave_engine_note_on(&g_luxwave_engine, ev->data1, ev->data2);
        else
            luxwave_engine_note_off(&g_luxwave_engine, ev->data1);

        rp = (rp + 1) % LUXWAVE_MIDI_RING_SIZE;
    }

    atomic_store_explicit(&lw_midi_read_pos, rp, memory_order_release);
}
