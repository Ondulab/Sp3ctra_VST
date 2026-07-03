/**
 * @file MediaSourcePage.h
 * @brief M9 — ZONE 3 (PLAY face) for the IMAGE / VIDEO / CAMERA SRC modules.
 *
 * Layout: a large media preview with a draggable horizontal LINE cursor (the
 * row injected into the chain), plus the transport row:
 *   IMAGE  — PLAY/STOP, loop mode (Once/Loop/Reverse/Ping-Pong), scan time.
 *   VIDEO  — PLAY/STOP, loop mode, speed, position scrub.
 *   CAMERA — line only (live feed, no transport).
 *
 * The line cursor binds to the automatable APVTS param (imgSrcPos / vidSrcLine
 * / camSrcLine); a second thinner cursor shows the IMAGE engine's playhead
 * while its transport runs.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../PluginProcessor.h"

class MediaSourcePage : public juce::Component,
                        private juce::Timer
{
public:
    enum class Kind { Image, Video, Camera };

    MediaSourcePage(Sp3ctraAudioProcessor& p, Kind k);
    ~MediaSourcePage() override;

    static constexpr int kPreferredH = 420;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

private:
    void timerCallback() override;

    juce::String lineParamId() const;
    float  lineParamValue() const;
    void   setLineParam(float v, bool gestureBegin, bool gestureEnd);

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
        juce::String emptyHint;

    private:
        juce::Rectangle<float> imageArea() const;
        void dragToLine(const juce::MouseEvent& e, bool begin, bool end);
        MediaSourcePage& owner;
    };

    Sp3ctraAudioProcessor& processor;
    const Kind kind;

    PreviewComponent preview { *this };

    // Transport row (kind-dependent subset)
    juce::TextButton playButton { "PLAY" };
    juce::ComboBox   loopCombo;
    juce::Slider     speedSlider;      // IMAGE: scan time (s) / VIDEO: speed (x)
    juce::Slider     positionSlider;   // VIDEO only: scrub 0..1
    juce::Label      speedLabel, loopLabel, positionLabel, statusLabel;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   playAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> loopAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   speedAttach;

    bool scrubbing_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MediaSourcePage)
};
