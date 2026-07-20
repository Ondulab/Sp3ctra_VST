/*
 * luxstral_wavetable.h
 *
 * User-sample timbre wavetable for the LuxStral additive bank ("tuned grains").
 *
 * A sample is loaded once and RETAINED (non-RT): fundamental detection (NSDF)
 * at its loudest window, then a scannable extraction point — one averaged
 * cycle is taken AT THE CURRENT POSITION in the file, projected onto
 * LUXSTRAL_WT_MAX_HARMONICS harmonic coefficients and resynthesized into
 * LUXSTRAL_WT_LEVELS band-limited mipmap tables. Moving the position re-runs
 * the extraction (a few ms — trig-recurrence projection, no libm in the
 * loops) and republishes, so scanning the file morphs the bank's timbre in
 * real time while the IMAGE keeps driving melody/rhythm.
 *
 * Every extraction is RMS-normalized to the sine table's RMS (1/√2): a quiet
 * passage yields the same loudness as a loud one — only the harmonic color
 * changes — and the dB decode law / auto-calibrated phase gate keep their
 * calibration. A near-silent window refuses to publish (the previous timbre
 * holds) instead of collapsing to noise.
 *
 * The tables share the oscillators' phase-accumulator domain (phase_acc ∈
 * [0, SINE_TABLE_SIZE)): the RT gather scales the phase by
 * LUXSTRAL_WT_TABLE_SIZE / SINE_TABLE_SIZE.
 *
 * Publication is a lock-free round-robin over LUXSTRAL_WT_SLOTS slots: builds
 * go to the next slot, a single release-store flips the active pointer. RT
 * workers acquire the pointer once per pass (one audio buffer, ms). With 4
 * slots and scan updates throttled by the 30 ms UI drain, a slot is reused
 * ≥ 90 ms after retirement — an order of magnitude above any RT pass.
 *
 * All mutating calls (load / set_position / clear / load_from_harmonics) are
 * message-thread only.
 *
 * Author: zhonx
 */

#ifndef __LUXSTRAL_WAVETABLE_H__
#define __LUXSTRAL_WAVETABLE_H__

#include <stdint.h>
#include <math.h>   /* log2f in the RT inline helpers */

#ifdef __cplusplus
extern "C" {
#endif

/* Table length per mip level. 2× the shared sine table: with harmonics capped
 * at LUXSTRAL_WT_MAX_HARMONICS = TABLE_SIZE/8, linear interpolation noise
 * stays well below the additive bank's summed noise floor.                   */
#define LUXSTRAL_WT_TABLE_SIZE     2048
#define LUXSTRAL_WT_TABLE_MASK     (LUXSTRAL_WT_TABLE_SIZE - 1)

/* Mip levels: level L holds (LUXSTRAL_WT_MAX_HARMONICS >> L) harmonics.
 * 9 levels ⇒ 256, 128, 64, 32, 16, 8, 4, 2, 1 — level 8 is a pure sine.     */
#define LUXSTRAL_WT_LEVELS         9
#define LUXSTRAL_WT_MAX_HARMONICS  256

/* Round-robin publish slots (see reuse contract above).                      */
#define LUXSTRAL_WT_SLOTS          4

#define LUXSTRAL_WT_NAME_MAX       128

/* Fixed-size min/max waveform overview for the UI position strip.            */
#define LUXSTRAL_WT_OVERVIEW_PAIRS 512

/* Spectral-envelope follower ("formants"): at every extraction the sample's
 * magnitude envelope around the scan position is measured on a fixed
 * log-frequency axis (constant-Q probes, Q≈4, smoothed) and max-normalized
 * to 1 (attenuation-only filter). The RT workers weight each note by
 * env(note_freq) × depth — the vocoder-like color that makes the source
 * recognizable, independent of the waveform mix.                             */
#define LUXSTRAL_WT_ENV_POINTS     96
#define LUXSTRAL_WT_ENV_FMIN       30.0f
#define LUXSTRAL_WT_ENV_FMAX       16000.0f
/* log2(LUXSTRAL_WT_ENV_FMAX / LUXSTRAL_WT_ENV_FMIN) — axis span             */
#define LUXSTRAL_WT_ENV_LOG_SPAN   9.058894f
/* Envelope floor (−60 dB): fully rejected bands attenuate, never mute-lock. */
#define LUXSTRAL_WT_ENV_FLOOR      0.001f

typedef struct {
  /* Band-limited resynthesis tables, one per mip level (see above).         */
  float mips[LUXSTRAL_WT_LEVELS][LUXSTRAL_WT_TABLE_SIZE];
  /* Harmonic coefficients the tables were built from (cos/sin, harmonic k at
   * index k-1). Kept for exact session persistence — ~2 KB vs the source
   * file's megabytes, and survives a moved/deleted WAV.                     */
  float harm_re[LUXSTRAL_WT_MAX_HARMONICS];
  float harm_im[LUXSTRAL_WT_MAX_HARMONICS];
  int   num_harmonics;
  /* Spectral envelope at the extraction position (log axis, max-norm ≤ 1).  */
  float env[LUXSTRAL_WT_ENV_POINTS];
  float root_hz;      /* detected (or overridden) fundamental                */
  float confidence;   /* NSDF peak value ∈ [0,1]; 1.0 when root was forced   */
  char  source_name[LUXSTRAL_WT_NAME_MAX];
} luxstral_wavetable_t;

/**
 * @brief Load and retain a mono sample; publish the timbre at the current
 *        scan position.
 *
 * Non-RT (message thread). The fundamental is detected at the file's loudest
 * window (or forced by root_hz_override) and stays FIXED while scanning. The
 * cycle is extracted at the current position (luxstral_wavetable_set_position);
 * if that window is silent the loudest window is used as a fallback so a
 * fresh load always publishes.
 *
 * @param mono             Mono samples, any level (normalized per extraction)
 * @param num_samples      Sample count (≥ 512)
 * @param sample_rate      Source sample rate in Hz
 * @param root_hz_override > 0 forces the fundamental (skips detection)
 * @param source_name      Display name persisted with the table (may be NULL)
 * @return 0 on success, -1 on invalid input / no periodicity found
 */
int luxstral_wavetable_load(const float *mono, int num_samples,
                            float sample_rate, float root_hz_override,
                            const char *source_name);

/**
 * @brief Move the scan position (0..1 across the retained file) and republish.
 *
 * Message thread, a few ms — callers should coalesce (the APVTS drain does).
 * The extraction is renormalized AT the new position: quiet material plays as
 * loud as strong material, only the color changes.
 *
 * @return 0 published, -1 no retained sample or window too silent (previous
 *         timbre kept).
 */
int luxstral_wavetable_set_position(float pos01);

/** @brief Current scan position (0..1). */
float luxstral_wavetable_get_position(void);

/** @brief Duration of the retained sample in seconds (0 = none). */
float luxstral_wavetable_get_duration_s(void);

/** @brief 1 if a source sample is retained (scanning available). */
int luxstral_wavetable_has_sample(void);

/**
 * @brief Copy the active table's spectral envelope (for persistence).
 * @return 1 if a table is loaded (env96 filled), 0 otherwise.
 */
int luxstral_wavetable_get_env(float *env96);

/**
 * @brief Copy the min/max overview of the retained sample for UI drawing.
 * @param minmax Interleaved [min,max] pairs, LUXSTRAL_WT_OVERVIEW_PAIRS of them.
 * @return 1 if a sample is retained (buffer filled), 0 otherwise.
 */
int luxstral_wavetable_get_overview(float *minmax);

/**
 * @brief Rebuild and publish the wavetable from persisted harmonics.
 *
 * Session-restore fallback when the source file is gone — static timbre,
 * no scanning (has_sample stays 0). Bit-exact rebuild of the mip tables.
 * @param env96 Persisted spectral envelope (LUXSTRAL_WT_ENV_POINTS floats),
 *              or NULL for a flat envelope (no formant filtering).
 * @return 0 on success, -1 on invalid input.
 */
int luxstral_wavetable_load_from_harmonics(const float *harm_re,
                                           const float *harm_im,
                                           int num_harmonics, float root_hz,
                                           const float *env96,
                                           const char *source_name);

/** @brief Unpublish and drop the retained sample (bank back to sine/square). */
void luxstral_wavetable_clear(void);

/**
 * @brief RT accessor — the currently published wavetable, or NULL.
 *
 * Acquire-load of the active pointer. Call ONCE per worker pass and reuse the
 * returned pointer for the whole pass (see round-robin contract above).
 */
const luxstral_wavetable_t *luxstral_wavetable_acquire(void);

/** @brief Non-RT convenience: 1 if a wavetable is currently published. */
int luxstral_wavetable_is_loaded(void);

/**
 * @brief Snapshot the active table's metadata (message thread, for UI/state).
 * @return 1 if a table is loaded (outputs filled), 0 otherwise.
 */
int luxstral_wavetable_get_info(char *name_out, int name_cap,
                                float *root_hz_out, float *confidence_out);

/**
 * @brief Copy the active table's harmonic coefficients (for persistence).
 * @return number of harmonics copied (0 = no table loaded).
 */
int luxstral_wavetable_get_harmonics(float *harm_re_out, float *harm_im_out,
                                     int max_harmonics, float *root_hz_out);

/**
 * @brief Pick the mip level for an oscillator from its phase increment.
 *
 * phase_inc lives in the shared sine-table domain: table Nyquist maps to
 * audio Nyquist, so harmonic h aliases when h × phase_inc ≥ sine_table_size/2.
 * Returns the richest level whose harmonic count fits below that.
 * Pure function, RT-safe (≤ LUXSTRAL_WT_LEVELS iterations).
 */
/**
 * @brief Sample the published spectral envelope at a note's frequency.
 *
 * Log-axis linear interpolation. RT-safe (one log2f + lerp). Returns ≤ 1
 * (max-normalized attenuation) with a −60 dB floor.
 */
static inline float luxstral_wavetable_env_for_freq(
    const luxstral_wavetable_t *wt, float freq_hz)
{
    if (freq_hz < LUXSTRAL_WT_ENV_FMIN) freq_hz = LUXSTRAL_WT_ENV_FMIN;
    if (freq_hz > LUXSTRAL_WT_ENV_FMAX) freq_hz = LUXSTRAL_WT_ENV_FMAX;
    /* x = log-position × (POINTS−1) / log2(FMAX/FMIN)                        */
    const float x = log2f(freq_hz / LUXSTRAL_WT_ENV_FMIN) *
                    ((float)(LUXSTRAL_WT_ENV_POINTS - 1) /
                     LUXSTRAL_WT_ENV_LOG_SPAN);
    const int   i0 = (int)x;
    const float fr = x - (float)i0;
    const int   i1 = (i0 + 1 < LUXSTRAL_WT_ENV_POINTS) ? i0 + 1 : i0;
    return wt->env[i0] + fr * (wt->env[i1] - wt->env[i0]);
}

static inline int luxstral_wavetable_mip_for_phase_inc(float phase_inc,
                                                       int sine_table_size)
{
    const float allowed = (phase_inc > 1e-9f)
                              ? ((float)sine_table_size * 0.5f) / phase_inc
                              : (float)LUXSTRAL_WT_MAX_HARMONICS;
    int level = 0;
    int h     = LUXSTRAL_WT_MAX_HARMONICS;
    while (level < LUXSTRAL_WT_LEVELS - 1 && (float)h > allowed) {
        h >>= 1;
        level++;
    }
    return level;
}

#ifdef __cplusplus
}
#endif

#endif /* __LUXSTRAL_WAVETABLE_H__ */
