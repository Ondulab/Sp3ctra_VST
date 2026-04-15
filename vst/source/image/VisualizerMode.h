/**
 * @file VisualizerMode.h
 * @brief Visualization modes for the CIS visualizer.
 *
 * 12 modes, each activated by clicking a pipeline node in the Image page tabs.
 * The active mode is persisted in the APVTS parameter "visualizerMode".
 */
#pragma once

enum class VisualizerMode
{
    RAW = 0,            // Raw UDP stream (always live, independent of transport)
    LIVE,               // Live stream (subject to play/pause/stop transport)
    SAMPLER,            // Sampler frame from active slot
    MIX,                // Darken-blend of S and L with opacities
    SPCTR_GRAY,         // LuxStral grayscale: additive.notes[N] with gamma
    SYNTH_GRAY,         // LuxSynth grayscale: polyphonic.grayscale[3456] linear
    SPCTR_COLOR,        // LuxStral color temperature: blue (cold) → red (warm)
    SYNTH_COLOR,        // LuxSynth color temperature per-pixel (pre-FFT)
    SYNTH_FFT_GRAY,     // LuxSynth FFT magnitudes: polyphonic.magnitudes[128]
    SYNTH_FFT_COLOR,    // LuxSynth FFT color temperature: polyphonic positions
    SPCTR_BLOB,         // StrokeForge overlay on LuxStral path
    SYNTH_BLOB,         // StrokeForge overlay on LuxSynth path
    COUNT
};

/**
 * @brief Human-readable short label for each visualizer mode.
 */
inline const char* visualizerModeLabel(VisualizerMode m)
{
    switch (m)
    {
        case VisualizerMode::RAW:            return "RAW";
        case VisualizerMode::LIVE:           return "LIVE";
        case VisualizerMode::SAMPLER:        return "SAMPLER";
        case VisualizerMode::MIX:            return "MIX";
        case VisualizerMode::SPCTR_GRAY:     return "LUXSTRAL GRAY";
        case VisualizerMode::SYNTH_GRAY:     return "SYNTH GRAY";
        case VisualizerMode::SPCTR_COLOR:    return "LUXSTRAL COLOR";
        case VisualizerMode::SYNTH_COLOR:    return "SYNTH COLOR";
        case VisualizerMode::SYNTH_FFT_GRAY: return "FFT GRAY";
        case VisualizerMode::SYNTH_FFT_COLOR: return "FFT COLOR";
        case VisualizerMode::SPCTR_BLOB:     return "LUXSTRAL BLOB";
        case VisualizerMode::SYNTH_BLOB:     return "SYNTH BLOB";
        default:                             return "???";
    }
}
