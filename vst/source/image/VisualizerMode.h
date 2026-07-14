/**
 * @file VisualizerMode.h
 * @brief Visualization modes for the CIS visualizer (zone-1 panels).
 *
 * P4-M5: the legacy bus modes (RAW / LIVE / MODULATED and the SAMPLER / MIX
 * aliases, LUXPITCH_OUTPUT / LUXMASK_OUTPUT insert previews) are GONE —
 * every module selection is contextual (SELECTED_TAP: the stream at the
 * selected module's position in ITS chain) and the engine views read the
 * per-engine input taps. This enum is never persisted (the APVTS
 * "visualizerMode" parameter is the RENDER STYLE — Image/Waveform — not a
 * bus selection), so entries can be removed freely.
 */
#pragma once

enum class VisualizerMode
{
    SPCTR_GRAY = 0,     // LuxStral grayscale: additive.notes[N] with gamma
    SYNTH_GRAY,         // LuxSynth grayscale: polyphonic.grayscale[3456] linear
    SPCTR_COLOR,        // LuxStral color temperature: blue (cold) → red (warm)
    SYNTH_COLOR,        // LuxSynth color temperature per-pixel (pre-FFT)
    SYNTH_FFT_COLOR,    // LuxSynth FFT magnitudes + colour-coded harmonicity
    SPCTR_BLOB,         // StrokeForge overlay on the LuxStral view
    SYNTH_BLOB,         // StrokeForge overlay on the LuxSynth view
    SRC_IMAGE,          // IMAGE module's own line (internal source pool, M9)
    SRC_VIDEO,          // VIDEO module's own line (internal source pool, M9)
    SRC_CAMERA,         // CAMERA module's own line (internal source pool, M9)
    SELECTED_TAP,       // stream AT the selected module's position in ITS chain
    COUNT
};

/**
 * @brief Human-readable short label for each visualizer mode.
 */
inline const char* visualizerModeLabel(VisualizerMode m)
{
    switch (m)
    {
        case VisualizerMode::SPCTR_GRAY:      return "LUXSTRAL GRAY";
        case VisualizerMode::SYNTH_GRAY:      return "LUXSYNTH GRAY";
        case VisualizerMode::SPCTR_COLOR:     return "LUXSTRAL COLOR";
        case VisualizerMode::SYNTH_COLOR:     return "LUXSYNTH COLOR";
        case VisualizerMode::SYNTH_FFT_COLOR: return "LUXSYNTH FFT";
        case VisualizerMode::SPCTR_BLOB:      return "LUXSTRAL BLOB";
        case VisualizerMode::SYNTH_BLOB:      return "LUXSYNTH BLOB";
        case VisualizerMode::SRC_IMAGE:       return "IMAGE SRC";
        case VisualizerMode::SRC_VIDEO:       return "VIDEO SRC";
        case VisualizerMode::SRC_CAMERA:      return "CAMERA SRC";
        case VisualizerMode::SELECTED_TAP:    return "MODULE";   // editor overrides with "NAME - CHAIN n"
        default:                              return "???";
    }
}
