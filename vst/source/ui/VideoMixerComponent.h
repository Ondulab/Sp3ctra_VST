#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "ChainModel.h"                       // kMaxVideoSlots (solo preview slots)
#include "VideoMixRadar.h"                    // the toile (spokes + projector)
#include "../video/VideoScrollRenderCore.h"
#include "../video/VideoScrollPreviewSource.h"
#include "../video/VideoTiming.h"
#include "../video/NativeVideoView.h"
#include "../video/VideoFramePacer.h"
#include <atomic>
#include <memory>
#include <vector>

/**
 * @brief Right-band VIDEO mixer — composites every patched VIDEO SCROLL output
 *        into ONE master waterfall.
 *
 * For each VideoScroll output instance currently in a chain (processor.
 * activeVideoSlots()) the mixer owns a VideoScrollRenderCore that drains that
 * slot's capture ring and renders its waterfall. The RADAR toile (ui/
 * VideoMixRadar.h) sits at the top: one spoke per output carrying its level
 * + blend Mix/Add/Screen (videoMix{slot}_*), and the PROJECTOR at the centre
 * (videoMixFocusX/Y) — the one-hand sweep that dims what it points away
 * from. The compositor applies the same law (video/VideoMixFocus.h): each
 * layer is blended at level × weight(spoke, focus). Spokes follow the RACK
 * order and are labelled by host chain ("CHAIN n"), not by pool slot — the
 * slot stays a hidden implementation detail of the param bank.
 *
 * Rendering architecture (perf):
 * ─────────────────────────────────────────────────────────────────────────────
 * All heavy work — ring drain, scroll, per-pixel warp, per-layer blend — runs on
 * a dedicated BACKGROUND render thread (Renderer), paced at ~60 fps with a real
 * dt. It publishes each finished composite into a small triple-buffered image
 * pool. On macOS, the render thread submits that image to a hosted Core
 * Animation layer; the message timer only maintains layout and meters. Other
 * platforms (or SP3CTRA_VIDEO_NATIVE=0) use the JUCE timer/paint presenter.
 * Published images stay immutable until all presenters release them. The
 * per-output render resolution is budgeted by 1/sqrt(numOutputs).
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

    /** Detached master window (the right-band "window management"). While it
     *  is open it is the ONLY live view: the column's master area shows a
     *  static placeholder (no duplicate 60 Hz blit). */
    void toggleDetachedWindow();
    void requestFullscreenWindow();
    bool isWindowOpen() const noexcept;
    std::function<void()> onWindowStateChanged;

    int numActiveOutputs() const noexcept { return (int) activeSlots_.size(); }

    /** A click on a spoke's "CHAIN n" label — the column forwards it to
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
        void setPresentationTarget(std::shared_ptr<VideoPresentationTarget>);

        /** Any thread: blank every waterfall on the next render pass. */
        void requestClear() noexcept { clearGen_.fetch_add(1, std::memory_order_release); }

        /** Message thread: while `on`, the composite is rendered at a FIXED
         *  w×h (independent of the preview view size / √N budget) and every
         *  published frame is streamed to the processor's recorder. The preview
         *  simply downsamples the same hi-res front image. */
        void setRecordTarget(int w, int h, bool on) noexcept;

        /** Message thread: latest published composite (ref-copy under lock). */
        juce::Image frontImage() const;

        /** Message thread: what the PRESENTER should blit — a view-sized
         *  downsample of the composite when the two differ enough to matter
         *  (recording renders to the record aspect, not the window), else the
         *  composite itself. Doing that rescale here, once and in parallel,
         *  keeps multi-megapixel resampling out of every paint. */
        juce::Image presentImage() const;

        /** Message thread: which outputs (bit = slot) must ALSO be published
         *  alone — the VIEWPORT pads' thumbnails. 0 = none (no extra work). */
        void setSoloMask(uint32_t mask) noexcept { soloMask_.store(mask, std::memory_order_release); }

        /** Message thread: latest published solo image of `slot` (ref-copy
         *  under lock); invalid when that slot is not being soloed. */
        juce::Image soloImage(int slot) const;

        uint32_t frameCounter() const noexcept { return frameCounter_.load(std::memory_order_acquire); }

        /** Message thread: the live-stream envelopes of `slot` (0…1, see
         *  VideoScrollRenderCore::flowNow/flowPeak/flowPower) — the toile's
         *  blinker and its printed power. layers_ is only ever reshaped by
         *  setSlots() on this same thread, so the walk needs no lock; the
         *  values are atomics. false = the slot is not rendered. */
        bool flowOf(int slot, float& now, float& peak, float& power) const noexcept;

        /** Message thread: the AUDIO-FOLLOW mask of `slot` (0…1, 1 = not
         *  masked) — the third factor of the composite's effective level,
         *  computed and smoothed once by the presenter so the toile and the
         *  compositor can never disagree (video/VideoMixFollow.h). */
        void setFollow(int slot, float w) noexcept
        {
            if (slot >= 0 && slot < ChainModel::kMaxVideoSlots)
                follow_[slot].store(juce::jlimit(0.0f, 1.0f, w), std::memory_order_relaxed);
        }

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

        // One history tick (~60 Hz); warp/composite at the selected display FPS
        // or the existing recording cadence. Hidden layers keep live history.
        // Returns true when a new frame was published.
        bool renderFrame(double nowMs, double dtMs);
        // Pick an image of `pool` that nothing else references (a published
        // front, an in-flight paint), (re)sized to w×h. Invalid = all busy.
        static juce::Image acquireFrom(juce::Image* pool, int n, int w, int h);
        juce::Image acquireTarget(int w, int h) { return acquireFrom(pool_, 3, w, h); }

        Sp3ctraAudioProcessor& processor_;

        juce::CriticalSection coreLock_;
        std::vector<Layer> layers_;

        // Audio-follow mask per slot, written by the presenter, read by the
        // render thread (relaxed: a frame of lag on a visual mask is nothing).
        std::atomic<float>    follow_[ChainModel::kMaxVideoSlots];
        std::atomic<uint64_t> viewState_ { 0 };   // packed w:24 | h:24 | visible:1
        std::atomic<uint32_t> soloMask_  { 0 };   // bit = slot to publish alone
        std::atomic<int>      clearGen_  { 0 };
        int                   lastClearGen_ { 0 };
        std::vector<float>    lastSig_;           // mix/draw param signature (change detection)

        mutable juce::CriticalSection frontLock_;
        std::shared_ptr<VideoPresentationTarget> presentationTarget_;
        std::atomic<bool> targetChanged_ { false };
        juce::Image           front_;
        juce::Image           previewFront_;      // under frontLock_; invalid = use front_
        juce::Image           previewPool_[2];    // render thread only
        juce::Image           soloFront_[ChainModel::kMaxVideoSlots];   // under frontLock_
        juce::Image           pool_[3];
        std::atomic<uint32_t> frameCounter_ { 0 };
        bool                  haveFrame_ { false };
        bool                  pendingFrameChange_ { false };
        VideoFramePacer       renderPacer_;

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
    VideoFramePacer softwarePresentPacer_;
    void timerCallback() override;    // presenter: push view state, repaint on new frames
    bool updatePresentationTarget(); // message thread; native surface if available
#if JUCE_MAC
    std::unique_ptr<NativeVideoView> nativePreview_;
#endif
    // The view the renderer targets (logical px): the detached window content
    // when open, else the column master area; `visible` = anything on screen.
    void currentView(int& w, int& h, bool& visible) const;
    void rebuildStrip();                 // push the outputs to the toile
    // Presenter: read the AUDIO MIX, smooth it, hand the resulting mask to
    // BOTH the renderer and the toile. One smoother, one truth.
    void updateFollow(double nowMs);
    void layoutStrip();
    int  stripHeight() const noexcept;   // radar band (0 when no output)
    static int radarBandFor(int height) noexcept;   // its height at this mixer height
    // Draw the latest published composite into `dest` (shared by the column
    // preview and the detached window).
    void renderMaster(juce::Graphics& g, juce::Rectangle<int> dest);

    videotiming::Window paintTiming_;
    uint32_t timingPresentedFrame_ { 0 };

    class MasterView;     // detached-window content (paints the front image)
    class MasterWindow;

    Sp3ctraAudioProcessor& processor_;
    std::unique_ptr<VideoMixRadar> radar_;          // the toile (strip band)
    std::vector<std::pair<int, int>> activeSlots_;   // {slot, chainIdx} mirror of
                                                     // the current voices, in chain
                                                     // order (drives change detection
                                                     // incl. cross-chain moves)
    juce::StringArray labels_;                       // row labels (chain rename can
                                                     // change them with slots intact)

    std::unique_ptr<Renderer> renderer_;
    uint32_t lastPresented_ { 0 };
    // Measured cost of one master blit (ms, smoothed) and when the last one was
    // handed out — the presenter's adaptive throttle, see timerCallback.
    double   lastPaintMs_   { 0.0 };
    double   lastPresentMs_ { 0.0 };
    // Last requestOutputPreview() time per slot (message thread); the
    // presenter folds the recent ones into the renderer's solo mask.
    double   soloReqMs_[ChainModel::kMaxVideoSlots] { };

    juce::Rectangle<int> masterArea_;
    juce::Rectangle<int> stripArea_;

    std::unique_ptr<MasterWindow> window_;

    // The radar band takes kRadarShare of the mixer height, clamped: below
    // kRadarMinH the labels crowd the rim, above kRadarMaxH the master
    // preview (the point of the column) gives up too much.
    static constexpr int   kStripPad  = 4;
    static constexpr int   kRadarMinH = 176;
    static constexpr int   kRadarMaxH = 240;
    static constexpr float kRadarShare = 0.32f;
    static constexpr int   kFps       = 60;   // presenter poll rate (renderer self-paces)
    int flowTick_ { 0 };                      // flow push at kFps / 2

    // ── Audio-follow state (presenter/message thread only) ──────────────────
    // Smoothed per-LAYER audio level (index = activeSlots_ order), the arm
    // edge and the clock the envelope is advanced on.
    float  followLvl_[ChainModel::kMaxVideoSlots] { };
    bool   followExempt_[ChainModel::kMaxVideoSlots] { };
    bool   followArmed_  { false };
    double lastFollowMs_ { 0.0 };
    static constexpr double kSoloHoldMs = 300.0;   // request lifetime (pads tick at 20 Hz)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoMixerComponent)
};
