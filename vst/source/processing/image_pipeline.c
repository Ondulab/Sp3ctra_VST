/*
 * image_pipeline.c
 *
 * Pipeline orchestrator — single entry point for all image preprocessing.
 * Replaces image_preprocess_frame() and image_preprocess_lux_sampler().
 *
 * Author: zhonx
 * Created: 2026-04-14
 */

#include "image_pipeline.h"
#include "image_pipeline_stages.h"
#include "config/config_loader.h"
#include "config/config_instrument.h"
#include "synthesis/luxwave/luxwave_vst_adapter.h"
#include "utils/logger.h"
#include <string.h>
#include <stddef.h>
#include <sys/time.h>

/* ============================================================================
 * Private: timestamp helper
 * ============================================================================ */
static uint64_t pipeline_get_timestamp_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ============================================================================
 * Private: Freeze / Opacity / Fade envelope
 *
 * Two independent static state blocks — one per stream (live, sampler).
 * Identified by envelope_id: 0 = live, 1 = sampler.
 * ============================================================================ */

#define ENVELOPE_LIVE    0
#define ENVELOPE_SAMPLER 1
#define ENVELOPE_COUNT   2

typedef struct {
    float    held_notes[PREPROCESS_MAX_NOTES];
    float    held_gray[PREPROCESS_MAX_NOTES];
    int      held_notes_count;
    int      held_gray_count;
    int      prev_freeze;
    uint64_t fade_ts_us;
    int      fade_dir;        /* +1 = fade-in, -1 = fade-out, 0 = none */
} EnvelopeState;

static EnvelopeState g_envelope[ENVELOPE_COUNT] = {
    { .prev_freeze = -1 },
    { .prev_freeze = -1 }
};

/**
 * @brief Apply freeze/opacity/fade envelope to notes and grayscale arrays.
 *
 * Ported from preprocess_luxstral() STEP 6 and preprocess_luxstral_sampler() STEP 6.
 *
 * freeze_mode:
 *   0 = PLAY  — live stream; apply opacity, save for HOLD restore
 *   1 = HOLD  — freeze at last PLAY frame immediately
 *   2 = STOP  — fade-out then silence
 */
static void pipeline_apply_envelope(
    int            envelope_id,
    int            freeze_mode,
    float          opacity,
    int            fade_ms,
    float         *notes,
    int            num_notes,
    float         *grayscale,
    int            nb_pixels)
{
    EnvelopeState *env;
    uint64_t       now_us;
    int            i, note;
    int            gn;

    if (envelope_id < 0 || envelope_id >= ENVELOPE_COUNT)
        return;

    env    = &g_envelope[envelope_id];
    now_us = pipeline_get_timestamp_us();
    gn     = (nb_pixels < PREPROCESS_MAX_NOTES) ? nb_pixels : PREPROCESS_MAX_NOTES;

    /* ── Detect mode transition ──────────────────────────────────────── */
    if (freeze_mode != env->prev_freeze && env->prev_freeze >= 0)
    {
        if (env->prev_freeze == 0 && freeze_mode == 2)
        {
            /* PLAY → STOP/WHITE: fade-out to silence */
            if (fade_ms > 0) { env->fade_ts_us = now_us; env->fade_dir = -1; }
        }
        else if (freeze_mode == 0)
        {
            /* HOLD/STOP → PLAY: fade-in */
            if (fade_ms > 0) { env->fade_ts_us = now_us; env->fade_dir = 1; }
        }
        else
        {
            /* PLAY → HOLD, or STOP ↔ HOLD: cancel any in-progress fade */
            env->fade_dir = 0;
        }
    }
    if (env->prev_freeze < 0) env->prev_freeze = freeze_mode; /* first call */

    /* ── Apply freeze / hold / play ──────────────────────────────────── */
    if (freeze_mode == 0)
    {
        /* PLAY: apply opacity to notes AND grayscale, then save for HOLD */
        if (opacity < 0.999f)
        {
            for (note = 0; note < num_notes; note++)
                notes[note] *= opacity;
            for (i = 0; i < gn; i++)
                grayscale[i] *= opacity;
        }

        int n = (num_notes < PREPROCESS_MAX_NOTES) ? num_notes : PREPROCESS_MAX_NOTES;
        for (note = 0; note < n; note++)
            env->held_notes[note] = notes[note];
        for (i = 0; i < gn; i++)
            env->held_gray[i] = grayscale[i];
        env->held_notes_count = n;
        env->held_gray_count  = gn;
    }
    else if (freeze_mode == 1)
    {
        /* HOLD: restore last PLAY frame */
        if (env->held_notes_count > 0)
        {
            int n = (num_notes < env->held_notes_count) ? num_notes : env->held_notes_count;
            for (note = 0; note < n; note++)
                notes[note] = env->held_notes[note];
        }
        if (env->held_gray_count > 0)
        {
            int g = (gn < env->held_gray_count) ? gn : env->held_gray_count;
            for (i = 0; i < g; i++)
                grayscale[i] = env->held_gray[i];
        }
    }
    else
    {
        /* STOP / WHITE (freeze=2): silence */
        for (note = 0; note < num_notes; note++)
            notes[note] = 0.0f;
        for (i = 0; i < gn; i++)
            grayscale[i] = 0.0f;
    }

    /* ── Apply fade envelope ─────────────────────────────────────────── */
    if (env->fade_dir != 0 && fade_ms > 0)
    {
        float t = (float)(now_us - env->fade_ts_us) / ((float)fade_ms * 1000.0f);
        if (t >= 1.0f)
        {
            env->fade_dir = 0; /* ramp complete */
        }
        else
        {
            if (env->fade_dir == -1 && freeze_mode == 2 && env->held_notes_count > 0)
            {
                /* STOP fade-out: decay from held values */
                float ramp = 1.0f - t;
                int n = (num_notes < env->held_notes_count) ? num_notes : env->held_notes_count;
                for (note = 0; note < n; note++)
                    notes[note] = env->held_notes[note] * ramp;
                int g = (gn < env->held_gray_count) ? gn : env->held_gray_count;
                for (i = 0; i < g; i++)
                    grayscale[i] = env->held_gray[i] * ramp;
            }
            else
            {
                float ramp = (env->fade_dir > 0) ? t : (1.0f - t);
                for (note = 0; note < num_notes; note++)
                    notes[note] *= ramp;
                for (i = 0; i < gn; i++)
                    grayscale[i] *= ramp;
            }
        }
    }

    env->prev_freeze = freeze_mode;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

static int g_pipeline_initialized = 0;

void pipeline_init(void)
{
    if (g_pipeline_initialized) return;

    /* Reset envelope states */
    int i;
    for (i = 0; i < ENVELOPE_COUNT; i++)
    {
        memset(&g_envelope[i], 0, sizeof(EnvelopeState));
        g_envelope[i].prev_freeze = -1;
    }

    log_info("PIPELINE", "Image pipeline initialized");
    g_pipeline_initialized = 1;
}

void pipeline_cleanup(void)
{
    if (!g_pipeline_initialized) return;
    log_info("PIPELINE", "Image pipeline cleaned up");
    g_pipeline_initialized = 0;
}

/* ============================================================================
 * Config builders — read g_sp3ctra_config and build PipelineConfig
 * ============================================================================ */

PipelineConfig pipeline_build_config_live(void)
{
    PipelineConfig cfg;

    /* Path A — LuxStral: per-path source/inversion/AC from APVTS config */
    cfg.luxstral_path.source     = (ImageSourceType)g_sp3ctra_config.luxstral_source_type;
    cfg.luxstral_path.inversion  = g_sp3ctra_config.luxstral_inversion;
    cfg.luxstral_path.ac_removal = g_sp3ctra_config.luxstral_ac_removal;
    cfg.luxstral_path.gamma      = g_sp3ctra_config.additive_enable_non_linear_mapping
                                   ? g_sp3ctra_config.additive_gamma_value : 0.0f;

    /* Path B — LuxSynth+LuxWave: per-path source/inversion/AC from APVTS config */
    cfg.luxsynth_luxwave_path.source     = (ImageSourceType)g_sp3ctra_config.luxsynth_source_type;
    cfg.luxsynth_luxwave_path.inversion  = g_sp3ctra_config.luxsynth_inversion;
    cfg.luxsynth_luxwave_path.ac_removal = g_sp3ctra_config.luxsynth_ac_removal;
    /* Gamma for LuxSynth FFT input (photo convention: pow(x, 1/gamma)).
     * No enable flag — preprocess_luxsynth() skips it as a no-op when value == 1.0. */
    cfg.luxsynth_luxwave_path.gamma      = g_sp3ctra_config.luxsynth_gamma_value;

    /* Mix opacities (not used for live-only, kept for API consistency) */
    cfg.sampler_opacity = g_sp3ctra_config.image_sampler_opacity;
    cfg.live_opacity    = g_sp3ctra_config.image_live_opacity;

    /* Freeze / Fade (live stream parameters) */
    cfg.freeze_mode    = g_sp3ctra_config.image_freeze_mode;
    cfg.fade_in_ms     = g_sp3ctra_config.image_fade_in_ms;
    /* Source-aware opacity: when Source=L (pure live), bypass the mix-balance
     * crossfader that might have reduced image_live_opacity to 0.
     * Only in MIX mode does the crossfader-driven opacity apply. */
    cfg.stream_opacity = (cfg.luxstral_path.source == IMAGE_SOURCE_MIX)
                         ? g_sp3ctra_config.image_live_opacity : 1.0f;
    cfg.contrast_min   = g_sp3ctra_config.additive_contrast_min;

    /* Misc */
    cfg.stereo_enabled  = g_sp3ctra_config.stereo_mode_enabled;
    cfg.pixels_per_note = g_sp3ctra_config.pixels_per_note;

    /* Envelope identity: always LIVE for this builder, regardless of source routing */
    cfg.envelope_id = ENVELOPE_LIVE;

    return cfg;
}

PipelineConfig pipeline_build_config_sampler(void)
{
    PipelineConfig cfg;

    /* Path A — LuxStral: per-path source/inversion/AC from APVTS.
     * FIX(gamma): Use the SAME gamma as the live path (additive_gamma_value)
     * controlled by the single "Gamma" slider in the LuxStral tab.
     * The old sampler_gamma was a separate hidden parameter with no UI control,
     * causing gamma to have no effect when Source=Sampler. */
    cfg.luxstral_path.source     = (ImageSourceType)g_sp3ctra_config.luxstral_source_type;
    cfg.luxstral_path.inversion  = g_sp3ctra_config.luxstral_inversion;
    cfg.luxstral_path.ac_removal = g_sp3ctra_config.luxstral_ac_removal;
    cfg.luxstral_path.gamma      = g_sp3ctra_config.additive_enable_non_linear_mapping
                                   ? g_sp3ctra_config.additive_gamma_value : 0.0f;

    /* Path B — LuxSynth+LuxWave: per-path from APVTS */
    cfg.luxsynth_luxwave_path.source     = (ImageSourceType)g_sp3ctra_config.luxsynth_source_type;
    cfg.luxsynth_luxwave_path.inversion  = g_sp3ctra_config.luxsynth_inversion;
    cfg.luxsynth_luxwave_path.ac_removal = g_sp3ctra_config.luxsynth_ac_removal;
    /* Gamma for LuxSynth FFT input — mirrors the live path (same user control, no enable flag). */
    cfg.luxsynth_luxwave_path.gamma      = g_sp3ctra_config.luxsynth_gamma_value;

    /* Mix opacities */
    cfg.sampler_opacity = g_sp3ctra_config.image_sampler_opacity;
    cfg.live_opacity    = g_sp3ctra_config.image_live_opacity;

    /* Freeze / Fade (sampler stream parameters) */
    cfg.freeze_mode    = g_sp3ctra_config.sampler_freeze_mode;
    cfg.fade_in_ms     = g_sp3ctra_config.sampler_fade_in_ms;
    /* Source-aware opacity: when Source=S (pure sampler), bypass the mix-balance
     * crossfader that might have reduced image_sampler_opacity to 0.
     * Only in MIX mode does the crossfader-driven opacity apply. */
    cfg.stream_opacity = (cfg.luxstral_path.source == IMAGE_SOURCE_MIX)
                         ? g_sp3ctra_config.image_sampler_opacity : 1.0f;
    cfg.contrast_min   = g_sp3ctra_config.sampler_contrast_min;

    /* Misc */
    cfg.stereo_enabled  = g_sp3ctra_config.stereo_mode_enabled;
    cfg.pixels_per_note = g_sp3ctra_config.pixels_per_note;

    /* Envelope identity: always SAMPLER for this builder, regardless of source routing */
    cfg.envelope_id = ENVELOPE_SAMPLER;

    return cfg;
}

/* ============================================================================
 * pipeline_path_luxstral — Path A using composable stages + envelope
 * ============================================================================ */
void pipeline_path_luxstral(
    const uint8_t        *raw_r,
    const uint8_t        *raw_g,
    const uint8_t        *raw_b,
    const PipelineConfig *config,
    PreprocessedImageData *out)
{
    int nb_pixels       = get_cis_pixels_nb();
    int pixels_per_note = config->pixels_per_note;
    int num_notes       = 0;
    int envelope_id;

    if (raw_r == NULL || raw_g == NULL || raw_b == NULL || out == NULL)
        return;

    /* Stage 1: RGB → Grayscale [0.0, 1.0] */
    img_stage_rgb_to_grayscale(
        raw_r, raw_g, raw_b, nb_pixels,
        out->additive.grayscale);

    /* Stage 2: Contrast (on RAW grayscale, before inversion/gamma) */
    out->additive.contrast_factor = img_stage_calculate_contrast(
        out->additive.grayscale,
        nb_pixels,
        config->contrast_min,
        g_sp3ctra_config.additive_contrast_adjustment_power,
        g_sp3ctra_config.additive_contrast_stride);

    /* Stage 3: Inversion */
    if (config->luxstral_path.inversion)
        img_stage_invert(out->additive.grayscale, nb_pixels);

    /* Stage 4: AC removal */
    if (config->luxstral_path.ac_removal)
        img_stage_remove_dc(out->additive.grayscale, nb_pixels);

    /* Stage 5: Gamma correction */
    if (config->luxstral_path.gamma > 0.0f && config->luxstral_path.gamma != 1.0f)
        img_stage_apply_gamma(out->additive.grayscale, nb_pixels,
                              config->luxstral_path.gamma);

    /* Stage 6: Per-note averaging */
    img_stage_grayscale_luxstral(
        out->additive.grayscale,
        nb_pixels,
        pixels_per_note,
        PREPROCESS_MAX_NOTES,
        out->additive.notes,
        &num_notes);

    /* Stage 7: Freeze / Opacity / Fade envelope
     *
     * RAW upstream gate: raw_freeze_mode overrides per-stream freeze when more
     * restrictive (HOLD > PLAY, STOP > HOLD), but ONLY for LIVE and MIX sources.
     *
     * FIX(routing): SAMPLER and SEQUENCER transports are independent of the
     * RAW/Live transport.  Applying raw_freeze_mode to IMAGE_SOURCE_SAMPLER would
     * silence sequencer and sampler playback whenever the RAW input is on STOP —
     * exactly the bug described (sequencer running + RAW stopped → no sound).
     * The sampler's own freeze_mode (set by pipeline_build_config_sampler() from
     * sampler_freeze_mode, or overridden to 0 by FramePlayerThread when seqDriven)
     * is the sole authority for the SAMPLER envelope.
     */
    {
        int effective_freeze = config->freeze_mode;
        int effective_fade   = config->fade_in_ms;

        /* Apply RAW upstream gate only for non-SAMPLER sources */
        if (config->luxstral_path.source != IMAGE_SOURCE_SAMPLER)
        {
            int raw_freeze = g_sp3ctra_config.raw_freeze_mode;
            if (raw_freeze > effective_freeze)
            {
                effective_freeze = raw_freeze;
                effective_fade   = g_sp3ctra_config.raw_fade_in_ms;
            }
        }

        /* Use caller-provided envelope_id — NOT derived from source routing.
         * This prevents the live thread from corrupting ENVELOPE_SAMPLER state
         * when source=S routes through the live pipeline path. */
        envelope_id = config->envelope_id;

        pipeline_apply_envelope(
            envelope_id,
            effective_freeze,
            config->stream_opacity,
            effective_fade,
            out->additive.notes, num_notes,
            out->additive.grayscale, nb_pixels);
    }

    /* Stage 8: Stereo pan from color temperature */
    if (config->stereo_enabled)
    {
        img_stage_compute_pan_luxstral(
            raw_r, raw_g, raw_b,
            nb_pixels,
            pixels_per_note,
            PREPROCESS_MAX_NOTES,
            out->stereo.pan_positions,
            out->stereo.left_gains,
            out->stereo.right_gains);
    }

    /* Stage 9: StrokeForge blob detection */
    {
        int sf_num_notes = nb_pixels / pixels_per_note;
        if (sf_num_notes > PREPROCESS_MAX_NOTES)
            sf_num_notes = PREPROCESS_MAX_NOTES;

        img_stage_blob_detect(
            out->additive.notes,
            sf_num_notes,
            out->additive.contrast_factor,
            &out->strokeforge);
    }
}

/* ============================================================================
 * pipeline_path_luxsynth_luxwave — Path B
 *
 * Delegates to existing preprocess_luxsynth() for FFT (stateful).
 * Uses stage function for LuxWave RGB copy.
 * ============================================================================ */
void pipeline_path_luxsynth_luxwave(
    const uint8_t  *raw_r,
    const uint8_t  *raw_g,
    const uint8_t  *raw_b,
    const PathConfig *path_cfg,
    PreprocessedImageData *out)
{
    int nb_pixels = get_cis_pixels_nb();

    if (raw_r == NULL || raw_g == NULL || raw_b == NULL || out == NULL)
        return;

    /* LuxSynth path: delegate to existing FFT pipeline */
#ifndef DISABLE_LUXSYNTH
    preprocess_luxsynth(raw_r, raw_g, raw_b, out);
#endif

    /* LuxWave path: feed LuxSynth grayscale line as wavetable source.
     * polyphonic.grayscale is filled by preprocess_luxsynth() above.
     * luxwave_engine_set_image_line() is a non-owning pointer assignment
     * (O(1), RT-safe). The engine reads the data during processBlock. */
    if (g_luxwave_engine.initialized && nb_pixels > 0)
    {
        luxwave_engine_set_image_line(&g_luxwave_engine,
                                       out->polyphonic.grayscale,
                                       nb_pixels);
    }

    /* LuxWave path: direct RGB copy (kept for future photowave use) */
    img_stage_copy_rgb_raw(
        raw_r, raw_g, raw_b, nb_pixels,
        out->photowave.r, out->photowave.g, out->photowave.b);
}

/* ============================================================================
 * pipeline_process_frame — Main orchestrator
 * ============================================================================ */
int pipeline_process_frame(
    const uint8_t         *raw_r,
    const uint8_t         *raw_g,
    const uint8_t         *raw_b,
    const PipelineConfig  *config,
    PreprocessedImageData *out)
{
    if (raw_r == NULL || raw_g == NULL || raw_b == NULL ||
        config == NULL || out == NULL)
    {
        return -1;
    }

    /* Timestamp */
    out->timestamp_us = pipeline_get_timestamp_us();

    /* Path A: LuxStral additive synthesis */
    pipeline_path_luxstral(raw_r, raw_g, raw_b, config, out);

    /* Path B: LuxSynth + LuxWave */
    pipeline_path_luxsynth_luxwave(
        raw_r, raw_g, raw_b,
        &config->luxsynth_luxwave_path,
        out);

    return 0;
}
