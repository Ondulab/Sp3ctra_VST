/*
 * luxgrain_offline_test.c
 *
 * Offline harness for the LuxGrain core — no JUCE, no RT, no build-system
 * dependency (this file is NOT in CMakeLists; compile it directly):
 *
 *   cc -O2 -std=c11 -o /tmp/luxgrain_test \
 *      test/luxgrain_offline_test.c synth_luxgrain_engine.c -lm
 *   /tmp/luxgrain_test [output_dir]
 *
 * Simulates the image feed (3456-px conditioned lines at 250 lines/s, the
 * device's push rate), renders reference WAVs for listening, and runs the
 * quantitative PASS/FAIL suite (determinism, pitch anchoring, density law,
 * chord, glissando, spread-freeze, silence contract).
 */

#include "../synth_luxgrain_engine.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#define SR        48000
#define NPX       3456
#define LINE_RATE 250
#define SPL       (SR / LINE_RATE) /* samples per line push = 192 */

/* Axis used by every scenario (harness-side mirror of the engine config). */
#define AXIS_LOW  65.406f
#define AXIS_OCT  6

static float px_of_hz(float hz) {
  return (float)NPX / (float)AXIS_OCT * log2f(hz / AXIS_LOW);
}
static float hz_of_px(float px) {
  return AXIS_LOW * exp2f((float)AXIS_OCT * px / (float)NPX);
}

/* ── tiny deterministic RNG for scenario noise ─────────────────────────── */
static uint32_t h_rng = 0xC0FFEEu;
static float h_frand(void) {
  h_rng ^= h_rng << 13; h_rng ^= h_rng >> 17; h_rng ^= h_rng << 5;
  return (float)(h_rng >> 8) * (1.0f / 16777216.0f);
}

/* ── WAV writer (16-bit PCM stereo) ─────────────────────────────────────── */
static void write_wav(const char *path, const float *l, const float *r,
                      long n) {
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
  long data_bytes = n * 2 * 2;
  uint32_t u32; uint16_t u16;
  fwrite("RIFF", 1, 4, f); u32 = (uint32_t)(36 + data_bytes);
  fwrite(&u32, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  u32 = 16; fwrite(&u32, 4, 1, f);
  u16 = 1;  fwrite(&u16, 2, 1, f);            /* PCM        */
  u16 = 2;  fwrite(&u16, 2, 1, f);            /* stereo     */
  u32 = SR; fwrite(&u32, 4, 1, f);
  u32 = SR * 4; fwrite(&u32, 4, 1, f);        /* byte rate  */
  u16 = 4;  fwrite(&u16, 2, 1, f);            /* block align*/
  u16 = 16; fwrite(&u16, 2, 1, f);
  fwrite("data", 1, 4, f); u32 = (uint32_t)data_bytes; fwrite(&u32, 4, 1, f);
  for (long i = 0; i < n; i++) {
    float cl = l[i] > 1.f ? 1.f : (l[i] < -1.f ? -1.f : l[i]);
    float cr = r[i] > 1.f ? 1.f : (r[i] < -1.f ? -1.f : r[i]);
    int16_t s[2] = { (int16_t)(cl * 32767.f), (int16_t)(cr * 32767.f) };
    fwrite(s, 2, 2, f);
  }
  fclose(f);
}

/* ── analysis helpers ───────────────────────────────────────────────────── */
static double rms(const float *l, const float *r, long from, long to) {
  double acc = 0.0;
  for (long i = from; i < to; i++) {
    double m = 0.5 * ((double)l[i] + (double)r[i]);
    acc += m * m;
  }
  return sqrt(acc / (double)(to - from));
}

/* Banded energy at `hz`: short-window Goertzel (4096 ≈ 12 Hz bandwidth —
 * wide enough to catch per-grain jitter/centroid spread), summed over the
 * range. Normalised per window so lengths compare. */
static double band_energy(const float *l, const float *r, long from, long to,
                          float hz) {
  const long W = 4096, H = 2048;
  double k = 6.283185307179586 * (double)hz / (double)SR;
  double coef = 2.0 * cos(k);
  double total = 0.0;
  long wins = 0;
  for (long s = from; s + W <= to; s += H) {
    double q0, q1 = 0.0, q2 = 0.0;
    for (long i = 0; i < W; i++) {
      double x = 0.5 * ((double)l[s + i] + (double)r[s + i]);
      q0 = coef * q1 - q2 + x;
      q2 = q1; q1 = q0;
    }
    total += (q1 * q1 + q2 * q2 - coef * q1 * q2) / (double)W;
    wins++;
  }
  return wins ? total / (double)wins : 0.0;
}

static double peak_abs(const float *l, const float *r, long n) {
  double p = 0.0;
  for (long i = 0; i < n; i++) {
    double a = fabs((double)l[i]); if (a > p) p = a;
    a = fabs((double)r[i]);        if (a > p) p = a;
  }
  return p;
}

/* ── line generators ────────────────────────────────────────────────────── */
typedef void (*line_fn)(float *line, int line_idx);

static void gauss_band(float *line, float centre_px, float sigma, float amp) {
  for (int p = 0; p < NPX; p++) {
    float d = ((float)p - centre_px) / sigma;
    line[p] += amp * expf(-0.5f * d * d);
  }
}

static void ln_black(float *l, int i)  { (void)i; memset(l, 0, NPX * 4); }
static void ln_220(float *l, int i)    { (void)i; ln_black(l, 0); gauss_band(l, px_of_hz(220.f), 25.f, 0.9f); }
static void ln_ramp(float *l, int i)   { float v = 0.8f * (float)i / (6.f * LINE_RATE); for (int p = 0; p < NPX; p++) l[p] = v; }
static void ln_chord(float *l, int i)  { (void)i; ln_black(l, 0);
  gauss_band(l, px_of_hz(130.81f), 15.f, 0.9f);
  gauss_band(l, px_of_hz(164.81f), 15.f, 0.9f);
  gauss_band(l, px_of_hz(196.00f), 15.f, 0.9f); }
static void ln_gliss(float *l, int i)  { ln_black(l, 0);
  float c = 500.f + (2200.f - 500.f) * (float)i / (5.f * LINE_RATE);
  gauss_band(l, c, 30.f, 0.9f); }
static void ln_gliss2s(float *l, int i){ ln_black(l, 0);
  float c = 600.f + (2000.f - 600.f) * (float)i / (2.f * LINE_RATE);
  gauss_band(l, c, 60.f, 0.9f); }
static void ln_noise(float *l, int i)  { (void)i; for (int p = 0; p < NPX; p++) l[p] = h_frand() * h_frand(); }
static void ln_flash(float *l, int i) {
  /* Dim flat field; ONE bright line at t = 1 s (edge-burst test). */
  float v = (i == LINE_RATE) ? 0.9f : 0.15f;
  for (int p = 0; p < NPX; p++) l[p] = v;
}
static void ln_smooth_noisy(float *l, int i) {
  /* 0–3 s: wide smooth band (low intra-band contrast → long grains);
   * 3–6 s: same envelope × per-pixel noise (high contrast → short grains). */
  ln_black(l, 0);
  gauss_band(l, px_of_hz(220.f), 180.f, 0.9f);
  if (i >= 3 * LINE_RATE)
    for (int p = 0; p < NPX; p++) l[p] *= h_frand() > 0.5f ? 1.f : 0.05f;
}

/* ── scenario runner ────────────────────────────────────────────────────── */
typedef struct {
  double peak, secs, cpu_x;
  int    max_grains;
} RunStats;

/* Optional scenario extensions (reset to 0 after each run):
 *   h_rgb_mode   0 = no RGB staged; 1 = all-red material; 2 = all-blue
 *   h_sample_hz  > 0 → publish a 2 s sine at that Hz as SAMPLE material
 *                 (root auto-detected — also exercises the NSDF). */
static int   h_rgb_mode = 0;
static float h_sample_hz = 0.0f;

static long run(LuxGrainEngine *e, const LuxGrainConfig *cfg, line_fn fn,
                float seconds, float push_seconds, float *out_l, float *out_r,
                RunStats *st) {
  luxgrain_engine_init(e, (float)SR);
  luxgrain_engine_set_config(e, cfg);
  h_rng = 0xC0FFEEu; /* scenario noise is deterministic too */

  if (h_sample_hz > 0.0f) {
    static float smp[2 * SR];
    for (int i = 0; i < 2 * SR; i++)
      smp[i] = 0.5f * sinf(6.2831853f * h_sample_hz * (float)i / (float)SR);
    if (luxgrain_engine_set_sample(e, smp, 2 * SR, (float)SR, 0.0f) != 0)
      fprintf(stderr, "set_sample FAILED (root not detected)\n");
  }

  static float line[NPX];
  static uint8_t hr[NPX], hg[NPX], hb[NPX];
  long total = (long)(seconds * SR);
  long done = 0;
  int line_idx = 0, maxg = 0;
  clock_t t0 = clock();
  while (done < total) {
    if ((float)line_idx / LINE_RATE < push_seconds) {
      fn(line, line_idx);
      if (h_rgb_mode) {
        for (int p = 0; p < NPX; p++) {
          uint8_t v = (uint8_t)(line[p] * 255.0f);
          hr[p] = h_rgb_mode == 1 ? v : 0;
          hb[p] = h_rgb_mode == 2 ? v : 0;
          hg[p] = 0;
        }
        luxgrain_engine_stage_line(e, line, hr, hg, hb, NPX,
                                   (uint32_t)line_idx);
      } else {
        luxgrain_engine_stage_line(e, line, 0, 0, 0, NPX,
                                   (uint32_t)line_idx);
      }
    }
    line_idx++;
    int n = SPL;
    if (done + n > total) n = (int)(total - done);
    luxgrain_engine_process(e, out_l + done, out_r + done, n);
    if (e->active_grains > maxg) maxg = e->active_grains;
    done += n;
  }
  if (st) {
    st->secs = seconds;
    st->cpu_x = ((double)(clock() - t0) / CLOCKS_PER_SEC) / seconds;
    st->peak = peak_abs(out_l, out_r, total);
    st->max_grains = maxg;
  }
  return total;
}

/* ── main ───────────────────────────────────────────────────────────────── */
static int n_pass = 0, n_fail = 0;
static void check(const char *name, int ok, const char *detail) {
  printf("  [%s] %-28s %s\n", ok ? "PASS" : "FAIL", name, detail);
  if (ok) n_pass++; else n_fail++;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "luxgrain_wavs";
  mkdir(dir, 0755);
  char path[512];

  static LuxGrainEngine eng;
  const long MAXS = 8L * SR;
  float *L  = malloc(MAXS * 4), *R  = malloc(MAXS * 4);
  float *L2 = malloc(MAXS * 4), *R2 = malloc(MAXS * 4);
  if (!L || !R || !L2 || !R2) return 1;
  RunStats st;

  LuxGrainConfig base = luxgrain_config_default();
  base.master_volume = 0.35f;
  base.pitch_jitter_st = 0.10f;

  printf("LuxGrain offline harness — %d px lines @ %d Hz, SR %d\n\n",
         NPX, LINE_RATE, SR);

  /* 1 ── single band + determinism + pitch anchoring ─────────────────── */
  long n = run(&eng, &base, ln_220, 4.f, 4.f, L, R, &st);
  snprintf(path, sizeof path, "%s/01_single_band_220Hz.wav", dir);
  write_wav(path, L, R, n);
  printf("01 single_band   peak %.3f  grains<=%d  cpu %.1f%%\n",
         st.peak, st.max_grains, st.cpu_x * 100);

  run(&eng, &base, ln_220, 4.f, 4.f, L2, R2, NULL);
  check("determinism", !memcmp(L, L2, n * 4) && !memcmp(R, R2, n * 4),
        "same seed => bit-identical");

  double e220 = band_energy(L, R, SR / 2, n, 220.f);
  double e138 = band_energy(L, R, SR / 2, n, 138.f);
  double e311 = band_energy(L, R, SR / 2, n, 311.f);
  char d[128];
  snprintf(d, sizeof d, "E220/E138=%.0f E220/E311=%.0f",
           e220 / (e138 + 1e-12), e220 / (e311 + 1e-12));
  check("pitch anchoring 220Hz", e220 > 5 * e138 && e220 > 5 * e311, d);

  /* 2 ── density law: luminance ramp => rising RMS ───────────────────── */
  LuxGrainConfig c = base;
  c.density_hz = 2.0f; c.dur_max_ms = 60.f; c.master_volume = 0.15f;
  c.amp_follow = 1.0f;
  n = run(&eng, &c, ln_ramp, 6.f, 6.f, L, R, &st);
  snprintf(path, sizeof path, "%s/02_density_ramp.wav", dir);
  write_wav(path, L, R, n);
  printf("02 density_ramp  peak %.3f  grains<=%d  cpu %.1f%%\n",
         st.peak, st.max_grains, st.cpu_x * 100);
  double r_early = rms(L, R, (long)(0.75f * SR), (long)(1.5f * SR));
  double r_late  = rms(L, R, (long)(5.25f * SR), n);
  snprintf(d, sizeof d, "rms %.4f -> %.4f (x%.1f)", r_early, r_late,
           r_late / (r_early + 1e-12));
  check("density follows luminance", r_late > 3.0 * r_early, d);

  /* 3 ── chord: three bands => three pitches ─────────────────────────── */
  n = run(&eng, &base, ln_chord, 4.f, 4.f, L, R, &st);
  snprintf(path, sizeof path, "%s/03_chord_CEG.wav", dir);
  write_wav(path, L, R, n);
  printf("03 chord_CEG     peak %.3f  grains<=%d  cpu %.1f%%\n",
         st.peak, st.max_grains, st.cpu_x * 100);
  double eC = band_energy(L, R, SR / 2, n, 130.81f);
  double eE = band_energy(L, R, SR / 2, n, 164.81f);
  double eG = band_energy(L, R, SR / 2, n, 196.f);
  double eX = band_energy(L, R, SR / 2, n, 233.08f);
  snprintf(d, sizeof d, "C/off=%.0f E/off=%.0f G/off=%.0f",
           eC / (eX + 1e-12), eE / (eX + 1e-12), eG / (eX + 1e-12));
  check("chord C-E-G", eC > 5 * eX && eE > 5 * eX && eG > 5 * eX, d);

  /* 4 ── glissando: cloud follows the moving band ────────────────────── */
  n = run(&eng, &base, ln_gliss, 5.f, 5.f, L, R, &st);
  snprintf(path, sizeof path, "%s/04_glissando.wav", dir);
  write_wav(path, L, R, n);
  printf("04 glissando     peak %.3f  grains<=%d  cpu %.1f%%\n",
         st.peak, st.max_grains, st.cpu_x * 100);
  float f_lo = hz_of_px(650.f), f_hi = hz_of_px(2050.f);
  double lo_first = band_energy(L, R, 0, SR, f_lo);
  double hi_first = band_energy(L, R, 0, SR, f_hi);
  double lo_last  = band_energy(L, R, n - SR, n, f_lo);
  double hi_last  = band_energy(L, R, n - SR, n, f_hi);
  snprintf(d, sizeof d, "start lo/hi=%.0f end hi/lo=%.0f",
           lo_first / (hi_first + 1e-12), hi_last / (lo_last + 1e-12));
  check("glissando tracks", lo_first > 3 * hi_first && hi_last > 3 * lo_last,
        d);

  /* 5 ── spread freeze: history survives a stopped feed ──────────────── */
  c = base;
  c.spread_lines = 500.f; c.density_hz = 12.f; c.dur_max_ms = 400.f;
  c.master_volume = 0.15f;
  n = run(&eng, &c, ln_gliss2s, 5.f, 2.f, L, R, &st); /* push stops at 2 s */
  snprintf(path, sizeof path, "%s/05_freeze_spread.wav", dir);
  write_wav(path, L, R, n);
  printf("05 freeze_spread peak %.3f  grains<=%d  cpu %.1f%%\n",
         st.peak, st.max_grains, st.cpu_x * 100);
  LuxGrainConfig c1 = c; c1.spread_lines = 1.f;
  run(&eng, &c1, ln_gliss2s, 5.f, 2.f, L2, R2, NULL);
  float f_early = hz_of_px(800.f); /* bright only EARLY in the glissando */
  double frozen_spread = band_energy(L,  R,  (long)(2.5f * SR), n, f_early);
  double frozen_narrow = band_energy(L2, R2, (long)(2.5f * SR), n, f_early);
  snprintf(d, sizeof d, "E(spread500)/E(spread1)=%.0f",
           frozen_spread / (frozen_narrow + 1e-12));
  check("freeze keeps history", frozen_spread > 10 * frozen_narrow, d);

  /* 6 ── silence contract ────────────────────────────────────────────── */
  n = run(&eng, &base, ln_black, 2.f, 2.f, L, R, NULL);
  double r_sil = rms(L, R, 0, n);
  c = base; c.enabled = 0;
  run(&eng, &c, ln_220, 2.f, 2.f, L2, R2, NULL);
  double r_off = rms(L2, R2, 0, n);
  snprintf(d, sizeof d, "black rms=%.2e, disabled rms=%.2e", r_sil, r_off);
  check("silence contract", r_sil < 1e-6 && r_off < 1e-9, d);

  /* 7 ── colour → pan: all-red band left, all-blue band right ────────── */
  c = base;
  c.stereo_width = 0.0f; /* isolate the deterministic colour part */
  h_rgb_mode = 1;        /* red material → LEFT */
  n = run(&eng, &c, ln_220, 3.f, 3.f, L, R, &st);
  double rmsL_red = rms(L, L, SR / 2, n);   /* mono-channel RMS via (L+L)/2 */
  double rmsR_red = rms(R, R, SR / 2, n);
  h_rgb_mode = 2;        /* blue material → RIGHT */
  run(&eng, &c, ln_220, 3.f, 3.f, L2, R2, NULL);
  double rmsL_blu = rms(L2, L2, SR / 2, n);
  double rmsR_blu = rms(R2, R2, SR / 2, n);
  h_rgb_mode = 0;
  snprintf(d, sizeof d, "red L/R=%.1f blue R/L=%.1f",
           rmsL_red / (rmsR_red + 1e-12), rmsR_blu / (rmsL_blu + 1e-12));
  check("colour drives pan", rmsL_red > 5 * rmsR_red && rmsR_blu > 5 * rmsL_blu,
        d);

  /* 8 ── SAMPLE material: 300 Hz sine file, NSDF root, 220 Hz cloud ──── */
  c = base;
  c.material = LUXGRAIN_MAT_SAMPLE;
  c.scrub = 0.2f;
  h_sample_hz = 300.0f;
  n = run(&eng, &c, ln_220, 4.f, 4.f, L, R, &st);
  h_sample_hz = 0.0f;
  float root = 0.f, dur_s = 0.f;
  int has_smp = luxgrain_engine_sample_info(&eng, &root, &dur_s);
  double eS220 = band_energy(L, R, SR / 2, n, 220.f);
  double eS138 = band_energy(L, R, SR / 2, n, 138.f);
  double eS311 = band_energy(L, R, SR / 2, n, 311.f);
  snprintf(d, sizeof d, "root=%.1fHz E220/off=%.0f/%.0f", root,
           eS220 / (eS138 + 1e-12), eS220 / (eS311 + 1e-12));
  check("sample material tuned",
        has_smp && fabsf(root - 300.f) < 9.f /* ±3% */
            && eS220 > 5 * eS138 && eS220 > 5 * eS311,
        d);
  snprintf(path, sizeof path, "%s/09_sample_material.wav", dir);
  write_wav(path, L, R, n);
  printf("09 sample_mat    peak %.3f  grains<=%d  root %.1f Hz\n",
         st.peak, st.max_grains, root);

  /* 9 ── edge burst: one bright flash line → emission spike ──────────── */
  c = base;
  c.density_hz = 3.0f;
  c.edge_amount = 1.0f;
  n = run(&eng, &c, ln_flash, 2.f, 2.f, L, R, &st);
  LuxGrainConfig c0 = c;
  c0.edge_amount = 0.0f;
  run(&eng, &c0, ln_flash, 2.f, 2.f, L2, R2, NULL);
  double burst_on  = rms(L, R, (long)(1.0f * SR), (long)(1.3f * SR));
  double burst_off = rms(L2, R2, (long)(1.0f * SR), (long)(1.3f * SR));
  snprintf(d, sizeof d, "rms(flash) on/off=%.1f",
           burst_on / (burst_off + 1e-12));
  check("edge fires a burst", burst_on > 2.0 * burst_off, d);
  snprintf(path, sizeof path, "%s/10_edge_burst.wav", dir);
  write_wav(path, L, R, n);

  /* ── listening-only references ─────────────────────────────────────── */
  c = base; c.density_hz = 3.f; c.dur_min_ms = 15.f; c.dur_max_ms = 350.f;
  const char *env_names[] = { "hann", "tukey", "expodec", "rexpodec" };
  for (int envs = 0; envs < LUXGRAIN_NUM_ENVS; envs++) {
    c.env_shape = envs;
    n = run(&eng, &c, ln_220, 3.f, 3.f, L, R, &st);
    snprintf(path, sizeof path, "%s/07_env_%s.wav", dir, env_names[envs]);
    write_wav(path, L, R, n);
  }
  printf("07 env_shapes    4 wavs (hann/tukey/expodec/rexpodec)\n");

  c = base; c.master_volume = 0.18f;
  n = run(&eng, &c, ln_smooth_noisy, 6.f, 6.f, L, R, &st);
  snprintf(path, sizeof path, "%s/06_texture_smooth_vs_noisy.wav", dir);
  write_wav(path, L, R, n);
  printf("06 texture       peak %.3f  grains<=%d  cpu %.1f%%\n",
         st.peak, st.max_grains, st.cpu_x * 100);

  c = base; c.master_volume = 0.30f; c.density_hz = 4.f;
  n = run(&eng, &c, ln_noise, 4.f, 4.f, L, R, &st);
  snprintf(path, sizeof path, "%s/08_noise_field.wav", dir);
  write_wav(path, L, R, n);
  printf("08 noise_field   peak %.3f  grains<=%d  cpu %.1f%%\n",
         st.peak, st.max_grains, st.cpu_x * 100);

  printf("\nLuxGrain offline: %d/%d PASS — wavs in %s/\n", n_pass,
         n_pass + n_fail, dir);
  free(L); free(R); free(L2); free(R2);
  return n_fail ? 1 : 0;
}
