#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "VideoMixerComponent.h"
#include <functional>

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

    //==========================================================================
    Sp3ctraAudioProcessor& processor_;
    VideoMixerComponent    mixer_;

    TransportButton playBtn_  { TransportButton::Glyph::PlayPause };
    TransportButton stopBtn_  { TransportButton::Glyph::Stop };
    MiniButton      detachBtn_     { MiniButton::Glyph::Detach };
    MiniButton      fullscreenBtn_ { MiniButton::Glyph::Fullscreen };
    MiniButton      collapseBtn_   { MiniButton::Glyph::Collapse };
    MiniButton      expandBtn_     { MiniButton::Glyph::Expand };

    bool collapsed_ { false };

    static constexpr int kHeaderH = 24;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoMixerColumn)
};
