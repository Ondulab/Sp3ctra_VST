/**
 * @file WaterfallColumnComponent.h
 * @brief ZONE 4 — video scroll column (M4 four-zone shell).
 *
 * Thin wrapper around the existing VideoScrollTab (live waterfall controls +
 * detached VideoWindow ownership).  Adds a mini header with:
 *   [⧉]  — opens the detached VideoWindow (same code path the old VIDEO tab
 *          used: VideoScrollTab::openVideoWindow()).
 *   [✕]  — collapses the column to a 24 px vertical grip; the editor
 *          relayouts (zone 3 takes the width) and persists "scrollCollapsed".
 * When collapsed, a [▶] grip button expands it back.
 *
 * M5: a compact display toolbar sits under the header (former gear-wheel
 * "Video Scroll" tab controls, same APVTS parameter IDs):
 *   [☀ brightness slider]  [◐ invert toggle]  [▥ RGB/grayscale toggle]
 *   → videoScrollBrightness / videoInvertColor / videoColorMode
 *
 * The hosted VideoScrollTab lives inside a vertical Viewport so the full
 * control stack stays reachable at any window height.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../video/VideoScrollTab.h"
#include <functional>

class WaterfallColumnComponent : public juce::Component
{
public:
    /** Width of the collapsed vertical grip. */
    static constexpr int kGripW = 24;

    explicit WaterfallColumnComponent(Sp3ctraAudioProcessor& p);
    ~WaterfallColumnComponent() override = default;

    /** Fired AFTER the collapse state changed (editor relayouts + persists). */
    std::function<void(bool collapsed)> onCollapseToggled;

    void setCollapsed(bool shouldCollapse, bool notify);
    bool isCollapsed() const noexcept { return collapsed; }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    //==========================================================================
    /** Mini header button drawn with paths (no font/glyph dependency). */
    class MiniButton : public juce::Button
    {
    public:
        enum class Glyph { Detach, Collapse, Expand };

        explicit MiniButton(Glyph g) : juce::Button("mini"), glyph(g) {}
        void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override;

    private:
        Glyph glyph;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MiniButton)
    };

    /** Tiny icon toggle for the display toolbar (invert / colour mode).
     *  Glyphs drawn with paths; toggled state highlights with the accent. */
    class IconToggleButton : public juce::Button
    {
    public:
        enum class Glyph { Invert, ColorMode };

        explicit IconToggleButton(Glyph g) : juce::Button("iconToggle"), glyph(g)
        {
            setClickingTogglesState(true);
        }
        void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override;

    private:
        Glyph glyph;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IconToggleButton)
    };

    //==========================================================================
    Sp3ctraAudioProcessor& processor;

    juce::Viewport viewport;
    std::unique_ptr<VideoScrollTab> videoTab;

    MiniButton detachBtn   { MiniButton::Glyph::Detach };
    MiniButton collapseBtn { MiniButton::Glyph::Collapse };
    MiniButton expandBtn   { MiniButton::Glyph::Expand };

    // ── Display toolbar (M5 — former gear "Video Scroll" display settings) ────
    juce::Slider     brightnessSlider;
    IconToggleButton invertBtn    { IconToggleButton::Glyph::Invert };
    IconToggleButton colorModeBtn { IconToggleButton::Glyph::ColorMode };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> brightnessAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invertAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> colorModeAttachment;

    bool collapsed { false };

    static constexpr int kHeaderH      = 22;
    static constexpr int kToolbarH     = 24;    // display toolbar strip
    static constexpr int kContentMinH  = 430;   // natural VideoScrollTab height

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaterfallColumnComponent)
};
