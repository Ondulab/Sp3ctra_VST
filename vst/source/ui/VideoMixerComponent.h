#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "Sp3ctraBarSlider.h"
#include "ChainModel.h"                       // kMaxVideoSlots (solo preview slots)
#include "../video/VideoScrollRenderCore.h"
#include "../video/VideoScrollPreviewSource.h"
#include <atomic>
#include <memory>
#include <vector>

/**
 * @brief Right-band VIDEO mixer — composites every patched VIDEO SCROLL output
 *        into ONE master waterfall.
 *
 * For each VideoScroll output instance currently in a chain (processor.
 * activeVideoSlots()) the mixer owns a VideoScrollRenderCore that drains that
 * slot's capture ring and renders its waterfall. A dynamic fader strip (one row
 * per output: level + blend Mix/Add/Screen, bound to videoMix{slot}_*) sits at
 * the top and controls how each is composited into the master image below it.
 * Rows follow the RACK order and are labelled by host chain ("CHAIN n"), not by
 * pool slot — the slot stays a hidden implementation detail of the param bank.
 *
 * Rendering architecture (perf):
 * ─────────────────────────────────────────────────────────────────────────────
 * All heavy work — ring drain, scroll, per-pixel warp, per-layer blend — runs on
 * a dedicated BACKGROUND render thread (Renderer), paced at ~60 fps with a real
 * dt. It publishes each finished composite into a small triple-buffered image
 * pool. The message thread only runs a light presenter timer: it pushes the
 * current view size/visibility to the renderer and, when a NEW frame counter is
 * seen, invalidates the views. paint() just blits the front image — the message
 * thread can therefore never be saturated by the video path, and only COMPLETE
 * frames ever reach the screen (the historical half-painted flicker is gone by
 * construction). Cost also stays bounded as outputs are added: the per-output
 * render resolution is budgeted by 1/sqrt(numOutputs).
 * ─────────────────────────────────────────────────────────────────────────────
 */
class VideoMixerComponent : public juce::Component,
                            public VideoScrollPreviewSource,
                            private juce::Timer
{
public:
    explicit VideoMixerComponent(Sp3ctraAudioProcessor& proc);
    ~VideoMixerComponent() override;

    /** VideoScrollPreviewSource — the VIDEO SCROLL pages' VIEWPORT pad draws
     *  the output it edits ALONE (not the composite) as its thumbnail and
     *  mirrors the view aspect. Solo renders are demand-driven: a request
     *  keeps the slot in the renderer's solo mask for kSoloHoldMs (the pad
     *  re-requests on every 20 Hz tick while showing). Message thread. */
    void             requestOutputPreview(int slot) override;
    juce::Image      outputFrame(int slot) const override;
    uint32_t         frameCounter() const override;
    juce::Point<int> viewSize()     const override;

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

    int numActiveOutputs() const noexcept { return (int) voices_.size(); }

    /** A click on a strip row's "CHAIN n" label — the column forwards it to
     *  the editor, which opens that output's chain tab. */
    std::function<void(int slot)> onOutputClicked;

    /** Mixer width beyond which the square master preview stops growing at
     *  this mixer height (it becomes height-limited) — extra width would be
     *  dead space. The editor caps the ZONE-4 splitter with it. */
    int maxUsefulWidth(int height) const noexcept;

    /** VIDEO MIX recording (macOS). Locks the render to a fixed hi-res composite
     *  (recW×recH derived from the current view aspect × `height`) and streams it
     *  plus the master audio to a .mov via the processor's recorder. Returns
     *  false + fills `err` on failure. Message thread. */
    bool beginRecording(const juce::File& out, int height, juce::String& err);
    void endRecording();
    bool isRecording() const noexcept;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    //==========================================================================
    /** Background render thread. Owns the per-slot VideoScrollRenderCores and
     *  the whole tick → warp → composite pipeline. Publishes finished frames
     *  through a triple-buffered pool (front image + atomic frame counter); the
     *  component's presenter timer picks them up on the message thread.
     *
     *  Thread-safety contract:
     *   - layers_ is mutated ONLY under coreLock_ (setSlots on the message
     *     thread vs. renderFrame on the render thread).
     *   - front_ is swapped ONLY under frontLock_; readers take a ref-copy.
     *   - A pool image is reused as a render target only while its refcount
     *     shows no other owner (front_ / an in-flight paint), so a buffer being
     *     displayed is never written to — no tearing. */
    class Renderer : public juce::Thread
    {
    public:
        explicit Renderer(Sp3ctraAudioProcessor& p);
        ~Renderer() override;

        /** Message thread: rebuild the layer list. Cores of slots that remain
         *  are KEPT (their waterfall history survives chain edits). */
        void setSlots(const std::vector<int>& slots);

        /** Message thread (presenter tick): desired output size in LOGICAL px
         *  (detached window content if open, else the column master area) and
         *  whether any view is actually visible. Invisible → the renderer keeps
         *  draining/scrolling (history stays truthful) but skips warp/composite. */
        void setViewState(int w, int h, bool visible) noexcept;

        /** Any thread: blank every waterfall on the next render pass. */
        void requestClear() noexcept { clearGen_.fetch_add(1, std::memory_order_release); }

        /** Message thread: while `on`, the composite is rendered at a FIXED
         *  w×h (independent of the preview view size / √N budget) and every
         *  published frame is streamed to the processor's recorder. The preview
         *  simply downsamples the same hi-res front image. */
        void setRecordTarget(int w, int h, bool on) noexcept;

        /** Message thread: latest published composite (ref-copy under lock). */
        juce::Image frontImage() const;

        /** Message thread: which outputs (bit = slot) must ALSO be published
         *  alone — the VIEWPORT pads' thumbnails. 0 = none (no extra work). */
        void setSoloMask(uint32_t mask) noexcept { soloMask_.store(mask, std::memory_order_release); }

        /** Message thread: latest published solo image of `slot` (ref-copy
         *  under lock); invalid when that slot is not being soloed. */
        juce::Image soloImage(int slot) const;

        uint32_t frameCounter() const noexcept { return frameCounter_.load(std::memory_order_acquire); }

        void run() override;

    private:
        struct Layer
        {
            int slot { -1 };
            std::unique_ptr<VideoScrollRenderCore> core;
            juce::Image scratch;   // per-layer drawWarp target for the blend path
            // Solo publish pool (only allocated while the slot is in the solo
            // mask): the image a pad is painting is never drawn into.
            juce::Image soloPool[2];
        };

        // One 60 fps pass: tick + warp every layer, composite, publish.
        // Returns true when a new frame was published.
        bool renderFrame(double nowMs, double dtMs);
        // Pick an image of `pool` that nothing else references (a published
        // front, an in-flight paint), (re)sized to w×h. Invalid = all busy.
        static juce::Image acquireFrom(juce::Image* pool, int n, int w, int h);
        juce::Image acquireTarget(int w, int h) { return acquireFrom(pool_, 3, w, h); }

        Sp3ctraAudioProcessor& processor_;

        juce::CriticalSection coreLock_;
        std::vector<Layer> layers_;

        std::atomic<uint64_t> viewState_ { 0 };   // packed w:24 | h:24 | visible:1
        std::atomic<uint32_t> soloMask_  { 0 };   // bit = slot to publish alone
        std::atomic<int>      clearGen_  { 0 };
        int                   lastClearGen_ { 0 };
        std::vector<float>    lastSig_;           // mix/draw param signature (change detection)

        mutable juce::CriticalSection frontLock_;
        juce::Image           front_;
        juce::Image           soloFront_[ChainModel::kMaxVideoSlots];   // under frontLock_
        juce::Image           pool_[3];
        std::atomic<uint32_t> frameCounter_ { 0 };
        bool                  haveFrame_ { false };

        // ── Recording (fixed hi-res composite streamed to the recorder) ──────
        std::atomic<bool> recOn_ { false };
        std::atomic<int>  recW_  { 0 };
        std::atomic<int>  recH_  { 0 };
        // Render-thread only: session clock + a ~2 fps heartbeat so the video
        // timeline keeps pace with the audio even while the waterfall is frozen.
        bool   lastRecOn_     { false };
        double recStartMs_    { 0.0 };
        double lastRecPushMs_ { 0.0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Renderer)
    };

    //==========================================================================
    void timerCallback() override;    // presenter: push view state, repaint on new frames
    // The view the renderer targets (logical px): the detached window content
    // when open, else the column master area; `visible` = anything on screen.
    void currentView(int& w, int& h, bool& visible) const;
    void rebuildStrip();
    void layoutStrip();
    int  stripHeight() const noexcept;   // fader rows band (0 when no output)
    // Draw the latest published composite into `dest` (shared by the column
    // preview and the detached window).
    void renderMaster(juce::Graphics& g, juce::Rectangle<int> dest);

    /** UI-only fader row (the render core lives in Renderer::Layer). */
    struct Voice
    {
        int slot { -1 };
        juce::String label;   ///< "CHAIN n" (+ a/b… when the chain hosts several)
        Sp3ctraBarSlider level;
        juce::ComboBox blend;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   levelAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> blendAtt;
        std::unique_ptr<MidiLearnAttachment> levelLearn, blendLearn;
    };

    class MasterView;     // detached-window content (paints the front image)
    class MasterWindow;

    Sp3ctraAudioProcessor& processor_;
    std::vector<std::unique_ptr<Voice>> voices_;
    std::vector<std::pair<int, int>> activeSlots_;   // {slot, chainIdx} mirror of
                                                     // the current voices, in chain
                                                     // order (drives change detection
                                                     // incl. cross-chain moves)

    std::unique_ptr<Renderer> renderer_;
    uint32_t lastPresented_ { 0 };
    // Last requestOutputPreview() time per slot (message thread); the
    // presenter folds the recent ones into the renderer's solo mask.
    double   soloReqMs_[ChainModel::kMaxVideoSlots] { };

    juce::Rectangle<int> masterArea_;
    juce::Rectangle<int> stripArea_;
    int  hoverRow_ { -1 };                              // strip row whose label is hovered
    int  rowLabelAt(juce::Point<int> p) const noexcept; // label hit-test (−1 = none)

    std::unique_ptr<MasterWindow> window_;

    static constexpr int kRowH     = 24;
    static constexpr int kStripPad = 6;
    static constexpr int kRowGap   = 4;
    static constexpr int kLabelW   = 58;   // fits "CHAIN 8b" at kFontBadge bold
    static constexpr int kFps      = 60;   // presenter poll rate (renderer self-paces)
    static constexpr double kSoloHoldMs = 300.0;   // request lifetime (pads tick at 20 Hz)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoMixerComponent)
};
