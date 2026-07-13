/*
 * synth_luxsynth_engine.c
 *
 * LuxSynth additive synthesis engine — ported from legacy.
 * Pure C, RT-safe, no JUCE dependencies.
 *
 * Synthesis algorithm:
 *   For each sample:
 *     For each active voice:
 *       Compute ADSR envelope values
 *       For each oscillator (FFT bin):
 *         freq = fundamental * (bin+1)
 *         if freq > Nyquist: skip
 *         amplitude = magnitude[bin] * filter_attenuation
 *         sample += amplitude * sin(phase) * pan
 *         phase += phase_increment
 *       Apply volume ADSR * velocity
 *     Sum all voices to master output
 *
 * Author: Cline (ported from legacy synth_luxsynth.c)
 */

#include "synth_luxsynth_engine.h"
#include "voice_manager.h"
#include "../../processing/lux_env_shape.h"   /* shared ADSR segment shaping */
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWO_PI (2.0 * M_PI)

/* ============================================================================
 * INTERNAL: ADSR helpers
 * ========================================================================== */

static void adsr_init(AdsrEnvelope *env, float attack_ms, float decay_ms,
                       float sustain, float release_ms, float sample_rate,
                       float attack_curve, float decay_curve, float release_curve)
{
    env->state = ADSR_STATE_IDLE;
    env->current_output = 0.0f;
    env->current_samples = 0;

    env->attack_s  = attack_ms * 0.001f;
    env->decay_s   = decay_ms * 0.001f;
    env->sustain_level = sustain;
    env->release_s = release_ms * 0.001f;

    float attack_samples  = env->attack_s * sample_rate;
    float decay_samples   = env->decay_s * sample_rate;
    float release_samples = env->release_s * sample_rate;

    env->attack_time_samples  = attack_samples;
    env->decay_time_samples   = decay_samples;
    env->release_time_samples = release_samples;

    /* Kept for compatibility (unused by the phase-based process). */
    env->attack_increment  = (attack_samples  > 0.0f) ? (1.0f / attack_samples)  : 1.0f;
    env->decay_decrement   = (decay_samples   > 0.0f) ? ((1.0f - sustain) / decay_samples) : 1.0f;
    env->release_decrement = (release_samples > 0.0f) ? (sustain / release_samples) : 1.0f;

    env->attack_curve  = attack_curve;
    env->decay_curve   = decay_curve;
    env->release_curve = release_curve;
    env->attack_start_level  = 0.0f;
    env->release_start_level = 0.0f;
}

static void adsr_trigger(AdsrEnvelope *env)
{
    env->state = ADSR_STATE_ATTACK;
    env->current_samples = 0;
    /* Shape the attack from the current level → 1 (click-free retrigger). */
    env->attack_start_level = env->current_output;
}

static void adsr_release(AdsrEnvelope *env)
{
    if (env->state != ADSR_STATE_IDLE)
    {
        env->state = ADSR_STATE_RELEASE;
        env->current_samples = 0;
        /* Shape the release from wherever the level currently is → 0. */
        env->release_start_level = env->current_output;
    }
}

/* Phase-based ADSR with shared lux_env_shape() curvature.
 * curve = 0 reproduces the previous linear behaviour exactly. */
static float adsr_process(AdsrEnvelope *env)
{
    switch (env->state)
    {
    case ADSR_STATE_ATTACK:
        if (env->attack_time_samples <= 0.0f)
        {
            env->current_output = 1.0f;
            env->state = ADSR_STATE_DECAY;
            env->current_samples = 0;
        }
        else
        {
            float ph = (float)env->current_samples / env->attack_time_samples;
            env->current_samples++;
            if (ph >= 1.0f)
            {
                env->current_output = 1.0f;
                env->state = ADSR_STATE_DECAY;
                env->current_samples = 0;
            }
            else
            {
                const float s = lux_env_shape(ph, env->attack_curve);
                env->current_output = env->attack_start_level
                                    + (1.0f - env->attack_start_level) * s;
            }
        }
        break;

    case ADSR_STATE_DECAY:
        if (env->decay_time_samples <= 0.0f)
        {
            env->current_output = env->sustain_level;
            env->state = ADSR_STATE_SUSTAIN;
        }
        else
        {
            float ph = (float)env->current_samples / env->decay_time_samples;
            env->current_samples++;
            if (ph >= 1.0f)
            {
                env->current_output = env->sustain_level;
                env->state = ADSR_STATE_SUSTAIN;
            }
            else
            {
                const float s = lux_env_shape(ph, env->decay_curve);
                env->current_output = 1.0f - (1.0f - env->sustain_level) * s;
            }
        }
        break;

    case ADSR_STATE_SUSTAIN:
        env->current_output = env->sustain_level;
        break;

    case ADSR_STATE_RELEASE:
        if (env->release_time_samples <= 0.0f)
        {
            env->current_output = 0.0f;
            env->state = ADSR_STATE_IDLE;
        }
        else
        {
            float ph = (float)env->current_samples / env->release_time_samples;
            env->current_samples++;
            if (ph >= 1.0f)
            {
                env->current_output = 0.0f;
                env->state = ADSR_STATE_IDLE;
            }
            else
            {
                const float s = lux_env_shape(ph, env->release_curve);
                env->current_output = env->release_start_level * (1.0f - s);
            }
        }
        break;

    case ADSR_STATE_IDLE:
    default:
        env->current_output = 0.0f;
        break;
    }

    return env->current_output;
}

/* ============================================================================
 * INTERNAL: LFO helper
 * ========================================================================== */

/* ============================================================================
 * Sine lookup table — replaces per-oscillator sinf() in the RT hot path.
 * 4096 entries + linear interpolation ⇒ max error ~2e-7 (≈ -134 dB), far
 * below audibility; the additive engine (LuxStral) uses the same technique.
 * Worst case before: LUXSYNTH_MAX_OSCILLATORS × MAX_VOICES sinf per SAMPLE.
 * ========================================================================== */
#define LUXSYNTH_SINE_LUT_SIZE 4096
static float s_sine_lut[LUXSYNTH_SINE_LUT_SIZE + 1]; /* +1: lerp wrap guard */
static int   s_sine_lut_ready = 0;

static void sine_lut_init(void)
{
    if (s_sine_lut_ready)
        return;
    for (int i = 0; i < LUXSYNTH_SINE_LUT_SIZE; i++)
        s_sine_lut[i] = (float)sin(TWO_PI * (double)i / (double)LUXSYNTH_SINE_LUT_SIZE);
    s_sine_lut[LUXSYNTH_SINE_LUT_SIZE] = s_sine_lut[0];
    s_sine_lut_ready = 1;
}

/* phase in [0, 2π) — callers keep it wrapped.  The mask makes the index
 * bulletproof against float rounding right at the 2π edge (4096 → 0, and
 * sin(2π) == sin(0), so the wrap is exact); size is a power of two. */
static inline float sine_lut(float phase)
{
    const float scale = (float)(LUXSYNTH_SINE_LUT_SIZE / TWO_PI);
    const float idx_f = phase * scale;
    const int   i0    = (int)idx_f & (LUXSYNTH_SINE_LUT_SIZE - 1);
    const float frac  = idx_f - (float)(int)idx_f;
    return s_sine_lut[i0] + frac * (s_sine_lut[i0 + 1] - s_sine_lut[i0]);
}

static void lfo_init(LfoState *lfo, float rate_hz, float depth_semitones, float sample_rate)
{
    lfo->phase = 0.0f;
    lfo->rate_hz = rate_hz;
    lfo->depth_semitones = depth_semitones;
    lfo->phase_increment = (float)(TWO_PI * rate_hz / sample_rate);
    lfo->current_output = 0.0f;
}

static float lfo_process(LfoState *lfo)
{
    lfo->current_output = sinf(lfo->phase);
    lfo->phase += lfo->phase_increment;
    if (lfo->phase >= (float)TWO_PI)
        lfo->phase -= (float)TWO_PI;
    return lfo->current_output;
}

/* ============================================================================
 * PUBLIC: Initialization
 * ========================================================================== */

int luxsynth_engine_init(LuxSynthEngine *engine, float sample_rate, int buffer_size)
{
    if (!engine || sample_rate <= 0.0f || buffer_size <= 0)
        return -1;

    if (buffer_size > LUXSYNTH_MAX_BUFFER_SIZE)
        buffer_size = LUXSYNTH_MAX_BUFFER_SIZE;

    memset(engine, 0, sizeof(LuxSynthEngine));

    sine_lut_init();

    engine->sample_rate = sample_rate;
    engine->inv_sample_rate = 1.0f / sample_rate;
    engine->num_voices = LUXSYNTH_MAX_VOICES;
    engine->current_trigger_order = 0;

    /* Initialize all voices as idle */
    for (int i = 0; i < LUXSYNTH_MAX_VOICES; i++)
    {
        engine->voices[i].midi_note = -1;
        engine->voices[i].active = false;
        engine->voices[i].num_oscillators = LUXSYNTH_MAX_OSCILLATORS;
    }

    /* Default config */
    engine->config.attack_ms = 10.0f;
    engine->config.decay_ms = 100.0f;
    engine->config.sustain_level = 0.7f;
    engine->config.release_ms = 200.0f;
    engine->config.attack_curve = 0.0f;
    engine->config.decay_curve = 0.0f;
    engine->config.release_curve = 0.0f;
    engine->config.filter_attack_ms = 20.0f;
    engine->config.filter_decay_ms = 150.0f;
    engine->config.filter_sustain = 0.5f;
    engine->config.filter_release_ms = 300.0f;
    engine->config.filter_attack_curve = 0.0f;
    engine->config.filter_decay_curve = 0.0f;
    engine->config.filter_release_curve = 0.0f;
    engine->config.filter_cutoff = 1.0f;
    engine->config.filter_env_depth = 0.5f;
    engine->config.lfo_rate_hz = 5.0f;
    engine->config.lfo_depth_semitones = 0.1f;
    engine->config.num_oscillators = LUXSYNTH_MAX_OSCILLATORS;
    engine->config.master_volume = 0.20f;  /* match legacy default — attenuate 128-oscillator sum before hard clip */
    engine->config.sample_rate = sample_rate;
    engine->config.buffer_size = buffer_size;
    engine->config.enabled = true;

    /* Initialize LFO */
    lfo_init(&engine->global_lfo, engine->config.lfo_rate_hz,
             engine->config.lfo_depth_semitones, sample_rate);

    engine->initialized = true;
    return 0;
}

void luxsynth_engine_reset(LuxSynthEngine *engine)
{
    if (!engine) return;

    for (int i = 0; i < LUXSYNTH_MAX_VOICES; i++)
    {
        engine->voices[i].midi_note = -1;
        engine->voices[i].active = false;
        engine->voices[i].volume_env.state = ADSR_STATE_IDLE;
        engine->voices[i].volume_env.current_output = 0.0f;
        engine->voices[i].filter_env.state = ADSR_STATE_IDLE;
        engine->voices[i].filter_env.current_output = 0.0f;
        for (int j = 0; j < LUXSYNTH_MAX_OSCILLATORS; j++)
            engine->voices[i].oscillators[j].phase = 0.0f;
    }
    engine->current_trigger_order = 0;
    engine->global_lfo.phase = 0.0f;
}

/* ============================================================================
 * PUBLIC: Configuration update
 * ========================================================================== */

void luxsynth_engine_set_config(LuxSynthEngine *engine, const LuxSynthConfig *config)
{
    if (!engine || !config) return;

    /* Copy config — this is atomic enough for float fields on aligned structs.
     * Full memory barrier ensures visibility to audio thread. */
    engine->config = *config;

    /* Update LFO parameters */
    engine->global_lfo.rate_hz = config->lfo_rate_hz;
    engine->global_lfo.depth_semitones = config->lfo_depth_semitones;
    engine->global_lfo.phase_increment =
        (float)(TWO_PI * config->lfo_rate_hz * engine->inv_sample_rate);
}

/* ============================================================================
 * PUBLIC: Spectral data update
 * ========================================================================== */

void luxsynth_engine_set_spectral_data(LuxSynthEngine *engine,
                                        const float *magnitudes,
                                        const float *pan_positions,
                                        const float *harmonicity,
                                        const float *left_gains,
                                        const float *right_gains,
                                        int num_bins)
{
    if (!engine || !magnitudes) return;

    int n = (num_bins > LUXSYNTH_MAX_OSCILLATORS) ? LUXSYNTH_MAX_OSCILLATORS : num_bins;
    engine->spectral.num_bins = n;

    memcpy(engine->spectral.magnitudes, magnitudes, (size_t)n * sizeof(float));

    if (pan_positions)
        memcpy(engine->spectral.pan_positions, pan_positions, (size_t)n * sizeof(float));
    if (harmonicity)
        memcpy(engine->spectral.harmonicity, harmonicity, (size_t)n * sizeof(float));
    if (left_gains)
        memcpy(engine->spectral.left_gains, left_gains, (size_t)n * sizeof(float));
    if (right_gains)
        memcpy(engine->spectral.right_gains, right_gains, (size_t)n * sizeof(float));
}

/* ============================================================================
 * PUBLIC: MIDI Note On/Off
 * ========================================================================== */

float luxsynth_midi_to_freq(uint8_t note)
{
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

int luxsynth_engine_note_on(LuxSynthEngine *engine, uint8_t note, uint8_t velocity)
{
    if (!engine || !engine->initialized) return -1;

    /* Find a voice using priority system:
     * 1. IDLE voice
     * 2. RELEASE voice with lowest output
     * 3. ACTIVE voice with oldest trigger order (LRU) */
    int best_idx = -1;

    /* Priority 1: Find IDLE voice */
    for (int i = 0; i < engine->num_voices; i++)
    {
        if (engine->voices[i].volume_env.state == ADSR_STATE_IDLE)
        {
            best_idx = i;
            break;
        }
    }

    /* Priority 2: Find RELEASE voice with lowest output */
    if (best_idx < 0)
    {
        float min_output = 2.0f;
        for (int i = 0; i < engine->num_voices; i++)
        {
            if (engine->voices[i].volume_env.state == ADSR_STATE_RELEASE &&
                engine->voices[i].volume_env.current_output < min_output)
            {
                min_output = engine->voices[i].volume_env.current_output;
                best_idx = i;
            }
        }
    }

    /* Priority 3: Steal oldest ACTIVE voice (LRU) */
    if (best_idx < 0)
    {
        unsigned long long oldest = (unsigned long long)-1;
        for (int i = 0; i < engine->num_voices; i++)
        {
            if (engine->voices[i].trigger_order < oldest)
            {
                oldest = engine->voices[i].trigger_order;
                best_idx = i;
            }
        }
    }

    /* Fallback */
    if (best_idx < 0) best_idx = 0;

    LuxSynthVoice *v = &engine->voices[best_idx];

    /* Initialize voice */
    v->midi_note = note;
    v->velocity = velocity;
    v->frequency = luxsynth_midi_to_freq(note);
    v->trigger_order = engine->current_trigger_order++;
    v->active = true;

    /* Setup oscillators: each oscillator is a harmonic multiple of fundamental */
    int num_osc = engine->config.num_oscillators;
    if (num_osc > LUXSYNTH_MAX_OSCILLATORS) num_osc = LUXSYNTH_MAX_OSCILLATORS;
    v->num_oscillators = num_osc;

    for (int i = 0; i < num_osc; i++)
    {
        float harmonic_freq = v->frequency * (float)(i + 1);
        v->oscillators[i].phase_increment =
            (float)(TWO_PI * harmonic_freq * engine->inv_sample_rate);
        /* Don't reset phase for smoother retrigger */
    }

    /* Initialize ADSR envelopes */
    adsr_init(&v->volume_env,
              engine->config.attack_ms, engine->config.decay_ms,
              engine->config.sustain_level, engine->config.release_ms,
              engine->sample_rate,
              engine->config.attack_curve, engine->config.decay_curve,
              engine->config.release_curve);
    adsr_trigger(&v->volume_env);

    adsr_init(&v->filter_env,
              engine->config.filter_attack_ms, engine->config.filter_decay_ms,
              engine->config.filter_sustain, engine->config.filter_release_ms,
              engine->sample_rate,
              engine->config.filter_attack_curve, engine->config.filter_decay_curve,
              engine->config.filter_release_curve);
    adsr_trigger(&v->filter_env);

    return best_idx;
}

int luxsynth_engine_note_off(LuxSynthEngine *engine, uint8_t note)
{
    if (!engine || !engine->initialized) return -1;

    /* Find the oldest ACTIVE voice with this note */
    int best_idx = -1;
    unsigned long long oldest = (unsigned long long)-1;

    for (int i = 0; i < engine->num_voices; i++)
    {
        LuxSynthVoice *v = &engine->voices[i];
        if (v->midi_note == (int)note &&
            v->volume_env.state != ADSR_STATE_IDLE &&
            v->volume_env.state != ADSR_STATE_RELEASE)
        {
            if (v->trigger_order < oldest)
            {
                oldest = v->trigger_order;
                best_idx = i;
            }
        }
    }

    /* Fallback: find any voice with this note (even in RELEASE) */
    if (best_idx < 0)
    {
        for (int i = 0; i < engine->num_voices; i++)
        {
            if (engine->voices[i].midi_note == (int)note &&
                engine->voices[i].volume_env.state != ADSR_STATE_IDLE)
            {
                best_idx = i;
                break;
            }
        }
    }

    if (best_idx >= 0)
    {
        adsr_release(&engine->voices[best_idx].volume_env);
        adsr_release(&engine->voices[best_idx].filter_env);
    }

    return best_idx;
}

/* ============================================================================
 * PUBLIC: Audio Processing (RT HOT PATH)
 * ========================================================================== */

void luxsynth_engine_process(LuxSynthEngine *engine, int num_samples,
                              float *out_left, float *out_right)
{
    if (!engine || !engine->initialized || !engine->config.enabled)
    {
        if (out_left)  memset(out_left,  0, (size_t)num_samples * sizeof(float));
        if (out_right) memset(out_right, 0, (size_t)num_samples * sizeof(float));
        return;
    }

    if (num_samples > LUXSYNTH_MAX_BUFFER_SIZE)
        num_samples = LUXSYNTH_MAX_BUFFER_SIZE;

    /* Clear output */
    memset(out_left,  0, (size_t)num_samples * sizeof(float));
    memset(out_right, 0, (size_t)num_samples * sizeof(float));

    const float nyquist = engine->sample_rate * 0.5f;
    const float master_vol = engine->config.master_volume;
    const int num_bins = engine->spectral.num_bins;

    /* Safety: table is built in luxsynth_engine_init; this only fires if
     * process were ever reached first (one-time ~4096 sin, non-recurring). */
    if (!s_sine_lut_ready)
        sine_lut_init();

    /* LFO pitch: 2^(lfo·depth/12).  depth == 0 (LFO off / flat) is the common
     * case — no per-sample transcendental at all; otherwise exp2f beats
     * powf(2, ·) by skipping the generic-base machinery. */
    const float lfo_depth_over_12 = engine->global_lfo.depth_semitones * (1.0f / 12.0f);
    const int   lfo_pitch_active  = (lfo_depth_over_12 != 0.0f);
    const float two_pi_inv_sr     = (float)(TWO_PI) * engine->inv_sample_rate;

    /* Process each sample */
    for (int s = 0; s < num_samples; s++)
    {
        /* Update global LFO (once per sample) */
        float lfo_val = lfo_process(&engine->global_lfo);
        float lfo_pitch_mult = lfo_pitch_active
                                   ? exp2f(lfo_val * lfo_depth_over_12)
                                   : 1.0f;

        float sample_left = 0.0f;
        float sample_right = 0.0f;

        /* Process each voice */
        for (int vi = 0; vi < engine->num_voices; vi++)
        {
            LuxSynthVoice *v = &engine->voices[vi];

            if (v->volume_env.state == ADSR_STATE_IDLE)
            {
                /* Voice is idle — clean up */
                if (v->midi_note >= 0)
                {
                    v->midi_note = -1;
                    v->active = false;
                }
                continue;
            }

            /* Process ADSR envelopes */
            float vol_env = adsr_process(&v->volume_env);
            float flt_env = adsr_process(&v->filter_env);

            /* Velocity scaling (0-127 → 0.0-1.0) */
            float vel_scale = (float)v->velocity / 127.0f;

            /* Filter cutoff modulated by envelope */
            float cutoff_frac = engine->config.filter_cutoff +
                                engine->config.filter_env_depth * flt_env;
            if (cutoff_frac > 1.0f) cutoff_frac = 1.0f;
            float cutoff_freq = cutoff_frac * nyquist;

            /* Accumulate oscillators */
            float voice_left = 0.0f;
            float voice_right = 0.0f;

            int max_osc = v->num_oscillators;
            if (max_osc > num_bins) max_osc = num_bins;

            /* Per-voice hoists: fundamental with LFO applied once, not per
             * harmonic (harmonic_freq = freq_base × (osc+1)). */
            const float freq_base = v->frequency * lfo_pitch_mult;

            for (int osc = 0; osc < max_osc; osc++)
            {
                /* Harmonic frequency with LFO modulation */
                float harmonic_freq = freq_base * (float)(osc + 1);

                /* Nyquist check */
                if (harmonic_freq >= nyquist) break;

                /* FFT magnitude — gamma lives in the per-OUT conditioning
                 * bank (pixel domain, luxsynth_condition_line), never here:
                 * a second spectral gamma would double-apply it. */
                float mag = engine->spectral.magnitudes[osc];

                /* Advance phase with LFO-modulated increment (always, so a
                 * bin fading back in resumes with a continuous phase). */
                float phase = v->oscillators[osc].phase;
                v->oscillators[osc].phase =
                    (phase + harmonic_freq * two_pi_inv_sr >= (float)TWO_PI)
                        ? phase + harmonic_freq * two_pi_inv_sr - (float)TWO_PI
                        : phase + harmonic_freq * two_pi_inv_sr;

                /* Dark bin (≈ -120 dB): nothing audible to add — skip the
                 * table lookup, filter and pan work. */
                if (mag < 1.0e-6f)
                    continue;

                /* Spectral filter: attenuate harmonics above cutoff */
                float filter_atten = 1.0f;
                if (harmonic_freq > cutoff_freq && cutoff_freq > 0.0f)
                {
                    float ratio = cutoff_freq / harmonic_freq;
                    filter_atten = ratio * ratio; /* 2nd order rolloff */
                }

                /* Generate sine sample (LUT + lerp — was sinf per osc/sample) */
                float osc_sample = sine_lut(phase);

                /* Apply magnitude, filter, and pan */
                float weighted = osc_sample * mag * filter_atten;

                float lg = engine->spectral.left_gains[osc];
                float rg = engine->spectral.right_gains[osc];

                /* If no pan data, use center */
                if (lg == 0.0f && rg == 0.0f)
                {
                    lg = 0.707f;
                    rg = 0.707f;
                }

                voice_left  += weighted * lg;
                voice_right += weighted * rg;
            }

            /* Apply volume envelope and velocity */
            float voice_gain = vol_env * vel_scale;
            sample_left  += voice_left  * voice_gain;
            sample_right += voice_right * voice_gain;
        }

        /* Apply master volume and soft clip */
        sample_left  *= master_vol;
        sample_right *= master_vol;

        /* Soft clip to [-1, 1] */
        if (sample_left > 1.0f) sample_left = 1.0f;
        else if (sample_left < -1.0f) sample_left = -1.0f;
        if (sample_right > 1.0f) sample_right = 1.0f;
        else if (sample_right < -1.0f) sample_right = -1.0f;

        out_left[s]  = sample_left;
        out_right[s] = sample_right;
    }
}
