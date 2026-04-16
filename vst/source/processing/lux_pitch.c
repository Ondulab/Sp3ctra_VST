/*
 * lux_pitch.c
 *
 * LuxPitch — MIDI-driven polyphonic image line shifter.
 *
 * Polyphonic blend rules:
 *   - White background: darkest pixel wins (min per channel)
 *   - Black background: brightest pixel wins (max per channel)
 *
 * RT-safety: Pure C, allocation-free, bounded O(N * MAX_VOICES).
 *
 * Author: zhonx
 * Created: 2026-04-16
 */

#include "lux_pitch.h"
#include <string.h>
#include <math.h>
#include <sys/time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Global instances ──────────────────────────────────────────────────────── */
LuxPitchState g_lux_pitch;
LuxPitchState g_lux_pitch_proc;

/* ── Timestamp helper ──────────────────────────────────────────────────────── */
static uint64_t lux_pitch_get_timestamp_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* ── Default config ────────────────────────────────────────────────────────── */
LuxPitchConfig lux_pitch_config_default(void)
{
    LuxPitchConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.enabled                 = 0;
    cfg.polyphony_enabled       = 0;
    cfg.background_mode         = LUX_PITCH_BG_BLACK;
    cfg.reference_note          = 57;   /* A3 */
    cfg.coupling_mode           = LUX_PITCH_COUPLING_LUXSTRAL;
    cfg.free_pixels_per_semitone = 36.0f;
    cfg.pitch_bend_range        = 2.0f;

    cfg.attack_ms               = 10.0f;
    cfg.decay_ms                = 50.0f;
    cfg.sustain_level           = 1.0f;
    cfg.release_ms              = 100.0f;

    cfg.glide_time_ms           = 0.0f;

    cfg.lfo_rate_hz             = 5.0f;
    cfg.lfo_depth_semitones     = 0.0f;

    cfg.velocity_coupling       = 0;

    return cfg;
}

/* ── Init ──────────────────────────────────────────────────────────────────── */
void lux_pitch_init(LuxPitchState *state)
{
    int v;
    if (!state) return;

    memset(state, 0, sizeof(LuxPitchState));
    state->config = lux_pitch_config_default();

    for (v = 0; v < LUX_PITCH_MAX_VOICES; v++)
    {
        atomic_init(&state->midi.voices[v].active,   0);
        atomic_init(&state->midi.voices[v].note,     57);
        atomic_init(&state->midi.voices[v].velocity,  0);
        state->voices[v].envelope_stage = LUX_PITCH_ENV_IDLE;
        state->voices[v].envelope_level = 0.0f;
        state->voices[v].age = 0;
    }
    atomic_init(&state->midi.pitch_bend,   0);
    atomic_init(&state->midi.voice_count,  0);

    state->next_age       = 1;
    state->lfo_phase      = 0.0f;
    state->last_frame_ts_us = 0;
}

/* ── Reset ─────────────────────────────────────────────────────────────────── */
void lux_pitch_reset(LuxPitchState *state)
{
    int v;
    if (!state) return;

    for (v = 0; v < LUX_PITCH_MAX_VOICES; v++)
    {
        state->voices[v].envelope_stage = LUX_PITCH_ENV_IDLE;
        state->voices[v].envelope_level = 0.0f;
        state->voices[v].current_shift  = 0.0f;
        state->voices[v].target_shift   = 0.0f;
        state->voices[v].prev_active    = 0;
    }
    state->lfo_phase      = 0.0f;
    state->last_frame_ts_us = 0;
}

/* ── MIDI event helpers (audio thread — RT-safe) ───────────────────────────── */

void lux_pitch_note_on(LuxPitchState *state, int note, float velocity)
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
        /* Mono mode: always use voice 0 */
        atomic_store_explicit(&state->midi.voices[0].note, note, memory_order_relaxed);
        atomic_store_explicit(&state->midi.voices[0].velocity, vel, memory_order_relaxed);
        atomic_store_explicit(&state->midi.voices[0].active, 1, memory_order_release);
        atomic_store_explicit(&state->midi.voice_count, 1, memory_order_relaxed);
        return;
    }

    /* Poly mode: find a free voice or steal the oldest */
    best = -1;

    /* First pass: find an idle voice (envelope_level ~0 and not active) */
    for (v = 0; v < LUX_PITCH_MAX_VOICES; v++)
    {
        if (!atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed) &&
            state->voices[v].envelope_stage == LUX_PITCH_ENV_IDLE)
        {
            best = v;
            break;
        }
    }

    /* Second pass: find any inactive voice (in release phase) */
    if (best < 0)
    {
        for (v = 0; v < LUX_PITCH_MAX_VOICES; v++)
        {
            if (!atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed))
            {
                best = v;
                break;
            }
        }
    }

    /* Third pass: steal oldest active voice */
    if (best < 0)
    {
        oldest_age = 0xFFFFFFFF;
        best = 0;
        for (v = 0; v < LUX_PITCH_MAX_VOICES; v++)
        {
            if (state->voices[v].age < oldest_age)
            {
                oldest_age = state->voices[v].age;
                best = v;
            }
        }
    }

    state->voices[best].age = state->next_age++;
    atomic_store_explicit(&state->midi.voices[best].note, note, memory_order_relaxed);
    atomic_store_explicit(&state->midi.voices[best].velocity, vel, memory_order_relaxed);
    atomic_store_explicit(&state->midi.voices[best].active, 1, memory_order_release);

    /* Update voice count */
    {
        int count = 0;
        for (v = 0; v < LUX_PITCH_MAX_VOICES; v++)
            if (atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed))
                count++;
        atomic_store_explicit(&state->midi.voice_count, count, memory_order_relaxed);
    }
}

void lux_pitch_note_off(LuxPitchState *state, int note)
{
    int v;
    if (!state) return;

    if (!state->config.polyphony_enabled)
    {
        /* Mono: release voice 0 if matching */
        int cur = atomic_load_explicit(&state->midi.voices[0].note, memory_order_relaxed);
        if (cur == note)
            atomic_store_explicit(&state->midi.voices[0].active, 0, memory_order_release);
        return;
    }

    /* Poly: find the voice playing this note and release it */
    for (v = 0; v < LUX_PITCH_MAX_VOICES; v++)
    {
        if (atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed) &&
            atomic_load_explicit(&state->midi.voices[v].note, memory_order_relaxed) == note)
        {
            atomic_store_explicit(&state->midi.voices[v].active, 0, memory_order_release);
            break;  /* Release first matching voice only */
        }
    }

    /* Update voice count */
    {
        int count = 0;
        for (v = 0; v < LUX_PITCH_MAX_VOICES; v++)
            if (atomic_load_explicit(&state->midi.voices[v].active, memory_order_relaxed))
                count++;
        atomic_store_explicit(&state->midi.voice_count, count, memory_order_relaxed);
    }
}

void lux_pitch_set_pitch_bend(LuxPitchState *state, float bend)
{
    if (!state) return;
    int pb = (int)(bend * 8192.0f);
    if (pb < -8192) pb = -8192;
    if (pb >  8191) pb =  8191;
    atomic_store_explicit(&state->midi.pitch_bend, pb, memory_order_release);
}

/* ── Internal: advance one voice envelope ──────────────────────────────────── */
static void advance_voice_envelope(
    LuxPitchVoiceState *voice,
    int                 midi_active,
    float               dt_s,
    const LuxPitchConfig *cfg)
{
    float peak_level, sustain_target, rate;

    /* Detect note on/off edges */
    if (midi_active && !voice->prev_active)
        voice->envelope_stage = LUX_PITCH_ENV_ATTACK;
    else if (!midi_active && voice->prev_active)
        voice->envelope_stage = LUX_PITCH_ENV_RELEASE;
    voice->prev_active = midi_active;

    peak_level = cfg->velocity_coupling ? voice->velocity_norm : 1.0f;
    if (peak_level < 0.01f) peak_level = 0.01f;

    switch (voice->envelope_stage)
    {
        case LUX_PITCH_ENV_IDLE:
            break;
        case LUX_PITCH_ENV_ATTACK:
            rate = (cfg->attack_ms > 0.1f)
                ? dt_s / (cfg->attack_ms / 1000.0f) : 100.0f;
            voice->envelope_level += rate;
            if (voice->envelope_level >= peak_level)
            {
                voice->envelope_level = peak_level;
                voice->envelope_stage = LUX_PITCH_ENV_DECAY;
            }
            break;
        case LUX_PITCH_ENV_DECAY:
            sustain_target = cfg->sustain_level * peak_level;
            rate = (cfg->decay_ms > 0.1f)
                ? dt_s / (cfg->decay_ms / 1000.0f) : 100.0f;
            voice->envelope_level -= rate;
            if (voice->envelope_level <= sustain_target)
            {
                voice->envelope_level = sustain_target;
                voice->envelope_stage = LUX_PITCH_ENV_SUSTAIN;
            }
            break;
        case LUX_PITCH_ENV_SUSTAIN:
            sustain_target = cfg->sustain_level * peak_level;
            voice->envelope_level = sustain_target;
            break;
        case LUX_PITCH_ENV_RELEASE:
            rate = (cfg->release_ms > 0.1f)
                ? dt_s / (cfg->release_ms / 1000.0f) : 100.0f;
            voice->envelope_level -= rate;
            if (voice->envelope_level <= 0.0f)
            {
                voice->envelope_level = 0.0f;
                voice->envelope_stage = LUX_PITCH_ENV_IDLE;
            }
            break;
    }

    if (voice->envelope_level < 0.0f) voice->envelope_level = 0.0f;
    if (voice->envelope_level > 1.0f) voice->envelope_level = 1.0f;
}

/* ── Process one frame ─────────────────────────────────────────────────────── */
void lux_pitch_process_frame(
    LuxPitchState    *state,
    const uint8_t    *in_r,
    const uint8_t    *in_g,
    const uint8_t    *in_b,
    int               pixel_count,
    int               luxstral_num_octaves,
    const uint8_t   **out_r,
    const uint8_t   **out_g,
    const uint8_t   **out_b)
{
    int i, v;
    float pps;
    float pb_semi;
    float dt_s;
    float lfo_px;
    uint8_t bg;
    float bg_f;
    uint64_t now_us;
    int pitch_bend_raw;
    int num_active_voices;
    int max_voices;

    /* ── Guard clauses ────────────────────────────────────────────────── */
    if (!state || !in_r || !in_g || !in_b || pixel_count <= 0)
    {
        if (out_r) *out_r = in_r;
        if (out_g) *out_g = in_g;
        if (out_b) *out_b = in_b;
        return;
    }

    if (pixel_count > LUX_PITCH_MAX_PIXELS)
        pixel_count = LUX_PITCH_MAX_PIXELS;

    if (!state->config.enabled)
    {
        if (out_r) *out_r = in_r;
        if (out_g) *out_g = in_g;
        if (out_b) *out_b = in_b;
        return;
    }

    /* ── Calculate dt ─────────────────────────────────────────────────── */
    now_us = lux_pitch_get_timestamp_us();
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

    /* ── Pixels per semitone ──────────────────────────────────────────── */
    if (state->config.coupling_mode == LUX_PITCH_COUPLING_LUXSTRAL)
    {
        int octaves = (luxstral_num_octaves > 0) ? luxstral_num_octaves : 8;
        pps = (float)pixel_count / ((float)octaves * 12.0f);
    }
    else
    {
        pps = state->config.free_pixels_per_semitone;
    }

    /* ── LFO (shared across all voices) ───────────────────────────────── */
    lfo_px = 0.0f;
    if (state->config.lfo_depth_semitones > 0.001f &&
        state->config.lfo_rate_hz > 0.001f)
    {
        state->lfo_phase += 2.0f * (float)M_PI * state->config.lfo_rate_hz * dt_s;
        if (state->lfo_phase > 2.0f * (float)M_PI)
            state->lfo_phase -= 2.0f * (float)M_PI;
        lfo_px = sinf(state->lfo_phase) * state->config.lfo_depth_semitones * pps;
    }

    /* ── Background ───────────────────────────────────────────────────── */
    bg  = (state->config.background_mode == LUX_PITCH_BG_WHITE) ? 255 : 0;
    bg_f = (float)bg;

    /* ── Determine how many voices to process ─────────────────────────── */
    max_voices = state->config.polyphony_enabled ? LUX_PITCH_MAX_VOICES : 1;

    /* ── Update all voices (MIDI read + envelope + shift) ─────────────── */
    num_active_voices = 0;
    for (v = 0; v < max_voices; v++)
    {
        int midi_active = atomic_load_explicit(&state->midi.voices[v].active, memory_order_acquire);
        int midi_note   = atomic_load_explicit(&state->midi.voices[v].note,   memory_order_relaxed);
        int midi_vel    = atomic_load_explicit(&state->midi.voices[v].velocity, memory_order_relaxed);

        state->voices[v].note          = midi_note;
        state->voices[v].velocity_norm = (float)midi_vel / 127.0f;

        /* Target shift */
        {
            float note_offset = (float)(midi_note - state->config.reference_note);
            state->voices[v].target_shift = (note_offset + pb_semi) * pps;
        }

        /* Glide */
        if (state->config.glide_time_ms > 0.1f && dt_s > 0.0f)
        {
            float tau   = state->config.glide_time_ms / 1000.0f;
            float alpha = 1.0f - expf(-dt_s / tau);
            state->voices[v].current_shift += alpha *
                (state->voices[v].target_shift - state->voices[v].current_shift);
        }
        else
        {
            state->voices[v].current_shift = state->voices[v].target_shift;
        }

        /* Envelope */
        advance_voice_envelope(&state->voices[v], midi_active, dt_s, &state->config);

        if (state->voices[v].envelope_level > 0.001f)
            num_active_voices++;
    }

    /* ── If no active voices, fill background ─────────────────────────── */
    if (num_active_voices == 0)
    {
        memset(state->out_r, bg, pixel_count);
        memset(state->out_g, bg, pixel_count);
        memset(state->out_b, bg, pixel_count);
        if (out_r) *out_r = state->out_r;
        if (out_g) *out_g = state->out_g;
        if (out_b) *out_b = state->out_b;
        return;
    }

    /* ── Initialize output with background ────────────────────────────── */
    memset(state->out_r, bg, pixel_count);
    memset(state->out_g, bg, pixel_count);
    memset(state->out_b, bg, pixel_count);

    /* ── Render and blend each active voice ───────────────────────────── */
    for (v = 0; v < max_voices; v++)
    {
        float env, inv_env, total_shift;
        int shift_int;

        if (state->voices[v].envelope_level <= 0.001f)
            continue;  /* Skip silent voices */

        env     = state->voices[v].envelope_level;
        inv_env = 1.0f - env;
        total_shift = state->voices[v].current_shift + lfo_px;
        shift_int   = (int)roundf(total_shift);

        for (i = 0; i < pixel_count; i++)
        {
            int src_idx = i - shift_int;
            float r_val, g_val, b_val;
            uint8_t vr, vg, vb;

            if (src_idx >= 0 && src_idx < pixel_count)
            {
                r_val = (float)in_r[src_idx];
                g_val = (float)in_g[src_idx];
                b_val = (float)in_b[src_idx];
            }
            else
            {
                r_val = bg_f;
                g_val = bg_f;
                b_val = bg_f;
            }

            /* Apply envelope fade (blend with background) */
            vr = (uint8_t)(env * r_val + inv_env * bg_f + 0.5f);
            vg = (uint8_t)(env * g_val + inv_env * bg_f + 0.5f);
            vb = (uint8_t)(env * b_val + inv_env * bg_f + 0.5f);

            /* Blend with accumulator:
             *   White bg → min (darkest wins)
             *   Black bg → max (brightest wins) */
            if (state->config.background_mode == LUX_PITCH_BG_WHITE)
            {
                if (vr < state->out_r[i]) state->out_r[i] = vr;
                if (vg < state->out_g[i]) state->out_g[i] = vg;
                if (vb < state->out_b[i]) state->out_b[i] = vb;
            }
            else
            {
                if (vr > state->out_r[i]) state->out_r[i] = vr;
                if (vg > state->out_g[i]) state->out_g[i] = vg;
                if (vb > state->out_b[i]) state->out_b[i] = vb;
            }
        }
    }

    if (out_r) *out_r = state->out_r;
    if (out_g) *out_g = state->out_g;
    if (out_b) *out_b = state->out_b;
}
