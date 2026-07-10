#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "VideoDisplayComponent.h"
#include <functional>

class Sp3ctraAudioProcessor;

/**
 * @brief Detachable floating window for CIS video scroll visualization.
 *
 * Contains a VideoDisplayComponent (waterfall renderer) and a thin toolbar:
 *   [⛶ Fullscreen]   [✕ Close]
 *
 * Double-clicking the display area also toggles fullscreen.
 *
 * Opened from VideoScrollTab (via the zone-4 column header window icons);
 * destroyed when the user closes it (onCloseRequested fires back to the owner).
 *
 * Window dimensions are restored from APVTS parameters "videoWindowWidth" /
 * "videoWindowHeight" on construction and saved there on close.
 */
class VideoWindow : public juce::DocumentWindow
{
public:
    explicit VideoWindow(Sp3ctraAudioProcessor& processor);
    ~VideoWindow() override;

    void closeButtonPressed() override;

    /** Fired when the user closes the window (title-bar [✕] or toolbar Close).
     *  The owner (VideoScrollTab) tears the window down — deferred, so we never
     *  delete the window from inside its own callback. */
    std::function<void()> onCloseRequested;

    /** Toggle fullscreen on/off. */
    void toggleFullscreen();

    /** Expose the display component so VideoScrollTab can configure callbacks. */
    VideoDisplayComponent* getDisplay() noexcept;

private:
    //==========================================================================
    // Inner content: toolbar strip + VideoDisplayComponent
    //==========================================================================
    class ContentComponent : public juce::Component
    {
    public:
        ContentComponent(Sp3ctraAudioProcessor& proc, VideoWindow& owner);
        ~ContentComponent() override = default;

        void resized() override;
        void paint(juce::Graphics& g) override;

        VideoDisplayComponent display;

    private:
        static constexpr int kToolbarH = 30;

        Sp3ctraAudioProcessor& proc_;   ///< for reading APVTS mode label
        VideoWindow&           owner_;
        juce::TextButton       fullscreenBtn_;
        juce::TextButton       closeBtn_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ContentComponent)
    };

    Sp3ctraAudioProcessor& processor_;
    ContentComponent*      content_  { nullptr }; ///< owned by DocumentWindow

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoWindow)
};
