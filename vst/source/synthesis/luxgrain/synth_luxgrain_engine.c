/*
 * synth_luxgrain_engine.c
 *
 * LuxGrain stochastic granular engine — see synth_luxgrain_engine.h for the
 * full design contract (luminance → emission density, band cells, spread
 * ring, deterministic RNG).
 */

#include "synth_luxgrain_engine.h"
#include <math.h>
#include <string.h>

/* ============================================================================
 * Sine LUT (engine-private; 4096 points over one period, no interpolation —
 * grain sinusoids under a stochastic cloud don't need better).
 * ========================================================================== */
#define LG_SINE_LUT_SIZE 4096
#define LG_SINE_LUT_MASK (LG_SINE_LUT_SIZE - 1)
static float lg_sine_lut[LG_SINE_LUT_SIZE];
static int   lg_lut_ready = 0;

static void lg_build_lut(void) {
  if (lg_lut_ready)
    return;
  for (int i = 0; i < LG_SINE_LUT_SIZE; i++)
    lg_sine_lut[i] = (float)sin(6.283185307179586 * (double)i /
                                (double)LG_SINE_LUT_SIZE);
  lg_lut_ready = 1;
}

static inline float lg_sin01(float phase01) {
  /* phase01 in [0,1) — one full period. */
  return lg_sine_lut[(int)(phase01 * (float)LG_SINE_LUT_SIZE) &
                     LG_SINE_LUT_MASK];
}

/* ============================================================================
 * RNG — xorshift32, one stream per engine. All draws go through here so a
 * fixed seed + fixed call sequence reproduces the cloud bit-exactly.
 * ========================================================================== */
static inline uint32_t lg_rng_next(uint32_t *s) {
  uint32_t x = *s;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *s = x ? x : 0x9e3779b9u; /* never park at 0 */
  return *s;
}

static inline float lg_frand(uint32_t *s) { /* uniform [0,1) */
  return (float)(lg_rng_next(s) >> 8) * (1.0f / 16777216.0f);
}

static inline float lg_gauss(uint32_t *s) {
  /* Irwin–Hall(3) approximation: mean 0, std 0.5 → scaled to std 1. */
  return (lg_frand(s) + lg_frand(s) + lg_frand(s) - 1.5f) * 2.0f;
}

/* Small-λ Poisson (Knuth). λ is bounded by density_hz × block duration, so
 * the loop is short; hard cap keeps it bounded no matter what. */
static inline int lg_poisson(uint32_t *s, float lambda) {
  if (lambda <= 0.0f)
    return 0;
  if (lambda > 16.0f)
    lambda = 16.0f;
  float L = expf(-lambda);
  int k = 0;
  float p = 1.0f;
  do {
    k++;
    p *= lg_frand(s);
  } while (p > L && k < 64);
  return k - 1;
}

/* ============================================================================
 * Defaults / lifecycle
 * ========================================================================== */
LuxGrainConfig luxgrain_config_default(void) {
  LuxGrainConfig c;
  memset(&c, 0, sizeof(c));
  c.enabled = 1;
  c.density_hz = 6.0f;
  c.density_shape = 1.5f;
  c.spread_lines = 1.0f;
  c.dur_min_ms = 8.0f;
  c.dur_max_ms = 220.0f;
  c.contrast_amount = 0.7f;
  c.env_shape = LUXGRAIN_ENV_HANN;
  c.pitch_jitter_st = 0.15f;
  c.stereo_width = 0.6f;
  c.amp_follow = 0.5f;
  c.color_pan = 1.0f;
  c.edge_amount = 0.0f;
  c.material = LUXGRAIN_MAT_SINE;
  c.scrub = 0.0f;
  c.master_volume = 0.5f;
  c.seed = 0x5eed5EEDu;
  c.axis_low_hz = 65.406f; /* C2 — same fallback as LuxHarmo */
  c.num_octaves = 6;
  c.num_bands = 128;
  return c;
}

int luxgrain_engine_init(LuxGrainEngine *e, float sample_rate) {
  if (!e || sample_rate <= 0.0f)
    return -1;
  lg_build_lut();
  memset(e, 0, sizeof(*e));
  e->config = luxgrain_config_default();
  e->config_pending = e->config;
  e->sample_rate = sample_rate;
  e->inv_sample_rate = 1.0f / sample_rate;
  e->rng = e->config.seed;
  e->axis_pixels = -1; /* force axis cache rebuild on first line */
  e->sample_active = -1;
  e->initialized = 1;
  return 0;
}

void luxgrain_engine_reset(LuxGrainEngine *e) {
  if (!e || !e->initialized)
    return;
  memset(e->grains, 0, sizeof(e->grains));
  e->active_grains = 0;
  e->ring_write = 0;
  e->ring_count = 0;
  e->rng = e->config.seed;
}

void luxgrain_engine_set_config(LuxGrainEngine *e, const LuxGrainConfig *c) {
  if (!e || !c)
    return;
  e->cfg_pending_seq++; /* odd: writer inside */
  e->config_pending = *c;
  e->cfg_pending_seq++; /* even: consistent */
}

int luxgrain_engine_active_grains(const LuxGrainEngine *e) {
  return e ? e->active_grains : 0;
}

/* ============================================================================
 * Line folding (producer side) — pixels → band cells
 * ========================================================================== */
static void lg_fold_line(const float *line, const uint8_t *rr,
                         const uint8_t *gg, const uint8_t *bb, int nb_pixels,
                         int num_bands, LuxGrainBandCell *cells) {
  (void)gg; /* green carries no temperature (red-blue opponent axis) */

  /* Whole-line red-blue bias — removed per band so a global sensor tint
   * never pans the whole cloud (same normalisation as the LuxSynth
   * harmonicity fold). */
  float global_bias = 0.0f;
  const int has_rgb = (rr != 0 && bb != 0);
  if (has_rgb) {
    float sr = 0.0f, sb = 0.0f;
    for (int p = 0; p < nb_pixels; p++) {
      sr += (float)rr[p];
      sb += (float)bb[p];
    }
    global_bias = (sr - sb) / ((float)nb_pixels * 255.0f);
  }

  for (int b = 0; b < num_bands; b++) {
    int p0 = (int)((int64_t)b * nb_pixels / num_bands);
    int p1 = (int)((int64_t)(b + 1) * nb_pixels / num_bands);
    if (p1 <= p0)
      p1 = p0 + 1;
    float sum = 0.0f, sum_sq = 0.0f, wsum = 0.0f, wpos = 0.0f;
    for (int p = p0; p < p1; p++) {
      float v = line[p];
      if (v < 0.0f)
        v = 0.0f;
      else if (v > 1.0f)
        v = 1.0f;
      sum += v;
      sum_sq += v * v;
      wsum += v;
      wpos += v * (float)p;
    }
    int n = p1 - p0;
    float inv_n = 1.0f / (float)n;
    float mean = sum * inv_n;
    float var = sum_sq * inv_n - mean * mean;
    if (var < 0.0f)
      var = 0.0f;
    LuxGrainBandCell *c = &cells[b];
    c->value = mean;
    c->contrast = 2.0f * sqrtf(var); /* std of [0,1] data maxes at 0.5 */
    if (c->contrast > 1.0f)
      c->contrast = 1.0f;
    if (wsum > 1e-6f) {
      float centroid = wpos / wsum;
      float acc = 0.0f;
      for (int p = p0; p < p1; p++) {
        float v = line[p] > 0.0f ? (line[p] < 1.0f ? line[p] : 1.0f) : 0.0f;
        float d = (float)p - centroid;
        acc += v * d * d;
      }
      c->centroid = centroid;
      c->spread_px = sqrtf(acc / wsum);
    } else {
      c->centroid = 0.5f * (float)(p0 + p1 - 1);
      c->spread_px = 0.25f * (float)n;
    }

    /* Colour temperature → pan. Red pulls LEFT (−1), blue RIGHT (+1) —
     * the SCORE-stereo convention. ×3 amplification like the UI FFT view. */
    c->pan = 0.0f;
    if (has_rgb) {
      float sr = 0.0f, sb = 0.0f;
      for (int p = p0; p < p1; p++) {
        sr += (float)rr[p];
        sb += (float)bb[p];
      }
      float temp = (sr - sb) / ((float)n * 255.0f) - global_bias;
      temp *= 3.0f;
      if (temp > 1.0f)
        temp = 1.0f;
      else if (temp < -1.0f)
        temp = -1.0f;
      c->pan = -temp; /* red = negative pan = LEFT */
    }
    c->edge = 0.0f; /* consumer-computed at latch (needs the ring history) */
  }
}

void luxgrain_engine_stage_line(LuxGrainEngine *e, const float *line,
                                const uint8_t *r, const uint8_t *g,
                                const uint8_t *b, int nb_pixels,
                                uint32_t frame_seq) {
  if (!e || !e->initialized || !line || nb_pixels <= 0)
    return;
  if (nb_pixels > LUXGRAIN_MAX_PIXELS)
    nb_pixels = LUXGRAIN_MAX_PIXELS;
  int nb = e->config_pending.num_bands; /* UI-thread copy is fine here: the
                                         * band count only changes on SETUP
                                         * edits, and the latch re-checks. */
  if (nb < 16)
    nb = 16;
  else if (nb > LUXGRAIN_MAX_BANDS)
    nb = LUXGRAIN_MAX_BANDS;

  e->line_pending_seq++; /* odd: writer inside */
  lg_fold_line(line, r, g, b, nb_pixels, nb, e->pending_cells);
  e->pending_bands = nb;
  e->pending_clear = 0;
  e->pending_frame_seq = frame_seq;
  /* The axis cache needs the true line width; it travels with the staged
   * fold and triggers a rebuild at latch time when it changes. */
  e->axis_pixels_pending = nb_pixels;
  e->line_pending_seq++; /* even: consistent */
}

void luxgrain_engine_stage_silence(LuxGrainEngine *e) {
  if (!e || !e->initialized)
    return;
  e->line_pending_seq++;
  e->pending_clear = 1;
  e->pending_bands = 0;
  e->line_pending_seq++;
}

/* ============================================================================
 * Axis cache — band centre frequencies for the current geometry
 * ========================================================================== */
static void lg_rebuild_axis(LuxGrainEngine *e, int nb_pixels) {
  const LuxGrainConfig *c = &e->config;
  float low = c->axis_low_hz > 0.0f ? c->axis_low_hz : 65.406f;
  int oct = c->num_octaves > 0 ? c->num_octaves : 6;
  int nb = c->num_bands;
  if (nb < 16)
    nb = 16;
  else if (nb > LUXGRAIN_MAX_BANDS)
    nb = LUXGRAIN_MAX_BANDS;
  e->px_to_oct = (float)oct / (float)nb_pixels;
  for (int b = 0; b < nb; b++) {
    float centre_px = ((float)b + 0.5f) * (float)nb_pixels / (float)nb;
    e->band_freq[b] = low * exp2f(centre_px * e->px_to_oct);
  }
  e->axis_pixels = nb_pixels;
}

/* ============================================================================
 * Scheduler + grain spawn (audio thread)
 * ========================================================================== */
static void lg_spawn_grain(LuxGrainEngine *e, const LuxGrainBandCell *cell,
                           int onset_delay) {
  const LuxGrainConfig *c = &e->config;

  /* Slot: first free, else steal the grain closest to its end (least
   * audible cut, frees the soonest). */
  LuxGrainVoice *g = 0;
  for (int i = 0; i < LUXGRAIN_MAX_GRAINS; i++) {
    if (!e->grains[i].active) {
      g = &e->grains[i];
      break;
    }
  }
  if (!g) {
    uint32_t best = 0xffffffffu;
    for (int i = 0; i < LUXGRAIN_MAX_GRAINS; i++) {
      uint32_t left = e->grains[i].delay + e->grains[i].remaining;
      if (left < best) {
        best = left;
        g = &e->grains[i];
      }
    }
    e->active_grains--; /* replaced, re-counted below */
  }

  /* Frequency: centroid ± intra-band spread (gaussian) ± jitter. */
  float px = cell->centroid + lg_gauss(&e->rng) * cell->spread_px * 0.5f;
  float jitter_oct =
      c->pitch_jitter_st * (lg_frand(&e->rng) * 2.0f - 1.0f) * (1.0f / 12.0f);
  float freq = (c->axis_low_hz > 0.0f ? c->axis_low_hz : 65.406f) *
               exp2f(px * e->px_to_oct + jitter_oct);
  float nyq = 0.45f * e->sample_rate;
  if (freq > nyq)
    freq = nyq;
  if (freq < 10.0f)
    freq = 10.0f;

  /* Duration: smooth material → long, textured → short (quadratic law). */
  float t = 1.0f - cell->contrast * c->contrast_amount;
  float dur_ms = c->dur_min_ms + (c->dur_max_ms - c->dur_min_ms) * t * t;
  uint32_t body = (uint32_t)(dur_ms * 0.001f * e->sample_rate);
  if (body < 32)
    body = 32;

  /* Amplitude: optionally follows the band value; sqrt for equal power. */
  float follow = 1.0f + c->amp_follow * (sqrtf(cell->value) - 1.0f);
  float amp = c->master_volume * follow;

  /* Pan: deterministic colour part (cell temperature × Color Pan) + random
   * part (× Width), constant-power. θ ∈ [0, π/2], L = cos θ, R = sin θ;
   * with sin01(x) = sin(2πx), t = θ/2π and cos θ = sin01(0.25 − t). */
  float pan = cell->pan * c->color_pan +
              (lg_frand(&e->rng) * 2.0f - 1.0f) * c->stereo_width;
  if (pan > 1.0f)
    pan = 1.0f;
  else if (pan < -1.0f)
    pan = -1.0f;
  float pt = (pan + 1.0f) * 0.125f;
  g->amp_l = amp * lg_sin01(0.25f - pt);
  g->amp_r = amp * lg_sin01(pt);

  g->delay = (uint32_t)onset_delay;
  g->remaining = body;
  g->body_len = body;
  g->phase = lg_frand(&e->rng);
  g->phase_inc = freq * e->inv_sample_rate;
  g->env_pos = 0.0f;
  g->env_inc = 1.0f / (float)body;
  g->env_shape = (uint8_t)c->env_shape;

  /* Material: snapshot the published sample bank (grains keep reading their
   * bank across a republish — see LUXGRAIN_SAMPLE_MAX contract). */
  g->smp = 0;
  const int bank = e->sample_active;
  if (c->material == LUXGRAIN_MAT_SAMPLE && bank >= 0) {
    const int len = e->sample_len[bank];
    const float root = e->sample_root_hz[bank];
    if (len > 64 && root > 0.0f) {
      g->smp = e->sample_data[bank];
      g->smp_len = len;
      g->smp_inc = (freq / root) *
                   (e->sample_srate[bank] * e->inv_sample_rate);
      /* Read window: scrub position ± 2.5% positional jitter — grains from
       * one column shimmer instead of comb-filtering on the same slice. */
      float span = (float)len - (float)body * g->smp_inc - 2.0f;
      if (span < 0.0f)
        span = 0.0f;
      float pos = c->scrub * span +
                  (lg_frand(&e->rng) - 0.5f) * 0.05f * (float)len;
      if (pos < 0.0f)
        pos = 0.0f;
      else if (pos > span)
        pos = span;
      g->smp_pos = pos;
      g->amp_l *= e->sample_gain[bank];
      g->amp_r *= e->sample_gain[bank];
    }
  }

  if (g->env_shape == LUXGRAIN_ENV_EXPODEC) {
    g->exp_state = 1.0f;
    g->exp_coef = expf(-6.9077553f / (float)body); /* −60 dB across body */
  } else if (g->env_shape == LUXGRAIN_ENV_REXPODEC) {
    g->exp_state = 0.001f;
    g->exp_coef = expf(6.9077553f / (float)body); /* 0.001 → 1 across body */
  } else {
    g->exp_state = 1.0f;
    g->exp_coef = 1.0f;
  }
  g->active = 1;
  e->active_grains++;
}

static void lg_schedule_block(LuxGrainEngine *e, int num_samples) {
  const LuxGrainConfig *c = &e->config;
  if (!c->enabled || e->ring_count == 0)
    return;

  int nb = c->num_bands;
  if (nb < 16)
    nb = 16;
  else if (nb > LUXGRAIN_MAX_BANDS)
    nb = LUXGRAIN_MAX_BANDS;

  int window = (int)c->spread_lines;
  if (window < 1)
    window = 1;
  if (window > e->ring_count)
    window = e->ring_count;

  float block_s = (float)num_samples * e->inv_sample_rate;

  for (int b = 0; b < nb; b++) {
    /* One random depth per band per block: an unbiased sample of the spread
     * window that also becomes the spawned grains' source cell — a bright
     * historical column emits ITS pitch, not the live one's. */
    int depth = window > 1 ? (int)(lg_frand(&e->rng) * (float)window) : 0;
    if (depth >= window)
      depth = window - 1;
    int idx = e->ring_write - 1 - depth;
    while (idx < 0)
      idx += LUXGRAIN_MAX_SPREAD;
    const LuxGrainBandCell *cell = &e->ring[idx][b];
    if (cell->value <= 0.001f)
      continue;

    float lambda = c->density_hz * powf(cell->value, c->density_shape) *
                   block_s;
    /* Contours become attacks: a luminance RISE multiplies the emission
     * rate (×1 flat → ×25 on a full edge at amount 1). */
    if (c->edge_amount > 0.0f && cell->edge > 0.0f)
      lambda *= 1.0f + c->edge_amount * cell->edge * 24.0f;
    int n = lg_poisson(&e->rng, lambda);
    for (int k = 0; k < n; k++) {
      int onset = (int)(lg_frand(&e->rng) * (float)num_samples);
      lg_spawn_grain(e, cell, onset);
    }
  }
}

/* ============================================================================
 * Envelope evaluation (per sample, recurrence-based — no expf in the loop)
 * ========================================================================== */
static inline float lg_env(LuxGrainVoice *g) {
  switch (g->env_shape) {
  case LUXGRAIN_ENV_HANN: {
    float s = lg_sin01(g->env_pos * 0.5f); /* sin(π·pos) */
    return s * s;
  }
  case LUXGRAIN_ENV_TUKEY: {
    /* 20% cosine edges, flat middle. */
    float p = g->env_pos;
    if (p < 0.2f) {
      float s = lg_sin01(p * 2.5f * 0.5f); /* sin(π·p/0.4): 0→1 over edge */
      return s * s;
    }
    if (p > 0.8f) {
      float s = lg_sin01((1.0f - p) * 2.5f * 0.5f);
      return s * s;
    }
    return 1.0f;
  }
  case LUXGRAIN_ENV_EXPODEC: {
    g->exp_state *= g->exp_coef;
    /* 2 ms-ish linear attack to avoid the click of a true step. */
    float att = g->env_pos * (float)g->body_len * (1.0f / 96.0f);
    return g->exp_state * (att < 1.0f ? att : 1.0f);
  }
  case LUXGRAIN_ENV_REXPODEC: {
    g->exp_state *= g->exp_coef;
    float rel = (1.0f - g->env_pos) * (float)g->body_len * (1.0f / 96.0f);
    float env = g->exp_state * (rel < 1.0f ? rel : 1.0f);
    return env < 1.0f ? env : 1.0f;
  }
  default:
    return 0.0f;
  }
}

/* ============================================================================
 * Latch + process
 * ========================================================================== */
static void lg_latch_pending(LuxGrainEngine *e) {
  /* Config (retry-free: writer is quick, torn read waits for next block). */
  uint32_t seq = e->cfg_pending_seq;
  if (seq != e->cfg_applied_seq && (seq & 1u) == 0u) {
    LuxGrainConfig snap = e->config_pending;
    if (e->cfg_pending_seq == seq) {
      int geometry_changed = snap.num_bands != e->config.num_bands ||
                             snap.num_octaves != e->config.num_octaves ||
                             snap.axis_low_hz != e->config.axis_low_hz;
      e->config = snap;
      e->cfg_applied_seq = seq;
      if (geometry_changed)
        e->axis_pixels = -1; /* rebuild on next line latch */
    }
  }

  /* Line → ring. */
  seq = e->line_pending_seq;
  if (seq != e->line_applied_seq && (seq & 1u) == 0u) {
    LuxGrainBandCell snap[LUXGRAIN_MAX_BANDS];
    int nb = e->pending_bands;
    int clear = e->pending_clear;
    int npx = e->axis_pixels_pending;
    uint32_t fseq = e->pending_frame_seq;
    if (clear) {
      if (e->line_pending_seq == seq) { /* not torn — wipe the history */
        e->ring_count = 0;
        e->ring_write = 0;
        e->line_applied_seq = seq;
      }
    } else if (nb > 0 && nb <= LUXGRAIN_MAX_BANDS) {
      memcpy(snap, e->pending_cells, (size_t)nb * sizeof(LuxGrainBandCell));
      if (e->line_pending_seq == seq) { /* not torn — commit */
        /* Edge = per-band luminance RISE vs the previously latched line
         * (consumer-side: only the ring owner sees the history). */
        if (e->ring_count > 0) {
          int prev = e->ring_write - 1;
          if (prev < 0)
            prev += LUXGRAIN_MAX_SPREAD;
          const LuxGrainBandCell *pc = e->ring[prev];
          for (int b2 = 0; b2 < nb; b2++) {
            float d = snap[b2].value - pc[b2].value;
            snap[b2].edge = d > 0.0f ? (d > 1.0f ? 1.0f : d) : 0.0f;
          }
        }
        memcpy(e->ring[e->ring_write], snap,
               (size_t)nb * sizeof(LuxGrainBandCell));
        e->ring_write = (e->ring_write + 1) % LUXGRAIN_MAX_SPREAD;
        if (e->ring_count < LUXGRAIN_MAX_SPREAD)
          e->ring_count++;
        e->line_applied_seq = seq;
        if (npx > 0 && npx != e->axis_pixels)
          lg_rebuild_axis(e, npx);
        /* Mix the frame sequence into the RNG: same feed = same cloud. */
        e->rng ^= fseq * 0x9e3779b9u;
        if (!e->rng)
          e->rng = e->config.seed;
      }
    } else {
      e->line_applied_seq = seq; /* malformed push — drop it */
    }
  }
}

void luxgrain_engine_process(LuxGrainEngine *e, float *out_l, float *out_r,
                             int num_samples) {
  if (!e || !e->initialized || !out_l || !out_r || num_samples <= 0)
    return;
  if (num_samples > LUXGRAIN_MAX_BUFFER_SIZE)
    num_samples = LUXGRAIN_MAX_BUFFER_SIZE;

  memset(out_l, 0, (size_t)num_samples * sizeof(float));
  memset(out_r, 0, (size_t)num_samples * sizeof(float));

  lg_latch_pending(e);
  lg_schedule_block(e, num_samples);

  for (int i = 0; i < LUXGRAIN_MAX_GRAINS; i++) {
    LuxGrainVoice *g = &e->grains[i];
    if (!g->active)
      continue;

    int n = 0;
    if (g->delay >= (uint32_t)num_samples) {
      g->delay -= (uint32_t)num_samples;
      continue;
    }
    n = (int)g->delay;
    g->delay = 0;

    int todo = num_samples - n;
    if ((uint32_t)todo > g->remaining)
      todo = (int)g->remaining;

    float *l = out_l + n, *r = out_r + n;
    if (g->smp) {
      /* SAMPLE material — linear-interp read, transposed by smp_inc. */
      for (int s = 0; s < todo; s++) {
        float env = lg_env(g);
        const int i0 = (int)g->smp_pos;
        float smp = 0.0f;
        if (i0 + 1 < g->smp_len) {
          const float fr = g->smp_pos - (float)i0;
          smp = (g->smp[i0] + fr * (g->smp[i0 + 1] - g->smp[i0])) * env;
        }
        l[s] += smp * g->amp_l;
        r[s] += smp * g->amp_r;
        g->smp_pos += g->smp_inc;
        g->env_pos += g->env_inc;
      }
    } else {
      for (int s = 0; s < todo; s++) {
        float env = lg_env(g);
        float smp = lg_sin01(g->phase) * env;
        l[s] += smp * g->amp_l;
        r[s] += smp * g->amp_r;
        g->phase += g->phase_inc;
        if (g->phase >= 1.0f)
          g->phase -= 1.0f;
        g->env_pos += g->env_inc;
      }
    }
    g->remaining -= (uint32_t)todo;
    if (g->remaining == 0) {
      g->active = 0;
      e->active_grains--;
    }
  }
}

/* ============================================================================
 * SAMPLE material (message thread only — never called from RT)
 * ========================================================================== */

/* Compact NSDF fundamental detector: loudest 4096-sample window, normalised
 * square-difference over 40..1200 Hz lags, highest peak above 0.5 wins.
 * Same family as the LuxStral wavetable detector, self-contained. */
static float lg_detect_root(const float *x, int n, float sr) {
  const int W = 4096;
  if (n < W || sr <= 0.0f)
    return -1.0f;

  /* Loudest window (hop W/2). */
  int best_at = 0;
  float best_e = -1.0f;
  for (int at = 0; at + W <= n; at += W / 2) {
    float acc = 0.0f;
    for (int i = 0; i < W; i += 4) /* stride 4: energy estimate only */
      acc += x[at + i] * x[at + i];
    if (acc > best_e) {
      best_e = acc;
      best_at = at;
    }
  }
  const float *w = x + best_at;

  const int lag_min = (int)(sr / 1200.0f);
  int lag_max = (int)(sr / 40.0f);
  if (lag_max > W / 2)
    lag_max = W / 2;
  if (lag_min < 2 || lag_min >= lag_max)
    return -1.0f;

  /* MPM two-pass: full NSDF curve, then the FIRST local peak above 90% of
   * the global max (avoids octave-down locks). Message thread only. */
  static float s_nsdf[2049];
  if (lag_max - lag_min + 1 > (int)(sizeof(s_nsdf) / sizeof(float)))
    lag_max = lag_min + (int)(sizeof(s_nsdf) / sizeof(float)) - 1;

  float maxv = 0.0f;
  for (int lag = lag_min; lag <= lag_max; lag++) {
    float ac = 0.0f, m = 0.0f;
    for (int i = 0; i + lag < W; i++) {
      ac += w[i] * w[i + lag];
      m += w[i] * w[i] + w[i + lag] * w[i + lag];
    }
    const float nsdf = m > 1e-12f ? 2.0f * ac / m : 0.0f;
    s_nsdf[lag - lag_min] = nsdf;
    if (nsdf > maxv)
      maxv = nsdf;
  }
  if (maxv < 0.5f)
    return -1.0f;

  const float thr = 0.9f * maxv;
  for (int lag = lag_min + 1; lag < lag_max; lag++) {
    const float v = s_nsdf[lag - lag_min];
    if (v >= thr && v >= s_nsdf[lag - lag_min - 1] &&
        v >= s_nsdf[lag - lag_min + 1])
      return sr / (float)lag;
  }
  return -1.0f;
}

int luxgrain_engine_set_sample(LuxGrainEngine *e, const float *mono,
                               int num_samples, float sample_rate,
                               float root_hz) {
  if (!e || !e->initialized || !mono || num_samples < 512 ||
      sample_rate <= 0.0f)
    return -1;
  if (num_samples > LUXGRAIN_SAMPLE_MAX)
    num_samples = LUXGRAIN_SAMPLE_MAX;

  if (root_hz <= 0.0f) {
    root_hz = lg_detect_root(mono, num_samples, sample_rate);
    if (root_hz <= 0.0f)
      return -1; /* no stable pitch — caller may retry with an override */
  }

  const int bank = (e->sample_active == 0) ? 1 : 0; /* fill the inactive one */
  memcpy(e->sample_data[bank], mono, (size_t)num_samples * sizeof(float));

  /* RMS-normalise toward the sine material's level (1/√2), bounded. */
  double acc = 0.0;
  for (int i = 0; i < num_samples; i++)
    acc += (double)mono[i] * (double)mono[i];
  const float rms = (float)sqrt(acc / (double)num_samples);
  float gain = rms > 1e-5f ? 0.70710678f / rms : 1.0f;
  if (gain > 8.0f)
    gain = 8.0f;
  else if (gain < 0.05f)
    gain = 0.05f;

  e->sample_len[bank] = num_samples;
  e->sample_root_hz[bank] = root_hz;
  e->sample_srate[bank] = sample_rate;
  e->sample_gain[bank] = gain;
  __atomic_store_n(&e->sample_active, bank, __ATOMIC_RELEASE);
  return 0;
}

void luxgrain_engine_clear_sample(LuxGrainEngine *e) {
  if (!e)
    return;
  __atomic_store_n(&e->sample_active, -1, __ATOMIC_RELEASE);
}

int luxgrain_engine_sample_info(const LuxGrainEngine *e, float *root_hz_out,
                                float *duration_s_out) {
  if (!e || !e->initialized)
    return 0;
  const int bank = e->sample_active;
  if (bank < 0)
    return 0;
  if (root_hz_out)
    *root_hz_out = e->sample_root_hz[bank];
  if (duration_s_out)
    *duration_s_out = e->sample_srate[bank] > 0.0f
                          ? (float)e->sample_len[bank] / e->sample_srate[bank]
                          : 0.0f;
  return 1;
}
