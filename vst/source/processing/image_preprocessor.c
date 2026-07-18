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
#ifndef DISABLE_LUXSYNTH
static int image_preprocess_color_fft(const uint8_t *raw_r, const uint8_t *raw_g,
                                      const uint8_t *raw_b, PreprocessedImageData *data);
#endif

/* Module cleanup */
void image_preprocess_cleanup(void) {
    if (!module_initialized) {
        return;
    }
    
    log_info("PREPROCESS", "Image preprocessor module cleaned up");
    module_initialized = 0;
}

/**
 * @brief LuxSynth synthesis preprocessing
 * Pipeline: RGB → Grayscale → Inversion (optional) → DC Blocking (optional) → Gamma (optional) → FFT
 *
 * All three image corrections are independent per-path toggles, identical in intent
 * to the LUXSTRAL pipeline but operating on out->polyphonic.grayscale (FFT input).
 *
 * Synth-split P1: all conditioning comes from the LuxSynth OUT bank
 * (g_sp3ctra_config.luxsynth_out[0]) — Negative / DC Blocking / Gamma
 * (photo convention pow(x, 1/gamma), 1.0 = off) + pre-FFT Intensity.
 */
void luxsynth_condition_line(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    int bank_slot,
    float *line_out,
    int nb_pixels
) {
    int i;
    if (bank_slot < 0 || bank_slot >= LUX_OUT_MAX_SLOTS)
        bank_slot = 0;
    const lux_out_params_t *bank = &g_sp3ctra_config.luxsynth_out[bank_slot];

    /* STEP 1: RGB → Grayscale [0.0, 1.0] with preventive clamping */
    for (i = 0; i < nb_pixels; i++) {
        float gray = (0.299f * raw_r[i] + 0.587f * raw_g[i] + 0.114f * raw_b[i]);
        float normalized = gray / 255.0f;
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;
        line_out[i] = normalized;
    }

    /* STEP 2: Inversion — per-OUT bank (synth-split P1). */
    if (bank->negative) {
        for (i = 0; i < nb_pixels; i++)
            line_out[i] = 1.0f - line_out[i];
    }

    /* STEP 3: DC Blocking (AC removal) — subtract per-line mean, clamp [0, 1]. */
    if (bank->dc_blocking) {
        float sum = 0.0f;
        float mean;
        for (i = 0; i < nb_pixels; i++)
            sum += line_out[i];
        mean = sum / (float)nb_pixels;
        for (i = 0; i < nb_pixels; i++) {
            line_out[i] -= mean;
            if (line_out[i] < 0.0f) line_out[i] = 0.0f;
            if (line_out[i] > 1.0f) line_out[i] = 1.0f;
        }
    }

    /* STEP 4: Gamma correction (photo convention: pow(x, 1/gamma)).
     * Per-OUT bank value; 1.0 = mathematical no-op — skip for performance. */
    {
        float gamma = bank->gamma;
        if (gamma > 0.0f && gamma != 1.0f) {
            const float exponent = 1.0f / gamma;
            for (i = 0; i < nb_pixels; i++) {
                float val = line_out[i];
                if (val < 0.0f) val = 0.0f;
                if (val > 1.0f) val = 1.0f;
                float result = powf(val, exponent);
                /* NaN/Inf protection (portable, no isnan/isinf) */
                if (result != result || result * 0.0f != 0.0f)
                    result = val;
                line_out[i] = result;
            }
        }
    }
}

void preprocess_luxsynth(
    const uint8_t *raw_r,
    const uint8_t *raw_g,
    const uint8_t *raw_b,
    PreprocessedImageData *out
) {
    int nb_pixels = get_cis_pixels_nb();
    int i;

    /* STEPS 1-4: conditioning from the LuxSynth OUT bank (slot 0) — shared
     * with the M4 staging producers (luxsynth_condition_line). */
    luxsynth_condition_line(raw_r, raw_g, raw_b, 0,
                            out->polyphonic.grayscale, nb_pixels);

    /* STEP 4b: per-OUT Intensity (synth-split P1) — pre-FFT mix weight of the
     * LuxSynth send.  The FFT is linear, so scaling the line scales the
     * magnitudes; 1.0 = bit-exact parity. */
    {
        float k = g_sp3ctra_config.luxsynth_out[0].intensity;
        if (k < 0.0f) k = 0.0f;
        if (k != 1.0f) {
            for (i = 0; i < nb_pixels; i++)
                out->polyphonic.grayscale[i] *= k;
        }
    }

    /* STEP 5: FFT with temporal smoothing (amplitude) */
    image_preprocess_fft(out);

    /* STEP 6: Color FFT for spectral panning (stereo) */
    image_preprocess_color_fft(raw_r, raw_g, raw_b, out);
}

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
        log_startup_detail("PREPROCESS", "FFT: Initializing KissFFT for %d pixels", nb_pixels);
        
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
        
        log_startup_detail("PREPROCESS", "FFT: Initialized with %d-frame temporal smoothing (%.1fms @ 1kHz)",
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
        log_startup_detail("PREPROCESS", "Color FFT: Initializing KissFFT for %d pixels", nb_pixels);
        
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
        
        log_startup_detail("PREPROCESS", "Color FFT: Initialized with %d-frame temporal smoothing (%.1fms @ 1kHz)",
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
