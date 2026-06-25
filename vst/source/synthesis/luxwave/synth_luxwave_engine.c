/*
 * synth_luxwave_engine.c
 *
 * LuxWave dynamic optical wavetable synthesis — pure C, RT-safe.
 * Reads pixel luminance from the LuxSynth preprocessed grayscale line.
 * Each image line is one waveform period; MIDI pitch controls scan speed.
 *
 * RT-SAFETY:
 *   - Double-buffered wavetable with atomic swap (no pointer aliasing)
 *   - Crossfade on wavetable update to eliminate discontinuities
 *   - No allocation, no locks, no I/O in hot path
 *   - LFO computed once per block (not per sample) to avoid powf/sinf overhead
 */

#include "synth_luxwave_engine.h"
#include "../../processing/lux_env_shape.h"   /* shared ADSR segment shaping */
#include <string.h>
#include <math.h>

/* ============================================================================
 * MIDI NOTE -> FREQUENCY CONVERSION (RT-safe)
 * ========================================================================== */

static float midi_to_freq(int note)
{
    if (note < 0)   note = 0;
    if (note > 127) note = 127;
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

/* ============================================================================
 * ADSR HELPERS (self-contained, uses LwAdsrEnv / LwAdsrStage)
 * ========================================================================== */

static void lw_adsr_init(LwAdsrEnv *env)
{
    env->stage = LW_ADSR_IDLE;
    env->level = 0.0f;
    env->phase = 0.0f;
    env->seg_start = 0.0f;
}

static void lw_adsr_gate_on(LwAdsrEnv *env)
{
    env->stage = LW_ADSR_ATTACK;
    env->phase = 0.0f;
    env->seg_start = env->level;   /* click-free retrigger from current level */
}

static void lw_adsr_gate_off(LwAdsrEnv *env)
{
    if (env->stage != LW_ADSR_IDLE)
    {
        env->stage = LW_ADSR_RELEASE;
        env->phase = 0.0f;
        env->seg_start = env->level;   /* release from wherever we are */
    }
}

/* Phase-based ADSR with shared lux_env_shape() curvature.
 * curve = 0 reproduces the previous linear behaviour. */
static float lw_adsr_process(LwAdsrEnv *env,
                              float attack_ms, float decay_ms,
                              float sustain, float release_ms,
                              float inv_sr,
                              float attack_curve, float decay_curve,
                              float release_curve)
{
    float rate;
    switch (env->stage)
    {
    case LW_ADSR_ATTACK:
        rate = (attack_ms > 0.1f) ? inv_sr * 1000.0f / attack_ms : 1.0f;
        env->phase += rate;
        if (env->phase >= 1.0f)
        {
            env->level = 1.0f;
            env->stage = LW_ADSR_DECAY;
            env->phase = 0.0f;
        }
        else
        {
            float s = lux_env_shape(env->phase, attack_curve);
            env->level = env->seg_start + (1.0f - env->seg_start) * s;
        }
        break;
    case LW_ADSR_DECAY:
        rate = (decay_ms > 0.1f) ? inv_sr * 1000.0f / decay_ms : 1.0f;
        env->phase += rate;
        if (env->phase >= 1.0f)
        {
            env->level = sustain;
            env->stage = LW_ADSR_SUSTAIN;
        }
        else
        {
            float s = lux_env_shape(env->phase, decay_curve);
            env->level = 1.0f - (1.0f - sustain) * s;
        }
        break;
    case LW_ADSR_SUSTAIN:
        env->level = sustain;
        break;
    case LW_ADSR_RELEASE:
        rate = (release_ms > 0.1f) ? inv_sr * 1000.0f / release_ms : 1.0f;
        env->phase += rate;
        if (env->phase >= 1.0f)
        {
            env->level = 0.0f;
            env->stage = LW_ADSR_IDLE;
        }
        else
        {
            float s = lux_env_shape(env->phase, release_curve);
            env->level = env->seg_start * (1.0f - s);
        }
        break;
    case LW_ADSR_IDLE:
    default:
        env->level = 0.0f;
        break;
    }
    return env->level;
}

/* ============================================================================
 * WAVETABLE LOOKUP — image line as waveform (linear interpolation)
 * ========================================================================== */

static float wavetable_lookup(const float *line, int pixel_count, float phase)
{
    if (!line || pixel_count <= 0)
        return 0.0f;

    /* phase is [0, 1) — map to pixel index with linear interpolation */
    float pos = phase * (float)pixel_count;
    int   idx = (int)pos;
    float frac = pos - (float)idx;

    if (idx < 0) idx = 0;
    if (idx >= pixel_count) idx = pixel_count - 1;

    int next = idx + 1;
    if (next >= pixel_count) next = 0; /* wrap around */

    /* Input is [0.0, 1.0] from LuxSynth grayscale — convert to [-1.0, +1.0] */
    float s0 = line[idx]  * 2.0f - 1.0f;
    float s1 = line[next] * 2.0f - 1.0f;

    return s0 + frac * (s1 - s0);
}

static float wavetable_lookup_reverse(const float *line, int pixel_count, float phase)
{
    return wavetable_lookup(line, pixel_count, 1.0f - phase);
}

/* Helper: lookup from a specific buffer index with scan mode */
static float wt_lookup_with_mode(const LuxWaveEngine *engine, int buf_idx,
                                  float phase, LuxWaveScanMode scan_mode)
{
    const float *line = engine->wt_buf[buf_idx];
    const int    pxc  = engine->wt_pixel_count[buf_idx];

    switch (scan_mode)
    {
    case LUXWAVE_SCAN_RIGHT_TO_LEFT:
        return wavetable_lookup_reverse(line, pxc, phase);
    case LUXWAVE_SCAN_DUAL:
    {
        float fwd = wavetable_lookup(line, pxc, phase);
        float rev = wavetable_lookup_reverse(line, pxc, phase);
        return 0.5f * (fwd + rev);
    }
    case LUXWAVE_SCAN_LEFT_TO_RIGHT:
    default:
        return wavetable_lookup(line, pxc, phase);
    }
}

/* ============================================================================
 * ONE-POLE LOWPASS FILTER (RT-safe)
 * ========================================================================== */

static float lowpass_process(LuxWaveLowpass *lp, float input,
                              float cutoff_hz, float inv_sr)
{
    if (cutoff_hz <= 0.0f)
        return input;

    /* Simple one-pole: alpha = dt / (RC + dt) */
    float dt = inv_sr;
    float rc = 1.0f / (6.2831853f * cutoff_hz);
    float alpha = dt / (rc + dt);
    if (alpha > 1.0f) alpha = 1.0f;

    lp->prev_output += alpha * (input - lp->prev_output);
    return lp->prev_output;
}

/* ============================================================================
 * PUBLIC API
 * ========================================================================== */

int luxwave_engine_init(LuxWaveEngine *engine, float sample_rate, int buffer_size)
{
    if (!engine || sample_rate <= 0.0f || buffer_size <= 0)
        return -1;
    if (buffer_size > LUXWAVE_MAX_BUFFER_SIZE)
        buffer_size = LUXWAVE_MAX_BUFFER_SIZE;

    memset(engine, 0, sizeof(*engine));
    engine->sample_rate     = sample_rate;
    engine->inv_sample_rate = 1.0f / sample_rate;
    engine->f_min           = 20.0f;
    engine->f_max           = sample_rate * 0.5f;

    /* Default config */
    engine->config.attack_ms        = 10.0f;
    engine->config.decay_ms         = 100.0f;
    engine->config.sustain_level    = 0.7f;
    engine->config.release_ms       = 200.0f;
    engine->config.attack_curve     = 0.0f;
    engine->config.decay_curve      = 0.0f;
    engine->config.release_curve    = 0.0f;
    engine->config.filter_attack_ms = 20.0f;
    engine->config.filter_decay_ms  = 150.0f;
    engine->config.filter_sustain   = 0.5f;
    engine->config.filter_release_ms = 300.0f;
    engine->config.filter_cutoff_hz  = 8000.0f;
    engine->config.filter_env_depth_hz = 4000.0f;
    engine->config.lfo_rate_hz       = 5.0f;
    engine->config.lfo_depth_semitones = 0.1f;
    engine->config.scan_mode         = LUXWAVE_SCAN_LEFT_TO_RIGHT;
    engine->config.amplitude         = LUXWAVE_DEFAULT_AMPLITUDE;
    engine->config.sample_rate       = sample_rate;
    engine->config.buffer_size       = buffer_size;
    engine->config.enabled           = false;

    /* Double-buffer init: both buffers empty, RT reads from 0 */
    atomic_store(&engine->wt_write_idx, 1);
    engine->wt_read_idx    = 0;
    atomic_store(&engine->wt_new_ready, 0);
    engine->xfade_remaining = 0;
    engine->xfade_old_idx   = 0;
    engine->wt_pixel_count[0] = 0;
    engine->wt_pixel_count[1] = 0;

    for (int i = 0; i < LUXWAVE_MAX_VOICES; ++i)
        lw_adsr_init(&engine->voices[i].volume_env);

    engine->initialized = true;
    return 0;
}

void luxwave_engine_reset(LuxWaveEngine *engine)
{
    if (!engine) return;
    for (int v = 0; v < LUXWAVE_MAX_VOICES; ++v)
    {
        engine->voices[v].active = false;
        lw_adsr_init(&engine->voices[v].volume_env);
        lw_adsr_init(&engine->voices[v].filter_env);
        engine->voices[v].phase = 0.0f;
        engine->voices[v].lowpass.prev_output = 0.0f;
    }
    engine->lfo_phase = 0.0f;
    engine->current_trigger_order = 0;
    engine->xfade_remaining = 0;
}

void luxwave_engine_set_config(LuxWaveEngine *engine, const LuxWaveConfig *config)
{
    if (!engine || !config) return;
    engine->config = *config;
    engine->config.sample_rate = engine->sample_rate;
}

/**
 * Called from image processing thread. Copies data into the write buffer
 * then atomically signals the RT thread that new data is available.
 */
void luxwave_engine_set_image_line(LuxWaveEngine *engine,
                                    const float *image_line,
                                    int pixel_count)
{
    if (!engine || !image_line || pixel_count <= 0)
        return;
    if (pixel_count > LUXWAVE_MAX_PIXELS)
        pixel_count = LUXWAVE_MAX_PIXELS;

    /* Write to the buffer the RT thread is NOT currently reading from */
    int wr = atomic_load(&engine->wt_write_idx);
    memcpy(engine->wt_buf[wr], image_line, (size_t)pixel_count * sizeof(float));
    engine->wt_pixel_count[wr] = pixel_count;

    /* Signal: new data available (RT thread will pick it up and crossfade) */
    atomic_store(&engine->wt_new_ready, 1);
}

/* ============================================================================
 * NOTE ON / OFF (voice stealing: oldest voice)
 * ========================================================================== */

int luxwave_engine_note_on(LuxWaveEngine *engine, uint8_t note, uint8_t velocity)
{
    if (!engine || !engine->initialized) return -1;

    /* Find free voice or steal oldest */
    int target = -1;
    unsigned long long oldest = ~0ULL;

    for (int i = 0; i < LUXWAVE_MAX_VOICES; ++i)
    {
        if (!engine->voices[i].active && engine->voices[i].volume_env.stage == LW_ADSR_IDLE)
        {
            target = i;
            break;
        }
    }

    if (target < 0)
    {
        /* Steal oldest active voice */
        for (int i = 0; i < LUXWAVE_MAX_VOICES; ++i)
        {
            if (engine->voices[i].trigger_order < oldest)
            {
                oldest = engine->voices[i].trigger_order;
                target = i;
            }
        }
    }

    if (target < 0) return -1;

    LuxWaveVoice *v = &engine->voices[target];
    v->midi_note     = note;
    v->velocity      = velocity;
    v->frequency     = midi_to_freq(note);
    v->phase         = 0.0f;
    v->active        = true;
    v->trigger_order = engine->current_trigger_order++;

    lw_adsr_init(&v->volume_env);
    lw_adsr_gate_on(&v->volume_env);
    lw_adsr_init(&v->filter_env);
    lw_adsr_gate_on(&v->filter_env);
    v->lowpass.prev_output = 0.0f;

    return target;
}

int luxwave_engine_note_off(LuxWaveEngine *engine, uint8_t note)
{
    if (!engine) return -1;
    for (int i = 0; i < LUXWAVE_MAX_VOICES; ++i)
    {
        if (engine->voices[i].active && engine->voices[i].midi_note == (int)note)
        {
            lw_adsr_gate_off(&engine->voices[i].volume_env);
            lw_adsr_gate_off(&engine->voices[i].filter_env);
            engine->voices[i].active = false;
            return i;
        }
    }
    return -1;
}

/* ============================================================================
 * AUDIO GENERATION — inline in processBlock (RT-safe)
 *
 * Key improvements over v1:
 *   1. Wavetable is read from a LOCAL copy (double-buffered, no race condition)
 *   2. Crossfade between old and new wavetable on update (no discontinuity)
 *   3. LFO ratio computed once per block (no per-sample powf/sinf)
 * ========================================================================== */

void luxwave_engine_process(LuxWaveEngine *engine, int num_samples,
                             float *out_left, float *out_right)
{
    if (!engine || !engine->initialized || !out_left || !out_right)
        return;

    memset(out_left,  0, (size_t)num_samples * sizeof(float));
    memset(out_right, 0, (size_t)num_samples * sizeof(float));

    /* Check if new wavetable data is ready (atomic, lock-free) */
    if (atomic_load(&engine->wt_new_ready))
    {
        /* Swap: the write buffer becomes the new read buffer */
        int old_read = engine->wt_read_idx;
        int new_read = atomic_load(&engine->wt_write_idx);

        engine->xfade_old_idx   = old_read;
        engine->wt_read_idx     = new_read;
        engine->xfade_remaining = LUXWAVE_CROSSFADE_SAMPLES;

        /* Toggle write index so next set_image_line writes to the old buffer */
        atomic_store(&engine->wt_write_idx, old_read);
        atomic_store(&engine->wt_new_ready, 0);
    }

    /* Bail out if no wavetable data yet */
    const int rd_idx = engine->wt_read_idx;
    if (engine->wt_pixel_count[rd_idx] <= 0)
        return;

    const LuxWaveConfig *cfg = &engine->config;
    const float inv_sr = engine->inv_sample_rate;

    /* LFO: compute ratio ONCE per block (saves per-sample powf+sinf) */
    float lfo_val = sinf(engine->lfo_phase * 6.2831853f);
    float lfo_semitones = lfo_val * cfg->lfo_depth_semitones;
    float lfo_ratio = powf(2.0f, lfo_semitones / 12.0f);
    /* Advance LFO phase for next block */
    float lfo_inc_total = cfg->lfo_rate_hz * inv_sr * (float)num_samples;
    engine->lfo_phase += lfo_inc_total;
    if (engine->lfo_phase >= 1.0f)
        engine->lfo_phase -= (float)(int)engine->lfo_phase;

    for (int s = 0; s < num_samples; ++s)
    {
        float mix_l = 0.0f;
        float mix_r = 0.0f;

        /* Crossfade weight: 1.0 = fully new, 0.0 = fully old */
        float xfade_new = 1.0f;
        if (engine->xfade_remaining > 0)
        {
            xfade_new = 1.0f - (float)engine->xfade_remaining
                              / (float)LUXWAVE_CROSSFADE_SAMPLES;
            engine->xfade_remaining--;
        }

        for (int v = 0; v < LUXWAVE_MAX_VOICES; ++v)
        {
            LuxWaveVoice *voice = &engine->voices[v];
            if (voice->volume_env.stage == LW_ADSR_IDLE)
                continue;

            /* ADSR */
            float vol_env = lw_adsr_process(&voice->volume_env,
                                          cfg->attack_ms, cfg->decay_ms,
                                          cfg->sustain_level, cfg->release_ms,
                                          inv_sr,
                                          cfg->attack_curve, cfg->decay_curve,
                                          cfg->release_curve);
            float flt_env = lw_adsr_process(&voice->filter_env,
                                          cfg->filter_attack_ms, cfg->filter_decay_ms,
                                          cfg->filter_sustain, cfg->filter_release_ms,
                                          inv_sr,
                                          0.0f, 0.0f, 0.0f);  /* filter ADSR: linear (no UI) */

            if (vol_env < LUXWAVE_MIN_AUDIBLE)
                continue;

            /* Frequency with LFO */
            float freq = voice->frequency * lfo_ratio;

            /* Phase increment: freq / sample_rate mapped to [0,1) */
            float phase_inc = freq * inv_sr;

            /* Wavetable lookup from current (new) buffer */
            float sample_new = wt_lookup_with_mode(engine, rd_idx,
                                                    voice->phase,
                                                    cfg->scan_mode);

            /* If crossfading, blend with old buffer */
            float sample_val;
            if (xfade_new < 0.999f)
            {
                float sample_old = wt_lookup_with_mode(engine,
                                                        engine->xfade_old_idx,
                                                        voice->phase,
                                                        cfg->scan_mode);
                sample_val = sample_old + xfade_new * (sample_new - sample_old);
            }
            else
            {
                sample_val = sample_new;
            }

            /* Apply lowpass filter with envelope modulation */
            float cutoff = cfg->filter_cutoff_hz + flt_env * cfg->filter_env_depth_hz;
            if (cutoff > engine->f_max) cutoff = engine->f_max;
            sample_val = lowpass_process(&voice->lowpass, sample_val, cutoff, inv_sr);

            /* Apply velocity and volume envelope */
            float vel_gain = (float)voice->velocity / 127.0f;
            float out_val = sample_val * vol_env * vel_gain * cfg->amplitude;

            /* Simple stereo panning based on MIDI note */
            float pan = (float)(voice->midi_note - 60) / 60.0f;
            if (pan < -1.0f) pan = -1.0f;
            if (pan >  1.0f) pan =  1.0f;
            float pan_l = 0.5f * (1.0f - pan);
            float pan_r = 0.5f * (1.0f + pan);

            mix_l += out_val * pan_l;
            mix_r += out_val * pan_r;

            /* Advance phase */
            voice->phase += phase_inc;
            if (voice->phase >= 1.0f)
                voice->phase -= 1.0f;
        }

        out_left[s]  = mix_l;
        out_right[s] = mix_r;
    }
}
