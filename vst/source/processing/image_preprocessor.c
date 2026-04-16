/*
 * image_preprocessor.c
 *
 * Implementation of image preprocessing module for Sp3ctra
 *
 * Author: zhonx
 * Created: 2025-10-29
 */

#include "image_preprocessor.h"
#include "config/config_instrument.h"
#include "../synthesis/luxstral/synth_luxstral_stereo.h"
#include "../synthesis/luxstral/synth_luxstral_math.h"
#include "config/config_loader.h"
#include "utils/logger.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>

#ifndef DISABLE_LUXSYNTH
#include "synthesis/luxsynth/kissfft/kiss_fftr.h"

/* FFT temporal smoothing configuration */
#define FFT_HISTORY_SIZE 5  /* 5ms @ 1kHz - good compromise for bass stability */
#define AMPLITUDE_SMOOTHING_ALPHA 0.1f  /* Exponential smoothing factor */
#define MAX_FFT_BINS 128  /* Must match MAX_MAPPED_OSCILLATORS */

/* Normalization factors from original polyphonic implementation */
#define NORM_FACTOR_BIN0 (881280.0f * 1.1f)
#define NORM_FACTOR_HARMONICS (220320.0f * 2.0f)

/* Normalization factor for color FFT magnitude */
#define NORM_FACTOR_COLOR (220320.0f * 2.0f)  /* Same as harmonics for consistency */

/* Private state for FFT temporal smoothing */
static struct {
    float history[FFT_HISTORY_SIZE][MAX_FFT_BINS];  /* Circular buffer for magnitude history */
    int write_index;  /* Current write position in circular buffer */
    int fill_count;   /* Number of valid frames in history (0 to FFT_HISTORY_SIZE) */
    int initialized;  /* 1 if FFT state is initialized, 0 otherwise */
    kiss_fftr_cfg fft_cfg;  /* KissFFT configuration (lazy init) */
    kiss_fft_scalar *fft_input;  /* FFT input buffer (lazy alloc) */
    kiss_fft_cpx *fft_output;    /* FFT output buffer (lazy alloc) */
} fft_history_state = {0};

/* Private state for color FFT temporal smoothing */
static struct {
    float pan_history[FFT_HISTORY_SIZE][MAX_FFT_BINS];  /* Circular buffer for pan position history */
    int write_index;  /* Current write position in circular buffer */
    int fill_count;   /* Number of valid frames in history (0 to FFT_HISTORY_SIZE) */
    int initialized;  /* 1 if color FFT state is initialized, 0 otherwise */
    kiss_fftr_cfg color_fft_cfg;  /* KissFFT configuration for color (lazy init) */
    kiss_fft_scalar *color_fft_input;  /* Color FFT input buffer (lazy alloc) */
    kiss_fft_cpx *color_fft_output;    /* Color FFT output buffer (lazy alloc) */
} color_fft_history_state = {0};
#endif

/* Private variables */
static int module_initialized = 0;


/* Private helper function prototypes */
static uint64_t get_timestamp_us(void);
static float calculate_contrast(float *imageData, size_t size);
static void preprocess_stereo(const uint8_t *raw_r, const uint8_t *raw_g,
                               const uint8_t *raw_b, PreprocessedImageData *out);
#ifdef USE_DMX
static void preprocess_dmx(const uint8_t *raw_r, const uint8_t *raw_g,
                            const uint8_t *raw_b, PreprocessedImageData *out);
#endif
#ifndef DISABLE_LUXSYNTH
static int image_preprocess_color_fft(const uint8_t *raw_r, const uint8_t *raw_g,
                                      const uint8_t *raw_b, PreprocessedImageData *data);
#endif

/* Module initialization */
void image_preprocess_init(void) {
    if (module_initialized) {
        return;
    }
    
    log_info("PREPROCESS", "Image preprocessor module initialized");
    module_initialized = 1;
}

/* Module cleanup */
void image_preprocess_cleanup(void) {
    if (!module_initialized) {
        return;
    }
    
    log_info("PREPROCESS", "Image preprocessor module cleaned up");
    module_initialized = 0;
}

/* Main preprocessing function */
int image_preprocess_frame(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    PreprocessedImageData *out
) {
    if (!module_initialized) {
        log_error("PREPROCESS", "Module not initialized");
        return -1;
    }
    
    if (!raw_r || !raw_g || !raw_b || !out) {
        log_error("PREPROCESS", "NULL pointer passed");
        return -1;
    }
    
    /* Get timestamp */
    out->timestamp_us = get_timestamp_us();
    
    /* 1. Preprocess for additive synthesis (with gamma) */
    preprocess_luxstral(raw_r, raw_g, raw_b, out);
    
    /* 1b. StrokeForge: blob detection + harmonic recipe (after additive notes) */
    {
        int sf_num_notes = get_cis_pixels_nb() / g_sp3ctra_config.pixels_per_note;
        if (sf_num_notes > PREPROCESS_MAX_NOTES) sf_num_notes = PREPROCESS_MAX_NOTES;
        strokeforge_analyze_frame(
            out->additive.notes,
            sf_num_notes,
            out->additive.contrast_factor,
            &out->strokeforge);
    }
    
    /* 2. Preprocess for polyphonic synthesis (without gamma) */
#ifndef DISABLE_LUXSYNTH
    preprocess_luxsynth(raw_r, raw_g, raw_b, out);
#endif
    
    /* 3. Preprocess for photowave synthesis (native RGB) */
    preprocess_luxwave(raw_r, raw_g, raw_b, out);
    
    /* 4. Calculate stereo panning data (only if stereo enabled) */
    if (g_sp3ctra_config.stereo_mode_enabled) {
        preprocess_stereo(raw_r, raw_g, raw_b, out);
    }
    
    /* 5. Calculate DMX zone averages (only if DMX enabled) */
#ifdef USE_DMX
    preprocess_dmx(raw_r, raw_g, raw_b, out);
#endif
    
    return 0;
}

/* Private helper functions */

/**
 * @brief Get current timestamp in microseconds
 */
static uint64_t get_timestamp_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/**
 * @brief Calculate contrast of an image by measuring pixel value variance
 * Optimized for performance with sampling
 * Returns a value between contrast_min (low contrast) and 1.0 (high contrast)
 * 
 * CRITICAL: This function must be called on INDIVIDUAL PIXELS, not averaged notes
 * White noise has high pixel-to-pixel variance but low note-to-note variance after averaging
 * 
 * This function was moved from synth_luxstral_stereo.c to image_preprocessor.c
 * for better architectural coherence (preprocessing logic belongs in preprocessor)
 */
static float calculate_contrast(float *imageData, size_t size) {
    size_t sample_stride, sample_count, valid_samples, i;
    float sum, sum_sq, mean, raw_variance, variance, val;
    float max_possible_variance, contrast_ratio, adjusted_contrast, result;
    
    // Protection against invalid inputs
    if (imageData == NULL || size == 0) {
        log_error("PREPROCESS", "Invalid image data in calculate_contrast");
        return 1.0f; // Default value = maximum volume
    }

    // Sampling - don't process all pixels to optimize performance  
    sample_stride = (size_t)g_sp3ctra_config.additive_contrast_stride > 0 ? 
                    (size_t)g_sp3ctra_config.additive_contrast_stride : 1;
    sample_count = size / sample_stride;

    if (sample_count == 0) {
        log_error("PREPROCESS", "No valid samples in calculate_contrast");
        return 1.0f; // Default value = maximum volume
    }

    // Calculate mean and variance in a single pass
    sum = 0.0f;
    sum_sq = 0.0f;
    valid_samples = 0;

    for (i = 0; i < size; i += sample_stride) {
        val = (float)imageData[i];
        // Protection against invalid values (robust version without isnan/isinf)
        if (val != val || val * 0.0f != 0.0f) // equivalent to isnan(val) || isinf(val)
            continue;

        sum += val;
        sum_sq += val * val;
        valid_samples++;
    }

    // Protection against no valid samples
    if (valid_samples == 0) {
        log_error("PREPROCESS", "No valid samples in calculate_contrast");
        return 1.0f; // Default value = maximum volume
    }

    // Statistical calculation
    mean = sum / valid_samples;

    // Calculate variance with protection against rounding errors
    raw_variance = (sum_sq / valid_samples) - (mean * mean);
    variance = raw_variance > 0.0f ? raw_variance : 0.0f;

    // Normalization with min-max thresholds for stability
    // VOLUME_AMP_RESOLUTION = 1.0 (normalized float range)
    max_possible_variance = (VOLUME_AMP_RESOLUTION * VOLUME_AMP_RESOLUTION) / 4.0f;

    if (max_possible_variance <= 0.0f) {
        log_error("PREPROCESS", "Invalid maximum variance in calculate_contrast");
        return 1.0f; // Default value = maximum volume
    }

    contrast_ratio = sqrtf(variance) / sqrtf(max_possible_variance);

    // Protection against NaN and infinity (robust version without isnan/isinf)
    if (contrast_ratio != contrast_ratio || contrast_ratio * 0.0f != 0.0f) {
        log_error("PREPROCESS", "Invalid contrast ratio: %f / %f = %f",
                  sqrtf(variance), sqrtf(max_possible_variance), contrast_ratio);
        return 1.0f; // Default value = maximum volume
    }

    // Apply response curve for better perception
    adjusted_contrast = powf(contrast_ratio, g_sp3ctra_config.additive_contrast_adjustment_power);

    // ORIGINAL FORMULA (restored): Linear mapping from contrast_min to 1.0
    // High adjusted_contrast (sharp image) → result near 1.0 (loud)
    // Low adjusted_contrast (blurry image) → result near contrast_min (quiet)
    result = g_sp3ctra_config.additive_contrast_min + 
             (1.0f - g_sp3ctra_config.additive_contrast_min) * adjusted_contrast;
    
    // Safety clamps
    if (result > 1.0f)
        result = 1.0f;
    if (result < g_sp3ctra_config.additive_contrast_min)
        result = g_sp3ctra_config.additive_contrast_min;

    return result;
}

/**
 * @brief LuxStral synthesis preprocessing
 * Pipeline: RGB → Grayscale → Contrast → Inversion (optional) → Gamma → Averaging
 */
void preprocess_luxstral(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    PreprocessedImageData *out
) {
    int nb_pixels = get_cis_pixels_nb();
    int num_notes = nb_pixels / g_sp3ctra_config.pixels_per_note;
    int pixels_per_note = g_sp3ctra_config.pixels_per_note;
    int i, note, pix;
    
    /* STEP 1: RGB → Grayscale [0.0, 1.0] with preventive clamping */
    for (i = 0; i < nb_pixels; i++) {
        float gray = (0.299f * raw_r[i] + 0.587f * raw_g[i] + 0.114f * raw_b[i]);
        float normalized = gray / 255.0f;
        
        /* Preventive clamping to handle floating-point rounding errors */
        /* This prevents negative zeros after inversion and reduces downstream corrections */
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;
        
        out->additive.grayscale[i] = normalized;
    }
    
    /* STEP 2: Calculate contrast factor on RAW grayscale (BEFORE any transformations) */
    /* CRITICAL: Contrast must be calculated on untransformed data to get objective variance
     * Gamma and inversion are non-linear transformations that would distort the variance measurement
     * This prevents volume jumps when inserting objects into the image */
    out->additive.contrast_factor = calculate_contrast(out->additive.grayscale, nb_pixels);
    
    /* Debug logging for contrast calculation (periodic) - DISABLED */
    /* This log was too verbose in production and has been removed */
    /*
    static int contrast_log_counter = 0;
    if (++contrast_log_counter % 1000 == 0) {
        log_debug("PREPROCESS", "Contrast factor: %.3f (min=%.2f, power=%.2f, stride=%.0f)",
                  out->additive.contrast_factor,
                  g_sp3ctra_config.additive_contrast_min,
                  g_sp3ctra_config.additive_contrast_adjustment_power,
                  g_sp3ctra_config.additive_contrast_stride);
    }
    */
    
    /* STEP 3: Inversion (optional, AFTER contrast calculation) */
    if (g_sp3ctra_config.invert_intensity) {
        for (i = 0; i < nb_pixels; i++) {
            out->additive.grayscale[i] = 1.0f - out->additive.grayscale[i];
        }
    }
    
    /* STEP 4: Gamma correction (non-linear mapping) - LUXSTRAL SPECIFIC */
    if (g_sp3ctra_config.additive_enable_non_linear_mapping) {
        float gamma = g_sp3ctra_config.additive_gamma_value;
        
        for (i = 0; i < nb_pixels; i++) {
            float val = out->additive.grayscale[i];
            
            /* Protection against invalid values before powf */
            if (val < 0.0f) val = 0.0f;  /* Clamp negative values */
            if (val > 1.0f) val = 1.0f;  /* Clamp values above 1.0 */
            
            /* Apply gamma with protection (photo convention: pow(x, 1/gamma)) */
            float result = powf(val, 1.0f / gamma);
            
            /* Verify result is valid (not NaN/Inf) */
            if (result != result || result * 0.0f != 0.0f) {
                /* NaN or Inf detected - use original clamped value */
                result = val;
            }
            
            out->additive.grayscale[i] = result;
        }
    }
    
    /* STEP 5: Averaging per note */
    if (num_notes > PREPROCESS_MAX_NOTES) {
        num_notes = PREPROCESS_MAX_NOTES;
    }
    
    for (note = 0; note < num_notes; note++) {
        float sum = 0.0f;
        int valid_pixels = 0;
        
        for (pix = 0; pix < pixels_per_note; pix++) {
            int pixel_idx = note * pixels_per_note + pix;
            if (pixel_idx < nb_pixels) {
                float val = out->additive.grayscale[pixel_idx];
                /* Protection against NaN/Inf in grayscale data */
                if (val == val && val * 0.0f == 0.0f) {  /* equivalent to !isnan(val) && !isinf(val) */
                    sum += val;
                    valid_pixels++;
                }
            }
        }
        
        /* Protection against division by zero and NaN propagation */
        if (valid_pixels > 0) {
            out->additive.notes[note] = sum / (float)valid_pixels;
        } else {
            out->additive.notes[note] = 0.0f;  /* Default to silence if no valid pixels */
        }
        
        /* Final NaN check - replace any NaN with 0 */
        if (out->additive.notes[note] != out->additive.notes[note]) {
            out->additive.notes[note] = 0.0f;
        }
    }
    
    /* Bug correction: note 0 = 0 */
    out->additive.notes[0] = 0.0f;

    /* STEP 6: Apply live image opacity, freeze mode, and fade-in/out.
     *
     * Applies to BOTH out->additive.notes (used by StrokeForge) AND
     * out->additive.grayscale (used by synth_AudioProcess for LuxStral audio).
     * synth_AudioProcess reads db->preprocessed_data.additive.grayscale, NOT notes.
     *
     * freeze_mode == 2 (STOP/WHITE): fade-out over fade_ms, then silence.
     * freeze_mode == 1 (HOLD)      : freeze at last PLAY frame immediately (no fade-out).
     * freeze_mode == 0 (PLAY)      : live stream; fade-in from 0 when entering from HOLD/STOP.
     *
     * image_live_opacity [0..1]: darken-blend weight (now applied to grayscale too).
     * image_fade_in_ms         : ramp duration in ms.
     */
    {
        static float    held_notes[PREPROCESS_MAX_NOTES];
        static float    held_gray [PREPROCESS_MAX_NOTES]; /* Parallel hold for grayscale */
        static int      held_count      = 0;
        static int      held_gray_count = 0;
        static int      prev_freeze = -1;   /* -1 = first call (uninitialised) */
        static uint64_t fade_ts_us  = 0;
        static int      fade_dir    = 0;    /* +1=fade-in, -1=fade-out, 0=none */

        int      freeze   = g_sp3ctra_config.image_freeze_mode;
        /* RAW upstream gate: if RAW is held/stopped, override live freeze */
        int raw_freeze = g_sp3ctra_config.raw_freeze_mode;
        if (raw_freeze > freeze) freeze = raw_freeze;
        float    opacity  = g_sp3ctra_config.image_live_opacity;
        int      fade_ms  = g_sp3ctra_config.image_fade_in_ms;
        /* Use RAW fade timing when RAW gate overrides */
        if (raw_freeze > g_sp3ctra_config.image_freeze_mode)
            fade_ms = g_sp3ctra_config.raw_fade_in_ms;
        uint64_t now_us   = get_timestamp_us();

        /* Clamp nb_pixels to static array bounds */
        int gn = (nb_pixels < PREPROCESS_MAX_NOTES) ? nb_pixels : PREPROCESS_MAX_NOTES;

        /* ── Detect mode transition ──────────────────────────────────────── */
        if (freeze != prev_freeze && prev_freeze >= 0)
        {
            if (prev_freeze == 0 && freeze == 2)
            {
                /* PLAY → STOP/WHITE: fade-out to silence */
                if (fade_ms > 0)
                {
                    fade_ts_us = now_us;
                    fade_dir   = -1;
                }
            }
            else if (freeze == 0)
            {
                /* HOLD/STOP → PLAY: fade-in from silence */
                if (fade_ms > 0)
                {
                    fade_ts_us = now_us;
                    fade_dir   = 1;
                }
            }
            else
            {
                /* PLAY → HOLD, or STOP ↔ HOLD: cancel any in-progress fade.
                 * PLAY → HOLD is seamless because held_gray == last PLAY frame. */
                fade_dir = 0;
            }
        }
        if (prev_freeze < 0) prev_freeze = freeze; /* first call: no fade */

        /* ── Apply freeze / hold / play logic ───────────────────────────── */
        if (freeze == 0)
        {
            /* PLAY: apply opacity to notes AND grayscale, then save for HOLD restore */
            if (opacity < 0.999f)
            {
                for (note = 0; note < num_notes; note++)
                    out->additive.notes[note] *= opacity;
                for (i = 0; i < gn; i++)
                    out->additive.grayscale[i] *= opacity;
            }

            int n = (num_notes < PREPROCESS_MAX_NOTES) ? num_notes : PREPROCESS_MAX_NOTES;
            for (note = 0; note < n; note++)
                held_notes[note] = out->additive.notes[note];
            for (i = 0; i < gn; i++)
                held_gray[i] = out->additive.grayscale[i];
            held_count      = n;
            held_gray_count = gn;
        }
        else if (freeze == 1)
        {
            /* HOLD: restore last PLAY frame for notes and grayscale */
            if (held_count > 0)
            {
                int n = (num_notes < held_count) ? num_notes : held_count;
                for (note = 0; note < n; note++)
                    out->additive.notes[note] = held_notes[note];
            }
            if (held_gray_count > 0)
            {
                int g = (gn < held_gray_count) ? gn : held_gray_count;
                for (i = 0; i < g; i++)
                    out->additive.grayscale[i] = held_gray[i];
            }
        }
        else
        {
            /* STOP / WHITE (freeze=2): silence notes and grayscale */
            for (note = 0; note < num_notes; note++)
                out->additive.notes[note] = 0.0f;
            for (i = 0; i < gn; i++)
                out->additive.grayscale[i] = 0.0f;
        }

        /* ── Apply fade envelope (notes AND grayscale) ───────────────────── */
        if (fade_dir != 0 && fade_ms > 0)
        {
            float t = (float)(now_us - fade_ts_us) / ((float)fade_ms * 1000.0f);
            if (t >= 1.0f)
            {
                fade_dir = 0; /* ramp complete */
            }
            else
            {
                /* For STOP fade-out, start from held values (not silence) so
                 * the signal decays smoothly from the last PLAY frame. */
                if (fade_dir == -1 && freeze == 2 && held_count > 0)
                {
                    float ramp = 1.0f - t;
                    int n = (num_notes < held_count) ? num_notes : held_count;
                    for (note = 0; note < n; note++)
                        out->additive.notes[note] = held_notes[note] * ramp;
                    /* Apply to grayscale: decay from held values */
                    int g = (gn < held_gray_count) ? gn : held_gray_count;
                    for (i = 0; i < g; i++)
                        out->additive.grayscale[i] = held_gray[i] * ramp;
                }
                else
                {
                    float ramp = (fade_dir > 0) ? t : (1.0f - t);
                    for (note = 0; note < num_notes; note++)
                        out->additive.notes[note] *= ramp;
                    for (i = 0; i < gn; i++)
                        out->additive.grayscale[i] *= ramp;
                }
            }
        }

        prev_freeze = freeze;
    }
}

/**
 * @brief LuxSynth synthesis preprocessing
 * Pipeline: RGB → Grayscale → Inversion (optional) → DC Blocking (optional) → Gamma (optional) → FFT
 *
 * All three image corrections are independent per-path toggles, identical in intent
 * to the LUXSTRAL pipeline but operating on out->polyphonic.grayscale (FFT input).
 *
 * STEP 2 — Inversion  : uses luxsynth_inversion  (NOT the global invert_intensity)
 * STEP 3 — DC Blocking: uses luxsynth_ac_removal  — subtracts per-line mean, clamps [0,1]
 * STEP 4 — Gamma      : uses luxsynth_enable_non_linear_mapping + luxsynth_gamma_value
 *                        photo convention: output = pow(input, 1/gamma)
 */
void preprocess_luxsynth(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    PreprocessedImageData *out
) {
    int nb_pixels = get_cis_pixels_nb();
    int i;

    /* STEP 1: RGB → Grayscale [0.0, 1.0] with preventive clamping */
    for (i = 0; i < nb_pixels; i++) {
        float gray = (0.299f * raw_r[i] + 0.587f * raw_g[i] + 0.114f * raw_b[i]);
        float normalized = gray / 255.0f;
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;
        out->polyphonic.grayscale[i] = normalized;
    }

    /* STEP 2: Inversion — per-path flag (luxsynth_inversion, NOT global invert_intensity).
     * Bug fix: the old code used g_sp3ctra_config.invert_intensity which is the
     * LUXSTRAL global flag; LUXSYNTH must use its own independent toggle. */
    if (g_sp3ctra_config.luxsynth_inversion) {
        for (i = 0; i < nb_pixels; i++)
            out->polyphonic.grayscale[i] = 1.0f - out->polyphonic.grayscale[i];
    }

    /* STEP 3: DC Blocking (AC removal) — subtract per-line mean, clamp [0, 1].
     * Bug fix: this step was entirely missing from the LUXSYNTH path. */
    if (g_sp3ctra_config.luxsynth_ac_removal) {
        float sum = 0.0f;
        float mean;
        for (i = 0; i < nb_pixels; i++)
            sum += out->polyphonic.grayscale[i];
        mean = sum / (float)nb_pixels;
        for (i = 0; i < nb_pixels; i++) {
            out->polyphonic.grayscale[i] -= mean;
            if (out->polyphonic.grayscale[i] < 0.0f) out->polyphonic.grayscale[i] = 0.0f;
            if (out->polyphonic.grayscale[i] > 1.0f) out->polyphonic.grayscale[i] = 1.0f;
        }
    }

    /* STEP 4: Gamma correction (photo convention: pow(x, 1/gamma)).
     * Always active when gamma_value != 1.0 (no separate enable flag).
     * gamma_value == 1.0 is a mathematical no-op — skip for performance. */
    {
        float gamma = g_sp3ctra_config.luxsynth_gamma_value;
        if (gamma > 0.0f && gamma != 1.0f) {
            const float exponent = 1.0f / gamma;
            for (i = 0; i < nb_pixels; i++) {
                float val = out->polyphonic.grayscale[i];
                if (val < 0.0f) val = 0.0f;
                if (val > 1.0f) val = 1.0f;
                float result = powf(val, exponent);
                /* NaN/Inf protection (portable, no isnan/isinf) */
                if (result != result || result * 0.0f != 0.0f)
                    result = val;
                out->polyphonic.grayscale[i] = result;
            }
        }
    }

    /* STEP 5: FFT with temporal smoothing (amplitude) */
    image_preprocess_fft(out);

    /* STEP 6: Color FFT for spectral panning (stereo) */
    image_preprocess_color_fft(raw_r, raw_g, raw_b, out);
}

/**
 * @brief LuxWave synthesis preprocessing
 * Pipeline: Direct RGB copy (native sampling, no conversion)
 */
void preprocess_luxwave(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    PreprocessedImageData *out
) {
    int nb_pixels = get_cis_pixels_nb();
    
    /* Direct RGB copy for native waveform sampling */
    memcpy(out->photowave.r, raw_r, nb_pixels);
    memcpy(out->photowave.g, raw_g, nb_pixels);
    memcpy(out->photowave.b, raw_b, nb_pixels);
    
    /* LuxWave handles its own inversion/processing via its parameters */
}

/**
 * @brief Preprocess stereo panning data
 * Calculates color temperature and pan gains for each note
 */
static void preprocess_stereo(const uint8_t *raw_r, const uint8_t *raw_g,
                               const uint8_t *raw_b, PreprocessedImageData *out) {
    int nb_pixels;
    int num_notes;
    int pixels_per_note;
    int note;
    
    nb_pixels = get_cis_pixels_nb();
    num_notes = nb_pixels / g_sp3ctra_config.pixels_per_note;
    pixels_per_note = g_sp3ctra_config.pixels_per_note;
    
    /* Ensure we don't exceed array bounds */
    if (num_notes > PREPROCESS_MAX_NOTES) {
        num_notes = PREPROCESS_MAX_NOTES;
    }
    
    for (note = 0; note < num_notes; note++) {
        /* Calculate average RGB for this note's pixels */
        uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
        uint32_t pixel_count = 0;
        int pix;
        
        for (pix = 0; pix < pixels_per_note; pix++) {
            uint32_t pixel_idx = note * pixels_per_note + pix;
            if (pixel_idx < (uint32_t)nb_pixels) {
                r_sum += raw_r[pixel_idx];
                g_sum += raw_g[pixel_idx];
                b_sum += raw_b[pixel_idx];
                pixel_count++;
            }
        }
        
        if (pixel_count > 0) {
            /* Calculate average RGB values */
            uint8_t r_avg = r_sum / pixel_count;
            uint8_t g_avg = g_sum / pixel_count;
            uint8_t b_avg = b_sum / pixel_count;
            
            /* Calculate color temperature and pan position */
            float temperature = calculate_color_temperature(r_avg, g_avg, b_avg);
            out->stereo.pan_positions[note] = temperature;
            
            /* Calculate pan gains using constant power law */
            calculate_pan_gains(temperature, 
                              &out->stereo.left_gains[note],
                              &out->stereo.right_gains[note]);
        } else {
            /* Default to center if no pixels */
            out->stereo.pan_positions[note] = 0.0f;
            out->stereo.left_gains[note] = 0.707f;
            out->stereo.right_gains[note] = 0.707f;
        }
    }
}

/**
 * @brief Preprocess DMX zone average colors
 * Calculates average RGB values for each DMX zone
 */
#ifdef USE_DMX
static void preprocess_dmx(const uint8_t *raw_r, const uint8_t *raw_g,
                            const uint8_t *raw_b, PreprocessedImageData *out) {
    int nb_pixels;
    int pixels_per_zone;
    int zone;
    
    nb_pixels = get_cis_pixels_nb();
    
    /* Calculate pixels per zone */
    pixels_per_zone = nb_pixels / DMX_NUM_SPOTS;
    
    for (zone = 0; zone < DMX_NUM_SPOTS; zone++) {
        uint32_t r_sum = 0, g_sum = 0, b_sum = 0;
        uint32_t pixel_count = 0;
        int start_pixel;
        int end_pixel;
        int pix;
        
        /* Calculate start and end pixel indices for this zone */
        start_pixel = zone * pixels_per_zone;
        end_pixel = (zone == DMX_NUM_SPOTS - 1) ? nb_pixels : start_pixel + pixels_per_zone;
        
        /* Sum RGB values across zone */
        for (pix = start_pixel; pix < end_pixel && pix < nb_pixels; pix++) {
            r_sum += raw_r[pix];
            g_sum += raw_g[pix];
            b_sum += raw_b[pix];
            pixel_count++;
        }
        
        /* Calculate average */
        if (pixel_count > 0) {
            out->dmx.zone_r[zone] = r_sum / pixel_count;
            out->dmx.zone_g[zone] = g_sum / pixel_count;
            out->dmx.zone_b[zone] = b_sum / pixel_count;
        } else {
            out->dmx.zone_r[zone] = 0;
            out->dmx.zone_g[zone] = 0;
            out->dmx.zone_b[zone] = 0;
        }
    }
}
#endif

#ifndef DISABLE_LUXSYNTH
/**
 * @brief Calculate harmonicity parameters from color temperature
 * Maps temperature to harmonic/inharmonic behavior for timbre control
 * 
 * Temperature mapping:
 * - Warm colors (red, T=-1.0) → Harmonic sounds (strings, voice)
 * - Neutral colors (T=0.0) → Semi-harmonic sounds (guitar, piano)
 * - Cold colors (blue, T=+1.0) → Inharmonic sounds (bells, percussion)
 * 
 * @param data PreprocessedImageData with pan_positions already computed
 */
static void calculate_harmonicity_from_temperature(PreprocessedImageData *data) {
    /* Inharmonic ratio tables for different physical models */
    static const float membrane_ratios[] = {
        1.000f, 1.593f, 2.136f, 2.296f, 2.653f, 
        2.918f, 3.156f, 3.501f, 3.652f, 4.060f
    };
    static const int membrane_ratios_count = sizeof(membrane_ratios) / sizeof(membrane_ratios[0]);
    
    static const float bell_ratios[] = {
        1.000f, 2.400f, 2.990f, 3.970f, 5.050f,
        6.200f, 7.450f, 8.800f, 10.20f, 11.70f
    };
    static const int bell_ratios_count = sizeof(bell_ratios) / sizeof(bell_ratios[0]);
    
    int i;
    
    for (i = 0; i < MAX_FFT_BINS; i++) {
        float temperature = data->polyphonic.pan_positions[i];  /* [-1, 1] */
        
        /* Apply curve exponent for response shaping */
        float temp_abs = fabsf(temperature);
        float temp_shaped = powf(temp_abs, g_sp3ctra_config.poly_harmonicity_curve_exponent);
        float temp_signed = (temperature >= 0.0f) ? temp_shaped : -temp_shaped;
        
        /* Calculate harmonicity: warm (T=-1) → h=1.0, cold (T=+1) → h=0.0 */
        float h = (1.0f - temp_signed) / 2.0f;  /* Map [-1,1] to [1,0] */
        
        /* Clamp to [0, 1] */
        if (h < 0.0f) h = 0.0f;
        if (h > 1.0f) h = 1.0f;
        
        data->polyphonic.harmonicity[i] = h;
        
        /* Calculate detune for semi-harmonic sounds (neutral colors) */
        /* Maximum detune occurs at h=0.5 (neutral temperature) */
        float detune_factor = 1.0f - fabsf(h - 0.5f) * 2.0f;  /* Peak at h=0.5 */
        data->polyphonic.detune_cents[i] = detune_factor * g_sp3ctra_config.poly_detune_max_cents;
        
        /* Assign inharmonic ratios for cold colors (h < 0.3) */
        if (h < 0.3f) {
            /* Use membrane ratios for moderately cold */
            if (h > 0.15f) {
                int ratio_idx = i % membrane_ratios_count;
                data->polyphonic.inharmonic_ratios[i] = membrane_ratios[ratio_idx];
            }
            /* Use bell ratios for very cold */
            else {
                int ratio_idx = i % bell_ratios_count;
                data->polyphonic.inharmonic_ratios[i] = bell_ratios[ratio_idx];
            }
        }
        /* Harmonic ratios for warm/neutral colors */
        else {
            data->polyphonic.inharmonic_ratios[i] = (float)(i + 1);  /* Standard harmonic series */
        }
    }
}

/**
 * @brief FFT preprocessing function for polyphonic synthesis
 * Computes FFT magnitudes from grayscale data with temporal smoothing
 * 
 * Architecture:
 * - Lazy initialization of KissFFT on first call
 * - Converts grayscale [0.0-1.0] to FFT input [0-255]
 * - Computes FFT using KissFFT
 * - Calculates and normalizes magnitudes
 * - Applies temporal smoothing via circular buffer (5 frames @ 1kHz = 5ms)
 * - Applies exponential smoothing for additional stability
 * 
 * @param data PreprocessedImageData with grayscale already computed
 * @return 0 on success, -1 on error
 */
int image_preprocess_fft(PreprocessedImageData *data) {
    int nb_pixels;
    int i, h, idx;
    float real, imag, magnitude, target_mag;
    float sum, averaged;
    
    if (!data) {
        log_error("PREPROCESS", "FFT: NULL data pointer");
        return -1;
    }
    
    nb_pixels = get_cis_pixels_nb();
    
    /* Lazy initialization of FFT state */
    if (!fft_history_state.initialized) {
        log_info("PREPROCESS", "FFT: Initializing KissFFT for %d pixels", nb_pixels);
        
        /* Allocate FFT buffers */
        fft_history_state.fft_input = (kiss_fft_scalar *)calloc(nb_pixels, sizeof(kiss_fft_scalar));
        fft_history_state.fft_output = (kiss_fft_cpx *)calloc(nb_pixels / 2 + 1, sizeof(kiss_fft_cpx));
        
        if (!fft_history_state.fft_input || !fft_history_state.fft_output) {
            log_error("PREPROCESS", "FFT: Failed to allocate FFT buffers");
            if (fft_history_state.fft_input) free(fft_history_state.fft_input);
            if (fft_history_state.fft_output) free(fft_history_state.fft_output);
            return -1;
        }
        
        /* Initialize KissFFT configuration */
        fft_history_state.fft_cfg = kiss_fftr_alloc(nb_pixels, 0, NULL, NULL);
        if (!fft_history_state.fft_cfg) {
            log_error("PREPROCESS", "FFT: Failed to initialize KissFFT configuration");
            free(fft_history_state.fft_input);
            free(fft_history_state.fft_output);
            return -1;
        }
        
        /* Pre-fill history buffer with white spectrum (all bins = 1.0) */
        /* This prevents transients at startup */
        for (h = 0; h < FFT_HISTORY_SIZE; h++) {
            for (i = 0; i < MAX_FFT_BINS; i++) {
                fft_history_state.history[h][i] = 1.0f;
            }
        }
        
        fft_history_state.write_index = 0;
        fft_history_state.fill_count = FFT_HISTORY_SIZE;  /* History pre-filled */
        fft_history_state.initialized = 1;
        
        log_info("PREPROCESS", "FFT: Initialized with %d-frame temporal smoothing (%.1fms @ 1kHz)", 
                 FFT_HISTORY_SIZE, FFT_HISTORY_SIZE * 1.0f);
    }
    
    /* Convert grayscale [0.0-1.0] to FFT input [0-255] */
    for (i = 0; i < nb_pixels; i++) {
        fft_history_state.fft_input[i] = data->polyphonic.grayscale[i] * 255.0f;
    }
    
    /* Compute FFT */
    kiss_fftr(fft_history_state.fft_cfg, fft_history_state.fft_input, fft_history_state.fft_output);
    
    /* Calculate raw magnitudes and store in circular buffer */
    /* Bin 0 (DC component) uses different normalization */
    magnitude = fft_history_state.fft_output[0].r / NORM_FACTOR_BIN0;
    fft_history_state.history[fft_history_state.write_index][0] = magnitude;
    
    /* Bins 1 to MAX_FFT_BINS-1 (harmonics) */
    for (i = 1; i < MAX_FFT_BINS && i < (nb_pixels / 2 + 1); i++) {
        real = fft_history_state.fft_output[i].r;
        imag = fft_history_state.fft_output[i].i;
        magnitude = sqrtf(real * real + imag * imag);
        target_mag = fminf(1.0f, magnitude / NORM_FACTOR_HARMONICS);
        fft_history_state.history[fft_history_state.write_index][i] = target_mag;
    }
    
    /* Fill remaining bins with zero if nb_pixels is small */
    for (i = (nb_pixels / 2 + 1); i < MAX_FFT_BINS; i++) {
        fft_history_state.history[fft_history_state.write_index][i] = 0.0f;
    }
    
    /* Update circular buffer indices */
    fft_history_state.write_index = (fft_history_state.write_index + 1) % FFT_HISTORY_SIZE;
    if (fft_history_state.fill_count < FFT_HISTORY_SIZE) {
        fft_history_state.fill_count++;
    }
    
    /* Calculate moving average over history buffer */
    for (i = 0; i < MAX_FFT_BINS; i++) {
        sum = 0.0f;
        for (h = 0; h < fft_history_state.fill_count; h++) {
            idx = (fft_history_state.write_index - 1 - h + FFT_HISTORY_SIZE) % FFT_HISTORY_SIZE;
            sum += fft_history_state.history[idx][i];
        }
        averaged = sum / fft_history_state.fill_count;
        
        /* Apply exponential smoothing on top of moving average */
        /* This provides additional temporal stability */
        data->polyphonic.magnitudes[i] = 
            AMPLITUDE_SMOOTHING_ALPHA * averaged +
            (1.0f - AMPLITUDE_SMOOTHING_ALPHA) * data->polyphonic.magnitudes[i];
    }
    
    /* Mark FFT data as valid */
    data->polyphonic.valid = 1;
    
    return 0;
}

/**
 * @brief Color FFT preprocessing function for polyphonic synthesis spectral panning
 * Computes FFT on color temperature data to extract per-harmonic pan positions
 * 
 * Architecture:
 * - Lazy initialization of KissFFT on first call
 * - Computes color temperature per pixel
 * - Converts temperature [-1.0, 1.0] to FFT input [0-255]
 * - Computes FFT using KissFFT
 * - Extracts magnitude and phase to determine pan position per harmonic
 * - Applies temporal smoothing via circular buffer (5 frames @ 1kHz = 5ms)
 * - Precalculates stereo gains using constant power law
 * 
 * @param raw_r, raw_g, raw_b Raw RGB data
 * @param data PreprocessedImageData to store pan positions and gains
 * @return 0 on success, -1 on error
 */
static int image_preprocess_color_fft(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    PreprocessedImageData *data
) {
    int nb_pixels;
    int i, h, idx;
    float real, imag, magnitude, phase, pan_pos;
    float sum, averaged;
    
    if (!data) {
        log_error("PREPROCESS", "Color FFT: NULL data pointer");
        return -1;
    }
    
    nb_pixels = get_cis_pixels_nb();
    
    /* Lazy initialization of color FFT state */
    if (!color_fft_history_state.initialized) {
        log_info("PREPROCESS", "Color FFT: Initializing KissFFT for %d pixels", nb_pixels);
        
        /* Allocate color FFT buffers */
        color_fft_history_state.color_fft_input = (kiss_fft_scalar *)calloc(nb_pixels, sizeof(kiss_fft_scalar));
        color_fft_history_state.color_fft_output = (kiss_fft_cpx *)calloc(nb_pixels / 2 + 1, sizeof(kiss_fft_cpx));
        
        if (!color_fft_history_state.color_fft_input || !color_fft_history_state.color_fft_output) {
            log_error("PREPROCESS", "Color FFT: Failed to allocate FFT buffers");
            if (color_fft_history_state.color_fft_input) free(color_fft_history_state.color_fft_input);
            if (color_fft_history_state.color_fft_output) free(color_fft_history_state.color_fft_output);
            return -1;
        }
        
        /* Initialize KissFFT configuration for color */
        color_fft_history_state.color_fft_cfg = kiss_fftr_alloc(nb_pixels, 0, NULL, NULL);
        if (!color_fft_history_state.color_fft_cfg) {
            log_error("PREPROCESS", "Color FFT: Failed to initialize KissFFT configuration");
            free(color_fft_history_state.color_fft_input);
            free(color_fft_history_state.color_fft_output);
            return -1;
        }
        
        /* Pre-fill pan history buffer with center position (0.0) */
        for (h = 0; h < FFT_HISTORY_SIZE; h++) {
            for (i = 0; i < MAX_FFT_BINS; i++) {
                color_fft_history_state.pan_history[h][i] = 0.0f;
            }
        }
        
        color_fft_history_state.write_index = 0;
        color_fft_history_state.fill_count = FFT_HISTORY_SIZE;  /* History pre-filled */
        color_fft_history_state.initialized = 1;
        
        log_info("PREPROCESS", "Color FFT: Initialized with %d-frame temporal smoothing (%.1fms @ 1kHz)", 
                 FFT_HISTORY_SIZE, FFT_HISTORY_SIZE * 1.0f);
    }
    
    /* Step 1: Compute color temperature per pixel */
    for (i = 0; i < nb_pixels; i++) {
        /* Calculate temperature [-1.0 (warm/red), +1.0 (cold/blue)] */
        float temperature = calculate_color_temperature(raw_r[i], raw_g[i], raw_b[i]);
        
        /* Convert to FFT input [0-255] for KissFFT scalar */
        /* Shift from [-1, 1] to [0, 1] then scale to [0, 255] */
        color_fft_history_state.color_fft_input[i] = (temperature + 1.0f) * 127.5f;
    }
    
    /* Step 2: Compute FFT on color temperature */
    kiss_fftr(color_fft_history_state.color_fft_cfg, 
              color_fft_history_state.color_fft_input, 
              color_fft_history_state.color_fft_output);
    
    /* Step 3: Extract pan positions from FFT magnitude and phase */
    for (i = 0; i < MAX_FFT_BINS && i < (nb_pixels / 2 + 1); i++) {
        real = color_fft_history_state.color_fft_output[i].r;
        imag = color_fft_history_state.color_fft_output[i].i;
        
        if (i == 0) {
            /* Bin 0 (DC component): Average temperature of entire image */
            /* DC represents global color bias: positive=cold/blue, negative=warm/red */
            float avg_temp = real / (nb_pixels * 127.5f);  /* Normalize back to [-1, 1] */
            pan_pos = avg_temp;
        } else {
            /* Harmonics: Use magnitude and phase to determine pan position */
            magnitude = sqrtf(real * real + imag * imag);
            phase = atan2f(imag, real);  /* Phase in [-π, π] */
            
            /* HYBRID APPROACH: Phase for direction, magnitude for intensity */
            /* Phase gives full directional range [-1, 1] instead of binary ±1 */
            /* Magnitude modulates the pan intensity (strong patterns = extreme pan) */
            
            /* Convert phase from [-π, π] to [-1, 1] for full directional range */
            float phase_normalized = phase / M_PI;
            
            /* Normalize magnitude with reduced factor for better sensitivity */
            /* Original NORM_FACTOR_COLOR was too large (440640), reducing by 20x */
            float magnitude_factor = fminf(1.0f, magnitude / (NORM_FACTOR_COLOR / 20.0f));
            
            /* Pan position = phase direction × magnitude intensity */
            /* Strong color patterns → pan follows phase direction fully */
            /* Weak color patterns → pan reduced toward center */
            pan_pos = phase_normalized * magnitude_factor;
        }
        
        /* Clamp to [-1, 1] */
        if (pan_pos > 1.0f) pan_pos = 1.0f;
        if (pan_pos < -1.0f) pan_pos = -1.0f;
        
        /* Store in circular buffer for temporal smoothing */
        color_fft_history_state.pan_history[color_fft_history_state.write_index][i] = pan_pos;
    }
    
    /* Fill remaining bins with center position if nb_pixels is small */
    for (i = (nb_pixels / 2 + 1); i < MAX_FFT_BINS; i++) {
        color_fft_history_state.pan_history[color_fft_history_state.write_index][i] = 0.0f;
    }
    
    /* Update circular buffer indices */
    color_fft_history_state.write_index = (color_fft_history_state.write_index + 1) % FFT_HISTORY_SIZE;
    if (color_fft_history_state.fill_count < FFT_HISTORY_SIZE) {
        color_fft_history_state.fill_count++;
    }
    
    /* Step 4: Calculate moving average over history buffer and compute stereo gains */
    for (i = 0; i < MAX_FFT_BINS; i++) {
        sum = 0.0f;
        for (h = 0; h < color_fft_history_state.fill_count; h++) {
            idx = (color_fft_history_state.write_index - 1 - h + FFT_HISTORY_SIZE) % FFT_HISTORY_SIZE;
            sum += color_fft_history_state.pan_history[idx][i];
        }
        averaged = sum / color_fft_history_state.fill_count;
        
        /* Store smoothed pan position */
        data->polyphonic.pan_positions[i] = averaged;
        
        /* Precalculate stereo gains using constant power law */
        calculate_pan_gains(averaged, 
                          &data->polyphonic.left_gains[i],
                          &data->polyphonic.right_gains[i]);
    }
    
    /* Step 5: Calculate harmonicity parameters from temperature */
    calculate_harmonicity_from_temperature(data);
    
    return 0;
}
#endif

/* ============================================================================
 * SAMPLER-SPECIFIC PREPROCESSING
 * Mirrors preprocess_luxstral() but uses sampler parameters:
 *   sampler_gamma, sampler_contrast_min, sampler_freeze_mode,
 *   sampler_fade_in_ms, image_sampler_opacity.
 * Called from FramePlayerThread via image_preprocess_lux_sampler().
 * ============================================================================ */

/**
 * @brief LuxStral sampler preprocessing
 * Identical pipeline to preprocess_luxstral() but uses the independent
 * sampler parameters instead of the live-stream parameters.
 */
void preprocess_luxstral_sampler(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    PreprocessedImageData *out
) {
    int nb_pixels      = get_cis_pixels_nb();
    int pixels_per_note = g_sp3ctra_config.pixels_per_note;
    int num_notes      = nb_pixels / pixels_per_note;
    int i, note, pix;

    /* STEP 1: RGB → Grayscale [0.0, 1.0] */
    for (i = 0; i < nb_pixels; i++) {
        float gray = (0.299f * raw_r[i] + 0.587f * raw_g[i] + 0.114f * raw_b[i]);
        float normalized = gray / 255.0f;
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;
        out->additive.grayscale[i] = normalized;
    }

    /* STEP 2: Contrast factor (computed before any non-linear transform).
     * We re-use calculate_contrast() which internally reads additive_contrast_min.
     * Then we rescale the result to use sampler_contrast_min instead. */
    {
        float raw_cf = calculate_contrast(out->additive.grayscale, nb_pixels);
        /* Undo additive_contrast_min scaling, apply sampler_contrast_min */
        float live_cmin = g_sp3ctra_config.additive_contrast_min;
        float smp_cmin  = g_sp3ctra_config.sampler_contrast_min;
        float denom = (1.0f - live_cmin);
        float adj   = (denom > 1e-6f)
                      ? (raw_cf - live_cmin) / denom
                      : raw_cf;
        if (adj < 0.0f) adj = 0.0f;
        if (adj > 1.0f) adj = 1.0f;
        out->additive.contrast_factor = smp_cmin + (1.0f - smp_cmin) * adj;
        if (out->additive.contrast_factor > 1.0f)
            out->additive.contrast_factor = 1.0f;
        if (out->additive.contrast_factor < smp_cmin)
            out->additive.contrast_factor = smp_cmin;
    }

    /* STEP 3: Inversion (same flag as live path) */
    if (g_sp3ctra_config.invert_intensity) {
        for (i = 0; i < nb_pixels; i++)
            out->additive.grayscale[i] = 1.0f - out->additive.grayscale[i];
    }

    /* STEP 4: Gamma — use additive_gamma_value (same as live path).
     * FIX(gamma): The old sampler_gamma was a separate hidden parameter
     * not connected to the LuxStral Gamma slider — replaced to unify. */
    if (g_sp3ctra_config.additive_enable_non_linear_mapping) {
        float gamma = g_sp3ctra_config.additive_gamma_value;
        for (i = 0; i < nb_pixels; i++) {
            float val = out->additive.grayscale[i];
            if (val < 0.0f) val = 0.0f;
            if (val > 1.0f) val = 1.0f;
            float result = powf(val, 1.0f / gamma);
            if (result != result || result * 0.0f != 0.0f) result = val;
            out->additive.grayscale[i] = result;
        }
    }

    /* STEP 5: Averaging per note */
    if (num_notes > PREPROCESS_MAX_NOTES) num_notes = PREPROCESS_MAX_NOTES;

    for (note = 0; note < num_notes; note++) {
        float sum = 0.0f;
        int valid_pixels = 0;
        for (pix = 0; pix < pixels_per_note; pix++) {
            int idx = note * pixels_per_note + pix;
            if (idx < nb_pixels) {
                float val = out->additive.grayscale[idx];
                if (val == val && val * 0.0f == 0.0f) {
                    sum += val;
                    valid_pixels++;
                }
            }
        }
        out->additive.notes[note] = (valid_pixels > 0)
                                    ? (sum / (float)valid_pixels)
                                    : 0.0f;
        if (out->additive.notes[note] != out->additive.notes[note])
            out->additive.notes[note] = 0.0f;
    }
    out->additive.notes[0] = 0.0f;

    /* STEP 6: Sampler opacity, freeze mode, and fade-in/out
     *
     * sampler_freeze_mode  0=PLAY, 1=HOLD, 2=STOP (silence)
     * image_sampler_opacity [0..1]
     * sampler_fade_in_ms   ramp duration in ms
     */
    {
        static float    smp_held[PREPROCESS_MAX_NOTES];
        static int      smp_held_count = 0;
        static int      smp_prev_freeze = -1;
        static uint64_t smp_fade_ts_us  = 0;
        static int      smp_fade_dir    = 0;  /* +1=in, -1=out, 0=none */

        int      freeze  = g_sp3ctra_config.sampler_freeze_mode;
        /* RAW upstream gate: if RAW is held/stopped, override sampler freeze */
        int raw_freeze = g_sp3ctra_config.raw_freeze_mode;
        if (raw_freeze > freeze) freeze = raw_freeze;
        float    opacity = g_sp3ctra_config.image_sampler_opacity;
        int      fade_ms = g_sp3ctra_config.sampler_fade_in_ms;
        /* Use RAW fade timing when RAW gate overrides */
        if (raw_freeze > g_sp3ctra_config.sampler_freeze_mode)
            fade_ms = g_sp3ctra_config.raw_fade_in_ms;
        uint64_t now_us  = get_timestamp_us();

        /* Detect transition */
        if (freeze != smp_prev_freeze && smp_prev_freeze >= 0)
        {
            if (smp_prev_freeze == 0 && freeze != 0) {
                if (fade_ms > 0) { smp_fade_ts_us = now_us; smp_fade_dir = -1; }
            } else if (smp_prev_freeze != 0 && freeze == 0) {
                if (fade_ms > 0) { smp_fade_ts_us = now_us; smp_fade_dir = 1; }
            }
        }
        if (smp_prev_freeze < 0) smp_prev_freeze = freeze;

        /* Apply freeze/hold/play */
        if (freeze == 0)
        {
            if (opacity < 0.999f)
                for (note = 0; note < num_notes; note++)
                    out->additive.notes[note] *= opacity;

            int n = (num_notes < PREPROCESS_MAX_NOTES) ? num_notes : PREPROCESS_MAX_NOTES;
            for (note = 0; note < n; note++)
                smp_held[note] = out->additive.notes[note];
            smp_held_count = n;
        }
        else if (freeze == 1 && smp_held_count > 0)
        {
            int n = (num_notes < smp_held_count) ? num_notes : smp_held_count;
            for (note = 0; note < n; note++)
                out->additive.notes[note] = smp_held[note];
        }
        else
        {
            /* STOP/2: silence */
            for (note = 0; note < num_notes; note++)
                out->additive.notes[note] = 0.0f;
        }

        /* Fade envelope */
        if (smp_fade_dir != 0 && fade_ms > 0)
        {
            float t = (float)(now_us - smp_fade_ts_us) / ((float)fade_ms * 1000.0f);
            if (t >= 1.0f)
            {
                smp_fade_dir = 0;
            }
            else
            {
                if (smp_fade_dir == -1 && freeze == 2 && smp_held_count > 0)
                {
                    float ramp = 1.0f - t;
                    int n = (num_notes < smp_held_count) ? num_notes : smp_held_count;
                    for (note = 0; note < n; note++)
                        out->additive.notes[note] = smp_held[note] * ramp;
                }
                else
                {
                    float ramp = (smp_fade_dir > 0) ? t : (1.0f - t);
                    for (note = 0; note < num_notes; note++)
                        out->additive.notes[note] *= ramp;
                }
            }
        }

        smp_prev_freeze = freeze;
    }
}

/**
 * @brief Sampler-specific variant of image_preprocess_frame().
 * Uses sampler parameters for the LuxStral additive path.
 * The StrokeForge blob step is re-run on the sampler notes so blob detection
 * reflects the sampler stream when a slot is playing.
 */
int image_preprocess_lux_sampler(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    PreprocessedImageData *out
) {
    if (!module_initialized) {
        log_error("PREPROCESS", "Module not initialized (sampler)");
        return -1;
    }
    if (!raw_r || !raw_g || !raw_b || !out) {
        log_error("PREPROCESS", "NULL pointer passed (sampler)");
        return -1;
    }

    out->timestamp_us = get_timestamp_us();

    /* 1. LuxStral additive path — sampler parameters */
    preprocess_luxstral_sampler(raw_r, raw_g, raw_b, out);

    /* 1b. StrokeForge blob detection on the sampler notes */
    {
        int sf_num_notes = get_cis_pixels_nb() / g_sp3ctra_config.pixels_per_note;
        if (sf_num_notes > PREPROCESS_MAX_NOTES) sf_num_notes = PREPROCESS_MAX_NOTES;
        strokeforge_analyze_frame(
            out->additive.notes,
            sf_num_notes,
            out->additive.contrast_factor,
            &out->strokeforge);
    }

    /* 2. LuxSynth (polyphonic) path — reuse live pipeline (no sampler variant needed) */
#ifndef DISABLE_LUXSYNTH
    preprocess_luxsynth(raw_r, raw_g, raw_b, out);
#endif

    /* 3. LuxWave (photowave) path */
    preprocess_luxwave(raw_r, raw_g, raw_b, out);

    /* 4. Stereo panning */
    if (g_sp3ctra_config.stereo_mode_enabled)
        preprocess_stereo(raw_r, raw_g, raw_b, out);

#ifdef USE_DMX
    preprocess_dmx(raw_r, raw_g, raw_b, out);
#endif

    return 0;
}
