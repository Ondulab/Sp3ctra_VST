/*
 * strokeforge.c
 *
 * StrokeForge — Blob-centric harmonic morphing for additive synthesis
 *
 * Implementation of blob detection, shape descriptor extraction,
 * harmonic recipe computation, and phase coherence targets.
 *
 * This module runs in the preprocessor thread (non-RT).
 * All outputs are stored in StrokeForgeFrameData for RT consumption.
 *
 * Author: zhonx
 * Created: 2026-03-13
 */

#include "strokeforge.h"
#include "../config/config_loader.h"
#include "../utils/logger.h"

/*
 * Waveform morph factor (defined in wave_generation.c).
 * extern declaration avoids pulling the full wave_generation.h include chain
 * (vst_adapters_c.h → doublebuffer.h) into this non-RT preprocessor module.
 * 0.0 = pure sine | 1.0 = pure square.
 */
extern volatile float g_waveform_morph;

#include <string.h>
#include <math.h>

/**************************************************************************************
 * Temporal Smoothing State
 *
 * Tracks previous frame's blob centroids and symmetry values for IIR smoothing.
 * Blobs are matched frame-to-frame by nearest center_note distance.
 * This state is non-RT (preprocessor thread only).
 **************************************************************************************/
#define SF_SMOOTH_IIR_SYMMETRY  0.30f   /* α for symmetry IIR (lower = smoother) */
#define SF_SMOOTH_IIR_CENTER    0.35f   /* α for center_note IIR                  */
#define SF_SMOOTH_MAX_DIST      200     /* max note distance for blob matching     */

typedef struct
{
    int   center_note;      /* smoothed center note (fixed-point: x16) */
    float symmetry;         /* smoothed symmetry value                 */
} SFSmoothEntry;

static struct
{
    int           valid;
    int           count;
    SFSmoothEntry entries[STROKEFORGE_MAX_BLOBS];
} s_smooth_state = {0, 0, {{0, 0.0f}}};

/**************************************************************************************
 * Internal Helper: Clamp float to [lo, hi]
 **************************************************************************************/
static inline float sf_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/**************************************************************************************
 * Module Lifecycle
 **************************************************************************************/

void strokeforge_init(void)
{
    /* Reset temporal smoothing state */
    s_smooth_state.valid = 0;
    s_smooth_state.count = 0;

    log_info("STROKEFORGE", "StrokeForge module initialized (enabled=%d, max_harmonics=%d)",
             g_sp3ctra_config.strokeforge_enabled,
             g_sp3ctra_config.strokeforge_max_harmonics);
}

void strokeforge_cleanup(void)
{
    log_info("STROKEFORGE", "StrokeForge module cleaned up");
}

/**************************************************************************************
 * Step 1: Blob Detection
 *
 * Scan notes[] for contiguous runs above threshold.
 * Merge runs separated by <= blob_merge_gap.
 * Filter by minimum width.
 **************************************************************************************/
static int detect_blobs(
    const float *notes,
    int num_notes,
    float threshold,
    int min_width,
    int merge_gap,
    StrokeForgeBlob *blobs_out,
    int max_blobs)
{
    int blob_count = 0;

    /* Temporary raw runs before merging */
    int run_starts[STROKEFORGE_MAX_BLOBS];
    int run_ends[STROKEFORGE_MAX_BLOBS]; /* exclusive */
    int run_count = 0;

    /* Pass 1: find contiguous runs above threshold */
    int in_run = 0;
    for (int i = 0; i < num_notes && run_count < STROKEFORGE_MAX_BLOBS; i++)
    {
        if (notes[i] >= threshold)
        {
            if (!in_run)
            {
                run_starts[run_count] = i;
                in_run = 1;
            }
        }
        else
        {
            if (in_run)
            {
                run_ends[run_count] = i;
                run_count++;
                in_run = 0;
            }
        }
    }
    /* Close last run if still active */
    if (in_run && run_count < STROKEFORGE_MAX_BLOBS)
    {
        run_ends[run_count] = num_notes;
        run_count++;
    }

    /* Pass 2: merge runs separated by <= merge_gap */
    int merged_starts[STROKEFORGE_MAX_BLOBS];
    int merged_ends[STROKEFORGE_MAX_BLOBS];
    int merged_count = 0;

    for (int i = 0; i < run_count; i++)
    {
        if (merged_count == 0)
        {
            merged_starts[merged_count] = run_starts[i];
            merged_ends[merged_count] = run_ends[i];
            merged_count++;
        }
        else
        {
            int gap = run_starts[i] - merged_ends[merged_count - 1];
            if (gap <= merge_gap)
            {
                /* Merge: extend the previous run */
                merged_ends[merged_count - 1] = run_ends[i];
            }
            else
            {
                /* New separate blob */
                if (merged_count < STROKEFORGE_MAX_BLOBS)
                {
                    merged_starts[merged_count] = run_starts[i];
                    merged_ends[merged_count] = run_ends[i];
                    merged_count++;
                }
            }
        }
    }

    /* Pass 3: filter by minimum width and populate blobs */
    for (int i = 0; i < merged_count && blob_count < max_blobs; i++)
    {
        int width = merged_ends[i] - merged_starts[i];
        if (width >= min_width)
        {
            StrokeForgeBlob *b = &blobs_out[blob_count];
            memset(b, 0, sizeof(StrokeForgeBlob));
            b->active = 1;
            b->start_note = merged_starts[i];
            b->end_note = merged_ends[i];
            b->width_notes = (float)width;
            blob_count++;
        }
    }

    return blob_count;
}

/**************************************************************************************
 * Step 2: Compute Blob Centroid, Peak, and Gaussian-Weighted Amplitude
 **************************************************************************************/
static void compute_blob_amplitudes(
    const float *notes,
    StrokeForgeBlob *blob)
{
    float sigma = g_sp3ctra_config.strokeforge_volume_center_sigma;

    /* Find peak and centroid */
    float sum_weighted_pos = 0.0f;
    float sum_weights = 0.0f;
    float peak = 0.0f;

    for (int i = blob->start_note; i < blob->end_note; i++)
    {
        float amp = notes[i];
        if (amp > peak) peak = amp;
        sum_weighted_pos += amp * (float)i;
        sum_weights += amp;
    }

    blob->peak_amplitude = peak;

    /* Centroid (amplitude-weighted center) */
    if (sum_weights > 1e-8f)
    {
        float centroid_f = sum_weighted_pos / sum_weights;
        blob->center_note = (int)(centroid_f + 0.5f);
    }
    else
    {
        blob->center_note = (blob->start_note + blob->end_note) / 2;
    }

    /* Gaussian-weighted amplitude around centroid */
    float width = blob->width_notes;
    float gauss_sigma = sigma * width; /* sigma as fraction of width */
    if (gauss_sigma < 0.5f) gauss_sigma = 0.5f;

    float gauss_sum = 0.0f;
    float gauss_weight_sum = 0.0f;
    float center_f = (float)blob->center_note;

    for (int i = blob->start_note; i < blob->end_note; i++)
    {
        float dist = (float)i - center_f;
        float gauss_w = expf(-0.5f * (dist * dist) / (gauss_sigma * gauss_sigma));
        gauss_sum += notes[i] * gauss_w;
        gauss_weight_sum += gauss_w;
    }

    if (gauss_weight_sum > 1e-8f)
    {
        blob->weighted_amplitude = gauss_sum / gauss_weight_sum;
    }
    else
    {
        blob->weighted_amplitude = peak;
    }
}

/**************************************************************************************
 * Step 3: Extract Shape Descriptors (Flatness, Symmetry, Edge Sharpness)
 *
 * Flatness  — fill factor: mean(notes/peak) over blob extent.
 *             Rectangle → ~1.0, Gaussian bell → ~0.4-0.6.
 *             More robust than kurtosis for CIS sensor data.
 *
 * Symmetry  — bilateral: compare left and right halves of the amplitude profile
 *             pixel by pixel and normalize by peak.
 *             Perfect mirror → 1.0, strongly asymmetric → 0.0.
 *             More robust than skewness for medium-width blobs.
 *
 * Edge      — gradient magnitude at the two boundary pixels.
 **************************************************************************************/
static void extract_shape_descriptors(
    const float *notes,
    StrokeForgeBlob *blob)
{
    int start = blob->start_note;
    int end = blob->end_note;
    int width = end - start;

    if (width < 2)
    {
        /* Degenerate blob: pure sine */
        blob->shape.flatness = 0.0f;
        blob->shape.symmetry = 1.0f;
        blob->shape.edge_sharpness = 0.0f;
        blob->shape.morph_depth = 0.0f;
        return;
    }

    float peak = blob->peak_amplitude;
    if (peak < 1e-8f) peak = 1e-8f; /* Avoid division by zero */

    /* ── Flatness (fill factor = mean normalized amplitude) ──
     * Rectangle profile → all values ≈ peak → mean/peak ≈ 1.0 → flatness=1.0
     * Gaussian profile  → mean ≈ 0.4..0.7 * peak           → flatness=0.4..0.7
     * Much more numerically stable than kurtosis for CIS sensor data.
     */
    float sum = 0.0f;
    for (int i = start; i < end; i++)
    {
        sum += notes[i];
    }
    float flatness = sf_clampf(sum / ((float)width * peak), 0.0f, 1.0f);

    /* ── Symmetry (bilateral mirror comparison with block averaging) ──
     *
     * CIS sensors introduce ~5-15% per-pixel noise, which makes pixel-by-pixel
     * bilateral comparison unreliable (symmetric traces get symmetry≈0.6 instead
     * of ≈0.9). Averaging over blocks of `block_size` pixels reduces the noise by
     * √block_size, bringing symmetric traces into the 0.85-0.92 range.
     *
     * block_size = max(1, width/10): for width=111 → block_size=11, ~5 block pairs.
     * Each block average reduces noise by √11 ≈ 3.3×.
     *
     * mean_diff=0   → perfect mirror → symmetry=1.0 (square wave territory)
     * mean_diff=0.3 → strongly asymmetric → symmetry=0.0 (sawtooth territory)
     * Scaling factor 3.0 maps mean_diff_norm=0.33 → symmetry=0.
     */
    int block_size = (width > 10) ? (width / 10) : 1;
    float bilateral_diff_sum = 0.0f;
    int block_count = 0;
    {
        int center = (start + end) / 2;
        for (int bi = 0; ; bi++)
        {
            int left_start  = start + bi * block_size;
            int right_start = end - (bi + 1) * block_size;

            /* Stop when the two windows overlap or cross */
            if (left_start + block_size > center) break;
            if (right_start < center) break;
            if (right_start < start) break;

            float left_sum = 0.0f;
            float right_sum = 0.0f;
            for (int j = 0; j < block_size; j++)
            {
                left_sum  += notes[left_start  + j];
                right_sum += notes[right_start + j];
            }
            bilateral_diff_sum += fabsf(left_sum - right_sum) / (float)block_size;
            block_count++;
        }
    }
    float symmetry;
    if (block_count > 0 && peak > 1e-8f)
    {
        float mean_diff_norm = (bilateral_diff_sum / (float)block_count) / peak;
        symmetry = sf_clampf(1.0f - mean_diff_norm * 3.0f, 0.0f, 1.0f);
    }
    else
    {
        symmetry = 1.0f; /* Single-block blob: assume symmetric */
    }

    /* ── Edge Sharpness (gradient at boundaries) ── */
    float left_edge = 0.0f;
    float right_edge = 0.0f;
    if (width >= 3)
    {
        left_edge  = fabsf(notes[start + 1] - notes[start]) / peak;
        right_edge = fabsf(notes[end - 1]   - notes[end - 2]) / peak;
    }
    else if (width == 2)
    {
        left_edge  = fabsf(notes[start + 1] - notes[start]) / peak;
        right_edge = left_edge;
    }
    float edge_sharpness = sf_clampf((left_edge + right_edge) * 0.5f, 0.0f, 1.0f);

    /* ── Morph Depth (derived from blob width) ── */
    float morph_width_scale = g_sp3ctra_config.strokeforge_morph_width_scale;
    float morph_depth = sf_clampf(blob->width_notes / morph_width_scale, 0.0f, 1.0f);

    /* Store descriptors */
    blob->shape.flatness    = flatness;
    blob->shape.symmetry    = symmetry;
    blob->shape.edge_sharpness = edge_sharpness;
    blob->shape.morph_depth = morph_depth;
}

/**************************************************************************************
 * Step 4: Compute Harmonic Recipe from Shape Descriptors
 **************************************************************************************/
static void compute_harmonic_recipe(StrokeForgeBlob *blob)
{
    int max_h = g_sp3ctra_config.strokeforge_max_harmonics;
    float amp_floor = g_sp3ctra_config.strokeforge_harmonic_amplitude_floor;

    float symmetry = blob->shape.symmetry;
    float edge_sharpness = blob->shape.edge_sharpness;
    float morph_depth = blob->shape.morph_depth;
    /* Note: flatness is captured in morph_depth via shape extraction */

    /* Rolloff exponent: blends between edge-driven (sawtooth) and symmetry-driven (square)
     *
     * Sawtooth (symmetry=0): all harmonics, rolloff_exp = 0.5 + 0.8*(1-edge)
     *   edge=0.0 → rolloff=1.30  (h4≈0.165, sawtooth-like)
     *   edge=0.5 → rolloff=0.90  (h4≈0.232, bright sawtooth)
     *
     * Square wave (symmetry=1): ONLY odd harmonics matter, rolloff_exp → 1.0
     *   → h3=1/3=0.333, h5=1/5=0.200, h7=1/7=0.143 (true 1/n series)
     *   → even harmonics zeroed out by parity=0 regardless of rolloff
     *
     * Blend: rolloff_exp = rolloff_base*(1-symmetry) + 1.0*symmetry
     */
    float rolloff_base = 0.5f + 0.8f * (1.0f - edge_sharpness);
    float rolloff_exp  = rolloff_base * (1.0f - symmetry) + 1.0f * symmetry;

    /* Fundamental is always 1.0 */
    blob->harmonic_amplitudes[0] = 1.0f;
    blob->harmonic_count = 1;

    for (int h = 2; h <= max_h; h++)
    {
        /* Parity factor: symmetric blobs suppress even harmonics.
         * Quadratic curve: symmetry=0.85 → parity=(0.15)²=0.023 (h2≈0.011, inaudible)
         *                  symmetry=1.00 → parity=0 (perfect square wave)
         *                  symmetry=0.00 → parity=1 (full sawtooth)
         * Linear would give parity=0.15 → h2=0.07 (still audible) for symmetry=0.85.
         */
        float parity = 1.0f;
        if (h % 2 == 0)
        {
            float asym = 1.0f - symmetry;
            parity = asym * asym; /* (1-sym)^2: steeper suppression near symmetry=1 */
        }

        /* Decay: 1/n^rolloff */
        float decay = 1.0f / powf((float)h, rolloff_exp);

        /* Morph gate: smooth transition based on morph_depth */
        float morph_gate = sf_clampf(1.0f + morph_depth * (float)max_h - (float)h, 0.0f, 1.0f);

        /* Combined amplitude */
        float amp = parity * decay * morph_gate;

        if (amp < amp_floor)
        {
            /* Below threshold: mark as inactive */
            blob->harmonic_amplitudes[h - 1] = 0.0f;
        }
        else
        {
            blob->harmonic_amplitudes[h - 1] = amp;
            blob->harmonic_count = h;
        }
    }
}

/**************************************************************************************
 * Step 4b: Compute Harmonic Recipe — Pulse Wave Model (WAVETABLE mode)
 *
 * Maps blob shape to a mathematically correct pulse wave with variable duty cycle.
 * This model unifies sine / triangle / square / pulse into one continuous formula:
 *
 *   symmetry  → duty_cycle D:
 *     sym=1.0 → D=0.50  square wave  (odd harmonics only)
 *     sym=0.0 → D=0.25  pulse wave   (all harmonics, more asymmetric)
 *
 *   flatness  → extra rolloff on top of the pulse 1/h base:
 *     flat=1.0 (rectangle) → extra=0.0 → total 1/h   rolloff  (square)
 *     flat=0.0 (gaussian)  → extra=1.0 → total 1/h²  rolloff  (triangle)
 *
 * Normalized pulse wave formula (h_1 always = 1.0):
 *   amp(h) = |sin(h×π×D)| / (h × sin(π×D))  ×  (1/h ^ extra_rolloff)
 *
 * Verification @ D=0.5 (square), extra=0:  h1=1, h2=0, h3=1/3, h5=1/5  ✓
 *              @ D=0.5 (square), extra=1:  h1=1, h2=0, h3=1/9, h5=1/25 ✓  (triangle)
 *              @ D=0.25,         extra=0:  h1=1, h2=0.707, h3=0.333, h4=0 (pulse)
 **************************************************************************************/
static void compute_harmonic_recipe_pulse_wave(StrokeForgeBlob *blob)
{
    int max_h       = g_sp3ctra_config.strokeforge_max_harmonics;
    float amp_floor = g_sp3ctra_config.strokeforge_harmonic_amplitude_floor;

    float symmetry    = blob->shape.symmetry;
    float flatness    = blob->shape.flatness;
    float morph_depth = blob->shape.morph_depth;

    /* duty_cycle in [0.25, 0.50]: guarantees sin(π×D) > 0 at all times */
    float duty_cycle = 0.25f + symmetry * 0.25f;
    float sin_fund   = sinf(3.14159265f * duty_cycle); /* always > 0 in this range */

    /* extra_rolloff: maps flatness to additional harmonic decay
     *   flat_mapped=1.0 (flatness ≥ 0.85, rectangle) → extra=0 → square series
     *   flat_mapped=0.0 (flatness ≤ 0.45, gaussian)  → extra=1 → triangle series
     */
    float flat_mapped   = sf_clampf((flatness - 0.45f) / 0.40f, 0.0f, 1.0f);
    float extra_rolloff = 1.0f - flat_mapped; /* [1.0=triangle .. 0.0=square] */

    /* Fundamental is always normalized to 1.0 */
    blob->harmonic_amplitudes[0] = 1.0f;
    blob->harmonic_count = 1;

    for (int h = 2; h <= max_h; h++)
    {
        /* Morph gate: limits how many harmonics are active (width-dependent) */
        float morph_gate = sf_clampf(
            1.0f + morph_depth * (float)max_h - (float)h, 0.0f, 1.0f);

        /* Normalized pulse wave: |sin(h×π×D)| / (h × sin(π×D)) */
        float h_pulse = fabsf(sinf((float)h * 3.14159265f * duty_cycle)) /
                        ((float)h * sin_fund);

        /* Additional flatness-driven rolloff: 1/h^extra_rolloff
         * At h=1 this is always 1.0 (normalization preserved).
         * At h=3, extra=1 (triangle): ×(1/3) → total h3 = (1/3)×(1/3) = 1/9 ✓
         */
        float h_rolloff = 1.0f / powf((float)h, extra_rolloff);

        float amp = h_pulse * h_rolloff * morph_gate;

        if (amp < amp_floor)
        {
            blob->harmonic_amplitudes[h - 1] = 0.0f;
        }
        else
        {
            blob->harmonic_amplitudes[h - 1] = amp;
            blob->harmonic_count = h;
        }
    }
}

/**************************************************************************************
 * Step 5: Find Nearest Oscillator for Each Harmonic
 *
 * Given the blob's fundamental frequency (center_note), compute the note index
 * for each harmonic (2nd, 3rd, ...) using logarithmic frequency spacing.
 **************************************************************************************/
static void find_harmonic_note_indices(
    StrokeForgeBlob *blob,
    int num_notes)
{
    int fund_note = blob->center_note;

    /* The frequency spacing is logarithmic:
     * harmonic h has frequency = f_fundamental * h
     * In log2 space: note_offset = log2(h) * notes_per_octave
     * notes_per_octave = semitone_per_octave * comma_per_semitone
     */
    int notes_per_octave = g_sp3ctra_config.semitone_per_octave *
                           g_sp3ctra_config.comma_per_semitone;

    blob->harmonic_note_indices[0] = fund_note; /* Fundamental */

    for (int h = 2; h <= blob->harmonic_count; h++)
    {
        if (blob->harmonic_amplitudes[h - 1] <= 0.0f)
        {
            blob->harmonic_note_indices[h - 1] = -1;
            continue;
        }

        /* Offset in notes for harmonic h */
        float offset = log2f((float)h) * (float)notes_per_octave;
        int target_note = fund_note + (int)(offset + 0.5f);

        if (target_note >= num_notes || target_note < 0)
        {
            /* Beyond range: disable this harmonic */
            blob->harmonic_note_indices[h - 1] = -1;
            blob->harmonic_amplitudes[h - 1] = 0.0f;
        }
        else
        {
            blob->harmonic_note_indices[h - 1] = target_note;
        }
    }
}

/**************************************************************************************
 * Step 6: Build Per-Note Lookup Tables
 **************************************************************************************/
static void build_note_lookups(
    StrokeForgeFrameData *out,
    int num_notes)
{
    /* Initialize all to sentinel values */
    for (int i = 0; i < num_notes && i < STROKEFORGE_MAX_NOTES; i++)
    {
        out->note_to_blob[i] = STROKEFORGE_NO_BLOB;
        out->note_is_harmonic_of_blob[i] = STROKEFORGE_NO_BLOB;
        out->note_harmonic_rank[i] = 0;
        out->note_harmonic_amplitude[i] = 0.0f;
        out->note_target_phase[i] = STROKEFORGE_PHASE_FREE;
    }

    int phase_coherence = g_sp3ctra_config.strokeforge_phase_coherence_enabled;

    for (int b = 0; b < out->blob_count; b++)
    {
        StrokeForgeBlob *blob = &out->blobs[b];
        if (!blob->active) continue;

        /* Mark blob membership for notes within the blob extent */
        for (int i = blob->start_note; i < blob->end_note && i < STROKEFORGE_MAX_NOTES; i++)
        {
            out->note_to_blob[i] = (int16_t)b;
        }

        /* Mark harmonic target notes */
        for (int h = 0; h < blob->harmonic_count; h++)
        {
            int note_idx = blob->harmonic_note_indices[h];
            if (note_idx < 0 || note_idx >= STROKEFORGE_MAX_NOTES) continue;

            float amp = blob->harmonic_amplitudes[h];
            if (amp <= 0.0f) continue;

            /* Scale harmonic amplitude by blob's weighted amplitude */
            float target_amp = amp * blob->weighted_amplitude;

            out->note_is_harmonic_of_blob[note_idx] = (int16_t)b;
            out->note_harmonic_rank[note_idx] = (int8_t)(h + 1); /* 1-based rank */
            out->note_harmonic_amplitude[note_idx] = target_amp;

            /* Phase coherence: all harmonics of a blob start in phase */
            if (phase_coherence)
            {
                /* Target phase = 0.0 (all harmonics aligned at zero crossing) */
                /* The RT path will smoothly converge toward this target */
                out->note_target_phase[note_idx] = 0.0f;
            }
        }
    }
}

/**************************************************************************************
 * Temporal Smoothing: Apply IIR filter to blob center_note and symmetry
 *
 * Matches each new blob to the closest previous blob (by center_note distance).
 * If matched, applies IIR smoothing. Otherwise, uses raw values.
 * Updates s_smooth_state for next frame.
 **************************************************************************************/
static void apply_temporal_smoothing(StrokeForgeBlob *blobs, int blob_count)
{
    /* Build new smooth entries from current blobs */
    SFSmoothEntry new_entries[STROKEFORGE_MAX_BLOBS];

    for (int b = 0; b < blob_count; b++)
    {
        StrokeForgeBlob *blob = &blobs[b];

        /* Center stored as fixed-point x16 for sub-note smoothing */
        int center_fp = blob->center_note * 16;
        float symmetry = blob->shape.symmetry;

        if (s_smooth_state.valid)
        {
            /* Find nearest previous blob */
            int best_idx = -1;
            int best_dist = SF_SMOOTH_MAX_DIST;

            for (int p = 0; p < s_smooth_state.count; p++)
            {
                int prev_center = s_smooth_state.entries[p].center_note / 16;
                int dist = blob->center_note - prev_center;
                if (dist < 0) dist = -dist;

                if (dist < best_dist)
                {
                    best_dist = dist;
                    best_idx  = p;
                }
            }

            if (best_idx >= 0)
            {
                /* IIR: new = α*current + (1-α)*previous */
                int prev_fp = s_smooth_state.entries[best_idx].center_note;
                center_fp = (int)(SF_SMOOTH_IIR_CENTER * (float)center_fp
                                  + (1.0f - SF_SMOOTH_IIR_CENTER) * (float)prev_fp);

                float prev_sym = s_smooth_state.entries[best_idx].symmetry;
                symmetry = SF_SMOOTH_IIR_SYMMETRY * symmetry
                           + (1.0f - SF_SMOOTH_IIR_SYMMETRY) * prev_sym;
            }
        }

        /* Write back smoothed values */
        blob->center_note = center_fp / 16;
        blob->shape.symmetry = sf_clampf(symmetry, 0.0f, 1.0f);

        new_entries[b].center_note = center_fp;
        new_entries[b].symmetry    = symmetry;
    }

    /* Update state for next frame */
    s_smooth_state.valid = 1;
    s_smooth_state.count = blob_count;
    for (int b = 0; b < blob_count && b < STROKEFORGE_MAX_BLOBS; b++)
    {
        s_smooth_state.entries[b] = new_entries[b];
    }
}

/**************************************************************************************
 * Main Public API: Analyze One Frame
 **************************************************************************************/

void strokeforge_analyze_frame(
    const float *notes,
    int num_notes,
    float contrast_factor,
    StrokeForgeFrameData *out)
{
    /* Clear output */
    memset(out, 0, sizeof(StrokeForgeFrameData));
    out->blob_count = 0;

    /* Early exit if disabled */
    if (!g_sp3ctra_config.strokeforge_enabled)
    {
        /* Initialize sentinel values for all notes */
        for (int i = 0; i < num_notes && i < STROKEFORGE_MAX_NOTES; i++)
        {
            out->note_to_blob[i] = STROKEFORGE_NO_BLOB;
            out->note_is_harmonic_of_blob[i] = STROKEFORGE_NO_BLOB;
            out->note_harmonic_rank[i] = 0;
            out->note_harmonic_amplitude[i] = 0.0f;
            out->note_target_phase[i] = STROKEFORGE_PHASE_FREE;
        }
        return;
    }

    /* Compute adaptive threshold */
    float base_threshold = g_sp3ctra_config.strokeforge_blob_base_threshold;
    float threshold = base_threshold;

    if (g_sp3ctra_config.strokeforge_blob_contrast_adaptive)
    {
        float sensitivity = g_sp3ctra_config.strokeforge_blob_contrast_sensitivity;
        /* High contrast → lower threshold (detect finer details) */
        /* Low contrast  → higher threshold (ignore noise floor) */
        threshold = base_threshold * (1.0f - contrast_factor * sensitivity);
        threshold = sf_clampf(threshold, 0.005f, 0.5f);
    }

    int min_width = g_sp3ctra_config.strokeforge_blob_min_width;
    int merge_gap = g_sp3ctra_config.strokeforge_blob_merge_gap;

    /* Step 1: Detect blobs */
    out->blob_count = detect_blobs(
        notes, num_notes, threshold, min_width, merge_gap,
        out->blobs, STROKEFORGE_MAX_BLOBS);

    /* -----------------------------------------------------------------------
     * Complexity gate — determine frame_mode BEFORE per-blob processing.
     *
     * BYPASS (blob_count != 1):
     *   Multiple blobs → StrokeForge harmonics from blob A would collide with
     *   blob B's fundamentals → bell-like beating artifacts.  Solution: bypass
     *   entirely and let the spectral synthesis run clean.
     *
     * PHASE_SMOOTH (single thin blob, width < STROKEFORGE_WAVETABLE_MIN_WIDTH):
     *   Set phase targets only.  No amplitude override.  Non-intrusive.
     *
     * WAVETABLE (single wide blob, width ≥ STROKEFORGE_WAVETABLE_MIN_WIDTH):
     *   Pulse-wave morphing — StrokeForge suppresses spectral amplitude within
     *   the blob range and injects its own harmonic recipe exclusively.
     * ----------------------------------------------------------------------- */
    if (out->blob_count != 1)
    {
        /* BYPASS: zero or multiple blobs → pure spectral passthrough.
         * Reset waveform morph to pure sine so the additive engine reverts to
         * its default sound as soon as the blob disappears or splits. */
        g_waveform_morph = 0.0f;
        out->frame_mode = STROKEFORGE_MODE_BYPASS;
        /* Reset smooth state: clean restart when single blob re-appears */
        s_smooth_state.valid = 0;
        s_smooth_state.count = 0;
        return;
    }

    /* -----------------------------------------------------------------------
     * Single blob path — compute waveform morph factor.
     *
     * morph = blob_width / morph_width_scale
     *   0.0 → pure sine  (narrow stroke, pen tip)
     *   1.0 → pure square (wide stroke, full marker)
     *
     * The RT synthesis engine (synth_process_worker_range) reads g_waveform_morph
     * once per cycle and applies a linear interpolation between g_sine_table and
     * g_square_table.  No harmonic-recipe injection, no note_harmonic_amplitude
     * tables — the waveform change is global and instantaneous.
     * ----------------------------------------------------------------------- */
    {
        StrokeForgeBlob *blob = &out->blobs[0];

        /* Centroid + amplitude (used for logging) */
        compute_blob_amplitudes(notes, blob);
        extract_shape_descriptors(notes, blob);
        apply_temporal_smoothing(out->blobs, 1);

        /* Morph factor: linear map from width to [0, 1] */
        float scale = g_sp3ctra_config.strokeforge_morph_width_scale;
        if (scale <= 0.0f) scale = 400.0f;  /* safety fallback */
        float morph = blob->width_notes / scale;
        if (morph > 1.0f) morph = 1.0f;

        /* Atomic write — single float, ARM64/x86-64 naturally atomic */
        g_waveform_morph = morph;

        out->frame_mode = (morph >= 0.01f) ? STROKEFORGE_MODE_WAVETABLE
                                            : STROKEFORGE_MODE_PHASE_SMOOTH;
    }

    /* ── DEBUG: Rate-limited logging of blob analysis results ── */
    {
        static int sf_debug_counter = 0;
        if (++sf_debug_counter % 500 == 1) /* ~2x per second @ 1kHz frame rate */
        {
            log_info("STROKEFORGE", "=== Frame Analysis: %d blobs detected (threshold=%.4f, contrast=%.3f) ===",
                     out->blob_count, threshold, contrast_factor);

            for (int b = 0; b < out->blob_count; b++)
            {
                StrokeForgeBlob *blob = &out->blobs[b];

                log_info("STROKEFORGE",
                         "  Blob[%d]: notes[%d..%d] width=%.0f center=%d | "
                         "peak=%.3f weighted_amp=%.3f",
                         b, blob->start_note, blob->end_note - 1,
                         blob->width_notes, blob->center_note,
                         blob->peak_amplitude, blob->weighted_amplitude);

                log_info("STROKEFORGE",
                         "    Shape: flatness=%.3f symmetry=%.3f edge=%.3f morph_depth=%.3f",
                         blob->shape.flatness, blob->shape.symmetry,
                         blob->shape.edge_sharpness, blob->shape.morph_depth);

                /* Print harmonic table */
                char harm_buf[512];
                int pos = 0;
                pos += snprintf(harm_buf + pos, sizeof(harm_buf) - pos,
                                "    Harmonics (%d): ", blob->harmonic_count);
                for (int h = 0; h < blob->harmonic_count && h < 16; h++)
                {
                    int note_idx = blob->harmonic_note_indices[h];
                    float amp = blob->harmonic_amplitudes[h];
                    if (amp > 0.0f && note_idx >= 0)
                    {
                        pos += snprintf(harm_buf + pos, sizeof(harm_buf) - pos,
                                        "h%d@n%d(%.3f) ", h + 1, note_idx, amp);
                    }
                    if (pos >= (int)sizeof(harm_buf) - 30) break;
                }
                log_info("STROKEFORGE", "%s", harm_buf);
            }

            if (out->blob_count == 0)
            {
                log_info("STROKEFORGE", "  (no blobs — all notes below threshold %.4f)", threshold);
            }
        }
    }
}
