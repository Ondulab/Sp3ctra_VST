/*
 * luxstral_wavetable.h
 *
 * User-sample timbre wavetable for the LuxStral additive bank ("tuned grains").
 *
 * A sample is analyzed once (non-RT): fundamental detection (NSDF), projection
 * of one averaged cycle onto LUXSTRAL_WT_MAX_HARMONICS harmonic coefficients,
 * then resynthesis into LUXSTRAL_WT_LEVELS band-limited mipmap tables (one per
 * halving of the harmonic count). Every oscillator of the bank then reads the
 * mip level matching its own frequency, so no harmonic ever crosses Nyquist —
 * the bank stays alias-free at any tuning/range.
 *
 * The tables share the oscillators' phase-accumulator domain (phase_acc ∈
 * [0, SINE_TABLE_SIZE)): the RT gather scales the phase by
 * LUXSTRAL_WT_TABLE_SIZE / SINE_TABLE_SIZE. Cycle RMS is normalized to the
 * sine table's RMS (1/√2) so the dB decode law and the auto-calibrated phase
 * gate keep their calibration whatever the sample.
 *
 * Publication is a lock-free ping-pong: builds go to the inactive slot, then a
 * single release-store flips the active pointer. RT workers acquire the
 * pointer once per pass. A slot is only reused on the second load after it —
 * UI-driven loads are seconds apart while an RT pass lasts one audio buffer,
 * so the retired slot is never still in use when overwritten.
 *
 * Author: zhonx
 */

#ifndef __LUXSTRAL_WAVETABLE_H__
#define __LUXSTRAL_WAVETABLE_H__

#include <stdint.h>

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

#define LUXSTRAL_WT_NAME_MAX       128

typedef struct {
  /* Band-limited resynthesis tables, one per mip level (see above).         */
  float mips[LUXSTRAL_WT_LEVELS][LUXSTRAL_WT_TABLE_SIZE];
  /* Harmonic coefficients the tables were built from (cos/sin, harmonic k at
   * index k-1). Kept for exact session persistence — ~2 KB vs the source
   * file's megabytes, and survives a moved/deleted WAV.                     */
  float harm_re[LUXSTRAL_WT_MAX_HARMONICS];
  float harm_im[LUXSTRAL_WT_MAX_HARMONICS];
  int   num_harmonics;
  float root_hz;      /* detected (or overridden) fundamental                */
  float confidence;   /* NSDF peak value ∈ [0,1]; 1.0 when root was forced   */
  char  source_name[LUXSTRAL_WT_NAME_MAX];
} luxstral_wavetable_t;

/**
 * @brief Analyze a mono sample and publish it as the active timbre wavetable.
 *
 * Non-RT (message thread). Costs ~tens of ms for the harmonic projection.
 *
 * @param mono             Mono samples, any level (normalized internally)
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
 * @brief Rebuild and publish the wavetable from persisted harmonics.
 *
 * Session restore path — bit-exact rebuild of the mip tables without the
 * source file. Coefficients must come from luxstral_wavetable_get_harmonics.
 * @return 0 on success, -1 on invalid input.
 */
int luxstral_wavetable_load_from_harmonics(const float *harm_re,
                                           const float *harm_im,
                                           int num_harmonics, float root_hz,
                                           const char *source_name);

/** @brief Unpublish the active wavetable (bank falls back to sine/square). */
void luxstral_wavetable_clear(void);

/**
 * @brief RT accessor — the currently published wavetable, or NULL.
 *
 * Acquire-load of the active pointer. Call ONCE per worker pass and reuse the
 * returned pointer for the whole pass (see ping-pong contract above).
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
