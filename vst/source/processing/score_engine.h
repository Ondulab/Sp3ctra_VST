/*
 * score_engine.h — Offline spectrogram "graphical score" engine (VST port)
 *
 * Port of the legacy Sp3ctraGen audio→spectrogram engine (spectral_fft.c /
 * spectral_wav_processing.c) adapted for the JUCE plugin:
 *   - FFTW3            → KissFFT (kiss_fftr, already vendored in this repo)
 *   - libsndfile       → juce::AudioFormatReader (done in ScoreGenRenderer)
 *   - Cairo / file I/O → juce::Image / juce::Graphics (done in ScoreGenRenderer)
 *
 * This engine produces a printable greyscale spectrogram destined to be printed
 * then optically scanned by the Sp3ctra instrument. The frequency axis is LINEAR
 * (not logarithmic) — this is essential for faithful audio reconstruction and
 * must not be changed.
 *
 * Pure C, no external dependency beyond <math.h> and KissFFT. Safe to call off
 * the message thread (no globals, no file I/O, no Qt).
 */
#ifndef SCORE_ENGINE_H
#define SCORE_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Zero-padding FFT size. The legacy engine padded to 65535 (odd) via FFTW r2c;
 * KissFFT's kiss_fftr requires an EVEN nfft, so we use 65536 = 2^16 (the fastest
 * and most precise case for KissFFT). The frequency-resolution difference is
 * sr/65535 vs sr/65536 (~1.5e-5 relative) — negligible. */
#define SCORE_FFT_EFFECTIVE_SIZE 65536

/* Default parameters — tuned to MATCH the PhonoPaper / Virtual ANS encoder
 * (the reference that resynthesises cleanly through LuxStral). PhonoPaper applies
 * a plain dB magnitude→brightness map over a 50 dB window with NO gamma, NO
 * contrast stretch, NO HF pre-emphasis and NO extra gate (the dB floor IS the
 * gate). It uses a Hann window of 4096 samples; we force a comparable useful FFT
 * size so harmonics are resolved (~10 Hz) instead of smeared (~43 Hz at auto). */
#define SCORE_DEFAULT_MIN_FREQ          65.0
#define SCORE_DEFAULT_MAX_FREQ          16640.0
#define SCORE_DEFAULT_DYNAMIC_RANGE_DB  50.0   /* PhonoPaper: −60..−10 dB = 50 dB */
#define SCORE_DEFAULT_GAMMA             1.0     /* PhonoPaper: none (linear dB map) */
#define SCORE_DEFAULT_CONTRAST          1.0     /* PhonoPaper: none */
#define SCORE_DEFAULT_FFT_SIZE          4096    /* PhonoPaper window size */
/* Optional noise gate (OFF to match PhonoPaper, whose dB floor already gates).
 * When enabled: normalized-intensity floor in [0,1[ below which a bin → silence. */
#define SCORE_DEFAULT_NOISE_GATE        0
#define SCORE_DEFAULT_NOISE_GATE_THRESH 0.18
#define SCORE_DEFAULT_BINS_PER_SECOND   150.0
#define SCORE_DEFAULT_OVERLAP_PRESET    2     /* 0=Low 1=Medium 2=High */
#define SCORE_DEFAULT_PRINTER_DPI       400.0
#define SCORE_DEFAULT_HIGH_BOOST_ALPHA  0.99
#define SCORE_DEFAULT_BOTTOM_MARGIN_MM  50.8
#define SCORE_DEFAULT_SPECTRO_HEIGHT_MM 216.7

#define SCORE_OVERLAP_LOW               0.50
#define SCORE_OVERLAP_MEDIUM            0.85
#define SCORE_OVERLAP_HIGH              0.95

#define SCORE_MIN_BINS_PER_SECOND       10.0
#define SCORE_MAX_BINS_PER_SECOND       1200.0

/* Page geometry (mm). */
#define SCORE_A4_WIDTH_MM   210.0
#define SCORE_A4_HEIGHT_MM  297.0
#define SCORE_A3_WIDTH_MM   420.0
#define SCORE_A3_HEIGHT_MM  297.0

/*-----------------------------------------------------------------------------
 * Generation settings — a flat C struct so it can be filled from C++ and passed
 * to the engine. Defaults match the legacy SpectrogramSettings.
 *---------------------------------------------------------------------------*/
typedef struct ScoreSettings
{
    double minFreq;             /* Hz, bottom of the image  (65)            */
    double maxFreq;             /* Hz, top of the image     (16640)         */
    double dynamicRangeDB;      /* dB floor below the max   (60)            */
    double gammaCorrection;     /* >0 ; <1 lifts low levels (0.8)           */
    double contrastFactor;      /* stretch around 0.5       (1.9)           */
    int    enableDithering;     /* 0/1                                       */
    double binsPerSecond;       /* time columns/s (used when writingSpeed=0)*/
    int    overlapPreset;       /* 0/1/2 → 0.50/0.85/0.95                    */
    double printerDpi;          /* output resolution        (400)           */
    int    pageFormat;          /* 0=A4 portrait, 1=A3 landscape            */
    double writingSpeed;        /* cm/s ; 0 ⇒ use binsPerSecond directly     */
    double spectroHeightMM;     /* height of the spectro band (216.7)       */
    double bottomMarginMM;      /* band offset above page bottom (50.8)     */
    int    enableHighBoost;     /* 0/1 — HF pre-emphasis (bass-cut/treble tilt)*/
    double highBoostAlpha;      /* 0..1 (0.99)                              */
    int    enableNoiseGate;     /* 0/1 — silence bins below gate (audio)     */
    double noiseGateThreshold;  /* [0,1[ normalized-intensity floor (0.18)   */
    int    enableHighPassFilter;/* 0/1                                       */
    double highPassCutoffFreq;  /* Hz                                        */
    int    highPassFilterOrder; /* 1..12 (passes)                           */
    int    enableNormalization; /* 0/1 — normalise to peak 1.0 before FFT    */
    int    fftSize;             /* useful window size; 0 ⇒ auto from bps     */
} ScoreSettings;

/* Fills *s with the legacy defaults. */
void score_settings_defaults(ScoreSettings *s);

/*-----------------------------------------------------------------------------
 * Spectrogram matrix produced by score_compute_spectrogram().
 * data layout: row-major [num_windows][num_bins], value = magnitude, then
 * overwritten in place with greyscale intensity by score_apply_image_processing.
 *---------------------------------------------------------------------------*/
typedef struct ScoreSpectrogramData
{
    double *data;        /* malloc'd, num_windows * num_bins doubles        */
    int     num_windows; /* time columns                                    */
    int     num_bins;    /* SCORE_FFT_EFFECTIVE_SIZE/2 + 1                   */
    int     index_min;   /* first visible bin (from minFreq)                */
    int     index_max;   /* last visible bin  (from maxFreq)                */
    double  global_max;  /* peak magnitude (for dB normalisation)           */
} ScoreSpectrogramData;

/*-----------------------------------------------------------------------------
 * Signal pre-processing (in place). Port of spectral_wav_processing.c.
 *---------------------------------------------------------------------------*/
void score_apply_high_freq_boost(double *signal, int num_samples, double alpha);
void score_apply_highpass(double *signal, int num_samples, int sample_rate,
                          double cutoff_freq, int order);

/*-----------------------------------------------------------------------------
 * Core STFT. Returns 0 on success, non-zero on error (e.g. signal too short).
 * On success out->data is malloc'd; free it with score_free_spectrogram().
 *---------------------------------------------------------------------------*/
int score_compute_spectrogram(const double *signal, int total_samples,
                              int sample_rate, int fft_size, double bins_per_second,
                              double min_freq, double max_freq,
                              ScoreSpectrogramData *out);

/* Maps magnitude → inverted greyscale intensity in [0,1] (white = silence).
 * Operates in place on data->data over [index_min,index_max]. */
void score_apply_image_processing(ScoreSpectrogramData *data,
                                  double dynamic_range_db, double gamma_correction,
                                  int enable_dither, double contrast_factor,
                                  int enable_noise_gate, double noise_gate_threshold);

void score_free_spectrogram(ScoreSpectrogramData *data);

/* Test helper: fill signal with a sine wave (used to validate the FFT port). */
void score_generate_sine(double *signal, int total_samples, double sample_rate,
                         double frequency, double amplitude);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SCORE_ENGINE_H */
