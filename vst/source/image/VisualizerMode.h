/**
 * @file VisualizerMode.h
 * @brief Visualization modes for the CIS visualizer.
 *
 * Activated by clicking a pipeline node in the Image page tabs.
 * The active mode is persisted in the APVTS parameter "visualizerMode".
 *
 * Channel model (since the "Modulated / Live" refactor):
 *   • RAW       — pure UDP feed, never affected by transport.
 *   • LIVE      — Channel B : direct UDP frame (subject to transport state).
 *   • MODULATED — Channel A : Live ► LuxSampler ► LuxPitch ► LuxMask.
 *                 Each insert auto-bypasses when inactive.
 *
 * The legacy SAMPLER and MIX entries are kept as deprecated aliases that
 * silently map to MODULATED so old presets and stored visualiser modes keep
 * loading.  They no longer correspond to distinct visualisations.
 */
#pragma once

enum class VisualizerMode
{
    RAW = 0,            // Raw UDP stream (always live, independent of transport)
    LIVE,               // Channel B — direct live frame (subject to transport)
    MODULATED,          // Channel A — Live ► LuxSampler ► LuxPitch ► LuxMask
    SPCTR_GRAY,         // LuxStral grayscale: additive.notes[N] with gamma
    SYNTH_GRAY,         // LuxSynth grayscale: polyphonic.grayscale[3456] linear
    SPCTR_COLOR,        // LuxStral color temperature: blue (cold) → red (warm)
    SYNTH_COLOR,        // LuxSynth color temperature per-pixel (pre-FFT)
    SYNTH_FFT_COLOR,    // LuxSynth FFT magnitudes + colour-coded harmonicity
    SPCTR_BLOB,         // StrokeForge overlay on LuxStral path
    SYNTH_BLOB,         // StrokeForge overlay on LuxSynth path
    LUXPITCH_OUTPUT,    // LuxPitch shifted image output (insert preview)
    LUXMASK_OUTPUT,     // LuxMask spotlight image output  (insert preview)
    COUNT,

    /* ── Deprecated aliases — silently map to MODULATED ────────────────── */
    SAMPLER = MODULATED,
    MIX     = MODULATED
};

/**
 * @brief Human-readable short label for each visualizer mode.
 */
inline const char* visualizerModeLabel(VisualizerMode m)
{
    switch (m)
    {
        case VisualizerMode::RAW:             return "RAW";
        case VisualizerMode::LIVE:            return "CHAIN 2";
        case VisualizerMode::MODULATED:       return "CHAIN 1";
        case VisualizerMode::SPCTR_GRAY:      return "LUXSTRAL GRAY";
        case VisualizerMode::SYNTH_GRAY:      return "LUXSYNTH GRAY";
        case VisualizerMode::SPCTR_COLOR:     return "LUXSTRAL COLOR";
        case VisualizerMode::SYNTH_COLOR:     return "LUXSYNTH COLOR";
        case VisualizerMode::SYNTH_FFT_COLOR: return "LUXSYNTH FFT";
        case VisualizerMode::SPCTR_BLOB:      return "LUXSTRAL BLOB";
        case VisualizerMode::SYNTH_BLOB:      return "LUXSYNTH BLOB";
        case VisualizerMode::LUXPITCH_OUTPUT: return "LUXPITCH";
        case VisualizerMode::LUXMASK_OUTPUT:  return "LUXMASK";
        default:                              return "???";
    }
}
