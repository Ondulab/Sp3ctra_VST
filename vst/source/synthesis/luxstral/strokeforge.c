/*
 * strokeforge.c
 *
 * StrokeForge — Blob-to-note mapping with waveform morphing
 *
 * Each detected blob (stroke on paper) produces:
 *   1. A morph factor: blob_width / morph_width_scale → g_waveform_morph
 *      (0.0 = pure sine,  1.0 = pure square wave)
 *   2. A Gaussian attenuation centered on the blob centroid:
 *      note_attenuation[n] = exp(-dist² / (2 × focus_sigma²))
 *      so only the drawn frequency is active; neighbors are attenuated.
 *
 * This module runs exclusively in the preprocessor thread (non-RT).
 *
 * Author: zhonx
 */

#include "strokeforge.h"
#include "../config/config_loader.h"
#include "../utils/logger.h"

/*
 * Waveform morph factor — defined in wave_generation.c.
 * extern avoids pulling the full wave_generation.h include chain.
 * 0.0 = pure sine | 1.0 = pure square.
 */
extern volatile float g_waveform_morph;

#include <string.h>
#include <math.h>

/**************************************************************************************
 * Blob Detection
 *
 * Scan notes[] for contiguous runs above threshold.
 * Merge runs separated by <= merge_gap.
 * Filter blobs narrower than min_width.
 **************************************************************************************/
static int detect_blobs(
    const float     *notes,
    int              num_notes,
    float            threshold,
    int              min_width,
    int              merge_gap,
    StrokeForgeBlob *blobs_out,
    int              max_blobs)
{
    int run_starts[STROKEFORGE_MAX_BLOBS];
    int run_ends[STROKEFORGE_MAX_BLOBS]; /* exclusive */
    int run_count = 0;

    /* Pass 1: find contiguous runs above threshold */
    int in_run = 0;
    for (int i = 0; i < num_notes && run_count < STROKEFORGE_MAX_BLOBS; i++)
    {
        if (notes[i] >= threshold)
        {
            if (!in_run) { run_starts[run_count] = i; in_run = 1; }
        }
        else
        {
            if (in_run) { run_ends[run_count++] = i; in_run = 0; }
        }
    }
    if (in_run && run_count < STROKEFORGE_MAX_BLOBS)
        run_ends[run_count++] = num_notes;

    /* Pass 2: merge runs separated by <= merge_gap */
    int ms[STROKEFORGE_MAX_BLOBS], me[STROKEFORGE_MAX_BLOBS], mc = 0;
    for (int i = 0; i < run_count; i++)
    {
        if (mc == 0)
        {
            ms[mc] = run_starts[i]; me[mc] = run_ends[i]; mc++;
        }
        else if (run_starts[i] - me[mc - 1] <= merge_gap)
        {
            me[mc - 1] = run_ends[i];
        }
        else if (mc < STROKEFORGE_MAX_BLOBS)
        {
            ms[mc] = run_starts[i]; me[mc] = run_ends[i]; mc++;
        }
    }

    /* Pass 3: filter by min_width and build output */
    int blob_count = 0;
    for (int i = 0; i < mc && blob_count < max_blobs; i++)
    {
        int width = me[i] - ms[i];
        if (width < min_width) continue;

        StrokeForgeBlob *b = &blobs_out[blob_count];
        memset(b, 0, sizeof(StrokeForgeBlob));
        b->active      = 1;
        b->start_note  = ms[i];
        b->end_note    = me[i];
        b->width_notes = (float)width;

        /* Amplitude-weighted centroid and peak */
        float sum_w = 0.0f, sum_wp = 0.0f, peak = 0.0f;
        for (int k = ms[i]; k < me[i]; k++)
        {
            float a = notes[k];
            if (a > peak) peak = a;
            sum_w  += a;
            sum_wp += a * (float)k;
        }
        b->peak_amplitude = peak;
        b->center_note = (sum_w > 1e-8f)
                         ? (int)(sum_wp / sum_w + 0.5f)
                         : (ms[i] + me[i]) / 2;
        blob_count++;
    }
    return blob_count;
}

/**************************************************************************************
 * Module Lifecycle
 **************************************************************************************/

void strokeforge_init(void)
{
    g_waveform_morph = 0.0f;
    log_info("STROKEFORGE", "StrokeForge initialized (enabled=%d)",
             g_sp3ctra_config.strokeforge_enabled);
}

void strokeforge_cleanup(void)
{
    g_waveform_morph = 0.0f;
    log_info("STROKEFORGE", "StrokeForge cleaned up");
}

/**************************************************************************************
 * Main Public API
 **************************************************************************************/

void strokeforge_analyze_frame(
    const float         *notes,
    int                  num_notes,
    float                contrast_factor,
    StrokeForgeFrameData *out)
{
    (void)contrast_factor; /* reserved for future adaptive threshold */

    /* Initialize output: all attenuations = 1.0 (no change) */
    out->blob_count = 0;
    for (int i = 0; i < num_notes && i < STROKEFORGE_MAX_NOTES; i++)
        out->note_attenuation[i] = 1.0f;

    /* StrokeForge is the MASTER switch.  When OFF the whole feature is inert —
     * no blob detection, no focus, no morph — regardless of Focus Only (which is
     * only a modifier of the ON state).  Early-return here (before the expensive
     * blob scan) resets morph + attenuation so no stale state leaks to the synth. */
    if (!g_sp3ctra_config.strokeforge_enabled)
    {
        out->morph = 0.0f;
        g_waveform_morph = 0.0f;
        return;
    }

    float threshold = g_sp3ctra_config.strokeforge_blob_base_threshold;
    int   min_width = g_sp3ctra_config.strokeforge_blob_min_width;
    int   merge_gap = g_sp3ctra_config.strokeforge_blob_merge_gap;

    /* Step 1: Detect blobs */
    out->blob_count = detect_blobs(
        notes, num_notes, threshold, min_width, merge_gap,
        out->blobs, STROKEFORGE_MAX_BLOBS);

    if (out->blob_count == 0)
    {
        /* No strokes → pure sine, no attenuation */
        out->morph = 0.0f;
        g_waveform_morph = 0.0f;
        return;
    }

    /* Step 2: Waveform morph from the widest blob — FULL mode only.
     * morph = width / morph_width_scale  (clamped to [0, 1]); 0 = sine, 1 = square.
     * Focus Only disables the morph (pure sine), keeping only the Gaussian focus —
     * so it is meaningful precisely BECAUSE StrokeForge is enabled here. */
    if (!g_sp3ctra_config.strokeforge_focus_only)
    {
        float widest = 0.0f;
        for (int b = 0; b < out->blob_count; b++)
            if (out->blobs[b].width_notes > widest)
                widest = out->blobs[b].width_notes;

        float scale = g_sp3ctra_config.strokeforge_morph_width_scale;
        if (scale <= 0.0f) scale = 400.0f;
        float morph = widest / scale;
        if (morph > 1.0f) morph = 1.0f;
        out->morph = morph;              /* per-frame, per-engine (M8) */
        g_waveform_morph = morph;        /* legacy global (diagnostics) */
    }
    else
    {
        /* Focus Only: Gaussian focus active but waveform stays pure sine */
        out->morph = 0.0f;
        g_waveform_morph = 0.0f;
    }

    /* Step 3: Per-blob: Gaussian focus OR spectral passthrough.
     *
     * Two modes depending on blob width vs spectral_width_threshold (T):
     *
     *   blob->width_notes < T  (or T == 0):
     *     Gaussian focus: only the center note plays at full volume.
     *     note_attenuation[n] = exp( -(n - center)² / (2 × sigma²) )
     *     sigma = strokeforge_blob_focus_sigma:
     *       Small  (3–5)  → pure tone (1–2 active notes)
     *       Medium (10)   → focused timbre (~semitone bandwidth)
     *       Large  (50+)  → spectral cloud
     *
     *   blob->width_notes >= T  (T > 0):
     *     Spectral passthrough: note_attenuation stays 1.0 for all notes
     *     in this blob → raw image pixel intensities flow through unchanged,
     *     as if StrokeForge were disabled for that region.
     *     Use case: wide painted areas → full spectral texture.
     *
     * Notes outside all blobs always keep attenuation = 1.0 (spectral).
     */
    {
        float sigma = g_sp3ctra_config.strokeforge_blob_focus_sigma;
        if (sigma < 0.5f) sigma = 0.5f;
        float inv_2sigma2 = 1.0f / (2.0f * sigma * sigma);
        float spectral_thresh = g_sp3ctra_config.strokeforge_spectral_width_threshold;

        for (int b = 0; b < out->blob_count; b++)
        {
            const StrokeForgeBlob *blob = &out->blobs[b];

            /* Wide blob → spectral passthrough: skip Gaussian, leave 1.0 */
            if (spectral_thresh > 0.0f && blob->width_notes >= spectral_thresh)
                continue;

            /* Narrow blob → Gaussian focus */
            float center = (float)blob->center_note;

            for (int n = blob->start_note;
                 n < blob->end_note && n < STROKEFORGE_MAX_NOTES; n++)
            {
                float dist = (float)n - center;
                float atten = expf(-dist * dist * inv_2sigma2);

                /* Multiple blobs can overlap: take the maximum attenuation
                 * (least aggressive) to avoid double-suppression */
                if (atten > out->note_attenuation[n])
                    out->note_attenuation[n] = atten;
                else if (out->note_attenuation[n] == 1.0f)
                    out->note_attenuation[n] = atten;
            }
        }
    }

    /* Rate-limited debug log */
    {
        static int sf_counter = 0;
        if (++sf_counter % 500 == 1)
        {
            log_info("STROKEFORGE",
                     "%d blob(s) | morph=%.3f | sigma=%.1f",
                     out->blob_count,
                     (double)g_waveform_morph,
                     (double)g_sp3ctra_config.strokeforge_blob_focus_sigma);
            for (int b = 0; b < out->blob_count; b++)
            {
                log_info("STROKEFORGE",
                         "  blob[%d]: notes[%d..%d] center=%d width=%.0f peak=%.3f",
                         b,
                         out->blobs[b].start_note,
                         out->blobs[b].end_note - 1,
                         out->blobs[b].center_note,
                         (double)out->blobs[b].width_notes,
                         (double)out->blobs[b].peak_amplitude);
            }
        }
    }
}
