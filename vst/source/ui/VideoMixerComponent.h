#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../video/VideoScrollRenderCore.h"
#include <memory>
#include <vector>

/**
 * @brief Right-band VIDEO mixer — composites every patched VIDEO SCROLL output
 *        into ONE master waterfall.
 *
 * For each VideoScroll output instance currently in a chain (processor.
 * activeVideoSlots()) the mixer owns a VideoScrollRenderCore that drains that
 * slot's capture ring and renders its waterfall. A dynamic fader strip (one row
 * per output: level + blend Mix/Add/Screen, bound to videoMix{slot}_*) controls
 * how each is composited into the master image shown above.
 *
 * Message-thread only. A 30 fps timer ticks every core then re-composites.
 */
class VideoMixerComponent : public juce::Component,
                            private juce::Timer
{
public:
    explicit VideoMixerComponent(Sp3ctraAudioProcessor& proc);
    ~VideoMixerComponent() override;

    /** Rebuild the voice list from processor.activeVideoSlots(). Cheap no-op when
     *  the active set is unchanged. Call whenever the chain model changes. */
    void refreshActiveSlots();

    /** Transport — operate on EVERY patched output. */
    void setAllPaused(bool paused);   // sets videoScroll{slot}_paused for all
    void stopAll();                   // pause + clear every waterfall

    /** Detached master window (the right-band "window management"). */
    void toggleDetachedWindow();
    void requestFullscreenWindow();
    bool isWindowOpen() const noexcept;
    std::function<void()> onWindowStateChanged;

    /** Latest composited master (copied cheaply — JUCE Images are ref-counted). */
    juce::Image masterImage() const { return master_; }

    int numActiveOutputs() const noexcept { return (int) voices_.size(); }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void rebuildStrip();
    void layoutStrip();
    // Tick + warp every voice and (re)build the master composite. Returns true
    // when anything visible changed this tick; false = the frozen frame already
    // on screen is still current, so timerCallback() skips the repaint (a 60 fps
    // repaint of a static image occasionally gets presented half-painted → the
    // pause-time flicker).
    bool composite();
    // True when exactly one output is patched at full level in Mix mode — the
    // common case, painted DIRECTLY (no offscreen / no blend / no cap).
    bool singleDirect() const;
    // Render the composited master into `dest` in the given Graphics: direct-paint
    // the single output, or draw the offscreen master_ scaled. Used by the column
    // preview AND the detached window so both share one render path.
    void renderMaster(juce::Graphics& g, juce::Rectangle<int> dest);

    struct Voice
    {
        int slot { -1 };
        std::unique_ptr<VideoScrollRenderCore> core;
        juce::Slider   level;
        juce::ComboBox blend;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   levelAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> blendAtt;
        juce::Image    scratch;
    };

    class MasterView;     // detached-window content (paints master_)
    class MasterWindow;

    Sp3ctraAudioProcessor& processor_;
    std::vector<std::unique_ptr<Voice>> voices_;
    std::vector<int> activeSlots_;        // mirror of the current voice slots

    juce::Image master_;                  // composited output
    juce::Rectangle<int> masterArea_;
    juce::Rectangle<int> stripArea_;
    // Last-seen composite signature (render size + per-voice mix/draw params);
    // composite() reports "changed" whenever it differs. See composite().
    std::vector<float> mixSig_;

    std::unique_ptr<MasterWindow> window_;

    static constexpr int kRowH     = 24;
    static constexpr int kStripPad = 6;
    static constexpr int kRowGap   = 4;
    static constexpr int kFps      = 60;   // match the original waterfall smoothness

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoMixerComponent)
};
