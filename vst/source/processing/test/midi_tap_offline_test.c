/*
 * midi_tap_offline_test.c
 *
 * Offline harness for the MIDI TAP extractor — no JUCE, no RT, no build-system
 * dependency (this file is NOT in CMakeLists; compile it directly):
 *
 *   cc -O2 -std=c11 -I.. -o /tmp/midi_tap_test \
 *      test/midi_tap_offline_test.c ../midi_tap.c -lm
 *   /tmp/midi_tap_test
 *
 * Pins the one claim that is easy to break and hard to notice: FUNDAMENTAL
 * mode must recover f0 on material whose LOUDEST partial is a harmonic — which
 * is the normal case for a voice, and precisely where BANDS mode transcribes
 * the harmonic stack instead of the melody.
 *
 *   BANDS       must elect 67 (the loud 2nd harmonic) — wrong note BY DESIGN
 *   FUNDAMENTAL must elect 55 (the actual f0)
 *   FUNDAMENTAL must still elect 55 with the 3rd harmonic collapsed, which is
 *               what the per-factor floor in midi_tap_build_hps() buys (a
 *               vowel change routinely wipes one partial out).
 *
 * Also pins DENSE ("black MIDI") mode on an amplitude-modulated partial stack:
 *   dense=0 must latch one velocity per band (a handful of note-ons);
 *   dense=1 must RESTRIKE as the amplitude moves (many note-ons, a wide
 *           velocity spread) while keeping every off/on pair well-formed —
 *           the envelope-carrying property that keeps a voice intelligible.
 */
#include "../midi_tap.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PX 3456
#define OCT 8

static uint8_t r[PX], g[PX], b[PX];

/* Place a Gaussian bump of amplitude `amp` centred on MIDI note `m`. */
static void bump(double m, double amp)
{
    const double pps = (double) PX / (OCT * 12.0);
    const double midi_low = 69.0 + 12.0 * log2(65.406 / 440.0);
    const double c = (m - midi_low) * pps;
    const double sigma = pps * 0.35;          /* ~1/3 semitone wide */
    for (int i = 0; i < PX; ++i) {
        const double d = ((double) i - c) / sigma;
        double v = amp * exp(-0.5 * d * d);
        double cur = (double) r[i] + v;
        if (cur > 255.0) cur = 255.0;
        r[i] = g[i] = b[i] = (uint8_t) cur;
    }
}

static int s_drop_h3 = 0;   /* simulate a vowel with a collapsed 3rd harmonic */

static void build_voice_line(void)
{
    memset(r, 0, sizeof r); memset(g, 0, sizeof g); memset(b, 0, sizeof b);
    bump(55.0,  60.0);    /* f0  — G3, the note we WANT */
    bump(67.0, 200.0);    /* k=2 — the LOUDEST partial  */
    if (!s_drop_h3)
        bump(74.0, 120.0);/* k=3 */
    bump(79.0,  90.0);    /* k=4 */
}

static int run(int mode, const char *label)
{
    MidiTapState *st = midi_tap_instance(0);
    midi_tap_init(st);

    MidiTapConfig c = midi_tap_config_default();
    c.enabled         = 1;
    c.mode            = mode;
    c.background_mode = MIDI_TAP_BG_BLACK;   /* bright material on black */
    c.max_poly        = 1;                   /* monophonic melody        */
    c.peak_only       = 1;
    c.thresh          = 5.0f;
    c.rel             = 0.0f;
    c.smooth          = 1.0f;                /* no temporal smoothing    */
    c.attack_ms       = 1.0f;
    c.min_on_ms       = 1.0f;
    c.release_ms      = 1.0f;
    c.max_events_per_s = 100000.0f;
    c.axis_low_hz     = 65.406f;
    st->config = c;

    build_voice_line();
    for (int i = 0; i < 40; ++i) {
        midi_tap_process_line(st, r, g, b, PX, OCT);
        usleep(2000);                        /* ~500 lines/s */
    }

    /* Drain the ring and report the first note-on. */
    const uint32_t w = midi_tap_ring_writepos(st);
    const uint32_t avail = (w > MIDI_TAP_RING_SLOTS) ? MIDI_TAP_RING_SLOTS : w;
    int first_on = -1, n_on = 0;
    for (uint32_t k = 0; k < avail; ++k) {
        MidiTapEvent e;
        if (!midi_tap_ring_get(st, w - avail + k, &e)) continue;
        if (e.status == 0x90) { if (first_on < 0) first_on = e.note; ++n_on; }
    }
    printf("%-12s -> first note-on = %3d   (%d note-on total)\n",
           label, first_on, n_on);
    return first_on;
}

/* Same stack as build_voice_line, globally scaled — the "syllable" envelope. */
static void build_voice_line_scaled(double s)
{
    memset(r, 0, sizeof r); memset(g, 0, sizeof g); memset(b, 0, sizeof b);
    bump(55.0,  60.0 * s);
    bump(67.0, 200.0 * s);
    bump(74.0, 120.0 * s);
    bump(79.0,  90.0 * s);
}

/* Feed ~250 lines whose amplitude breathes at ~10 Hz; report note-on count,
 * note-on velocity spread and pairing errors (off with nothing held / double
 * on). Returns the note-on count. */
static int run_dense(int dense, int *pair_errors, int *vel_spread)
{
    MidiTapState *st = midi_tap_instance(0);
    midi_tap_init(st);

    MidiTapConfig c = midi_tap_config_default();
    c.enabled          = 1;
    c.mode             = MIDI_TAP_MODE_BANDS;
    c.background_mode  = MIDI_TAP_BG_BLACK;
    c.max_poly         = 8;
    c.peak_only        = 1;
    c.thresh           = 5.0f;
    c.rel              = 0.0f;
    c.smooth           = 1.0f;
    c.attack_ms        = 1.0f;
    c.min_on_ms        = 1.0f;
    c.release_ms       = 8.0f;
    c.vel_curve        = MIDI_TAP_VEL_LINEAR;
    c.vel_span         = 200.0f;
    c.max_events_per_s = 100000.0f;
    c.axis_low_hz      = 65.406f;
    c.dense            = dense;
    c.retrig_ms        = 4.0f;
    c.retrig_delta     = 2;
    st->config = c;

    for (int i = 0; i < 250; ++i) {
        const double s = 0.65 + 0.35 * sin(6.283185307179586 * (double) i / 50.0);
        build_voice_line_scaled(s);
        midi_tap_process_line(st, r, g, b, PX, OCT);
        usleep(2000);                        /* ~500 lines/s */
    }

    const uint32_t w = midi_tap_ring_writepos(st);
    const uint32_t avail = (w > MIDI_TAP_RING_SLOTS) ? MIDI_TAP_RING_SLOTS : w;
    int n_on = 0, errors = 0, vmin = 127, vmax = 1;
    uint8_t held[128]; memset(held, 0, sizeof held);
    for (uint32_t k = 0; k < avail; ++k) {
        MidiTapEvent e;
        if (!midi_tap_ring_get(st, w - avail + k, &e)) continue;
        if (e.status == 0x90) {
            if (held[e.note]) ++errors;      /* double on — off/on order broken */
            held[e.note] = 1;
            ++n_on;
            if (e.vel < vmin) vmin = e.vel;
            if (e.vel > vmax) vmax = e.vel;
        } else if (e.status == 0x80) {
            if (!held[e.note]) ++errors;     /* orphan off                      */
            held[e.note] = 0;
        }
    }
    if (pair_errors) *pair_errors = errors;
    if (vel_spread)  *vel_spread  = (n_on > 0) ? (vmax - vmin) : 0;
    return n_on;
}

int main(void)
{
    printf("voice line: f0=55 (amp 60), h2=67 (amp 200 <- loudest), "
           "h3=74 (120), h4=79 (90)\n\n");
    const int bands = run(MIDI_TAP_MODE_BANDS,       "BANDS");
    const int fund  = run(MIDI_TAP_MODE_FUNDAMENTAL, "FUNDAMENTAL");

    printf("\nsame line with the 3rd harmonic COLLAPSED (vowel change):\n");
    s_drop_h3 = 1;
    const int fund2 = run(MIDI_TAP_MODE_FUNDAMENTAL, "FUNDAMENTAL");

    printf("\nDENSE (black MIDI) on the same stack, amplitude breathing at ~10 Hz:\n");
    s_drop_h3 = 0;
    int errC = 0, errD = 0, sprC = 0, sprD = 0;
    const int onsC = run_dense(0, &errC, &sprC);
    const int onsD = run_dense(1, &errD, &sprD);
    printf("dense=0      -> %4d note-on, vel spread %3d, %d pairing errors\n",
           onsC, sprC, errC);
    printf("dense=1      -> %4d note-on, vel spread %3d, %d pairing errors\n",
           onsD, sprD, errD);

    printf("\n");
    int ok = 1;
    if (bands != 67) { printf("UNEXPECTED: BANDS should follow the loudest partial (67)\n"); ok = 0; }
    if (fund  != 55) { printf("FAIL: FUNDAMENTAL should recover f0 = 55, got %d\n", fund); ok = 0; }
    if (fund2 != 55) { printf("FAIL: FUNDAMENTAL should survive a missing harmonic, got %d\n", fund2); ok = 0; }
    if (errC != 0 || errD != 0)
        { printf("FAIL: off/on pairing broken (classic %d, dense %d)\n", errC, errD); ok = 0; }
    if (onsD < 4 * ((onsC > 0) ? onsC : 1))
        { printf("FAIL: dense should restrike (got %d ons vs %d classic)\n", onsD, onsC); ok = 0; }
    if (sprD < 20)
        { printf("FAIL: dense velocity should track the envelope (spread %d)\n", sprD); ok = 0; }
    if (ok) printf("PASS: FUNDAMENTAL recovers f0 where BANDS follows the loudest\n"
                   "      partial and survives a missing harmonic; DENSE restrikes\n"
                   "      with envelope-tracking velocities and clean pairing.\n");
    return ok ? 0 : 1;
}
