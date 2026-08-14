/*
 * strokeforge.h
 *
 * StrokeForge — Blob-to-note mapping with waveform morphing
 *
 * Each stroke (blob) on the CIS scanner maps to:
 *   1. A single fundamental note (blob centroid = the played frequency)
 *   2. A waveform shape (blob width → sine to square morphing)
 *   3. Gaussian attenuation of neighboring oscillators within the blob
 *      (only the center oscillator plays at full volume)
 *
 * Architecture (non-RT preprocessor thread):
 *   1. Blob detection on per-note amplitude array
 *   2. Centroid computation → center_note (the one active oscillator)
 *   3. Morph = blob_width / morph_width_scale → g_waveform_morph
 *   4. note_attenuation[] = Gaussian(dist from center, focus_sigma)
 *
 * RT thread reads:
 *   - g_waveform_morph   (volatile float) for waveform lookup blend
 *   - note_attenuation[] (via double-buffer) to multiply precomputed_volume
 *
 * Author: zhonx
 */

#ifndef STROKEFORGE_H
#define STROKEFORGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*──────────────────────────────────────────────────────────────────────────────
 * Constants
 *──────────────────────────────────────────────────────────────────────────────*/

#define STROKEFORGE_MAX_BLOBS  16    /* max simultaneous strokes per frame      */
#define STROKEFORGE_MAX_NOTES  4096  /* max CIS oscillators (matches runtime)   */

/*──────────────────────────────────────────────────────────────────────────────
 * Blob descriptor — one detected stroke on the CIS image
 *──────────────────────────────────────────────────────────────────────────────*/

typedef struct
{
    int   active;        /* 1 = valid blob                                      */
    int   start_note;    /* first note index inside the blob (inclusive)        */
    int   end_note;      /* last  note index inside the blob (exclusive)        */
    int   center_note;   /* amplitude-weighted centroid → the played frequency  */
    float width_notes;   /* blob width in note-counts (end - start)             */
    float peak_amplitude;/* maximum amplitude within the blob                   */
} StrokeForgeBlob;

/*──────────────────────────────────────────────────────────────────────────────
 * Per-frame output — passed to the RT precompute stage via double-buffer
 *──────────────────────────────────────────────────────────────────────────────*/

typedef struct
{
    StrokeForgeBlob blobs[STROKEFORGE_MAX_BLOBS];
    int             blob_count;

    /*
     * Per-note volume multiplier — applied to precomputed_volume before mixing.
     *
     * Outside all blobs : 1.0  (spectral passthrough, image data unchanged)
     * Inside a blob     : exp( -(note - center_note)² / (2 × focus_sigma²) )
     *
     * focus_sigma (configured via strokeforge_blob_focus_sigma, default ≈ 10):
     *   Small (3–5)  → very tight, only 1–2 oscillators active → pure tone
     *   Medium (10)  → natural stroke spread, a few notes around center
     *   Large (50+)  → loose, nearly all blob oscillators active (spectral cloud)
     */
    float note_attenuation[STROKEFORGE_MAX_NOTES];

    /*
     * Waveform morph for THIS frame: 0.0 = pure sine … 1.0 = pure square.
     * Per-frame, per-pipeline-call output (M8) — each LuxStral engine reads its
     * own db's value (the global g_waveform_morph holds the LAST call's frame,
     * which cross-talks between engines A and B).
     */
    float morph;
} StrokeForgeFrameData;

/*──────────────────────────────────────────────────────────────────────────────
 * Public API
 *──────────────────────────────────────────────────────────────────────────────*/

/** @brief Initialize module state (call once at startup). */
void strokeforge_init(void);

/** @brief Release module state (call at shutdown). */
void strokeforge_cleanup(void);

/**
 * @brief Analyze one frame of per-note amplitude data.
 *
 * @param notes          Per-note amplitude array [0..num_notes-1], 0.0–1.0
 * @param num_notes      Number of CIS oscillators
 * @param out            Output structure to fill (zeroed internally)
 */
void strokeforge_analyze_frame(
    const float         *notes,
    int                  num_notes,
    StrokeForgeFrameData *out);

#ifdef __cplusplus
}
#endif

#endif /* STROKEFORGE_H */
