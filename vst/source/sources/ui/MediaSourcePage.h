/**
 * @file MediaSourcePage.h
 * @brief M9 — ZONE 3 (PLAY face) for the IMAGE / VIDEO / CAMERA SRC modules.
 *
 * Layout: a large media preview with a draggable horizontal LINE cursor (the
 * row injected into the chain), plus the transport row:
 *   IMAGE  — PLAY/STOP, loop mode (Once/Loop/Reverse/Ping-Pong), scan time,
 *            and two draggable SCAN BOUNDS confining the transport to a
 *            region of the image (imgSrcScanStart/End, automatable).
 *   VIDEO  — PLAY/STOP, loop mode, speed, position scrub.
 *   CAMERA — line only (live feed, no transport).
 *
 * The line cursor binds to the automatable APVTS param (imgSrcPos / vidSrcLine
 * / camSrcLine); a second thinner cursor shows the IMAGE engine's playhead
 * while its transport runs.
 *
 * Source picking (LOAD/CLEAR for files, device combo for CAMERA) lives HERE,
 * on the PLAY face — the media modules have no SETUP face any more.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../PluginProcessor.h"
#include "../../midi/MidiLearnAttachment.h"
#include <vector>

class MediaSourcePage : public juce::Component,
                        private juce::Timer
{
public:
    enum class Kind { Image, Video, Camera };

    MediaSourcePage(Sp3ctraAudioProcessor& p, Kind k);
    ~MediaSourcePage() override;

    /** P5-M3 — rebind the page to ONE IMAGE instance (pool slot 0..7):
     *  attachments, MIDI-learn targets and engine accessors follow the slot.
     *  No-op for VIDEO/CAMERA (single instance until their engines pool). */
    void setSlot(int slot);

    static constexpr int kPreferredH = 420;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

private:
    void timerCallback() override;

    juce::String lineParamId() const;
    float  lineParamValue() const;
    void   setLineParam(float v, bool gestureBegin, bool gestureEnd);

    // IMAGE scan bounds (imgSrcScanStart/End) — no-ops for VIDEO/CAMERA.
    float  scanParamValue(bool start) const;
    void   setScanParam(bool start, float v, bool gestureBegin, bool gestureEnd);

    // Source picking (formerly the SETUP face)
    void chooseMedia();
    void clearMedia();
    void refreshDevices();
    void openSelectedDevice();

    // IMAGE only — cycle imgSrcRotate (0° → 90° → 180° → 270°)
    void cycleRotation();

    //==========================================================================
    /** The media preview + draggable line cursor. */
    class PreviewComponent : public juce::Component
    {
    public:
        explicit PreviewComponent(MediaSourcePage& o) : owner(o) {}

        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;

        juce::Image image;          ///< latest preview (updated by the timer)
        float lineFrac     = 0.5f;  ///< param-bound line cursor
        float playheadFrac = -1.f;  ///< engine playhead (<0 = hidden)
        float scanStartFrac = -1.f; ///< IMAGE scan bounds (<0 = hidden)
        float scanEndFrac   = -1.f;
        juce::String emptyHint;

    private:
        enum class DragTarget { Line, ScanStart, ScanEnd };

        juce::Rectangle<float> imageArea() const;
        void dragTo(const juce::MouseEvent& e, bool begin, bool end);
        MediaSourcePage& owner;
        DragTarget drag_ = DragTarget::Line;
    };

    Sp3ctraAudioProcessor& processor;
    const Kind kind;

    PreviewComponent preview { *this };

    // Source picker row (top): IMAGE/VIDEO — LOAD/CLEAR; CAMERA — device combo
    juce::TextButton loadButton  { "LOAD..." };
    juce::TextButton clearButton { "CLEAR" };
    // IMAGE only: orientation cycle
    juce::TextButton rotateButton { "ROT" };
    juce::ComboBox   deviceCombo;                 // CAMERA only
    juce::TextButton refreshButton { "REFRESH" };
    // ACTIVE — source on/off (all kinds): off feeds NOTHING into the chain
    // (blank paper); media/params are kept, on resumes instantly.
    juce::TextButton activeButton { "ACTIVE" };
    std::unique_ptr<juce::FileChooser> chooser_;

    // Transport row (kind-dependent subset)
    juce::TextButton playButton { "PLAY" };
    juce::ComboBox   loopCombo;
    juce::Slider     speedSlider;      // IMAGE: scan time (s) / VIDEO: speed (x)
    juce::Slider     positionSlider;   // VIDEO only: scrub 0..1
    juce::Label      speedLabel, loopLabel, positionLabel, statusLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   playAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   activeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> loopAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   speedAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    bool scrubbing_ = false;
    int  slot_      = 0;   // P5-M3 — bound IMAGE instance (pool slot)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MediaSourcePage)
};
