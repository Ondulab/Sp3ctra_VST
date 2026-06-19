/*
 * image_pipeline_types.h
 *
 * Shared type definitions for the dual-path image processing pipeline.
 * These types are used across source routing, blending, and processing stages.
 *
 * Architecture: see docs/SPEC_ImagePipeline_Architecture.html
 *
 * ── Channel model (since "Modulated / Live" refactor) ──────────────────────
 *
 * Two fixed channels feed the synthesis engines:
 *
 *   Channel Modulated : Live ► [LuxPitch ⇄ LuxMask] ► LuxSampler ► OUT
 *   (insert order = chainInsertOrder param; sampler playback bypasses inserts)
 *                       Each insert auto-bypasses when inactive:
 *                         - LuxSampler  : pass-through when not playing
 *                         - LuxPitch    : pass-through when no shift active
 *                         - LuxMask     : pass-through when opacity == 0
 *
 *   Channel Live      : UDP image stream, direct, no processing
 *
 * Each synthesis engine (LuxStral, LuxSynth+LuxWave) selects which channel
 * feeds its preprocessing pipeline.  No more S/M/L choice.
 *
 * ── RT-safety ──────────────────────────────────────────────────────────────
 * All types are plain-old-data (POD).  No dynamic allocation, no virtual
 * methods, no JUCE dependencies.
 *
 * Author: zhonx
 */

#ifndef IMAGE_PIPELINE_TYPES_H
#define IMAGE_PIPELINE_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * ImageSourceType — Channel selector for each synthesis path
 *
 * Two values only.  Previous values (SAMPLER / MIX / LUXPITCH / LUXMASK) are
 * retained as deprecated aliases for source compatibility during the
 * transition; they all map to MODULATED at runtime since their effect is now
 * automatically baked into the modulated chain.
 * ============================================================================ */
typedef enum {
    IMAGE_SOURCE_MODULATED = 0,  /* Channel A : Live ► [Pitch ⇄ Mask] ► Sampler */
    IMAGE_SOURCE_LIVE      = 1,  /* Channel B : direct live UDP feed */

    /* ── Deprecated aliases — kept so any leftover preset/code keeps compiling.
     * They all behave as IMAGE_SOURCE_MODULATED at runtime. ─────────────── */
    IMAGE_SOURCE_SAMPLER   = IMAGE_SOURCE_MODULATED,
    IMAGE_SOURCE_MIX       = IMAGE_SOURCE_MODULATED,
    IMAGE_SOURCE_LUXPITCH  = IMAGE_SOURCE_MODULATED,
    IMAGE_SOURCE_LUXMASK   = IMAGE_SOURCE_MODULATED
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
 * Each synthesis path has its own channel selection and toggle states.
 * These are populated from APVTS parameters each frame.
 * ============================================================================ */
typedef struct {
    ImageSourceType source;     /* MODULATED | LIVE — which channel feeds this path */
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

    /* Legacy mix opacities — retained for binary compatibility with callers
     * that still set them.  Ignored by the new channel-based pipeline. */
    float      sampler_opacity;
    float      live_opacity;

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
     * Prevents the live thread from corrupting the sampler envelope state. */
    int        envelope_id;
} PipelineConfig;

/* ============================================================================
 * Helper: create a default PipelineConfig matching legacy behaviour
 *
 * Both paths default to MODULATED channel, inversion ON, AC removal ON,
 * LuxStral gamma = 2.2, LuxSynth gamma = 0 (bypass).
 * ============================================================================ */
static inline PipelineConfig pipeline_config_default(void)
{
    PipelineConfig cfg;

    /* Path A — LuxStral: MODULATED channel, inversion ON, AC removal ON, gamma 2.2 */
    cfg.luxstral_path.source     = IMAGE_SOURCE_MODULATED;
    cfg.luxstral_path.inversion  = 1;
    cfg.luxstral_path.ac_removal = 1;
    cfg.luxstral_path.gamma      = 2.2f;

    /* Path B — LuxSynth+LuxWave: MODULATED channel, inversion ON, AC removal ON, no gamma */
    cfg.luxsynth_luxwave_path.source     = IMAGE_SOURCE_MODULATED;
    cfg.luxsynth_luxwave_path.inversion  = 1;
    cfg.luxsynth_luxwave_path.ac_removal = 1;
    cfg.luxsynth_luxwave_path.gamma      = 0.0f; /* Linear for FFT */

    /* Legacy mix opacities (ignored by channel-based pipeline) */
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
