/*
 * image_pipeline_types.h
 *
 * Shared type definitions for the dual-path image processing pipeline.
 * These types are used across source routing, blending, and processing stages.
 *
 * Architecture: see docs/SPEC_ImagePipeline_Architecture.html
 *               see docs/PLAN_ImagePipeline_Refactoring.html
 *
 * RT-safety: All types are plain-old-data (POD). No dynamic allocation,
 *            no virtual methods, no JUCE dependencies.
 *
 * Author: zhonx
 * Created: 2026-04-14
 */

#ifndef IMAGE_PIPELINE_TYPES_H
#define IMAGE_PIPELINE_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * ImageSourceType — Source routing enum shared by both paths
 *
 * Each synthesis path (LuxStral, LuxSynth+LuxWave) can independently select
 * its input source from one of these three options.
 * ============================================================================ */
typedef enum {
    IMAGE_SOURCE_SAMPLER = 0,   /* S — LuxSampler playback (recorded slot) */
    IMAGE_SOURCE_LIVE    = 1,   /* L — Live UDP stream (real-time scanner) */
    IMAGE_SOURCE_MIX     = 2    /* M — Darken-blend of Sampler × Live */
} ImageSourceType;

/* ============================================================================
 * ImageFrameRGB — Lightweight descriptor for an RGB pixel frame
 *
 * Non-owning: holds pointers into externally owned buffers.
 * Zero-cost abstraction for passing RGB data between pipeline stages.
 * ============================================================================ */
typedef struct {
    const uint8_t *r;           /* Red channel pointer [0-255], NULL if unavailable */
    const uint8_t *g;           /* Green channel pointer [0-255], NULL if unavailable */
    const uint8_t *b;           /* Blue channel pointer [0-255], NULL if unavailable */
    int            pixel_count; /* Number of valid pixels (0 if source unavailable) */
} ImageFrameRGB;

/* ============================================================================
 * PathConfig — Per-path processing configuration
 *
 * Each synthesis path has its own source selection and toggle states.
 * These are populated from APVTS parameters each frame.
 * ============================================================================ */
typedef struct {
    ImageSourceType source;     /* S | L | M — which source feeds this path */
    int             inversion;  /* 0 = off, 1 = on — invert pixel intensities */
    int             ac_removal; /* 0 = off, 1 = on — subtract per-line DC offset */
    float           gamma;      /* γ value for non-linear mapping (0.0 = bypass) */
} PathConfig;

/* ============================================================================
 * PipelineConfig — Complete pipeline configuration for one frame
 *
 * Built from APVTS parameters at the start of each preprocessing call.
 * Passed to pipeline_process_frame() as the single configuration source.
 * ============================================================================ */
typedef struct {
    PathConfig luxstral_path;           /* Path A: LuxStral additive synthesis */
    PathConfig luxsynth_luxwave_path;   /* Path B: LuxSynth + LuxWave */
    float      sampler_opacity;         /* Sampler stream opacity [0.0, 1.0] */
    float      live_opacity;            /* Live stream opacity [0.0, 1.0] */

    /* Freeze / Fade envelope — applied to LuxStral notes + grayscale */
    int        freeze_mode;             /* 0 = PLAY, 1 = HOLD, 2 = STOP/WHITE */
    int        fade_in_ms;              /* Ramp duration in ms (0 = instant) */
    float      stream_opacity;          /* Per-stream opacity [0.0, 1.0] */
    float      contrast_min;            /* Minimum contrast value for this stream */

    /* Stereo and misc */
    int        stereo_enabled;          /* 1 = compute stereo panning, 0 = skip */
    int        pixels_per_note;         /* Averaging factor for LuxStral */

    /* Envelope identity — MUST match the caller, NOT the source routing.
     * 0 = ENVELOPE_LIVE  (set by live/UDP callers)
     * 1 = ENVELOPE_SAMPLER (set by FramePlayerThread callers)
     * Prevents the live thread from corrupting the sampler envelope state
     * when source routing directs Source=S through the live pipeline. */
    int        envelope_id;
} PipelineConfig;

/* ============================================================================
 * Helper: create a default PipelineConfig matching legacy behaviour
 *
 * Both paths use MIX source, inversion ON, AC removal ON,
 * LuxStral gamma = 2.2, LuxSynth gamma = 0 (bypass).
 * ============================================================================ */
static inline PipelineConfig pipeline_config_default(void)
{
    PipelineConfig cfg;

    /* Path A — LuxStral: MIX source, inversion ON, AC removal ON, gamma 2.2 */
    cfg.luxstral_path.source     = IMAGE_SOURCE_MIX;
    cfg.luxstral_path.inversion  = 1;
    cfg.luxstral_path.ac_removal = 1;
    cfg.luxstral_path.gamma      = 2.2f;

    /* Path B — LuxSynth+LuxWave: MIX source, inversion ON, AC removal ON, no gamma */
    cfg.luxsynth_luxwave_path.source     = IMAGE_SOURCE_MIX;
    cfg.luxsynth_luxwave_path.inversion  = 1;
    cfg.luxsynth_luxwave_path.ac_removal = 1;
    cfg.luxsynth_luxwave_path.gamma      = 0.0f; /* Linear for FFT */

    /* Mix opacities */
    cfg.sampler_opacity = 0.5f;
    cfg.live_opacity    = 0.5f;

    /* Freeze / Fade defaults (PLAY mode, no fade, full opacity) */
    cfg.freeze_mode    = 0;     /* PLAY */
    cfg.fade_in_ms     = 0;
    cfg.stream_opacity = 1.0f;
    cfg.contrast_min   = 0.05f;

    /* Misc */
    cfg.stereo_enabled  = 0;
    cfg.pixels_per_note = 1;

    /* Envelope: default to LIVE (0) */
    cfg.envelope_id = 0;

    return cfg;
}

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_PIPELINE_TYPES_H */
