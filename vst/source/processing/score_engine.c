/*
 * score_engine.c — see score_engine.h.
 *
 * Faithful port of the legacy Sp3ctraGen STFT + greyscale-mapping engine, with
 * FFTW replaced by KissFFT (kiss_fftr). The Blackman-Harris window, log-amplitude
 * dB mapping, gamma, inversion (white = silence) and contrast are copied
 * verbatim to preserve the printed/scanned reconstruction fidelity.
 */
#include "score_engine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "synthesis/luxsynth/kissfft/kiss_fftr.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Compile-time switch kept identical to the legacy engine.
 * (The frequency AXIS of the printed image is handled by the renderer:
 * ScoreGenRenderer maps rows to bin cells on a LOG axis matching the synth's
 * log-distributed oscillator bank. The engine data stays on the linear FFT
 * bin grid.) */
#define SCORE_USE_LOG_AMPLITUDE 1

/*---------------------------------------------------------------------------*/
void score_settings_defaults(ScoreSettings *s)
{
    if (s == NULL) return;
    s->minFreq             = SCORE_DEFAULT_MIN_FREQ;
    s->maxFreq             = SCORE_DEFAULT_MAX_FREQ;
    s->dynamicRangeDB      = SCORE_DEFAULT_DYNAMIC_RANGE_DB;
    s->gammaCorrection     = SCORE_DEFAULT_GAMMA;
    s->contrastFactor      = SCORE_DEFAULT_CONTRAST;
    s->enableDithering     = 0;
    s->binsPerSecond       = SCORE_DEFAULT_BINS_PER_SECOND;
    s->overlapPreset       = SCORE_DEFAULT_OVERLAP_PRESET;
    s->printerDpi          = SCORE_DEFAULT_PRINTER_DPI;
    s->pageFormat          = 0;
    s->writingSpeed        = 0.0;
    s->spectroHeightMM     = SCORE_DEFAULT_SPECTRO_HEIGHT_MM;
    s->spectroHeightManual = 0;   /* locked to the CIS sensor length by default */
    s->bottomMarginMM      = SCORE_DEFAULT_BOTTOM_MARGIN_MM;
    s->enableHighBoost     = 0;   /* PhonoPaper applies no HF pre-emphasis */
    s->highBoostAlpha      = SCORE_DEFAULT_HIGH_BOOST_ALPHA;
    s->enableNoiseGate     = SCORE_DEFAULT_NOISE_GATE;
    s->noiseGateThreshold  = SCORE_DEFAULT_NOISE_GATE_THRESH;
    s->enableHighPassFilter= 0;
    s->highPassCutoffFreq  = 100.0;
    s->highPassFilterOrder = 2;
    s->enableNormalization = 1;
    s->fftSize             = SCORE_DEFAULT_FFT_SIZE;  /* match PhonoPaper's 4096 window */
    s->startTimeSec        = 0.0;
    s->enableStereoMode    = 0;
    s->enableMultiRes      = 0;
}

/*---------------------------------------------------------------------------
 * Blackman-Harris 4-term window (copied verbatim from the legacy engine).
 *-------------------------------------------------------------------------*/
static void score_apply_blackman_harris_window(double *buffer, int size)
{
    const double a0 = 0.35875;
    const double a1 = 0.48829;
    const double a2 = 0.14128;
    const double a3 = 0.01168;

    for (int i = 0; i < size; i++)
    {
        double n = (double)i / (size - 1);
        double window = a0
                      - a1 * cos(2.0 * M_PI * n)
                      + a2 * cos(4.0 * M_PI * n)
                      - a3 * cos(6.0 * M_PI * n);
        buffer[i] *= window;
    }
}

/*---------------------------------------------------------------------------
 * HF pre-emphasis: y[n] = x[n] - alpha * x[n-1] (legacy verbatim).
 *-------------------------------------------------------------------------*/
void score_apply_high_freq_boost(double *signal, int num_samples, double alpha)
{
    if (num_samples < 2) return;

    double prev_sample = signal[0];
    for (int i = 1; i < num_samples; i++)
    {
        double current_sample = signal[i];
        signal[i] = current_sample - alpha * prev_sample;
        prev_sample = current_sample;
    }
}

/*---------------------------------------------------------------------------
 * Simple 1st-order high-pass applied "order" times (legacy verbatim, design +
 * apply collapsed into one call). y[n] = alpha * (y[n-1] + x[n] - x[n-1]).
 *-------------------------------------------------------------------------*/
void score_apply_highpass(double *signal, int num_samples, int sample_rate,
                          double cutoff_freq, int order)
{
    if (num_samples < 2) return;
    if (cutoff_freq <= 0.0) cutoff_freq = 100.0;
    if (order < 1) order = 1;
    if (order > 12) order = 12;

    double rc = 1.0 / (2.0 * M_PI * cutoff_freq);
    double dt = 1.0 / (double)sample_rate;
    double alpha = rc / (rc + dt);
    if (alpha < 0.1)  alpha = 0.1;
    if (alpha > 0.95) alpha = 0.95;

    /* Peak before filtering (for the legacy renormalisation heuristic). */
    double max_amplitude = 0.0;
    for (int i = 0; i < num_samples; i++)
    {
        double a = fabs(signal[i]);
        if (a > max_amplitude) max_amplitude = a;
    }

    for (int pass = 0; pass < order; pass++)
    {
        double prev_x = signal[0];
        double y = alpha * signal[0];
        signal[0] = y;
        double prev_y = y;

        for (int i = 1; i < num_samples; i++)
        {
            double x = signal[i];
            y = alpha * (prev_y + x - prev_x);
            prev_x = x;
            prev_y = y;

            if (isnan(y) || isinf(y)) y = 0.0;
            else if (y >  10.0)       y =  10.0;
            else if (y < -10.0)       y = -10.0;

            signal[i] = y;
        }
    }

    double max_filtered = 0.0;
    for (int i = 0; i < num_samples; i++)
    {
        double a = fabs(signal[i]);
        if (a > max_filtered) max_filtered = a;
    }

    if (max_filtered > 0.0 &&
        (max_filtered < 0.01 * max_amplitude || max_filtered > 2.0 * max_amplitude))
    {
        double k = max_amplitude / max_filtered;
        for (int i = 0; i < num_samples; i++)
            signal[i] *= k;
    }
}

/*---------------------------------------------------------------------------
 * STFT magnitude spectrogram (KissFFT port of compute_spectrogram).
 * _ex variant: configurable zero-pad size, window-center alignment to a
 * longer reference window (multi-resolution layers), optional coherent-gain
 * normalization. See score_engine.h.
 *-------------------------------------------------------------------------*/
int score_compute_spectrogram_ex(const double *signal, int total_samples,
                                 int sample_rate, int fft_size,
                                 int fft_pad_size, int align_fft_size,
                                 int normalize_gain, double bins_per_second,
                                 double min_freq, double max_freq,
                                 ScoreSpectrogramData *out)
{
    if (signal == NULL || out == NULL || fft_size <= 0) return 1;
    if (fft_pad_size < fft_size) fft_pad_size = fft_size;
    if (fft_pad_size & 1) fft_pad_size++;            /* kiss_fftr needs even */
    if (align_fft_size < fft_size) align_fft_size = fft_size;
    if (bins_per_second < 1.0) bins_per_second = 1.0;

    const int fft_effective_size = fft_pad_size;
    const int num_bins = fft_effective_size / 2 + 1;

    int step = (int)((double)sample_rate / bins_per_second);
    if (step < 1) step = 1;

    /* Frames are positioned so their CENTERS coincide with those of the
     * reference (longest-window) layer: start = w·step + (align − size)/2. */
    const int center_offset = (align_fft_size - fft_size) / 2;
    int num_windows = (total_samples - align_fft_size) / step + 1;
    if (num_windows <= 0) return 2;   /* signal too short for the FFT window */

    double freq_resolution = (double)sample_rate / (double)fft_effective_size;

    int index_min = (int)ceil(min_freq / freq_resolution);
    int index_max = (int)floor(max_freq / freq_resolution);
    if (index_min < 0) index_min = 0;
    if (index_max > num_bins - 1) index_max = num_bins - 1;
    if (index_min >= index_max) { index_min = 0; index_max = num_bins - 1; }

    kiss_fftr_cfg cfg = kiss_fftr_alloc(fft_effective_size, 0, NULL, NULL);
    if (cfg == NULL) return 3;

    kiss_fft_scalar *in  = (kiss_fft_scalar *)calloc((size_t)fft_effective_size,
                                                     sizeof(kiss_fft_scalar));
    kiss_fft_cpx    *fout= (kiss_fft_cpx *)malloc((size_t)num_bins * sizeof(kiss_fft_cpx));
    double          *win = (double *)malloc((size_t)fft_size * sizeof(double));
    double *spectrogram  = (double *)malloc((size_t)num_windows * (size_t)num_bins
                                            * sizeof(double));

    if (in == NULL || fout == NULL || win == NULL || spectrogram == NULL)
    {
        free(in); free(fout); free(win); free(spectrogram);
        kiss_fftr_free(cfg);
        return 4;
    }

    /* Coherent gain of the Blackman-Harris window (Σ w[i]) — a sinusoid's
     * peak magnitude scales with it, so dividing by it makes levels directly
     * comparable across layers with different window sizes. */
    double mag_scale = 1.0;
    if (normalize_gain)
    {
        double window_sum = 0.0;
        for (int i = 0; i < fft_size; i++)
            win[i] = 1.0;
        score_apply_blackman_harris_window(win, fft_size);
        for (int i = 0; i < fft_size; i++)
            window_sum += win[i];
        if (window_sum > 0.0)
            mag_scale = 1.0 / window_sum;
    }

    double global_max = 0.0;

    for (int w = 0; w < num_windows; w++)
    {
        int start = w * step + center_offset;

        /* Copy frame (double), then window in double for precision. */
        for (int i = 0; i < fft_size; i++)
            win[i] = (start + i < total_samples) ? signal[start + i] : 0.0;

        score_apply_blackman_harris_window(win, fft_size);

        /* Cast windowed frame to float and zero-pad the remainder. */
        for (int i = 0; i < fft_size; i++)
            in[i] = (kiss_fft_scalar)win[i];
        for (int i = fft_size; i < fft_effective_size; i++)
            in[i] = (kiss_fft_scalar)0;

        kiss_fftr(cfg, in, fout);

        for (int b = 0; b < num_bins; b++)
        {
            double re = (double)fout[b].r;
            double im = (double)fout[b].i;
            double mag = sqrt(re * re + im * im) * mag_scale;
            spectrogram[(size_t)w * num_bins + b] = mag;
            if (mag > global_max) global_max = mag;
        }
    }

    free(in);
    free(fout);
    free(win);
    kiss_fftr_free(cfg);

    out->data        = spectrogram;
    out->num_windows = num_windows;
    out->num_bins    = num_bins;
    out->index_min   = index_min;
    out->index_max   = index_max;
    out->global_max  = global_max;
    return 0;
}

/* Legacy single-layer entry — exact historical behaviour. */
int score_compute_spectrogram(const double *signal, int total_samples,
                              int sample_rate, int fft_size, double bins_per_second,
                              double min_freq, double max_freq,
                              ScoreSpectrogramData *out)
{
    return score_compute_spectrogram_ex(signal, total_samples, sample_rate,
                                        fft_size, SCORE_FFT_EFFECTIVE_SIZE,
                                        fft_size, 0, bins_per_second,
                                        min_freq, max_freq, out);
}

/*---------------------------------------------------------------------------
 * Magnitude → inverted greyscale intensity (copied verbatim from legacy
 * apply_image_processing). White = silence, black = energy.
 *-------------------------------------------------------------------------*/
void score_apply_image_processing(ScoreSpectrogramData *data,
                                  double dynamic_range_db, double gamma_correction,
                                  int enable_dither, double contrast_factor,
                                  int enable_noise_gate, double noise_gate_threshold)
{
    if (data == NULL || data->data == NULL) return;

    int num_windows = data->num_windows;
    int num_bins    = data->num_bins;
    int index_min   = data->index_min;
    int index_max   = data->index_max;
    double global_max = data->global_max;
    double *spectrogram = data->data;

    if (dynamic_range_db <= 0.0) dynamic_range_db = SCORE_DEFAULT_DYNAMIC_RANGE_DB;
    if (gamma_correction <= 0.0) gamma_correction = SCORE_DEFAULT_GAMMA;
    if (contrast_factor  <= 0.0) contrast_factor  = SCORE_DEFAULT_CONTRAST;

    /* Gate operates on the normalized intensity (1 = peak energy, 0 = floor).
     * gate_span renormalises the surviving [thr,1] window back to [0,1] so the
     * audible content keeps full contrast after the floor is removed. */
    if (noise_gate_threshold < 0.0)  noise_gate_threshold = 0.0;
    if (noise_gate_threshold > 0.95) noise_gate_threshold = 0.95;
    const double gate_span = (1.0 - noise_gate_threshold > 1e-6)
                             ? (1.0 - noise_gate_threshold) : 1.0;

    if (enable_dither)
        srand(1u);   /* fixed seed → deterministic export (legacy used time()) */

    for (int w = 0; w < num_windows; w++)
    {
        for (int b = index_min; b <= index_max; b++)
        {
            double magnitude = spectrogram[(size_t)w * num_bins + b];
            double intensity = 0.0;
            double epsilon = 1e-10;

#if SCORE_USE_LOG_AMPLITUDE
            double dB     = 20.0 * log10(magnitude + epsilon);
            double max_dB = 20.0 * log10(global_max + epsilon);
            double min_dB = max_dB - dynamic_range_db;
            intensity = (dB - min_dB) / (max_dB - min_dB);
            if (intensity < 0.0) intensity = 0.0;
            if (intensity > 1.0) intensity = 1.0;
#else
            intensity = (global_max > 0.0) ? (magnitude / global_max) : 0.0;
#endif

            /* Noise gate: drop the broadband floor to true silence, then expand
             * the survivors so the formants keep their dynamics. Applied before
             * gamma/contrast so those still shape the audible band only. */
            if (enable_noise_gate)
            {
                if (intensity <= noise_gate_threshold)
                    intensity = 0.0;
                else
                    intensity = (intensity - noise_gate_threshold) / gate_span;
            }

            if (gamma_correction != 1.0)
                intensity = pow(intensity, 1.0 / gamma_correction);

            double inverted = 1.0 - intensity;
            double quantized = inverted * 255.0;

            if (enable_dither)
            {
                double dither = ((double)rand() / (double)RAND_MAX) - 0.5;
                quantized += dither;
            }
            if (quantized < 0.0)   quantized = 0.0;
            if (quantized > 255.0) quantized = 255.0;

            double final_intensity = quantized / 255.0;
            final_intensity = (final_intensity - 0.5) * contrast_factor + 0.5;
            if (final_intensity < 0.0) final_intensity = 0.0;
            if (final_intensity > 1.0) final_intensity = 1.0;

            spectrogram[(size_t)w * num_bins + b] = final_intensity;
        }
    }
}

/*---------------------------------------------------------------------------*/
void score_free_spectrogram(ScoreSpectrogramData *data)
{
    if (data == NULL) return;
    free(data->data);
    data->data = NULL;
    data->num_windows = 0;
    data->num_bins = 0;
}
