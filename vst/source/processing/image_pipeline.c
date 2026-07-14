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
#include "synth_staging.h"     /* M5 — LuxWave send mix (audio-thread feed) */
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

/* Per-chain freeze envelopes — one held-frame state each.
 *
 * Each send is gated by ITS chain's transport (chain_send_transport — the
 * executor resolved freeze/fade into the config before the pipeline runs;
 * the player's builder carries the sampler-transport authority). Separate
 * live/sampler held states so the live thread never corrupts the sampler
 * hold during playback. (P4 2026-07-14: ENVELOPE_CHAIN2 + ENVELOPE_LUXWAVE
 * deleted — the engine-level "Chain 2" gates are gone.) */
#define ENVELOPE_LIVE       0   /* additive, live thread (legacy standalone) */
#define ENVELOPE_SAMPLER    1   /* additive, sampler/player thread */
/* Synth-split P3 — one held-frame state PER LuxStral SEND (chain-indexed):
 * N chains feed the engine mix concurrently, each send freezes/fades on its
 * own. BASE + chain_idx, CHAIN_MAX_CHAINS (8) states. */
#define ENVELOPE_LS_SEND_BASE 2
#define ENVELOPE_COUNT      (ENVELOPE_LS_SEND_BASE + 8)

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
    [0 ... ENVELOPE_COUNT - 1] = { .prev_freeze = -1 }
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

    /* Path A — LuxStral: per-OUT conditioning bank, engine A = slot 0
     * (synth-split P1: the pipeline reads luxstral_out[], not the legacy
     * per-engine globals). Gamma convention: 1.0 = off (stage skips it). */
    const lux_out_params_t *out_a = &g_sp3ctra_config.luxstral_out[0];
    cfg.luxstral_path.inversion  = out_a->negative;
    cfg.luxstral_path.ac_removal = out_a->dc_blocking;
    cfg.luxstral_path.gamma      = out_a->gamma;
    cfg.luxstral_db_range        = out_a->range_db;
    cfg.luxstral_intensity       = out_a->intensity;

    /* Path B — LuxSynth+LuxWave: per-OUT bank, slot 0. These fields are
     * informational for this path (preprocess_luxsynth and the LuxWave feed
     * read their banks directly), kept coherent for any config consumer. */
    cfg.luxsynth_luxwave_path.inversion  = g_sp3ctra_config.luxsynth_out[0].negative;
    cfg.luxsynth_luxwave_path.ac_removal = g_sp3ctra_config.luxsynth_out[0].dc_blocking;
    cfg.luxsynth_luxwave_path.gamma      = g_sp3ctra_config.luxsynth_out[0].gamma;

    /* Mix opacities (not used for live-only, kept for API consistency) */
    cfg.sampler_opacity = g_sp3ctra_config.image_sampler_opacity;
    cfg.live_opacity    = g_sp3ctra_config.image_live_opacity;

    /* Freeze / Fade (live stream parameters) */
    cfg.freeze_mode    = g_sp3ctra_config.image_freeze_mode;
    cfg.fade_in_ms     = g_sp3ctra_config.image_fade_in_ms;
    /* M8 — live streams: full opacity, RAW gate applies. Sampler-relayed
     * chains override sampler_relayed + stream_opacity per send (executor). */
    cfg.sampler_relayed = 0;
    cfg.stream_opacity  = 1.0f;
    cfg.contrast_min   = out_a->contrast_min;

    /* Misc */
    cfg.stereo_enabled  = g_sp3ctra_config.stereo_mode_enabled;
    cfg.stereo_temp_amp = g_sp3ctra_config.stereo_temperature_amplification;
    cfg.pixels_per_note = g_sp3ctra_config.pixels_per_note;

    /* Envelope identity: always LIVE for this builder, regardless of source routing */
    cfg.envelope_id = ENVELOPE_LIVE;
    cfg.live_regate = 1;

    return cfg;
}

/* Synth-split P3 — config for ONE LuxStral send: engine-A shape, the SEND's
 * conditioning bank (luxstral_out[bank_slot]) and its own envelope state
 * (ENVELOPE_LS_SEND_BASE + chain_idx). Intensity stays 1.0 per frame — the
 * audio-thread mixer applies the bank's intensity as the mix weight.
 * player_fed = 1 → FramePlayerThread drives this send: its freeze_mode is
 * authoritative (no live re-gate) and the sampler-stream base config applies. */
PipelineConfig pipeline_build_config_ls_send(int bank_slot, int chain_idx,
                                             int player_fed)
{
    PipelineConfig cfg = player_fed ? pipeline_build_config_sampler()
                                    : pipeline_build_config_live();

    if (bank_slot < 0) bank_slot = 0;
    if (bank_slot > 7) bank_slot = 7;
    if (chain_idx < 0) chain_idx = 0;
    if (chain_idx > 7) chain_idx = 7;
    const lux_out_params_t *out = &g_sp3ctra_config.luxstral_out[bank_slot];

    cfg.luxstral_path.inversion  = out->negative;
    cfg.luxstral_path.ac_removal = out->dc_blocking;
    cfg.luxstral_path.gamma      = out->gamma;
    cfg.contrast_min             = out->contrast_min;
    cfg.luxstral_db_range        = out->range_db;
    cfg.luxstral_intensity       = 1.0f;
    cfg.envelope_id              = ENVELOPE_LS_SEND_BASE + chain_idx;
    cfg.live_regate              = player_fed ? 0 : 1;

    return cfg;
}

PipelineConfig pipeline_build_config_sampler(void)
{
    PipelineConfig cfg;

    /* Path A — LuxStral: per-OUT conditioning bank, engine A = slot 0
     * (synth-split P1 — same bank as the live builder: the OUT owns its
     * conditioning regardless of which worker drives the pipeline).
     * contrast_min stays on the sampler-specific floor below (parity with
     * the legacy sampler stream); unification is a P3/P4 concern. */
    const lux_out_params_t *out_a = &g_sp3ctra_config.luxstral_out[0];
    cfg.luxstral_path.inversion  = out_a->negative;
    cfg.luxstral_path.ac_removal = out_a->dc_blocking;
    cfg.luxstral_path.gamma      = out_a->gamma;
    cfg.luxstral_db_range        = out_a->range_db;
    cfg.luxstral_intensity       = out_a->intensity;

    /* Path B — LuxSynth+LuxWave: per-OUT bank, slot 0 (informational — the
     * consumers read their banks directly, see pipeline_build_config_live). */
    cfg.luxsynth_luxwave_path.inversion  = g_sp3ctra_config.luxsynth_out[0].negative;
    cfg.luxsynth_luxwave_path.ac_removal = g_sp3ctra_config.luxsynth_out[0].dc_blocking;
    cfg.luxsynth_luxwave_path.gamma      = g_sp3ctra_config.luxsynth_out[0].gamma;

    /* Mix opacities */
    cfg.sampler_opacity = g_sp3ctra_config.image_sampler_opacity;
    cfg.live_opacity    = g_sp3ctra_config.image_live_opacity;

    /* Freeze / Fade (sampler stream parameters) */
    cfg.freeze_mode    = g_sp3ctra_config.sampler_freeze_mode;
    cfg.fade_in_ms     = 0;   /* sampler transport is instant (2026-07-14) */
    /* M8 — the player stream IS a sampler relay: RAW gate skipped, the
     * crossfader-driven opacity applies (legacy MIX-mode parity). */
    cfg.sampler_relayed = 1;
    cfg.stream_opacity  = g_sp3ctra_config.image_sampler_opacity;
    cfg.contrast_min   = g_sp3ctra_config.sampler_contrast_min;

    /* Misc */
    cfg.stereo_enabled  = g_sp3ctra_config.stereo_mode_enabled;
    cfg.stereo_temp_amp = g_sp3ctra_config.stereo_temperature_amplification;
    cfg.pixels_per_note = g_sp3ctra_config.pixels_per_note;

    /* Envelope identity: always SAMPLER for this builder, regardless of source routing */
    cfg.envelope_id = ENVELOPE_SAMPLER;
    cfg.live_regate = 0;   /* the player's freeze authority is preserved */

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

    /* Stage 5: Gamma — photographic shaping of the grey scale (1.0 = bypass) */
    if (config->luxstral_path.gamma > 0.0f && config->luxstral_path.gamma != 1.0f)
        img_stage_apply_gamma(out->additive.grayscale, nb_pixels,
                              config->luxstral_path.gamma);

    /* Stage 5b: inverse-dB decode law — ALWAYS ON.  Exact inverse of the
     * SCORE encoder's linear-in-dB brightness map, applied on top of the
     * (possibly gamma-shaped) grey scale; gamma 1.0 = pure dB decode.  See
     * config_loader.h for the knob recipe that recovers the exact encoder
     * inverse.  Range dB is per-OUT (synth-split P1). */
    img_stage_apply_db_decode(out->additive.grayscale, nb_pixels,
                              config->luxstral_db_range);

    /* Stage 6: Per-note averaging */
    img_stage_grayscale_luxstral(
        out->additive.grayscale,
        nb_pixels,
        pixels_per_note,
        PREPROCESS_MAX_NOTES,
        out->additive.notes,
        &num_notes);

    /* Stage 7: Freeze / Opacity / Fade envelope — PER-CHAIN transport
     * authority (P4 doctrine, fix 2026-07-14: the old global re-gate silenced
     * every producer-staged send with the sampler transport + RAW gate —
     * an IMAGE-fed chain was mute until two unrelated transports were set
     * to PLAY). The transport that gates a send is the one owning the
     * CHAIN'S SOURCE:
     *   • sampler/score-relayed chain (sampler_relayed):
     *       – producer-staged (live_regate=1): the SAMPLER transport, instant
     *         (no fade — 2026-07-14 per user);
     *       – player-fed (live_regate=0): keep config->freeze_mode — the
     *         sampler builder set it and FramePlayerThread overrides it to
     *         PLAY when the sequencer/score drives (authority preserved).
     *   • SP3CTRA/live-fed chain: the live transport (image_freeze_mode, set
     *     by the live builder) with the transport fade (imageFadeInMs) and
     *     the RAW upstream acquisition gate — the device's own signal.
     *   • internal-source chain (IMAGE/VIDEO/CAMERA): NO global transport at
     *     all — the executor neutralizes freeze/live_regate (the media module
     *     owns its transport; pause = frozen line in the stream itself). */
    {
        /* config->freeze_mode / fade_in_ms are AUTHORITATIVE: the executor
         * resolved the chain's transport before the pipeline
         * (chain_send_transport), and the player's builder carries the
         * sampler-transport authority (+ sequencer/score force-PLAY). The
         * live_regate branch only survives for the legacy non-VST standalone
         * (raw live frame: live + RAW transports merged here). */
        int effective_freeze = config->freeze_mode;
        if (config->live_regate)
        {
            int raw_freeze = g_sp3ctra_config.raw_freeze_mode;
            if (raw_freeze > effective_freeze)
                effective_freeze = raw_freeze;
        }

        /* Use caller-provided envelope_id — NOT derived from source routing.
         * This prevents the live thread from corrupting ENVELOPE_SAMPLER state
         * when source=S routes through the live pipeline path. */
        envelope_id = config->envelope_id;

        pipeline_apply_envelope(
            envelope_id,
            effective_freeze,
            config->stream_opacity,
            config->fade_in_ms,
            out->additive.notes, num_notes,
            out->additive.grayscale, nb_pixels);
    }

    /* Stage 7b: per-OUT Intensity (synth-split P1) — pre-engine mix weight
     * of this send.  Applied to the note amplitudes AFTER the envelope so a
     * change acts immediately even on a HELD frame; 1.0 = bit-exact parity.
     * The grayscale mirror is left untouched (display + deprecated path). */
    if (config->luxstral_intensity != 1.0f)
    {
        float k = config->luxstral_intensity;
        int   note;
        if (k < 0.0f) k = 0.0f;
        for (note = 0; note < num_notes; note++)
            out->additive.notes[note] *= k;
    }

    /* Stage 8: Stereo pan from color temperature */
    if (config->stereo_enabled)
    {
        img_stage_compute_pan_luxstral(
            raw_r, raw_g, raw_b,
            nb_pixels,
            pixels_per_note,
            PREPROCESS_MAX_NOTES,
            config->stereo_temp_amp,
            out->stereo.left_gains,
            out->stereo.right_gains);
    }

    /* Stage 9: StrokeForge blob detection.  Always invoked: strokeforge_analyze_frame
     * early-returns cheaply when StrokeForge is disabled (skipping the expensive
     * blob scan to save CPU) AND resets the morph + attenuation, so no stale state
     * leaks downstream.  The gating therefore lives in strokeforge.c, not here. */
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
    const PipelineConfig *config,
    PreprocessedImageData *out)
{
    if (raw_r == NULL || raw_g == NULL || raw_b == NULL ||
        config == NULL || out == NULL)
        return;

    /* LuxSynth path: delegate to existing FFT pipeline */
#ifndef DISABLE_LUXSYNTH
    preprocess_luxsynth(raw_r, raw_g, raw_b, out);
#endif

    /* (P4 — 2026-07-14: the global "Chain 2" view envelope is GONE. The
     * polyphonic views mirror the pb chain's stream as captured; the display
     * whitening/hold follows the chain's own transport in CisVisualizer, and
     * the ENGINE feeds are gated per send at staging time. One transport
     * authority — chain_send_transport.) */
}

/* ============================================================================
 * M5 — LuxWave send conditioning + audio-thread wavetable feed
 * ============================================================================ */

void luxwave_condition_line(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    int bank_slot,
    float *line_out,
    int nb_pixels)
{
    if (bank_slot < 0 || bank_slot >= LUX_OUT_MAX_SLOTS)
        bank_slot = 0;
    const lux_out_params_t *lw = &g_sp3ctra_config.luxwave_out[bank_slot];

    img_stage_rgb_to_grayscale(raw_r, raw_g, raw_b, nb_pixels, line_out);
    if (lw->negative)
        img_stage_invert(line_out, nb_pixels);
    if (lw->dc_blocking)
        img_stage_remove_dc(line_out, nb_pixels);
    if (lw->gamma > 0.0f && lw->gamma != 1.0f)
        img_stage_apply_gamma(line_out, nb_pixels, lw->gamma);
}

void pipeline_luxwave_feed_tick(const ChainPlan *plan)
{
    static float s_mixed_line[CIS_MAX_PIXELS_NB];   /* audio thread only */

    if (plan == NULL || !g_luxwave_engine.initialized)
        return;

    int nb = 0;
    const int mixed = synth_staging_mix_luxwave(plan, s_mixed_line,
                                                get_cis_pixels_nb(), &nb);
    if (mixed == 0 || nb <= 0)
        return;   /* no "→ LUXWAVE" send → the wavetable keeps its last
                   * content (it only sounds under held MIDI notes) */

    /* (P4 — 2026-07-14: the global Chain-2 gate is GONE — each "→ LUXWAVE"
     * send is gated at staging time by ITS chain's transport, with the fade
     * ramped into the staged line. The mix is already transport-correct.) */
    luxwave_engine_set_image_line(&g_luxwave_engine, s_mixed_line, nb);
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
        config,
        out);

    return 0;
}
