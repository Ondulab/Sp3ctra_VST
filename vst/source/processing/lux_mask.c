/*
 * lux_mask.c
 *
 * LuxMask — MIDI-driven mobile spotlight implementation.
 *
 * Per-frame pipeline:
 *   1. Read MIDI atomics -> update per-voice runtime state.
 *   2. Advance ADSR envelopes and glide.
 *   3. Update the position LFO (vibrato).
 *   4. For each voice the ADSR output sets the openness of a spatial LP/HP/BP
 *      filter (cutoff anchored on the note); accumulate the soft-edged passband
 *      reveal: alpha(i) = clamp(Σ_v gate_v(i), 0, 1).
 *   5. out(i,c) = lerp(bg, src(i,c), alpha(i)).
 *
 * RT-safety: Pure C, allocation-free, bounded O(N * MAX_VOICES).
 *
 * Author: zhonx
 * Created: 2026-06-08
 */

#include "lux_mask.h"
#include "lux_env_shape.h"
#include <string.h>
#include <math.h>
#include <sys/time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Global instance ───────────────────────────────────────────────────────────
 * Single simulation (M2): the synthesis-thread instance is the only one.
 * Visualizers read the insert taps published by the chain executor. */
LuxMaskState g_lux_mask_proc;

/* ── Timestamp helper ──────────────────────────────────────────────────────── */
static uint64_t lux_mask_get_timestamp_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Soft step in [0,1]: 0 for x << 0, 1 for x >> 0, smooth tanh edge around 0.
 * `x` is already normalised by the soft-edge half-width, so the ±4 guards skip
 * the expensive tanhf() outside the transition band. */
static inline float lux_mask_soft_gate(float x)
{
    if (x >  4.0f) return 1.0f;
    if (x < -4.0f) return 0.0f;
    return 0.5f * (1.0f + tanhf(x));
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxMaskConfig lux_mask_config_default(void)
{
    LuxMaskConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled                  = 0;
    cfg.polyphony_enabled        = 0;
    cfg.background_mode          = LUX_MASK_BG_BLACK;
    cfg.reference_note           = 57;        /* A3 */
    cfg.coupling_mode            = LUX_MASK_COUPLING_LUXSTRAL;
    cfg.free_pixels_per_semitone = 36.0f;
    cfg.pitch_bend_range         = 2.0f;

    cfg.filter_width_pct         = 30.0f;
    cfg.filter_offset_pct        = 0.0f;
    cfg.filter_slope             = 0.5f;

    cfg.attack_ms                = 20.0f;
    cfg.decay_ms                 = 120.0f;
    cfg.sustain_level            = 1.0f;
    cfg.release_ms               = 200.0f;

    cfg.attack_curve             = 0.0f;   /* linear by default */
    cfg.decay_curve              = 0.0f;
    cfg.release_curve            = 0.5f;   /* gentle convex — approximates the old musical exp release */

    cfg.glide_time_ms            = 0.0f;

    cfg.lfo_pos_rate_hz          = 5.0f;
    cfg.lfo_pos_depth_semitones  = 0.0f;

    cfg.velocity_coupling        = 0;
    return cfg;
}

/* ── Init / reset ──────────────────────────────────────────────────────────── */
void lux_mask_init(LuxMaskState *state)
{
    int v;
    if (!state) return;

    memset(state, 0, sizeof(LuxMaskState));
    state->config = lux_mask_config_default();

    for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
    {
        atomic_init(&state->midi.voices[v].active,    0);
        atomic_init(&state->midi.voices[v].note,      57);
        atomic_init(&state->midi.voices[v].velocity,  0);
        atomic_init(&state->midi.voices[v].retrigger, 0);
        atomic_init(&state->midi.voices[v].sustained, 0);
        state->voices[v].envelope_stage      = LUX_MASK_ENV_IDLE;
        state->voices[v].envelope_level      = 0.0f;
        state->voices[v].peak_level          = 1.0f;
        state->voices[v].release_start_level = 0.0f;
        state->voices[v].age = 0;
    }
    atomic_init(&state->midi.pitch_bend,  0);
    atomic_init(&state->midi.voice_count, 0);
    atomic_init(&state->midi.sustain,     0);

    state->next_age         = 1;
    state->lfo_pos_phase    = 0.0f;
    state->last_frame_ts_us = 0;
}

void lux_mask_reset(LuxMaskState *state)
{
    int v;
    if (!state) return;
    for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
    {
        state->voices[v].envelope_stage      = LUX_MASK_ENV_IDLE;
        state->voices[v].envelope_level      = 0.0f;
        state->voices[v].current_pos         = 0.0f;
        state->voices[v].target_pos          = 0.0f;
        state->voices[v].prev_active         = 0;
        state->voices[v].peak_level          = 1.0f;
        state->voices[v].release_start_level = 0.0f;
    }
    state->lfo_pos_phase    = 0.0f;
    state->last_frame_ts_us = 0;
}

/* ── MIDI event helpers (audio thread — RT-safe) ───────────────────────────── */

void lux_mask_note_on(LuxMaskState *state, int note, float velocity)
{
    int v, best;
    uint32_t oldest_age;
    int vel;

    if (!state) return;

    vel = (int)(velocity * 127.0f);
    if (vel < 0)   vel = 0;
    if (vel > 127) vel = 127;

    if (!state->config.polyphony_enabled)
    {
        /* Mono: legato (no retrigger) — glide covers the transition. */
        atomic_store_explicit(&state->midi.voices[0].note,      note, memory_order_relaxed);
        atomic_store_explicit(&state->midi.voices[0].velocity,  vel,  memory_order_relaxed);
        atomic_store_explicit(&state->midi.voices[0].sustained, 0,    memory_order_relaxed);
        atomic_store_explicit(&state->midi.voices[0].active,    1,    memory_order_release);
        atomic_store_explicit(&state->midi.voice_count,         1,    memory_order_relaxed);
        return;
    }

    best = -1;

    /* 1) idle voice */
    for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
    {
        if (!atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed) &&
            state->voices[v].envelope_stage == LUX_MASK_ENV_IDLE)
        {
            best = v;
            break;
        }
    }

    /* 2) any inactive voice (in release) */
    if (best < 0)
    {
        for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
        {
            if (!atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed))
            {
                best = v;
                break;
            }
        }
    }

    /* 3) steal oldest active voice */
    if (best < 0)
    {
        oldest_age = 0xFFFFFFFF;
        best = 0;
        for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
        {
            if (state->voices[v].age < oldest_age)
            {
                oldest_age = state->voices[v].age;
                best = v;
            }
        }
        /* Stolen voice never sees an inactive edge → flag a forced
         * re-ATTACK (consumed by process_frame) so the spotlight blooms
         * again instead of teleporting mid-sustain. */
        atomic_store_explicit(&state->midi.voices[best].retrigger, 1,
                              memory_order_relaxed);
    }

    state->voices[best].age = state->next_age++;
    atomic_store_explicit(&state->midi.voices[best].note,      note, memory_order_relaxed);
    atomic_store_explicit(&state->midi.voices[best].velocity,  vel,  memory_order_relaxed);
    atomic_store_explicit(&state->midi.voices[best].sustained, 0,    memory_order_relaxed);
    atomic_store_explicit(&state->midi.voices[best].active,    1,    memory_order_release);

    {
        int count = 0;
        for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
            if (atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed))
                count++;
        atomic_store_explicit(&state->midi.voice_count, count, memory_order_relaxed);
    }
}

void lux_mask_note_off(LuxMaskState *state, int note)
{
    int v;
    int sustain;
    if (!state) return;

    sustain = atomic_load_explicit(&state->midi.sustain, memory_order_relaxed);

    if (!state->config.polyphony_enabled)
    {
        int cur = atomic_load_explicit(&state->midi.voices[0].note, memory_order_relaxed);
        if (cur == note)
        {
            if (sustain)
                atomic_store_explicit(&state->midi.voices[0].sustained, 1, memory_order_relaxed);
            else
                atomic_store_explicit(&state->midi.voices[0].active, 0, memory_order_release);
        }
        return;
    }

    for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
    {
        if (atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed) &&
            atomic_load_explicit(&state->midi.voices[v].note,   memory_order_relaxed) == note)
        {
            if (sustain)
                atomic_store_explicit(&state->midi.voices[v].sustained, 1, memory_order_relaxed);
            else
                atomic_store_explicit(&state->midi.voices[v].active, 0, memory_order_release);
            break;
        }
    }

    {
        int count = 0;
        for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
            if (atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed))
                count++;
        atomic_store_explicit(&state->midi.voice_count, count, memory_order_relaxed);
    }
}

void lux_mask_set_pitch_bend(LuxMaskState *state, float bend)
{
    int pb;
    if (!state) return;
    pb = (int)(bend * 8192.0f);
    if (pb < -8192) pb = -8192;
    if (pb >  8191) pb =  8191;
    atomic_store_explicit(&state->midi.pitch_bend, pb, memory_order_release);
}

void lux_mask_set_sustain(LuxMaskState *state, int on)
{
    int v;
    if (!state) return;

    atomic_store_explicit(&state->midi.sustain, on ? 1 : 0, memory_order_relaxed);

    if (!on)
    {
        /* Pedal up: release every voice whose note-off was deferred. */
        for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
        {
            if (atomic_exchange_explicit(&state->midi.voices[v].sustained, 0,
                                         memory_order_relaxed))
                atomic_store_explicit(&state->midi.voices[v].active, 0,
                                      memory_order_release);
        }
    }
}

void lux_mask_all_notes_off(LuxMaskState *state)
{
    int v;
    if (!state) return;

    /* Release every voice that is currently held.  We DO NOT touch the
     * envelope_stage / envelope_level directly — clearing the `active`
     * atomic is exactly what advance_voice_envelope() listens for to
     * trigger the standard RELEASE transition (so the configured release
     * curve is preserved, including the exponential decay shape, as well
     * as the width-bloom snapshot of release_start_level). */
    for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
    {
        atomic_store_explicit(&state->midi.voices[v].sustained, 0,
                              memory_order_relaxed);
        atomic_store_explicit(&state->midi.voices[v].active, 0,
                              memory_order_release);
    }
    atomic_store_explicit(&state->midi.voice_count, 0, memory_order_relaxed);
}

/* ── Voice envelope advance ────────────────────────────────────────────────── */
static void advance_voice_envelope(
    LuxMaskVoiceState   *voice,
    int                  midi_active,
    float                dt_s,
    const LuxMaskConfig *cfg)
{
    float peak_level, sustain_target, seg_s, s;

    /* The envelope is phase-shaped (lux_env_shape per segment).  Its 0..1 output
     * later drives the spatial filter openness in process_frame(). */
    if (midi_active && !voice->prev_active)
    {
        voice->envelope_stage     = LUX_MASK_ENV_ATTACK;
        voice->env_phase          = 0.0f;
        voice->seg_start_level    = voice->envelope_level;
    }
    else if (!midi_active && voice->prev_active)
    {
        /* Snapshot the level at the moment release starts so the RELEASE
         * segment knows how far the envelope has to drop. */
        voice->release_start_level = voice->envelope_level;
        voice->seg_start_level     = voice->envelope_level;
        voice->envelope_stage      = LUX_MASK_ENV_RELEASE;
        voice->env_phase           = 0.0f;
    }
    voice->prev_active = midi_active;

    peak_level = cfg->velocity_coupling ? voice->velocity_norm : 1.0f;
    if (peak_level < 0.01f) peak_level = 0.01f;
    voice->peak_level = peak_level;  /* expose for width-bloom calculation */

    switch (voice->envelope_stage)
    {
        case LUX_MASK_ENV_IDLE:
            break;
        case LUX_MASK_ENV_ATTACK:
            seg_s = (cfg->attack_ms > 0.1f) ? cfg->attack_ms / 1000.0f : 1e-4f;
            voice->env_phase += dt_s / seg_s;
            if (voice->env_phase >= 1.0f)
            {
                voice->envelope_level  = peak_level;
                voice->envelope_stage  = LUX_MASK_ENV_DECAY;
                voice->env_phase       = 0.0f;
                voice->seg_start_level = peak_level;
            }
            else
            {
                s = lux_env_shape(voice->env_phase, cfg->attack_curve);
                voice->envelope_level =
                    voice->seg_start_level + (peak_level - voice->seg_start_level) * s;
            }
            break;
        case LUX_MASK_ENV_DECAY:
        {
            float decay_s = (cfg->decay_ms > 0.1f) ? cfg->decay_ms / 1000.0f : 1e-4f;
            sustain_target = cfg->sustain_level * peak_level;
            voice->env_phase += dt_s / decay_s;
            if (voice->env_phase >= 1.0f)
            {
                voice->envelope_level = sustain_target;
                voice->envelope_stage = LUX_MASK_ENV_SUSTAIN;
            }
            else
            {
                s = lux_env_shape(voice->env_phase, cfg->decay_curve);
                voice->envelope_level =
                    sustain_target + (voice->seg_start_level - sustain_target) * (1.0f - s);
            }
            break;
        }
        case LUX_MASK_ENV_SUSTAIN:
            sustain_target = cfg->sustain_level * peak_level;
            voice->envelope_level = sustain_target;
            break;
        case LUX_MASK_ENV_RELEASE:
        {
            seg_s = (cfg->release_ms > 0.1f) ? cfg->release_ms / 1000.0f : 1e-4f;
            voice->env_phase += dt_s / seg_s;
            if (voice->env_phase >= 1.0f)
            {
                voice->envelope_level = 0.0f;
                voice->envelope_stage = LUX_MASK_ENV_IDLE;
            }
            else
            {
                s = lux_env_shape(voice->env_phase, cfg->release_curve);
                voice->envelope_level = voice->release_start_level * (1.0f - s);
            }
            break;
        }
    }

    if (voice->envelope_level < 0.0f) voice->envelope_level = 0.0f;
    if (voice->envelope_level > 1.0f) voice->envelope_level = 1.0f;
}

/* ── Process one frame ─────────────────────────────────────────────────────── */
void lux_mask_process_frame(
    LuxMaskState   *state,
    const uint8_t  *in_r,
    const uint8_t  *in_g,
    const uint8_t  *in_b,
    int             pixel_count,
    int             luxstral_num_octaves,
    const uint8_t **out_r,
    const uint8_t **out_g,
    const uint8_t **out_b)
{
    int i, v;
    float pps;
    float pb_semi;
    float dt_s;
    float lfo_pos_px;
    uint8_t bg;
    float bg_f;
    uint64_t now_us;
    int pitch_bend_raw;
    int num_active_voices;
    int max_voices;

    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
    {
        if (out_r) *out_r = in_r;
        if (out_g) *out_g = in_g;
        if (out_b) *out_b = in_b;
        return;
    }

    if (pixel_count > LUX_MASK_MAX_PIXELS)
        pixel_count = LUX_MASK_MAX_PIXELS;

    state->last_pixel_count = pixel_count;   /* publish for the UI overlay */

    if (!state->config.enabled)
    {
        if (out_r) *out_r = in_r;
        if (out_g) *out_g = in_g;
        if (out_b) *out_b = in_b;
        return;
    }

    /* ── dt ───────────────────────────────────────────────────────────── */
    now_us = lux_mask_get_timestamp_us();
    dt_s = 0.016f;
    if (state->last_frame_ts_us > 0)
    {
        uint64_t elapsed = now_us - state->last_frame_ts_us;
        if (elapsed > 0 && elapsed < 1000000)
            dt_s = (float)elapsed / 1000000.0f;
    }
    state->last_frame_ts_us = now_us;

    /* ── Global pitch bend ────────────────────────────────────────────── */
    pitch_bend_raw = atomic_load_explicit(&state->midi.pitch_bend, memory_order_relaxed);
    pb_semi = ((float)pitch_bend_raw / 8192.0f) * state->config.pitch_bend_range;

    /* ── Pixels-per-semitone ──────────────────────────────────────────── */
    if (state->config.coupling_mode == LUX_MASK_COUPLING_LUXSTRAL)
    {
        int octaves = (luxstral_num_octaves > 0) ? luxstral_num_octaves : 8;
        pps = (float)pixel_count / ((float)octaves * 12.0f);
    }
    else
    {
        pps = state->config.free_pixels_per_semitone;
    }

    /* ── LFOs (shared across voices) ──────────────────────────────────────
     * Effective depth = the single configured depth. The CC1 mod wheel and
     * the on-screen "LFO Pos Depth" slider drive this same parameter (the
     * wheel moves the slider in the host), so there is no separate additive
     * contribution to combine here. */
    lfo_pos_px = 0.0f;
    {
        float depth = state->config.lfo_pos_depth_semitones;
        if (depth > 0.001f && state->config.lfo_pos_rate_hz > 0.001f)
        {
            state->lfo_pos_phase += 2.0f * (float)M_PI * state->config.lfo_pos_rate_hz * dt_s;
            if (state->lfo_pos_phase > 2.0f * (float)M_PI)
                state->lfo_pos_phase -= 2.0f * (float)M_PI;
            lfo_pos_px = sinf(state->lfo_pos_phase) * depth * pps;
        }
    }

    /* ── Background ───────────────────────────────────────────────────── */
    bg   = (state->config.background_mode == LUX_MASK_BG_WHITE) ? 255 : 0;
    bg_f = (float)bg;

    max_voices = state->config.polyphony_enabled ? LUX_MASK_MAX_VOICES : 1;

    /* ── Update voices ────────────────────────────────────────────────── */
    num_active_voices = 0;
    for (v = 0; v < max_voices; v++)
    {
        int midi_active = atomic_load_explicit(&state->midi.voices[v].active,   memory_order_acquire);
        int midi_note   = atomic_load_explicit(&state->midi.voices[v].note,     memory_order_relaxed);
        int midi_vel    = atomic_load_explicit(&state->midi.voices[v].velocity, memory_order_relaxed);
        int retrig      = atomic_exchange_explicit(&state->midi.voices[v].retrigger, 0,
                                                   memory_order_relaxed);
        float note_offset, half;

        state->voices[v].note          = midi_note;
        state->voices[v].velocity_norm = (float)midi_vel / 127.0f;

        /* Stolen voice: force a fresh ATTACK from the current level (no
         * flash) — without this the envelope never sees an edge and the
         * spotlight teleports. */
        if (retrig)
        {
            state->voices[v].envelope_stage  = LUX_MASK_ENV_ATTACK;
            state->voices[v].env_phase       = 0.0f;
            state->voices[v].seg_start_level = state->voices[v].envelope_level;
        }

        /* Target position: image centered on reference note + offset in px. */
        half = (float)pixel_count * 0.5f;
        note_offset = (float)(midi_note - state->config.reference_note);
        state->voices[v].target_pos = half + (note_offset + pb_semi) * pps;

        if (state->config.glide_time_ms > 0.1f && dt_s > 0.0f)
        {
            float tau   = state->config.glide_time_ms / 1000.0f;
            float alpha = 1.0f - expf(-dt_s / tau);
            state->voices[v].current_pos += alpha *
                (state->voices[v].target_pos - state->voices[v].current_pos);
        }
        else
        {
            state->voices[v].current_pos = state->voices[v].target_pos;
        }

        advance_voice_envelope(&state->voices[v], midi_active, dt_s, &state->config);

        /* A voice contributes while its envelope is non-negligible.  At env ~ 0
         * the band has collapsed to nothing, so it can be skipped. */
        if (state->voices[v].envelope_level > 0.001f)
            num_active_voices++;
    }

    /* ── Fast path: no active voice → fill the frame with the background. ── */
    if (num_active_voices == 0)
    {
        for (i = 0; i < pixel_count; i++)
        {
            state->out_r[i] = bg;
            state->out_g[i] = bg;
            state->out_b[i] = bg;
        }
        if (out_r) *out_r = state->out_r;
        if (out_g) *out_g = state->out_g;
        if (out_b) *out_b = state->out_b;
        return;
    }

    /* ── Accumulate alpha across all voices ───────────────────────────── */
    memset(state->alpha_buf, 0, sizeof(float) * (size_t)pixel_count);

    /* Soft-edge half-width (pixels): slope=1 -> ~1 px (sharp), slope=0 ->
     * 15 % of the image (very soft).  Shared by every voice this frame. */
    {
        float soft_px;
        float full_w_px;
        float offset_px;
        float slope = state->config.filter_slope;
        if (slope < 0.0f) slope = 0.0f;
        if (slope > 1.0f) slope = 1.0f;
        soft_px = (1.0f - slope) * 0.15f * (float)pixel_count + 1.0f;

        /* Band width + centre offset at full open (openness = 1), in pixels. */
        full_w_px = (state->config.filter_width_pct * 0.01f) * (float)pixel_count;
        if (full_w_px < 0.0f) full_w_px = 0.0f;
        offset_px = (state->config.filter_offset_pct * 0.01f) * (float)pixel_count;

        const float inv_soft = 1.0f / soft_px;

        for (v = 0; v < max_voices; v++)
        {
            float openness, pos, velf, w, centre, lo, hi;
            int   ii, i0, i1;

            openness = state->voices[v].envelope_level;   /* ADSR -> openness */
            if (openness <= 0.001f)
                continue;
            if (openness > 1.0f) openness = 1.0f;

            pos = state->voices[v].current_pos + lfo_pos_px;

            /* Velocity coupling scales the whole filter (width + offset). */
            velf = 1.0f;
            if (state->config.velocity_coupling)
                velf = state->voices[v].velocity_norm;

            /* Bandpass whose width AND centre offset are openness-scaled, so the
             * band collapses to the note at env == 0 (smooth release) and sweeps
             * out to its offset position as env opens (glide-like attack). */
            w      = openness * full_w_px * velf;
            centre = pos + openness * offset_px * velf;
            lo     = centre - w * 0.5f;
            hi     = centre + w * 0.5f;

            /* Transition bands extend ~4*soft beyond each edge. */
            i0 = (int)(lo - 4.0f * soft_px);
            i1 = (int)(hi + 4.0f * soft_px);
            if (i0 < 0)            i0 = 0;
            if (i1 >= pixel_count) i1 = pixel_count - 1;
            for (ii = i0; ii <= i1; ii++)
            {
                float a = lux_mask_soft_gate(((float)ii - lo) * inv_soft)
                        * lux_mask_soft_gate((hi - (float)ii) * inv_soft);
                state->alpha_buf[ii] += a;
            }
        }
    }

    /* ── Apply mask per pixel: out = lerp(bg, src, alpha) ─────────────── */
    for (i = 0; i < pixel_count; i++)
    {
        float a = state->alpha_buf[i];
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;

        /* Pure spotlight: alpha=0 → background, alpha=1 → source image. */
        float r = (float)in_r[i] * a + bg_f * (1.0f - a);
        float g = (float)in_g[i] * a + bg_f * (1.0f - a);
        float b = (float)in_b[i] * a + bg_f * (1.0f - a);

        state->out_r[i] = (uint8_t)(r + 0.5f);
        state->out_g[i] = (uint8_t)(g + 0.5f);
        state->out_b[i] = (uint8_t)(b + 0.5f);
    }

    if (out_r) *out_r = state->out_r;
    if (out_g) *out_g = state->out_g;
    if (out_b) *out_b = state->out_b;
}
