/*
 * luxwave_vst_adapter.h
 *
 * VST adapter for the LuxWave engine.
 * Provides MIDI ring buffer and global engine instance.
 * Mirrors the luxsynth_vst_adapter pattern.
 */

#ifndef LUXWAVE_VST_ADAPTER_H
#define LUXWAVE_VST_ADAPTER_H

#include "synth_luxwave_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Global engine instance (allocated statically) ─────────────────────── */
extern LuxWaveEngine g_luxwave_engine;

/* ── MIDI ring buffer (lock-free, RT-safe) ─────────────────────────────── */
#define LUXWAVE_MIDI_RING_SIZE 256

typedef struct {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
} LuxWaveMidiEvent;

void luxwave_push_midi_event(uint8_t status, uint8_t data1, uint8_t data2);
void luxwave_process_pending_midi(void);

#ifdef __cplusplus
}
#endif

#endif /* LUXWAVE_VST_ADAPTER_H */
