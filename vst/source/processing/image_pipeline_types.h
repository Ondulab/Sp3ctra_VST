/*
 * image_pipeline_types.h
 *
 * Shared type definitions for the dual-path image processing pipeline.
 * These types are used across source routing, blending, and processing stages.
 *
 *
 * ── Channel model (since "Modulated / Live" refactor) ──────────────────────
 *
 * Two fixed channels feed the synthesis engines:
 *
 *   Channel Modulated : Live ► [LuxPitch ⇄ LuxMask] ► LuxSampler ► OUT
 *   (insert order = each chain's own module order)
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

/* (M8: the ImageSourceType channel selector is gone — the ChainPlan recipes
 * route every stream; cfg.sampler_relayed carries the only per-stream
 * semantic the pipeline still needs.) */

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
    /* M8 — 1 = the stream is sampler/score-relayed (player or idle
     * passthrough of a sampler chain): the RAW upstream transport gate is
     * skipped (a stopped RAW input must not silence a playing sampler). */
    int        sampler_relayed;

    /* Synth-split P1 — per-OUT conditioning, from g_sp3ctra_config.luxstral_out[slot]
     * (slot picked by the builder). */
    float      luxstral_db_range;       /* inverse-dB decode window (Range dB) */
    float      luxstral_intensity;      /* pre-engine mix weight of this send (1.0 = unity;
                                         * P3 send configs keep 1.0 — the mixer weighs) */

    /* Freeze re-gate authority (P3): 1 = live-style — pipeline_path_luxstral
     * re-gates the envelope to the chain-1 transport (sampler_freeze_mode) +
     * RAW gate; 0 = the caller's freeze_mode is authoritative
     * (FramePlayerThread overrides). */
    int        live_regate;

    /* Stereo and misc */
    int        stereo_enabled;          /* 1 = compute stereo panning, 0 = skip */
    float      stereo_temp_amp;         /* Colour-temperature amplification (pan) */
    int        pixels_per_note;         /* Averaging factor for LuxStral */

    /* Envelope identity — MUST match the caller, NOT the source routing.
     * 0 = ENVELOPE_LIVE  (set by live/UDP callers)
     * 1 = ENVELOPE_SAMPLER (set by FramePlayerThread callers)
     * 2 = ENVELOPE_CHAIN2 (LuxSynth/LuxWave)
     * 3 = ENVELOPE_LUXWAVE, 4+ = ENVELOPE_LS_SEND_BASE (per-send states)
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

    /* Path A — LuxStral: inversion ON, AC removal ON, gamma 2.2 */
    cfg.luxstral_path.inversion  = 1;
    cfg.luxstral_path.ac_removal = 1;
    cfg.luxstral_path.gamma      = 2.2f;

    /* Path B — LuxSynth+LuxWave: inversion ON, AC removal ON, no gamma */
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
    cfg.sampler_relayed = 0;

    /* Per-OUT conditioning defaults (unity send) */
    cfg.luxstral_db_range  = 50.0f;
    cfg.luxstral_intensity = 1.0f;
    cfg.live_regate        = 1;   /* default envelope is LIVE */

    /* Misc */
    cfg.stereo_enabled  = 0;
    cfg.stereo_temp_amp = 2.5f;
    cfg.pixels_per_note = 1;

    /* Envelope: default to LIVE (0) */
    cfg.envelope_id = 0;

    return cfg;
}

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_PIPELINE_TYPES_H */
