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
 * Controls: SW1..SW3 (buttons), ACC X/Y/Z (g), GYRO X/Y/Z (dps), TILT P/R
 * (pitch / roll in degrees, derived from the accelerometer).
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
#include <array>
#include <atomic>
#include <vector>

class HidMidiMapper
{
public:
    enum Control { Sw1 = 0, Sw2, Sw3, AccX, AccY, AccZ, GyrX, GyrY, GyrZ, TiltP, TiltR, NumControls };
    static constexpr int kNumButtons = 3;

    // Button types                       Continuous types
    enum ButtonType { BtOff = 0, BtCC, BtNote, BtToggle };
    enum ContType   { CtOff = 0, CtCC, CtCC14 };

    static constexpr const char* kPrefix = "sp3ctraHid";

    static bool         isButton   (int c) noexcept { return c < kNumButtons; }
    static const char*  controlKey (int c) noexcept;   // "Sw1", "AccX", ... (param id part)
    static const char*  controlName(int c) noexcept;   // "SW1", "ACC X", ... (UI)
    static const char*  controlUnit(int c) noexcept;   // "g", "dps", "deg", ""
    static juce::String paramId    (int c, const char* suffix);   // sp3ctraHid<Key><Suffix>

    /** Declare every mapper parameter (call from createParameterLayout). */
    static void addParameters (std::vector<std::unique_ptr<juce::RangedAudioParameter>>& params);

    /** Cache the raw parameter pointers (after the APVTS exists). */
    void attach (juce::AudioProcessorValueTreeState& apvts);

    /** Audio thread: read the latest HID snapshot, emit MIDI into `out`. */
    void process (juce::MidiBuffer& out) noexcept;

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
    std::atomic<float>* deadzone_ = nullptr;     // % of the range
    std::atomic<float>* smoothMs_ = nullptr;

    void emitButton (int c, bool pressed, juce::MidiBuffer& out) noexcept;

    // audio-thread state
    uint32_t lastGen_ = 0;
    bool     haveSeq_ = false;
    std::array<uint32_t, 4> lastSeq_ {};
    std::array<bool, kNumButtons> lastPressed_ {};
    std::array<bool, kNumButtons> toggle_ {};
    std::array<float, NumControls> smoothed_ {};
    std::array<int, NumControls>   lastSent_ {};   // -1 = nothing sent yet
    std::array<std::atomic<float>, NumControls> live_ {};
    uint32_t lastTimestampUs_ = 0;
};
