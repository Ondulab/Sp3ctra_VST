/**
 * @file AcquisitionGate.h
 * @brief Clock driving the AudioImageBuffers frame-advance brake.
 *
 * "Vitesse d'acquisition" control for the SP3CTRA source.  The CIS device
 * streams lines over UDP at a fixed, read-only rate (freqLps); this gate brakes
 * how often that data is allowed to advance the *active* frame consumed by both
 * the synth engines and the visualizers.  Between ticks the last line is held
 * (sample-and-hold), so the sound freezes on the held spectrum and the image
 * freezes too — coherent audio + visual at the chosen rate.
 *
 * This object is the clock only: it runs on the audio thread (called once per
 * processBlock) and decides WHEN to grant an advance.  The HOLD itself is
 * enforced by the buffer module (audio_image_buffers_gate_*), consulted by the
 * UDP thread at the live publish site.
 *
 * Modes (acqGateMode):
 *   Off       — gate disabled, full-rate (legacy behaviour).
 *   Internal  — free-running clock, period set in milliseconds.
 *   DAW Sync  — locked to the host transport (PPQ), period set as a musical
 *               division; holds while the transport is stopped.
 * A common refresh-rate factor (multiplier/divider) stretches the period for
 * very slow updates ("refresh rate très lent").
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <cstdint>

// Opaque — the gate only forwards the pointer to the C buffer API below.
struct AudioImageBuffers;
extern "C" {
    void audio_image_buffers_gate_set_enabled(AudioImageBuffers* buffers, int enabled);
    void audio_image_buffers_gate_grant(AudioImageBuffers* buffers);
}

class AcquisitionGate
{
public:
    enum Mode { Off = 0, Internal = 1, DawSync = 2 };

    /**
     * @param buffers       Target AudioImageBuffers (may be null → no-op).
     * @param mode          Off / Internal / DawSync.
     * @param rateMs        Internal-mode base period in milliseconds.
     * @param syncDivBeats  DAW-sync base period in beats (e.g. 1.0 = 1/4 note).
     * @param refreshFactor Refresh-rate scale: period = base / refreshFactor
     *                      (<1 slows down, >1 speeds up).  Must be > 0.
     * @param numSamples    Block length (samples) — internal-mode time base.
     * @param sampleRate    Host sample rate (Hz).
     * @param playHead      Host play head (DAW-sync mode); may be null.
     */
    void process (AudioImageBuffers* buffers,
                  int mode, float rateMs, double syncDivBeats, double refreshFactor,
                  int numSamples, double sampleRate,
                  juce::AudioPlayHead* playHead) noexcept
    {
        if (buffers == nullptr)
            return;

        if (mode != lastMode_)      // a mode switch re-anchors the clock
        {
            reset();
            lastMode_ = mode;
        }

        if (mode == Off)
        {
            audio_image_buffers_gate_set_enabled (buffers, 0);
            return;
        }

        audio_image_buffers_gate_set_enabled (buffers, 1);

        if (refreshFactor <= 0.0)
            refreshFactor = 1.0;

        if (mode == DawSync)
            processDawSync  (buffers, syncDivBeats, refreshFactor, playHead);
        else
            processInternal (buffers, rateMs, refreshFactor, numSamples, sampleRate);
    }

    void reset() noexcept
    {
        phaseMs_       = 0.0;
        lastSyncPhase_ = INT64_MIN;
    }

private:
    void processInternal (AudioImageBuffers* buffers, float rateMs, double refreshFactor,
                          int numSamples, double sampleRate) noexcept
    {
        const double sr = (sampleRate > 0.0) ? sampleRate : 48000.0;
        double periodMs = static_cast<double> (rateMs) / refreshFactor; // smaller factor → longer hold
        if (periodMs < 1.0)
            periodMs = 1.0;

        phaseMs_ += static_cast<double> (numSamples) / sr * 1000.0;

        // Drain whole periods.  Multiple grants in one block collapse to a
        // single pending permit (we only brake) — the loop just keeps the
        // phase bounded so it cannot run away if the rate is very fast.
        while (phaseMs_ >= periodMs)
        {
            phaseMs_ -= periodMs;
            audio_image_buffers_gate_grant (buffers);
        }
    }

    void processDawSync (AudioImageBuffers* buffers, double syncDivBeats, double refreshFactor,
                         juce::AudioPlayHead* playHead) noexcept
    {
        if (playHead == nullptr)
            return;                                       // hold

        const auto pos = playHead->getPosition();
        if (! pos.hasValue() || ! pos->getIsPlaying())
            return;                                       // hold while transport stopped

        const auto ppqOpt = pos->getPpqPosition();
        if (! ppqOpt.hasValue())
            return;

        const double ppq = *ppqOpt;
        if (ppq < 0.0)
            return;

        double periodBeats = syncDivBeats / refreshFactor; // smaller factor → more beats per advance
        if (periodBeats < 1.0e-6)
            periodBeats = 1.0e-6;

        // Grant on every division boundary crossed (and once on first lock-in,
        // since lastSyncPhase_ starts at INT64_MIN → latches a fresh frame).
        const int64_t phase = static_cast<int64_t> (std::floor (ppq / periodBeats));
        if (phase != lastSyncPhase_)
        {
            lastSyncPhase_ = phase;
            audio_image_buffers_gate_grant (buffers);
        }
    }

    int     lastMode_      = -1;
    double  phaseMs_       = 0.0;
    int64_t lastSyncPhase_ = INT64_MIN;
};
