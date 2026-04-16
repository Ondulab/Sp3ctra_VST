#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "VideoDisplayComponent.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Detachable floating window for CIS video scroll visualization.
 *
 * Contains a VideoDisplayComponent (waterfall renderer) and a thin toolbar:
 *   [⛶ Plein écran]   [✕ Fermer]
 *
 * Double-clicking the display area also toggles fullscreen.
 *
 * Opened from VideoScrollTab; destroyed when the user closes it or disables
 * the "Video Scroll" toggle in the main editor.
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

        VideoWindow&     owner_;
        juce::TextButton fullscreenBtn_;
        juce::TextButton closeBtn_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ContentComponent)
    };

    Sp3ctraAudioProcessor& processor_;
    ContentComponent*      content_  { nullptr }; ///< owned by DocumentWindow

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoWindow)
};
