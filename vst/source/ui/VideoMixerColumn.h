#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "VideoMixerComponent.h"
#include <functional>
#include <memory>

/**
 * @brief ZONE 4 — right-band VIDEO MIX column (replaces the old waterfall column).
 *
 * Thin shell hosting the VideoMixerComponent: a header (title + transport +
 * detach/fullscreen + collapse), and the mixer (master composite + dynamic fader
 * strip) filling the rest. Collapses to a kGripW grip like the former column, so
 * the editor's zone-layout code is unchanged (same kGripW / setCollapsed /
 * isCollapsed / onCollapseToggled API). Header buttons reuse the path-drawn
 * glyphs of the former waterfall column (no font/glyph dependency).
 */
class VideoMixerColumn : public juce::Component
{
public:
    static constexpr int kGripW = 24;

    explicit VideoMixerColumn(Sp3ctraAudioProcessor& p);
    ~VideoMixerColumn() override = default;

    /** Fired AFTER the collapse state changed (editor relayouts + persists). */
    std::function<void(bool collapsed)> onCollapseToggled;

    void setCollapsed(bool shouldCollapse, bool notify);
    bool isCollapsed() const noexcept { return collapsed_; }

    /** Forwarded to the mixer when the chain model changes (outputs added/removed). */
    void refreshActiveSlots() { mixer_.refreshActiveSlots(); }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    //==========================================================================
    /** Header glyph button drawn with paths (detach / fullscreen / collapse / expand). */
    class MiniButton : public juce::Button
    {
    public:
        enum class Glyph { Detach, Fullscreen, Collapse, Expand };
        explicit MiniButton(Glyph g) : juce::Button("mini"), glyph(g) {}
        void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override;
    private:
        Glyph glyph;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniButton)
    };

    /** Transport: PlayPause is a toggle (state ON = paused → draws ▶); Stop is
     *  momentary (freeze + clear → draws a filled square). */
    class TransportButton : public juce::Button
    {
    public:
        enum class Glyph { PlayPause, Stop };
        explicit TransportButton(Glyph g) : juce::Button("transport"), glyph(g)
        {
            if (g == Glyph::PlayPause) setClickingTogglesState(true);
        }
        void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override;
    private:
        Glyph glyph;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportButton)
    };

    /** REC — red dot when idle, blinking red square while recording. Left-click
     *  toggles recording (wired in the column); right-click fires onRightClick
     *  (resolution menu). */
    class RecordButton : public juce::Button, private juce::Timer
    {
    public:
        RecordButton() : juce::Button("rec") {}
        void setRecording(bool r)
        {
            if (r == recording) return;
            recording = r; blinkOn = true;
            if (r) startTimerHz(2); else stopTimer();
            repaint();
        }
        bool isRecording() const noexcept { return recording; }
        std::function<void()> onRightClick;
        void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override;
    protected:
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
            juce::Button::mouseDown(e);
        }
    private:
        void timerCallback() override { blinkOn = ! blinkOn; repaint(); }
        bool recording { false };
        bool blinkOn   { true };
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecordButton)
    };

    //==========================================================================
    void startRecordingFlow();   // file chooser → mixer_.beginRecording()

    Sp3ctraAudioProcessor& processor_;   // session paths (recordings → session)
    VideoMixerComponent    mixer_;

    TransportButton playBtn_  { TransportButton::Glyph::PlayPause };
    TransportButton stopBtn_  { TransportButton::Glyph::Stop };
    RecordButton    recBtn_;
    MiniButton      detachBtn_     { MiniButton::Glyph::Detach };
    MiniButton      fullscreenBtn_ { MiniButton::Glyph::Fullscreen };
    MiniButton      collapseBtn_   { MiniButton::Glyph::Collapse };
    MiniButton      expandBtn_     { MiniButton::Glyph::Expand };

    // Recording: chosen vertical resolution (right-click menu) + the async
    // "Save As" chooser kept alive across its launchAsync callback.
    int  recordHeight_ { 1440 };
    std::unique_ptr<juce::FileChooser> fileChooser_;

    bool collapsed_ { false };

    static constexpr int kHeaderH = 24;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoMixerColumn)
};
