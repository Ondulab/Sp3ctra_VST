#pragma once

/**
 * @file VideoScrollMode.h
 * @brief Video scrolling modes — ported from legacy image_sequencer + synth_luxwave.
 *
 * Legacy mapping:
 *   LUXWAVE_SCAN_LEFT_TO_RIGHT → LiveLeftToRight
 *   LUXWAVE_SCAN_RIGHT_TO_LEFT → LiveRightToLeft
 *   LUXWAVE_SCAN_DUAL          → LiveDual (ping-pong L↔R)
 *   LOOP_MODE_SIMPLE           → SeqLoopSimple
 *   LOOP_MODE_PINGPONG         → SeqLoopPingPong
 *   LOOP_MODE_ONESHOT          → SeqOneShot
 *
 * Live modes:  render incoming CIS frames as a waterfall, direction varies.
 * Seq modes:   record N frames from the live stream, then replay at the given speed.
 */
enum class VideoScrollMode
{
    LiveLeftToRight = 0,  ///< Live: pixels 0→N, waterfall scrolls upward
    LiveRightToLeft,      ///< Live: pixels N→0 (mirrored), waterfall scrolls upward
    LiveDual,             ///< Live: alternates L→R / R→L every half-buffer (ping-pong)
    SeqLoopSimple,        ///< Seq: record then loop A→B→A→B (legacy LOOP_MODE_SIMPLE)
    SeqLoopPingPong,      ///< Seq: record then bounce A→B→A     (legacy LOOP_MODE_PINGPONG)
    SeqOneShot,           ///< Seq: record then play once A→B, freeze (legacy LOOP_MODE_ONESHOT)
    COUNT
};

/** Human-readable label for each mode — used in ComboBox and tooltips. */
inline const char* videoScrollModeLabel(VideoScrollMode m)
{
    switch (m)
    {
        case VideoScrollMode::LiveLeftToRight: return "Live L\xe2\x86\x92R";
        case VideoScrollMode::LiveRightToLeft: return "Live R\xe2\x86\x92L";
        case VideoScrollMode::LiveDual:        return "Live Dual (Ping-Pong)";
        case VideoScrollMode::SeqLoopSimple:   return "Seq. Boucle Simple";
        case VideoScrollMode::SeqLoopPingPong: return "Seq. Ping-Pong";
        case VideoScrollMode::SeqOneShot:      return "Seq. One-Shot";
        default:                               return "???";
    }
}
