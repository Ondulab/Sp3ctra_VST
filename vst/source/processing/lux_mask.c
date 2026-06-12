/*
 * lux_mask.c
 *
 * LuxMask — MIDI-driven mobile spotlight implementation.
 *
 * Per-frame pipeline:
 *   1. Read MIDI atomics → update per-voice runtime state.
 *   2. Advance ADSR envelopes and glide.
 *   3. Update two independent LFOs (position / width).
 *   4. Accumulate alpha(i) = clamp(Σ_v env_v · shape_v(i), 0, 1).
 *   5. weight(i) = floor + (gain - floor) * alpha(i).
 *   6. out(i,c) = src(i,c) * weight(i)  (clamped, optional bg fade).
 *
 * RT-safety: Pure C, allocation-free, bounded O(N * MAX_VOICES).
 *
 * Author: zhonx
 * Created: 2026-06-08
 */

#include "lux_mask.h"
#include <string.h>
#include <math.h>
#include <sys/time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Global instances ──────────────────────────────────────────────────────── */
LuxMaskState g_lux_mask;
LuxMaskState g_lux_mask_proc;
LuxMaskState g_lux_mask_vid;

/* LUT domain: d ∈ [0, LUT_DMAX] mapped onto [0, LUT_SIZE-1]. */
#define LUX_MASK_LUT_DMAX 4.0f

/* ── Timestamp helper ──────────────────────────────────────────────────────── */
static uint64_t lux_mask_get_timestamp_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ── Gauss shape LUT init ──────────────────────────────────────────────────── */
static void lux_mask_build_luts(LuxMaskState *state)
{
    int k;
    const float inv = LUX_MASK_LUT_DMAX / (float)(LUX_MASK_LUT_SIZE - 1);
    for (k = 0; k < LUX_MASK_LUT_SIZE; k++)
    {
        float d = (float)k * inv;        /* d in [0, LUT_DMAX] */
        /* Gauss: exp(-0.5 * (d*2)^2)  — σ = width/2 */
        state->shape_lut_gauss[k] = expf(-0.5f * (d * 2.0f) * (d * 2.0f));
    }
}

/* O(1) gauss sampling with linear interpolation.  Returns α in [0, 1]. */
static inline float lux_mask_sample_gauss(const LuxMaskState *state, float d)
{
    if (d < 0.0f) d = -d;
    if (d >= LUX_MASK_LUT_DMAX) return 0.0f;

    const float fk = d * ((float)(LUX_MASK_LUT_SIZE - 1) / LUX_MASK_LUT_DMAX);
    int   ki = (int)fk;
    float fr = fk - (float)ki;
    if (ki >= LUX_MASK_LUT_SIZE - 1) return 0.0f;
    return state->shape_lut_gauss[ki] * (1.0f - fr)
         + state->shape_lut_gauss[ki + 1] * fr;
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

    cfg.width_base               = 256.0f;

    cfg.attack_ms                = 20.0f;
    cfg.decay_ms                 = 120.0f;
    cfg.sustain_level            = 1.0f;
    cfg.release_ms               = 200.0f;

    /* Width horizon defaults — absolute widths in px, can go up to full image.
     * Attack opens wide (1024 px) and the spotlight focuses during DECAY.
     * Release blooms back to 1024 px as the envelope fades. */
    cfg.width_attack_px          = 1024.0f;
    cfg.width_release_px         = 1024.0f;

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
    lux_mask_build_luts(state);

    for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
    {
        atomic_init(&state->midi.voices[v].active,   0);
        atomic_init(&state->midi.voices[v].note,     57);
        atomic_init(&state->midi.voices[v].velocity, 0);
        state->voices[v].envelope_stage      = LUX_MASK_ENV_IDLE;
        state->voices[v].envelope_level      = 0.0f;
        state->voices[v].peak_level          = 1.0f;
        state->voices[v].release_start_level = 0.0f;
        state->voices[v].age = 0;
    }
    atomic_init(&state->midi.pitch_bend,  0);
    atomic_init(&state->midi.voice_count, 0);

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
        atomic_store_explicit(&state->midi.voices[0].note,     note, memory_order_relaxed);
        atomic_store_explicit(&state->midi.voices[0].velocity, vel,  memory_order_relaxed);
        atomic_store_explicit(&state->midi.voices[0].active,   1,    memory_order_release);
        atomic_store_explicit(&state->midi.voice_count,        1,    memory_order_relaxed);
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
    }

    state->voices[best].age = state->next_age++;
    atomic_store_explicit(&state->midi.voices[best].note,     note, memory_order_relaxed);
    atomic_store_explicit(&state->midi.voices[best].velocity, vel,  memory_order_relaxed);
    atomic_store_explicit(&state->midi.voices[best].active,   1,    memory_order_release);

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
    if (!state) return;

    if (!state->config.polyphony_enabled)
    {
        int cur = atomic_load_explicit(&state->midi.voices[0].note, memory_order_relaxed);
        if (cur == note)
            atomic_store_explicit(&state->midi.voices[0].active, 0, memory_order_release);
        return;
    }

    for (v = 0; v < LUX_MASK_MAX_VOICES; v++)
    {
        if (atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed) &&
            atomic_load_explicit(&state->midi.voices[v].note,   memory_order_relaxed) == note)
        {
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
    float peak_level, sustain_target, rate;

    if (midi_active && !voice->prev_active)
    {
        voice->envelope_stage    = LUX_MASK_ENV_ATTACK;
        voice->decay_progress_s  = 0.0f;   /* fresh attack -> reset width tracker */
        voice->release_progress_s = 0.0f;
    }
    else if (!midi_active && voice->prev_active)
    {
        /* Snapshot the level at the moment release starts so the width-bloom
         * envelope knows how far the alpha has to drop. */
        voice->release_start_level = voice->envelope_level;
        voice->envelope_stage      = LUX_MASK_ENV_RELEASE;
        voice->release_progress_s  = 0.0f; /* start tracking release time */
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
            rate = (cfg->attack_ms > 0.1f) ? dt_s / (cfg->attack_ms / 1000.0f) : 100.0f;
            voice->envelope_level += rate;
            if (voice->envelope_level >= peak_level)
            {
                voice->envelope_level = peak_level;
                voice->envelope_stage = LUX_MASK_ENV_DECAY;
            }
            break;
        case LUX_MASK_ENV_DECAY:
        {
            float decay_s = (cfg->decay_ms > 0.1f) ? cfg->decay_ms / 1000.0f : 1e-4f;
            sustain_target = cfg->sustain_level * peak_level;
            rate = dt_s / decay_s;
            voice->envelope_level -= rate;
            /* Advance the *time* progress of the DECAY segment, independent
             * of the audio level — width modulation tracks this, not the
             * envelope amplitude (which is squeezed by Sustain Level). */
            voice->decay_progress_s += dt_s;
            /* Clamp envelope at sustain but DO NOT leave the DECAY stage
             * until the *time* axis is exhausted — otherwise the width ramp
             * (driven by decay_progress_s) would be truncated and the width
             * would jump at the DECAY → SUSTAIN transition when Sustain
             * Level is high. */
            if (voice->envelope_level < sustain_target)
                voice->envelope_level = sustain_target;
            if (voice->decay_progress_s >= decay_s)
            {
                voice->envelope_level = sustain_target;
                voice->envelope_stage = LUX_MASK_ENV_SUSTAIN;
            }
            break;
        }
        case LUX_MASK_ENV_SUSTAIN:
            sustain_target = cfg->sustain_level * peak_level;
            voice->envelope_level = sustain_target;
            break;
        case LUX_MASK_ENV_RELEASE:
        {
            /* Exponential decay: musically natural release shape.
             * The envelope level is multiplied by k each frame, with k
             * chosen so that the level drops to ~-60 dB (≈ 0.001) over
             * the user-defined release_ms window. This yields a steep
             * initial fall followed by a long, smooth tail — much more
             * musical than the previous linear ramp (which sounded like
             * a mechanical "brake").
             *
             * tau = release_ms / ln(1000) ≈ release_ms / 6.9078
             * k   = exp(-dt_s / tau) = exp(-dt_s * 6.9078 / release_s) */
            const float release_s = (cfg->release_ms > 0.1f)
                                  ? cfg->release_ms / 1000.0f
                                  : 1e-4f;
            const float k = expf(-dt_s * 6.9078f / release_s);
            voice->envelope_level *= k;
            /* Time-based progress for the width interpolation (see above). */
            voice->release_progress_s += dt_s;
            /* Snap to zero once the level is inaudible. */
            if (voice->envelope_level <= 1e-4f)
            {
                voice->envelope_level = 0.0f;
                voice->envelope_stage = LUX_MASK_ENV_IDLE;
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

    /* ── LFOs (shared across voices) ──────────────────────────────────── */
    lfo_pos_px = 0.0f;
    if (state->config.lfo_pos_depth_semitones > 0.001f &&
        state->config.lfo_pos_rate_hz > 0.001f)
    {
        state->lfo_pos_phase += 2.0f * (float)M_PI * state->config.lfo_pos_rate_hz * dt_s;
        if (state->lfo_pos_phase > 2.0f * (float)M_PI)
            state->lfo_pos_phase -= 2.0f * (float)M_PI;
        lfo_pos_px = sinf(state->lfo_pos_phase) *
                     state->config.lfo_pos_depth_semitones * pps;
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
        float note_offset, half;

        state->voices[v].note          = midi_note;
        state->voices[v].velocity_norm = (float)midi_vel / 127.0f;

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

    for (v = 0; v < max_voices; v++)
    {
        float env, pos, width_eff, inv_width;
        int   ii, i0, i1, support_px;

        if (state->voices[v].envelope_level <= 0.001f)
            continue;

        env = state->voices[v].envelope_level;
        pos = state->voices[v].current_pos + lfo_pos_px;

        /* ── Width envelope — ADSR-driven absolute width in pixels ───────
         *
         * Time-based interpolation (decoupled from envelope amplitude so the
         * Sustain Level does NOT affect the width transition):
         *
         *   ATTACK  : width = w_attack                  (held throughout)
         *   DECAY   : width = lerp(w_attack, w_base, p_decay)
         *                     p_decay = decay_progress_s / decay_ms*1e-3
         *                     → 0 at decay start, 1 at decay end.
         *   SUSTAIN : width = w_base
         *   RELEASE : width = lerp(w_base, w_release, p_release)
         *                     p_release = release_progress_s / release_ms*1e-3
         *                     → 0 at release start, 1 at release end.
         *
         * Velocity coupling:
         *   When ON the horizons are pondered between w_base (vel=0) and
         *   the configured horizon (vel=1).
         * ──────────────────────────────────────────────────────────────── */
        {
            float w_base   = state->config.width_base;
            float w_attack = state->config.width_attack_px;
            float w_rel    = state->config.width_release_px;

            if (state->config.velocity_coupling)
            {
                const float vel = state->voices[v].velocity_norm;
                w_attack = w_base + (w_attack - w_base) * vel;
                w_rel    = w_base + (w_rel    - w_base) * vel;
            }

            float w;
            switch (state->voices[v].envelope_stage)
            {
                case LUX_MASK_ENV_ATTACK:
                    w = w_attack;
                    break;
                case LUX_MASK_ENV_DECAY:
                {
                    /* Time-based progress through the DECAY segment.
                     * Independent of Sustain Level — width always traverses
                     * the full w_attack -> w_base ramp over `decay_ms`. */
                    float decay_s = state->config.decay_ms * 1e-3f;
                    if (decay_s < 1e-4f) decay_s = 1e-4f;
                    float t = state->voices[v].decay_progress_s / decay_s;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    /* lerp(w_attack, w_base, t) */
                    w = w_attack + (w_base - w_attack) * t;
                    break;
                }
                case LUX_MASK_ENV_RELEASE:
                {
                    /* Time-based progress through the RELEASE segment.
                     * Independent of release_start_level → the visible bloom
                     * always spans the full `release_ms` window. */
                    float rel_s = state->config.release_ms * 1e-3f;
                    if (rel_s < 1e-4f) rel_s = 1e-4f;
                    float t = state->voices[v].release_progress_s / rel_s;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    /* lerp(w_base, w_rel, t) */
                    w = w_base + (w_rel - w_base) * t;
                    break;
                }
                case LUX_MASK_ENV_SUSTAIN:
                case LUX_MASK_ENV_IDLE:
                default:
                    w = w_base;
                    break;
            }

            if (w < 1.0f) w = 1.0f;
            width_eff = w;
        }
        inv_width = 1.0f / width_eff;

        /* Gauss support extends to d ≈ LUT_DMAX.
         * With σ = width/2 and d in units of width, d=LUT_DMAX means 2*LUT_DMAX
         * standard deviations — far enough for alpha to be negligible.
         * Clamp by pixel_count to avoid pathological iteration when width is
         * very large (e.g. width = full image). */
        support_px = (int)(width_eff * LUX_MASK_LUT_DMAX + 1.0f);
        if (support_px > pixel_count) support_px = pixel_count;

        i0 = (int)pos - support_px;
        i1 = (int)pos + support_px;
        if (i0 < 0)            i0 = 0;
        if (i1 >= pixel_count) i1 = pixel_count - 1;

        for (ii = i0; ii <= i1; ii++)
        {
            float d = ((float)ii - pos) * inv_width;
            float a = lux_mask_sample_gauss(state, d);
            state->alpha_buf[ii] += env * a;
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
