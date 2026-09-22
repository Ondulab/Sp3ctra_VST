/**
 * @file HidMidiMapper.h
 * @brief The CIS as a MIDI controller — turns the Sp3ctra Link HID stream
 *        (3 buttons + accelerometer + gyroscope) into MIDI events, INSIDE the
 *        plugin, according to the SP3CTRA module's CONTROLS page.
 *
 * Doctrine (docs/PLAN_SP3CTRA_LINK.md D7): the device sends raw sensor data;
 * the VST fabricates the MIDI. The events land in a PRIVATE MidiBuffer that
 * only MidiMappingEngine::processMidi() consumes, so a CIS button behaves
 * exactly like a hardware controller for MIDI-learn, yet never reaches the
 * synths as a played note nor leaks to the plugin's MIDI output.
 *
 * Controls: SW1..SW3 (buttons), GYRO X/Y/Z (dps), TILT P/R (pitch / roll in
 * degrees, derived from the accelerometer). The RAW accelerometer axes are
 * deliberately NOT mapped: TILT already carries the same information in a
 * musically usable law (atan2), and ACC Z is little more than gravity — three
 * rows that only duplicated TILT (dropped 2026-08-30).
 *
 * Gestures (computed on the DEVICE at 1 kHz, published in slp_hid): HIT — a
 * shock, with the velocity of the impact and WHICH FACE took it — and FACE,
 * a selector of the face the device rests on. Only the bar's four LONG faces
 * exist (paper side, back, left edge, right edge): its 25 mm ends are neither
 * a resting position nor a playing surface. HIT is five button-like rows
 * emitting one press+release pulse per event: HIT fires on every shock, HIT
 * PAPER / BACK / LEFT / RIGHT only on the one that was struck (2026-09-04 —
 * they replace the old TAP / DOUBLE TAP pair, which a sensitive per-face HIT
 * makes redundant). FACE does not go
 * through a Min..Max window: each face has its own table entry (0..100 % of
 * the MIDI course, sp3ctraHidFaceVal0..4) so any face can land on any value —
 * and in CC 14-bit the 0.01 % table step keeps nearly the full 16384-step
 * resolution. Continuous rows are
 * silenced by a REST GATE while the device lies still (gyro quiet > 400 ms):
 * no more chatter stealing every MIDI learn — moving the device during a
 * learn stays a deliberate assignment, like wiggling a hardware knob.
 *  - buttons: Off / CC (127 on press, 0 on release) / Note (on / off) /
 *    Toggle (CC flips 0 <-> 127 on every press)
 *  - continuous: Off / CC (7-bit) / CC 14-bit (MSB n, LSB n+32); physical
 *    Min..Max -> 0..127, optional bipolar deadzone around the centre,
 *    one-pole smoothing; a message only when the quantised value changes
 *    (half-step hysteresis) — 200 Hz of HID never floods the mapping engine.
 *
 * Audio thread: process() is lock-free (seqlock snapshot read, atomics).
 * Message thread: the parameter table + the live meters of the page.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "../communication/link/sp3ctra_link.h"
#include <array>
#include <atomic>
#include <vector>

class HidMidiMapper
{
public:
    enum Control { Sw1 = 0, Sw2, Sw3, GyrX, GyrY, GyrZ, TiltP, TiltR,
                   Hit, HitPaper, HitBack, HitLeft, HitRight, Face, NumControls };
    static constexpr int kNumButtons = 3;
    static constexpr int kNumFaces   = SLP_FACE_COUNT;   // MOVING, PAPER, BACK, LEFT, RIGHT

    // Button types                       Continuous types
    // BtVelocity is offered by the HIT rows only, and is what makes a shock
    // an expressive control: a CC carrying the strike STRENGTH, pulsed - it
    // falls straight back to 0, so the control follows the impulse and
    // nothing more. BtCC pulses a flat 127 instead, which carries no nuance
    // but is what an action target (sampler REC, a toggle) needs.
    enum ButtonType { BtOff = 0, BtCC, BtNote, BtToggle, BtVelocity };
    enum ContType   { CtOff = 0, CtCC, CtCC14 };

    static constexpr const char* kPrefix = "sp3ctraHid";

    /** Device-side gesture events (the HIT family): button-like controls,
     *  one press+release pulse per event. */
    static bool         isEvent    (int c) noexcept { return c >= Hit && c <= HitRight; }
    /** The per-face HIT row a struck face fires, or -1 for SLP_FACE_MOVING
     *  (a shock along the bar, which belongs to no face). */
    static int          hitRowForFace (int face) noexcept
    {
        return (face >= SLP_FACE_PAPER && face <= SLP_FACE_RIGHT)
             ? HitPaper + (face - SLP_FACE_PAPER) : -1;
    }
    static bool         isButton   (int c) noexcept { return c < kNumButtons || isEvent (c); }
    static const char*  controlKey (int c) noexcept;   // "Sw1", "AccX", ... (param id part)
    static const char*  controlName(int c) noexcept;   // "SW1", "ACC X", ... (UI)
    static const char*  controlUnit(int c) noexcept;   // "g", "dps", "deg", ""
    static juce::String paramId    (int c, const char* suffix);   // sp3ctraHid<Key><Suffix>
    static const char*  faceName   (int f) noexcept;   // "Moving", "Paper", ...
    static juce::String faceParamId(int f);            // sp3ctraHidFaceVal<f>

    /** Declare every mapper parameter (call from createParameterLayout). */
    static void addParameters (std::vector<std::unique_ptr<juce::RangedAudioParameter>>& params);

    /** Cache the raw parameter pointers (after the APVTS exists). */
    void attach (juce::AudioProcessorValueTreeState& apvts);

    /** Audio thread: read the latest HID snapshot, emit MIDI into `out`.
     *  @param blockMs duration of this audio block - the gesture pulses time
     *         their release on it, so a 64-sample block and a 512-sample one
     *         give the same 60 ms pulse (they did not, and long blocks used to
     *         stretch it past 200 ms: fast tapping lost its releases). */
    void process (juce::MidiBuffer& out, float blockMs) noexcept;

    //── UI (message thread) ──────────────────────────────────────────────────
    /** Live normalised value 0..1 of a control (button: 1 = pressed). */
    float liveNorm (int c) const noexcept { return live_[(size_t) c].load (std::memory_order_relaxed); }
    /** True while HID datagrams are arriving. */
    bool  hidAlive() const noexcept;
    /** The MIDI event a control currently produces (type 1 = Note, 2 = CC).
     *  Returns false when the control is Off. */
    bool  currentEvent (int c, int& type, int& channel, int& number) const noexcept;

private:
    struct Raw
    {
        std::atomic<float>* type = nullptr;
        std::atomic<float>* chan = nullptr;
        std::atomic<float>* num  = nullptr;
        std::atomic<float>* min  = nullptr;      // continuous only
        std::atomic<float>* max  = nullptr;
        std::atomic<float>* bipolar = nullptr;
    };
    std::array<Raw, NumControls> raw_ {};
    std::array<std::atomic<float>*, kNumFaces> faceVal_ {};   // FACE table, %
    std::atomic<float>* deadzone_ = nullptr;     // % of the range
    std::atomic<float>* smoothMs_ = nullptr;

    void emitButton (int c, bool pressed, juce::MidiBuffer& out) noexcept;

    void emitEventPulse (int c, int velocity, juce::MidiBuffer& out) noexcept;

    // audio-thread state
    uint32_t lastGen_ = 0;
    bool     haveSeq_ = false;
    std::array<uint32_t, 4> lastSeq_ {};
    std::array<bool, kNumButtons> lastPressed_ {};
    std::array<bool, NumControls> toggle_ {};      // buttons AND gesture events
    std::array<float, NumControls> smoothed_ {};
    std::array<int, NumControls>   lastSent_ {};   // -1 = nothing sent yet
    std::array<std::atomic<float>, NumControls> live_ {};
    uint32_t lastTimestampUs_ = 0;

    // gesture events: wrapping u8 counters + pending pulse releases
    bool    haveGestSeq_ = false;
    uint8_t lastHitSeq_ = 0;
    struct PendingRelease { float ms = 0; int type = 0, chan = 1, num = 0; };
    std::array<PendingRelease, NumControls> release_ {};
    // The METER outlives the MIDI pulse on purpose: a 15 ms pulse would fall
    // between two 30 Hz repaints and a light tap would look like nothing at all.
    std::array<float, NumControls> meterMs_ {};

    // rest gate: continuous IMU rows only speak while the device moves
    float restQuietMs_ = 1.0e6f;
};
