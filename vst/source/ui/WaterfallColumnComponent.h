/**
 * @file WaterfallColumnComponent.h
 * @brief ZONE 4 — video scroll column (M4 four-zone shell).
 *
 * Thin wrapper around the existing VideoScrollTab (live waterfall controls +
 * detached VideoWindow ownership).  Adds a mini header with:
 *   ●    — green when the window is open, grey when closed (centred, next to
 *          the "VIDEO SCROLL" title).
 *   [✕]  — collapses the column to a 24 px vertical grip; the editor
 *          relayouts (zone 3 takes the width) and persists "scrollCollapsed".
 * When collapsed, a [▶] grip button expands it back.
 *
 * A single control band sits under the header, grouping (left → right) the
 * window-display controls, the scroll transport (centred) and the colorimetry
 * toggles:
 *   [⧉ detach] [⛶ fullscreen]  …  [▶/⏸ play] [⏹ stop]  …  [◐ invert] [▥ RGB/grayscale]
 *   → window open/close + fullscreen / videoScrollPaused
 *     / videoInvertColor / videoColorMode
 * The window-display + transport buttons are also stacked in the collapsed grip.
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
        enum class Glyph { Detach, Fullscreen, Collapse, Expand };

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

    /** Transport control for the scroll engine.  PlayPause is a toggle bound to
     *  "videoScrollPaused" — it draws ▶ when paused (press to resume) and ⏸ when
     *  playing.  Stop is a momentary button (freeze + clear the waterfall). */
    class TransportButton : public juce::Button
    {
    public:
        enum class Glyph { PlayPause, Stop };

        explicit TransportButton(Glyph g) : juce::Button("transport"), glyph(g)
        {
            if (g == Glyph::PlayPause)
                setClickingTogglesState(true);
        }
        void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override;

    private:
        Glyph glyph;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportButton)
    };

    //==========================================================================
    Sp3ctraAudioProcessor& processor;

    juce::Viewport viewport;
    std::unique_ptr<VideoScrollTab> videoTab;

    // Window-display controls now live in the combined control band (alongside
    // the transport and colorimetry toggles): small-window [⧉] toggles the
    // detached window open/closed, large-window [⛶] opens it full screen. A
    // green dot in the header title band shows whether the window is open.
    MiniButton detachBtn     { MiniButton::Glyph::Detach };
    MiniButton fullscreenBtn { MiniButton::Glyph::Fullscreen };
    MiniButton collapseBtn   { MiniButton::Glyph::Collapse };
    MiniButton expandBtn     { MiniButton::Glyph::Expand };
    // Collapsed-grip copies of the window-display controls (same actions),
    // stacked in the vertical grip alongside the transport copies.
    MiniButton detachGrip     { MiniButton::Glyph::Detach };
    MiniButton fullscreenGrip { MiniButton::Glyph::Fullscreen };

    bool windowOpen { false };   ///< mirror of VideoScrollTab::isVideoWindowOpen()

    // ── Transport (Play/Pause + Stop) — shown in the compact band when expanded
    //    and stacked in the vertical grip when collapsed. ───────────────────────
    TransportButton playPauseBtn  { TransportButton::Glyph::PlayPause };
    TransportButton stopBtn       { TransportButton::Glyph::Stop };
    TransportButton playPauseGrip { TransportButton::Glyph::PlayPause };
    TransportButton stopGrip      { TransportButton::Glyph::Stop };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> playPauseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> playPauseGripAttachment;

    // ── Display toggles (M5 — former gear "Video Scroll" display settings) ────
    IconToggleButton invertBtn    { IconToggleButton::Glyph::Invert };
    IconToggleButton colorModeBtn { IconToggleButton::Glyph::ColorMode };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> invertAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> colorModeAttachment;

    bool collapsed { false };

    static constexpr int kHeaderH      = 22;
    static constexpr int kToolbarH     = 26;    // combined control band
                                                // (transport + display toggles)
    static constexpr int kContentMinH  = 430;   // natural VideoScrollTab height

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaterfallColumnComponent)
};
