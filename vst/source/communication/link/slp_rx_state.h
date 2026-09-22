/**
 * @file slp_rx_state.h
 * @brief Lock-free state published by the STREAM receiver (udpThread) for the
 *        rest of the plugin: the latest HID sample (buttons + IMU) and the
 *        reception statistics of the Sp3ctra Link flows.
 *
 * Single writer (udpThread), any number of readers (audio thread for the HID
 * → MIDI mapper, message thread for the SETUP page). Plain C so the C receiver
 * can publish; C++ readers include it as-is.
 */
#ifndef SLP_RX_STATE_H
#define SLP_RX_STATE_H

#include <stdint.h>
#include "sp3ctra_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One decoded slp_hid datagram (host byte order, natural alignment). */
typedef struct
{
    uint32_t timestamp_us;                 /* device monotonic clock */
    uint16_t valid_mask;                   /* SLP_HID_* */
    uint8_t  button_count;
    uint8_t  button_state;                 /* bit i = button i pressed */
    uint32_t button_seq[SLP_MAX_BUTTONS];  /* +1 per edge */
    float    acc[3];                       /* g */
    float    gyro[3];                      /* dps */
    float    temp_c;
    /* Device-side gestures (SLP_HID_GESTURES): the u8 counter wraps, react to
     * changes only. */
    uint8_t  gesture_face;                 /* enum slp_face: face it RESTS on */
    uint8_t  hit_seq;
    uint8_t  hit_velocity;                 /* 1..127 */
    uint8_t  hit_face;                     /* enum slp_face: face STRUCK */
} slp_hid_sample;

/** Writer (udpThread): publish a new sample. Never blocks. */
void     slp_hid_publish(const slp_hid_sample *s);

/** Reader (any thread, RT-safe): copy the latest sample.
 *  @return the sample generation (0 = nothing received yet); compare with the
 *          previous value to know whether anything new arrived. */
uint32_t slp_hid_read(slp_hid_sample *out);

/** Milliseconds since the last HID datagram (UINT32_MAX when none yet). */
uint32_t slp_hid_age_ms(void);

/* Reception statistics of the STREAM flows (monotonic counters). */
typedef struct
{
    uint32_t line_datagrams;      /* LINE datagrams accepted */
    uint32_t line_lost;           /* gaps in the LINE sequence numbers */
    uint32_t lines_complete;      /* lines assembled and published */
    uint32_t lines_incomplete;    /* lines abandoned with missing fragments */
    uint32_t line_pixels;         /* width of the last complete line (pixels) */
    uint32_t hid_datagrams;
    uint32_t hid_lost;
    uint32_t legacy_datagrams;    /* pre-4.0 firmware packets (0x11..0x15) */
    uint32_t bad_datagrams;       /* wrong magic / version / length */
    uint32_t last_line_ms;        /* wall clock (ms) of the last complete line, 0 = never */
    uint32_t last_hid_ms;
} slp_rx_stats;

/* Writer side: the receiver bumps the counters through these (single writer). */
void slp_rx_stats_writer(slp_rx_stats **out);   /* pointer to the live struct */
/** Reader side: coherent-enough copy for display purposes. */
void slp_rx_stats_snapshot(slp_rx_stats *out);
/** Wall clock helper shared with the receiver (ms, monotonic). */
uint32_t slp_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* SLP_RX_STATE_H */
