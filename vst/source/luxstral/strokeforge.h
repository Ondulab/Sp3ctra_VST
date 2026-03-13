/*
 * strokeforge.h
 *
 * StrokeForge — Blob-centric harmonic morphing for additive synthesis
 *
 * The physical stroke shape on the CIS scanner synesthetically determines
 * the timbre of the sound:
 *   - Thin pencil line  → pure sine (few harmonics)
 *   - Thick marker mark → square-like (rich harmonics, sharp edges)
 *   - Gradient brush    → triangle-like (soft harmonics)
 *   - Asymmetric stroke → sawtooth-like (all harmonics)
 *
 * Architecture:
 *   1. Blob detection in preprocessor (non-RT)
 *   2. Shape descriptors extraction (flatness, symmetry, edge sharpness)
 *   3. Harmonic recipe computation from shape descriptors
 *   4. Phase coherence within blobs (RT path)
 *   5. Harmonic injection in synthesis workers (RT path)
 *
 * Author: zhonx
 * Created: 2026-03-13
 */

#ifndef STROKEFORGE_H
#define STROKEFORGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************
 * Constants
 **************************************************************************************/

/* Maximum number of simultaneous blobs per frame */
#define STROKEFORGE_MAX_BLOBS 256

/* Maximum harmonics per blob (must be <= 16) */
#define STROKEFORGE_MAX_HARMONICS 16

/* Maximum notes — must match PREPROCESS_MAX_NOTES / CIS_MAX_PIXELS_NB */
#define STROKEFORGE_MAX_NOTES 4096

/* Sentinel value: note does not belong to any blob */
#define STROKEFORGE_NO_BLOB (-1)

/* Sentinel value: phase is free-running (not locked) */
#define STROKEFORGE_PHASE_FREE (-1.0f)

/* Minimum blob width (notes) to activate wavetable morphing mode */
#define STROKEFORGE_WAVETABLE_MIN_WIDTH 50

/**************************************************************************************
 * Frame Processing Mode
 *
 * Set once per frame in strokeforge_analyze_frame().
 * The RT engine reads this to decide how to apply StrokeForge data.
 *
 *  BYPASS       — Complex scene (multiple blobs or no blob): pure spectral passthrough.
 *                 No amplitude changes, no phase changes. Conflict-free.
 *
 *  PHASE_SMOOTH — Single thin blob: gentle phase coherence between neighboring notes
 *                 within the blob. No amplitude override. Non-intrusive.
 *
 *  WAVETABLE    — Single wide isolated blob: pulse-wave wavetable morphing.
 *                 Spectral amplitude within blob range is SUPPRESSED, replaced by
 *                 StrokeForge harmonic recipe. No conflicts outside blob.
 **************************************************************************************/
typedef enum
{
    STROKEFORGE_MODE_BYPASS       = 0, /* Complex scene → pure spectral passthrough    */
    STROKEFORGE_MODE_PHASE_SMOOTH = 1, /* Single thin blob → phase hints only          */
    STROKEFORGE_MODE_WAVETABLE    = 2  /* Single wide blob → wavetable morph, full ctrl */
} StrokeForgeMode;

/**************************************************************************************
 * Blob Shape Descriptors
 *
 * Extracted from the amplitude profile of each blob.
 * These drive the harmonic recipe synesthetically:
 *
 *   flatness       → how many harmonics (peaked=sine, flat=rich)
 *   symmetry       → odd-only vs all harmonics
 *   edge_sharpness → harmonic rolloff rate (sharp=slow, soft=fast)
 *   morph_depth    → combined depth (0=sine, 1=maximally rich)
 **************************************************************************************/
typedef struct
{
    float flatness;       /* 0.0 = peaked (gaussian/sine), 1.0 = flat (rectangular/square) */
    float symmetry;       /* 1.0 = symmetric (odd harmonics), 0.0 = asymmetric (all)       */
    float edge_sharpness; /* 0.0 = soft edges (triangle), 1.0 = sharp edges (square)       */
    float morph_depth;    /* Combined: 0.0 = pure sine, 1.0 = maximally rich               */
} StrokeForgeDescriptors;

/**************************************************************************************
 * Per-Blob Data
 *
 * Computed in preprocessor (non-RT), consumed by synthesis workers (RT).
 **************************************************************************************/
typedef struct
{
    int active; /* 1 if blob is valid, 0 otherwise */

    /* Spatial extent */
    int center_note; /* Centroid note index (amplitude-weighted)              */
    int start_note;  /* First note of blob (inclusive)                        */
    int end_note;    /* Last note of blob (exclusive)                         */

    /* Amplitude */
    float peak_amplitude;     /* Maximum amplitude in blob → reference volume */
    float weighted_amplitude; /* Gaussian-weighted amplitude → actual volume   */
    float width_notes;        /* Width in number of notes (float for precision)*/

    /* Shape descriptors (synesthetic mapping) */
    StrokeForgeDescriptors shape;

    /* Pre-computed harmonic amplitudes for RT path */
    float harmonic_amplitudes[STROKEFORGE_MAX_HARMONICS]; /* [0]=fundamental, [1]=2nd, ... */
    int   harmonic_count; /* Number of active harmonics (1 to STROKEFORGE_MAX_HARMONICS)   */

    /* Target harmonic note indices (closest oscillator to each harmonic freq) */
    int harmonic_note_indices[STROKEFORGE_MAX_HARMONICS]; /* -1 if beyond Nyquist */

} StrokeForgeBlob;

/**************************************************************************************
 * Frame-Level Blob Data
 *
 * Stored in PreprocessedImageData, copied to workers via DoubleBuffer.
 **************************************************************************************/
typedef struct
{
    /* Detected blobs for this frame */
    StrokeForgeBlob blobs[STROKEFORGE_MAX_BLOBS];
    int blob_count;

    /* Frame processing mode — set by strokeforge_analyze_frame() */
    StrokeForgeMode frame_mode;

    /*
     * Per-note lookup tables (indexed by note index 0..num_notes-1)
     * These are filled by strokeforge_analyze_frame() for O(1) RT access.
     */

    /* Which blob does this note belong to? (-1 = none / free oscillator) */
    int16_t note_to_blob[STROKEFORGE_MAX_NOTES];

    /* Is this note a harmonic target generated by some blob? */
    int16_t note_is_harmonic_of_blob[STROKEFORGE_MAX_NOTES]; /* blob index, -1 if not     */
    int8_t  note_harmonic_rank[STROKEFORGE_MAX_NOTES];       /* 1=fund, 2=2nd, 3=3rd, ... */

    /* Target amplitude from blob harmonic recipe (overrides raw notes[n]) */
    float note_harmonic_amplitude[STROKEFORGE_MAX_NOTES];

    /* Target phase for coherence within blob (STROKEFORGE_PHASE_FREE = free-running) */
    float note_target_phase[STROKEFORGE_MAX_NOTES];

} StrokeForgeFrameData;

/**************************************************************************************
 * Public API
 **************************************************************************************/

/**
 * @brief Initialize StrokeForge module.
 * Called once at startup. Non-RT safe.
 */
void strokeforge_init(void);

/**
 * @brief Cleanup StrokeForge module.
 * Called at shutdown. Non-RT safe.
 */
void strokeforge_cleanup(void);

/**
 * @brief Detect blobs and compute descriptors + harmonic recipe for one frame.
 *
 * Called from image_preprocessor.c after additive preprocessing (preprocess_luxstral).
 * Non-RT safe (runs in preprocessor / UDP thread).
 *
 * @param notes            Preprocessed per-note amplitudes [0.0, 1.0]
 * @param num_notes        Number of notes
 * @param contrast_factor  Contrast factor from additive preprocessing [0.0, 1.0]
 * @param out              Output frame data (blobs + per-note lookups)
 */
void strokeforge_analyze_frame(
    const float *notes,
    int num_notes,
    float contrast_factor,
    StrokeForgeFrameData *out);

#ifdef __cplusplus
}
#endif

#endif /* STROKEFORGE_H */
