#include "VideoMixerComponent.h"
#include "../video/VideoDisplaySettings.h"
#include "ModuleCatalog.h"
#include "../session/MachinePrefs.h"   // detached-window state is machine-scoped
#include "../video/VideoScrollMode.h"  // videoScrollOutputLabels() (shared spoke labels)
#include "../video/VideoMixFocus.h"    // projector law (shared with the toile)
#include "../video/VideoMixFollow.h"   // audio-follow law (shared with the toile)
#include "../video/VideoParallel.h"    // parallelChunks — the per-row fan-out
#include "../video/VideoBlit.h"        // scaleARGB — the preview downsample
#include <array>
#include <cmath>

//==============================================================================
// Detached master window — shows the composited master at any size / fullscreen.
//==============================================================================
class VideoMixerComponent::MasterView : public juce::Component
{
public:
    explicit MasterView(VideoMixerComponent& owner) : owner_(owner)
    {
        // renderMaster() fills the whole bounds (black + waterfall) every paint,
        // so the detached/fullscreen view is fully opaque — this prevents the
        // window content from flickering under the presenter-driven repaints.
        setOpaque(true);
#if JUCE_MAC
        if (NativeVideoView::enabled())
        {
            native_ = std::make_unique<NativeVideoView>();
            addAndMakeVisible(*native_);
        }
#endif
    }

    // The native child receives frames directly; paint is the fallback presenter.
    void paint(juce::Graphics& g) override
    {
#if JUCE_MAC
        if (native_) { g.fillAll(juce::Colours::white); return; }
#endif
        owner_.renderMaster(g, getLocalBounds());
    }

    void resized() override
    {
#if JUCE_MAC
        if (native_) native_->setBounds(getLocalBounds());
#endif
    }

    std::shared_ptr<VideoPresentationTarget> target() const
    {
#if JUCE_MAC
        if (native_) return native_->target();
#endif
        return {};
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (onFullscreenRequested) onFullscreenRequested();
    }

    std::function<void()> onFullscreenRequested;

private:
    VideoMixerComponent& owner_;
#if JUCE_MAC
    std::unique_ptr<NativeVideoView> native_;
#endif
};

class VideoMixerComponent::MasterWindow : public juce::DocumentWindow
{
public:
    explicit MasterWindow(VideoMixerComponent& owner)
        : juce::DocumentWindow("VIDEO MIX",
                               juce::Colours::black,
                               juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        auto* v = new MasterView(owner);
        v->onFullscreenRequested = [this] { toggleFullscreen(); };
        setContentOwned(v, false);
        centreWithSize(800, 600);
        // Machine-scoped: reopen on the display/bounds the user last used.
        if (const auto st = MachinePrefs::file().getValue("videoMixWin.state");
            st.isNotEmpty())
            restoreWindowStateFromString(st);
        setVisible(true);
    }

    ~MasterWindow() override { saveWindowState(); }

    void closeButtonPressed() override { if (onCloseRequested) onCloseRequested(); }

    void toggleFullscreen()
    {
        setFullScreen(! isFullScreen());
        saveWindowState();
    }

    /** Bounds + fullscreen → machine.settings (the "open" flag is written by
     *  the explicit open/close paths only, so an app quit with the window up
     *  reopens it on the next launch). */
    void saveWindowState()
    {
        auto& prefs = MachinePrefs::file();
        prefs.setValue("videoMixWin.fullscreen", isFullScreen());
        if (! isFullScreen())
            prefs.setValue("videoMixWin.state", getWindowStateAsString());
    }

    std::function<void()> onCloseRequested;
};

//==============================================================================
// Composite helpers (render thread)
//==============================================================================
namespace
{
    // Render-resolution ceiling for ONE output. The warp/blend passes are
    // per-pixel scalar work, so their cost is O(W×H) per output per frame; 1600
    // keeps a fullscreen window affordable while the final blit upscales with no
    // visible loss for a waterfall. With N outputs the ceiling is divided by
    // sqrt(N) (floor kMinRenderDim) so the TOTAL pixel cost stays roughly
    // constant as outputs are added instead of scaling linearly. Since the
    // continuous rotation (2026-08-28) the render canvas is a SQUARE on the
    // view diagonal, so the ceiling caps the DIAGONAL: the canvas never
    // exceeds kMaxRenderDim² pixels, the same worst case as before.
    constexpr int kMaxRenderDim = 1600;
    constexpr int kMinRenderDim = 640;

    // Composite one rendered layer into the master with level + blend mode.
    // Opaque ARGB line-pointer blend (fast enough for 60 fps at capped resolution).
    void blendLayer(juce::Image& dst, const juce::Image& src, float level, int mode)
    {
        const int w = juce::jmin(dst.getWidth(),  src.getWidth());
        const int h = juce::jmin(dst.getHeight(), src.getHeight());
        if (w <= 0 || h <= 0) return;

        const int L = juce::jlimit(0, 256, (int) (juce::jlimit(0.0f, 1.0f, level) * 256.0f));
        juce::Image::BitmapData db(dst, juce::Image::BitmapData::readWrite);
        juce::Image::BitmapData sb(src, juce::Image::BitmapData::readOnly);

        // Rows are independent — fan them out like every other per-pixel pass.
        videoparallel::parallelChunks(0, h, 24, [&](int, int yA, int yB)
        {
        for (int y = yA; y < yB; ++y)
        {
            auto* dp = (juce::PixelARGB*) db.getLinePointer(y);
            auto* sp = (juce::PixelARGB*) sb.getLinePointer(y);
            for (int x = 0; x < w; ++x)
            {
                const int sr = (sp[x].getRed()   * L) >> 8;
                const int sg = (sp[x].getGreen() * L) >> 8;
                const int sbb= (sp[x].getBlue()  * L) >> 8;
                int mr = dp[x].getRed(), mg = dp[x].getGreen(), mb = dp[x].getBlue();

                switch (mode)
                {
                    case 1: // Add
                        mr = juce::jmin(255, mr + sr);
                        mg = juce::jmin(255, mg + sg);
                        mb = juce::jmin(255, mb + sbb);
                        break;
                    case 2: // Screen
                        mr = 255 - (255 - mr) * (255 - sr) / 255;
                        mg = 255 - (255 - mg) * (255 - sg) / 255;
                        mb = 255 - (255 - mb) * (255 - sbb) / 255;
                        break;
                    default: // Mix (alpha over by level)
                        mr = (mr * (256 - L) >> 8) + sr;
                        mg = (mg * (256 - L) >> 8) + sg;
                        mb = (mb * (256 - L) >> 8) + sbb;
                        break;
                }
                dp[x].setARGB(255, (juce::uint8) juce::jmin(255, mr),
                                   (juce::uint8) juce::jmin(255, mg),
                                   (juce::uint8) juce::jmin(255, mb));
            }
        }
        });
    }
}

//==============================================================================
// Renderer — the background render thread
//==============================================================================
VideoMixerComponent::Renderer::Renderer(Sp3ctraAudioProcessor& p)
    : juce::Thread("VideoMixRender"), processor_(p)
{
    for (auto& f : follow_)   // no follow until the presenter says otherwise
        f.store(1.0f, std::memory_order_relaxed);
    VideoDisplaySettings::restore();
    startThread();
}

VideoMixerComponent::Renderer::~Renderer()
{
    stopThread(2000);
}

void VideoMixerComponent::Renderer::setSlots(const std::vector<int>& slots)
{
    const juce::ScopedLock cl(coreLock_);

    std::vector<Layer> next;
    next.reserve(slots.size());
    for (int slot : slots)
    {
        Layer nl;
        nl.slot = slot;
        // Keep the existing core (and its waterfall history) when the slot
        // survives the chain edit — previously every edit wiped ALL waterfalls.
        for (auto& l : layers_)
            if (l.slot == slot && l.core != nullptr)
            {
                nl.core    = std::move(l.core);
                nl.scratch = l.scratch;
                for (int i = 0; i < 2; ++i) nl.soloPool[i] = l.soloPool[i];
                break;
            }
        if (nl.core == nullptr)
            nl.core = std::make_unique<VideoScrollRenderCore>(processor_, slot);
        next.push_back(std::move(nl));
    }
    layers_ = std::move(next);
    lastSig_.clear();   // layer set changed → force a fresh publish
}

void VideoMixerComponent::Renderer::setViewState(int w, int h, bool visible) noexcept
{
    const uint64_t v = ((uint64_t) (uint32_t) juce::jlimit(0, 0xffffff, w) << 25)
                     | ((uint64_t) (uint32_t) juce::jlimit(0, 0xffffff, h) << 1)
                     | (visible ? 1u : 0u);
    viewState_.store(v, std::memory_order_release);
}

juce::Image VideoMixerComponent::Renderer::frontImage() const
{
    const juce::ScopedLock fl(frontLock_);
    return front_;
}

juce::Image VideoMixerComponent::Renderer::presentImage() const
{
    const juce::ScopedLock fl(frontLock_);
    return previewFront_.isValid() ? previewFront_ : front_;
}

bool VideoMixerComponent::Renderer::flowOf(int slot, float& now, float& peak,
                                           float& power) const noexcept
{
    for (const auto& l : layers_)
        if (l.slot == slot && l.core != nullptr)
        {
            now   = l.core->flowNow();
            peak  = l.core->flowPeak();
            power = l.core->flowPower();
            return true;
        }
    now = peak = power = 0.0f;
    return false;
}

juce::Image VideoMixerComponent::Renderer::soloImage(int slot) const
{
    if (slot < 0 || slot >= ChainModel::kMaxVideoSlots) return {};
    const juce::ScopedLock fl(frontLock_);
    return soloFront_[slot];
}

void VideoMixerComponent::Renderer::setRecordTarget(int w, int h, bool on) noexcept
{
    recW_.store(w, std::memory_order_release);
    recH_.store(h, std::memory_order_release);
    recOn_.store(on, std::memory_order_release);   // run() picks up the rec-start edge
}

void VideoMixerComponent::Renderer::run()
{
    const bool measure = videotiming::enabled();
    videotiming::Window timing;
    videotiming::Window nativeTiming;
    VideoPresentationRoute route;
    VideoFramePacer presentPacer;
    bool pendingPresent = false;
    double last = juce::Time::getMillisecondCounterHiRes();
    while (! threadShouldExit())
    {
        const double start = juce::Time::getMillisecondCounterHiRes();
        const double dt = start - last;
        last = start;

        bool changedTarget = false;
        if (targetChanged_.exchange(false, std::memory_order_acq_rel))
        {
            std::shared_ptr<VideoPresentationTarget> next;
            { const juce::ScopedLock lock(frontLock_); next = presentationTarget_; }
            changedTarget = route.select(std::move(next));
        }
        const bool published = renderFrame(start, dt);
        pendingPresent |= published || changedTarget;
        if (pendingPresent)
        {
            if (route && (changedTarget || presentPacer.due(start,VideoDisplaySettings::fps())))
            {
                const double before = measure ? juce::Time::getMillisecondCounterHiRes() : 0.0;
                route.present(frontImage());
                pendingPresent = false;
                if (measure)
                {
                    const double after = juce::Time::getMillisecondCounterHiRes();
                    if (nativeTiming.add(after, after - before, true))
                        nativeTiming.report("native-submit", after);
                }
            }
        }

        // Pace to ~60 fps: sleep whatever remains of the frame slot (min 1 ms so
        // an overloaded pass still yields; wait() returns early on stopThread).
        constexpr double kFrameMs = 1000.0 / 60.0;
        const double elapsed = juce::Time::getMillisecondCounterHiRes() - start;
        if (measure && timing.add(start + elapsed, elapsed, published))
            timing.report("render", start + elapsed);
        wait((int) juce::jlimit(1.0, kFrameMs, kFrameMs - elapsed));
    }
    route.select({});
}

void VideoMixerComponent::Renderer::setPresentationTarget(
    std::shared_ptr<VideoPresentationTarget> target)
{
    const juce::ScopedLock lock(frontLock_);
    if (presentationTarget_ == target) return;
    presentationTarget_ = std::move(target);
    targetChanged_.store(true, std::memory_order_release);
}

// Pick a pool image that nothing else references, (re)sized to w×h. Invalid
// return = all busy (transient). The pool slot itself holds one reference, so
// any other owner — the published front_ / soloFront_, a ref-copy taken by an
// in-flight paint on the message thread — shows as refcount > 1: that image
// may be on screen right now and is never drawn into (no tearing).
juce::Image VideoMixerComponent::Renderer::acquireFrom(juce::Image* pool, int n, int w, int h)
{
    for (int i = 0; i < n; ++i)
    {
        auto& im = pool[i];
        if (im.isValid() && im.getReferenceCount() > 1)
            continue;
        if (! im.isValid() || im.getWidth() != w || im.getHeight() != h)
            // SoftwareImageType: peint/lu sur le thread de rendu VIDEO MIX —
            // les images Direct2D (défaut JUCE 8 Windows) y crashent (AV
            // readFromDirect2DBitmap).
            im = juce::Image(juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
        return im;
    }
    return {};
}

bool VideoMixerComponent::Renderer::renderFrame(double nowMs, double dtMs)
{
    const juce::ScopedLock cl(coreLock_);

    // Transport Stop (any thread) → blank every waterfall once.
    bool changed = false;
    const int cg = clearGen_.load(std::memory_order_acquire);
    if (cg != lastClearGen_)
    {
        lastClearGen_ = cg;
        for (auto& l : layers_)
            l.core->clear();
        changed = true;
    }

    if (layers_.empty())
    {
        // While recording with no outputs, keep the stream valid by publishing
        // blank white paper at the fixed record size (~2 fps heartbeat).
        if (recOn_.load(std::memory_order_acquire))
        {
            if (! lastRecOn_) { lastRecOn_ = true; recStartMs_ = nowMs; lastRecPushMs_ = -1.0e12; }
            // Budgeted like every other composite — the recorder upsamples it.
            int rW = recW_.load(std::memory_order_acquire);
            int rH = recH_.load(std::memory_order_acquire);
            {
                const double diag = std::hypot((double) rW, (double) rH);
                if (diag > (double) kMaxRenderDim)
                {
                    rW = juce::jmax(1, (int) std::lround((double) rW * kMaxRenderDim / diag));
                    rH = juce::jmax(1, (int) std::lround((double) rH * kMaxRenderDim / diag));
                }
            }
            if (rW > 0 && rH > 0
                && (lastRecPushMs_ < 0.0 || (nowMs - lastRecPushMs_) > 500.0))
            {
                juce::Image target = acquireTarget(rW, rH);
                if (target.isValid())
                {
                    target.clear(target.getBounds(), juce::Colours::white);
                    { const juce::ScopedLock fl(frontLock_); front_ = target;
                      previewFront_ = juce::Image(); }
                    haveFrame_ = true;
                    frameCounter_.fetch_add(1, std::memory_order_release);
                    processor_.pushRecordVideoFrame(target, (nowMs - recStartMs_) / 1000.0);
                    lastRecPushMs_ = nowMs;
                    return true;
                }
            }
            return false;
        }
        if (lastRecOn_) lastRecOn_ = false;

        if (haveFrame_)
        {
            { const juce::ScopedLock fl(frontLock_); front_ = juce::Image();
              previewFront_ = juce::Image(); }
            haveFrame_ = false;
            frameCounter_.fetch_add(1, std::memory_order_release);
            return true;
        }
        return false;
    }

    const uint64_t vs = viewState_.load(std::memory_order_acquire);
    int W = (int) ((vs >> 25) & 0xffffff);
    int H = (int) ((vs >> 1)  & 0xffffff);
    // The size the composite will actually be SHOWN at, kept before recording
    // overrides W/H with the record aspect (see the preview publish below).
    const int viewWpx = W;
    const int viewHpx = H;

    // Outputs to publish ALONE (the VIEWPORT pads' thumbnails). A pad showing
    // is a view too: it keeps the warp/composite pass alive even when no
    // master view is on screen.
    const uint32_t solo = soloMask_.load(std::memory_order_acquire);
    auto wantsSolo = [solo](int slot)
    {
        return slot >= 0 && slot < ChainModel::kMaxVideoSlots && ((solo >> slot) & 1u) != 0;
    };
    bool visible = (vs & 1u) != 0 || solo != 0;

    // Recording overrides the view: render ONE composite at the RECORD ASPECT
    // (still budgeted — see below) and force it visible so frames flow even when
    // the preview is hidden or collapsed. The preview shows that same image.
    const bool rec = recOn_.load(std::memory_order_acquire);
    if (rec && ! lastRecOn_) { lastRecOn_ = true; recStartMs_ = nowMs; lastRecPushMs_ = -1.0e12; }
    else if (! rec && lastRecOn_) lastRecOn_ = false;
    if (rec) { W = recW_.load(std::memory_order_acquire);
               H = recH_.load(std::memory_order_acquire); visible = true; }

    if (W <= 0 || H <= 0)
        return false;

    {
        // Per-output render budget (see kMaxRenderDim above). It applies while
        // RECORDING too, which is the whole point of the pass: the composite is
        // rendered at the cost of a preview and the RECORDER upsamples it to the
        // chosen encode resolution on its way into the pixel buffer. Recording
        // used to lift the cap entirely, so a 2160p session asked this CPU warp
        // for 4406² canvases PER OUTPUT — that, not the encoder, is what froze
        // the interface. A waterfall loses nothing visible to the upsample: the
        // warp pass already box-averaged it down out of the history.
        const int n = (int) layers_.size();
        int cap = kMaxRenderDim;
        if (n > 1)
            cap = juce::jmax(kMinRenderDim, (int) ((double) kMaxRenderDim / std::sqrt((double) n)));
        const double diag = std::hypot((double) W, (double) H);
        if (diag > (double) cap) { W = juce::jmax(1, (int) std::lround((double) W * cap / diag));
                                   H = juce::jmax(1, (int) std::lround((double) H * cap / diag)); }
    }

    // Advance every output's waterfall (real-dt scroll + ring drain). This runs
    // even when no view is visible so the history stays truthful and the rings
    // never back up; the warp/composite below is skipped in that case.
    for (auto& l : layers_)
    {
        l.core->setDisplaySize(W, H);
        changed |= l.core->tick(nowMs, dtMs);
    }

    pendingFrameChange_ |= changed;
    if (! visible)
        return false;

    // Collect lines/history at the existing tick rate; throttle only expensive
    // warp/rotation/compositing. Recording retains its existing render cadence,
    // while the native/software screen presenters still obey the display cap.
    if (! renderPacer_.due(nowMs,rec ? kFps : VideoDisplaySettings::fps()))
        return false;
    changed = pendingFrameChange_;
    pendingFrameChange_ = false;

    // Signature of everything the composite depends on OUTSIDE the warps: render
    // size, per-output mix controls, and the drawWarp-time params (zoom/mode are
    // applied at draw time, not baked into the warp). A frozen (paused) output
    // with untouched controls yields an identical signature → no publish → the
    // presenter never invalidates a static view (the pause "blinking" fix).
    auto& apvts = processor_.getAPVTS();
    auto rawOf = [&apvts](const juce::String& id, float def)
    {
        auto* p = apvts.getRawParameterValue(id);
        return p ? p->load() : def;
    };
    const float fx = rawOf(VideoMixFocus::kXId, 0.0f);   // the projector
    const float fy = rawOf(VideoMixFocus::kYId, 0.0f);
    // Sample mix controls ONCE, before choosing which expensive warps to build.
    // A block-rate LFO may change again during rendering: culling and blending
    // must use the same snapshot or a newly visible layer could show a stale warp.
    struct MixState
    {
        bool enabled = false;
        float level = 0.0f;
        int blend = 0;
    };
    std::array<MixState, ChainModel::kMaxVideoSlots> mix;
    constexpr float kInvisibleLevel = 0.002f; // existing composite cutoff
    std::vector<float> sig;
    sig.reserve(5 + layers_.size() * 9);
    sig.push_back((float) W);
    sig.push_back((float) H);
    sig.push_back((float) solo);   // a pad appearing/leaving → (re)publish once
    sig.push_back(fx);
    sig.push_back(fy);
    for (size_t li = 0; li < layers_.size(); ++li)
    {
        auto& l = layers_[li];
        auto& m = mix[li];
        const float follow = follow_[l.slot].load(std::memory_order_relaxed);
        const float level = rawOf(vsMixParam(l.slot, "level"), 1.0f);
        m.enabled = rawOf(vsParam(l.slot, "enabled"), 1.0f) >= 0.5f;
        m.blend = (int) rawOf(vsMixParam(l.slot, "blend"), 0.0f);
        m.level = juce::jlimit(0.0f, 1.0f, level)
                * VideoMixFocus::weight((int) li, (int) layers_.size(), fx, fy)
                * follow;
        sig.push_back((float) l.slot);
        // QUANTIZED: the follow mask moves at every frame while it is armed,
        // and a raw value here would defeat the whole point of the signature
        // (a frozen output would republish 60 times a second).
        sig.push_back(VideoMixFollow::quantized(follow));
        sig.push_back(level);
        sig.push_back((float) m.blend);
        sig.push_back(m.enabled ? 1.0f : 0.0f);
        sig.push_back(rawOf(vsParam(l.slot, "zoom"),     1.0f));
        sig.push_back(rawOf(vsParam(l.slot, "rotation"), 0.0f));
        sig.push_back(rawOf(vsParam(l.slot, "centerX"),  0.0f));
        sig.push_back(rawOf(vsParam(l.slot, "centerY"),  0.0f));
    }
    if (sig != lastSig_) { lastSig_ = std::move(sig); changed = true; }

    // Hidden outputs keep ticking above (their live history must survive a
    // fast switch), but need no warp until they contribute or a pad requests
    // them. tick() leaves the warp dirty, so revealing one rebuilds from the
    // CURRENT history in this same frame, including after a pause.
    for (size_t li = 0; li < layers_.size(); ++li)
        if (mix[li].enabled
            && (mix[li].level > kInvisibleLevel || wantsSolo(layers_[li].slot)))
            changed |= layers_[li].core->buildWarp();

    // Recording heartbeat: guarantee the first frame and ≥~2 fps so the video
    // track's duration tracks the (continuous) audio even while frozen.
    if (rec && (lastRecPushMs_ < 0.0 || (nowMs - lastRecPushMs_) > 500.0))
        changed = true;

    if (! changed && haveFrame_)
        return false;

    // Release expired previews before acquiring, including aliases of master
    // pool images. Waiting until publication can prevent that publication.
    {
        const juce::ScopedLock lock(frontLock_);
        for (int slot = 0; slot < ChainModel::kMaxVideoSlots; ++slot)
            if (! wantsSolo(slot)) soloFront_[slot] = {};
    }
    juce::Image target = acquireTarget(W, H);
    if (! target.isValid())
    {
        // The warps/signature were already updated. Keep this frame dirty so
        // a frozen source retries when a presenter releases its buffer.
        lastSig_.clear();
        return false;
    }

    // Enabled outputs only (a disabled output is dropped from the composite —
    // and is NOT warped for its pad either: a disabled output costs nothing,
    // its thumbnail is the blank paper and the pad says "OFF").
    int numEnabled = 0;
    int numVisible = 0;
    Layer* single = nullptr;
    size_t singleIdx = 0;
    for (size_t li = 0; li < layers_.size(); ++li)
    {
        auto& l = layers_[li];
        if (mix[li].enabled)
        {
            ++numEnabled;
            if (mix[li].level > kInvisibleLevel)
            {
                ++numVisible;
                single    = &l;
                singleIdx = li;
            }
        }
        // Solo pool released as soon as the slot is no longer requested (or
        // no longer rendered).
        if (! wantsSolo(l.slot) || ! mix[li].enabled)
            for (auto& im : l.soloPool) im = juce::Image();
    }

    // Solo frames: an output drawn ALONE — full level, no blend, its own
    // paper — for the pads that asked. drawAlone() targets the layer's own
    // pool so the image a pad is painting is never written to. Invalid
    // result = pool momentarily busy → that slot keeps its previous solo.
    // soloBlank = requested but disabled → published as "no image".
    auto drawAlone = [&](Layer& l) -> juce::Image
    {
        juce::Image im = acquireFrom(l.soloPool, 2, W, H);
        if (im.isValid()) l.core->drawWarp(im);
        return im;
    };
    juce::Image soloNew[ChainModel::kMaxVideoSlots];
    bool        soloBlank[ChainModel::kMaxVideoSlots] = {};

    // Empty master (nothing enabled) = blank paper → WHITE. With layers the
    // compositing base stays black: Add/Screen accumulate light, so their
    // neutral element is black (a white base would saturate them).
    // The single-output fast path below writes EVERY pixel itself (drawWarp
    // paints its own border since 2026-09-04), so it needs no base at all —
    // skipping the clear there saves a full-surface pass per frame.
    // Full level must be exact: the integer blend scales 0.999 to 255/256.
    const bool soleFullLevel = (numVisible == 1 && mix[singleIdx].level >= 1.0f);
    if (! soleFullLevel)
        target.clear(target.getBounds(),
                     numEnabled == 0 ? juce::Colours::white : juce::Colours::black);

    if (soleFullLevel)
    {
        // One VISIBLE output at full level: over a black base Mix/Add/Screen all reduce
        // to "just the source" — draw straight into the target, skip the blend.
        single->core->drawWarp(target);
        // The composite IS that output alone: its solo shares the image (no
        // second warp draw). Masked but enabled outputs still serve their pads.
        for (size_t li = 0; li < layers_.size(); ++li)
        {
            auto& l = layers_[li];
            if (wantsSolo(l.slot))
            {
                if (&l == single) soloNew[l.slot]   = target;
                else if (mix[li].enabled) soloNew[l.slot] = drawAlone(l);
                else                     soloBlank[l.slot] = true;
            }
        }
    }
    else
    {
        for (size_t li = 0; li < layers_.size(); ++li)
        {
            auto& l = layers_[li];
            const bool alone = wantsSolo(l.slot);
            if (! mix[li].enabled)
            {
                if (alone) soloBlank[l.slot] = true;
                continue;
            }
            // Fully masked (projector and/or audio-follow): it adds nothing to
            // the composite, so it is not rasterized either — a mask that
            // hides an output SAVES its blit. Its VIEWPORT pad still gets its
            // own full-level image below (a thumbnail never lies about what
            // the output is producing).
            if (mix[li].level <= kInvisibleLevel && ! alone)
                continue;
            // A soloed layer's solo image doubles as its blend source (no
            // extra warp draw); otherwise (or if its pool was busy) the
            // reusable scratch.
            juce::Image src;
            if (alone)
                src = soloNew[l.slot] = drawAlone(l);
            if (! src.isValid())
            {
                if (! l.scratch.isValid() || l.scratch.getWidth() != W || l.scratch.getHeight() != H)
                    l.scratch = juce::Image(juce::Image::ARGB, W, H, true, juce::SoftwareImageType());
                l.core->drawWarp(l.scratch);
                src = l.scratch;
            }
            if (mix[li].level > kInvisibleLevel)   // a masked solo still gets its pad image
                blendLayer(target, src, mix[li].level, mix[li].blend);
        }
    }

    // ── Preview publish ──────────────────────────────────────────────────────
    // When the composite is much larger than the view that will show it — the
    // normal case while recording, where the render follows the record aspect
    // rather than the window — the message thread would rescale several
    // megapixels on EVERY paint, inside the same CoreGraphics call that already
    // owns the UI. Do that downsample once here instead, in parallel, and hand
    // the presenter something already the right size. Above the threshold the
    // presenter keeps blitting the composite directly (no extra pass, no loss).
    juce::Image previewNew;
    bool softwarePreview;
    { const juce::ScopedLock fl(frontLock_); softwarePreview = presentationTarget_ == nullptr; }
    // Core Animation scales the composite itself; avoid a redundant CPU pass.
    if (softwarePreview && viewWpx > 0 && viewHpx > 0)
    {
        const double viewDiag = std::hypot((double) viewWpx, (double) viewHpx);
        const double compDiag = std::hypot((double) W, (double) H);
        if (viewDiag > 8.0 && compDiag > viewDiag * 1.4)
        {
            // Keep the composite's aspect: the presenter letterboxes, and a
            // preview that lied about it would crop the master.
            const double k  = viewDiag / compDiag;
            const int    pw = juce::jmax(2, (int) std::lround((double) W * k));
            const int    ph = juce::jmax(2, (int) std::lround((double) H * k));
            juce::Image dst = acquireFrom(previewPool_, 2, pw, ph);
            if (dst.isValid() && videoblit::scaleARGB(dst, target))
                previewNew = dst;
        }
    }

    {
        const juce::ScopedLock fl(frontLock_);
        front_       = target;
        previewFront_ = previewNew;   // invalid → the presenter uses the composite
        for (int s = 0; s < ChainModel::kMaxVideoSlots; ++s)
        {
            if (! wantsSolo(s) || soloBlank[s]) soloFront_[s] = juce::Image();
            else if (soloNew[s].isValid())       soloFront_[s] = soloNew[s];
        }
    }
    haveFrame_ = true;
    frameCounter_.fetch_add(1, std::memory_order_release);

    if (rec)
    {
        processor_.pushRecordVideoFrame(target, (nowMs - recStartMs_) / 1000.0);
        lastRecPushMs_ = nowMs;
    }
    return true;
}

//==============================================================================
VideoMixerComponent::VideoMixerComponent(Sp3ctraAudioProcessor& proc)
    : processor_(proc)
{
    // paint() fills every pixel (dark bg + master + strip). Marking the component
    // opaque stops JUCE from repainting the (non-opaque) parent behind it on every
    // presenter repaint — a non-opaque fast-repainting component is a classic
    // flicker source. The original VideoDisplayComponent did the same.
    setOpaque(true);
    renderer_ = std::make_unique<Renderer>(proc);
    radar_ = std::make_unique<VideoMixRadar>(proc);
    radar_->onOutputClicked = [this](int slot) { if (onOutputClicked) onOutputClicked(slot); };
    addAndMakeVisible(*radar_);
#if JUCE_MAC
    if (NativeVideoView::enabled())
    {
        nativePreview_ = std::make_unique<NativeVideoView>();
        addAndMakeVisible(*nativePreview_);
    }
#endif
    refreshActiveSlots();
    startTimerHz(kFps);

    // Reopen the detached master window where the user left it (machine
    // prefs). Editor rebuilds (session restore) pass through here too — the
    // outgoing instance saved its bounds in ~MasterWindow.
    if (MachinePrefs::file().getBoolValue("videoMixWin.open", false))
    {
        toggleDetachedWindow();
        if (window_ != nullptr
            && MachinePrefs::file().getBoolValue("videoMixWin.fullscreen", false))
            window_->toggleFullscreen();
    }
}

VideoMixerComponent::~VideoMixerComponent()
{
    if (isRecording()) endRecording();   // never leave a writer without a source
    stopTimer();
    window_.reset();
    renderer_.reset();   // joins the render thread before members are destroyed
}

//==============================================================================
void VideoMixerComponent::refreshActiveSlots()
{
    auto slots = processor_.activeVideoSlots();
    const auto labels = videoScrollOutputLabels(slots, processor_.chainNames());
    if (slots == activeSlots_)
    {
        // Same topology, but a chain RENAME can change the spoke labels —
        // the toile relabels in place (same slots → bindings kept).
        if (labels != labels_)
        {
            labels_ = labels;
            rebuildStrip();
        }
        return;                 // unchanged — keep existing cores/bindings
    }
    activeSlots_ = slots;
    labels_      = labels;
    // The follow envelope is indexed by LAYER: a topology change invalidates
    // it. Release every mask to neutral and re-seed on the next tick, so a
    // slot that moved chains never inherits the mask of the one before it.
    for (int s2 = 0; s2 < ChainModel::kMaxVideoSlots; ++s2)
    {
        renderer_->setFollow(s2, 1.0f);
        followLvl_[s2] = 0.0f;
        followExempt_[s2] = false;
    }
    followArmed_ = false;
    std::vector<int> slotList;
    slotList.reserve(activeSlots_.size());
    for (const auto& [slot, chain] : activeSlots_)
        slotList.push_back(slot);
    renderer_->setSlots(slotList);   // compositing follows the rack order too
    rebuildStrip();
}

void VideoMixerComponent::rebuildStrip()
{
    // Spoke labels ("CHAIN n" or the user chain name, suffixed a/b when a
    // chain hosts SEVERAL probes) come from the shared helper — computed by
    // refreshActiveSlots() into labels_ — so the zone-3 chain tabs and the
    // ALL view name every output exactly like the toile.
    std::vector<VideoMixRadar::Output> outs;
    outs.reserve(activeSlots_.size());
    for (int i = 0; i < (int) activeSlots_.size(); ++i)
    {
        VideoMixRadar::Output o;
        o.slot     = activeSlots_[(size_t) i].first;
        o.chainIdx = activeSlots_[(size_t) i].second;
        o.label    = i < labels_.size() ? labels_[i] : juce::String();
        outs.push_back(std::move(o));
    }
    radar_->setOutputs(outs);   // same slots → relabel only

    layoutStrip();
    repaint();
}

//==============================================================================
void VideoMixerComponent::resized()
{
    layoutStrip();
}

int VideoMixerComponent::radarBandFor(int height) noexcept
{
    return juce::jlimit(kRadarMinH, kRadarMaxH,
                        (int) std::lround((float) height * kRadarShare)) + 2 * kStripPad;
}

int VideoMixerComponent::stripHeight() const noexcept
{
    return activeSlots_.empty() ? 0 : radarBandFor(getHeight());
}

int VideoMixerComponent::maxUsefulWidth(int height) const noexcept
{
    // layoutStrip(): avail = (bounds − strip).reduced(2), side = min(availW,
    // availH). Width only helps while availW <= availH, i.e. up to
    // height − stripHeight() (the ±4 of reduced(2) cancels on both axes).
    // The radar band depends on the height only, never on the width.
    return juce::jmax(0, height - (activeSlots_.empty() ? 0 : radarBandFor(height)));
}

void VideoMixerComponent::layoutStrip()
{
    auto r = getLocalBounds();

    stripArea_ = r.removeFromTop(stripHeight());
    // The toile follows the strip area, which stops stretching with a very
    // wide zone (its side labels need width, its rim needs height).
    stripArea_.setWidth(juce::jmin(stripArea_.getWidth(), Sp3ctraTheme::kMaxContentW));
    stripArea_.setX((getWidth() - stripArea_.getWidth()) / 2);   // centred like the preview
    radar_->setBounds(stripArea_.reduced(kStripPad));

    // The preview is kept square so it reads correctly whatever the scroll
    // direction is. Fit the largest centred square inside the remaining area.
    auto avail = r.reduced(2);
    const int side = juce::jmin(avail.getWidth(), avail.getHeight());
    masterArea_ = juce::Rectangle<int>(0, 0, side, side).withCentre(avail.getCentre());
#if JUCE_MAC
    if (nativePreview_) nativePreview_->setBounds(masterArea_);
#endif
}

//==============================================================================
void VideoMixerComponent::renderMaster(juce::Graphics& g, juce::Rectangle<int> dest)
{
    if (dest.isEmpty()) return;
    g.setColour(juce::Colours::white);   // no frame yet → blank paper, not black
    g.fillRect(dest);

    const double t0 = juce::Time::getMillisecondCounterHiRes();
    // Read before taking the image: a concurrent publish can undercount one
    // frame, but repeated paints of the same frame never masquerade as 60 fps.
    const bool measure = videotiming::enabled();
    const uint32_t timingFrame = measure ? renderer_->frameCounter() : 0;
    const juce::Image frame = renderer_->presentImage();
    if (frame.isValid())
    {
        // Medium, not high: this blit runs at 60 Hz on the message thread
        // (column preview + detached window) and the warp pass already did
        // a proper box-average — a waterfall shows no difference.
        g.setImageResamplingQuality(juce::Graphics::mediumResamplingQuality);
        // Uniform fit (letterbox on white): the published frame's aspect can lag
        // the destination (resize in flight, square column preview vs detached
        // window) — never crop or distort it.
        g.drawImage(frame, dest.toFloat(), juce::RectanglePlacement::centred);
    }

    // What this blit actually costs on THIS machine, at this window size, in
    // this theme — smoothed. The presenter uses it to stop queueing frames the
    // message thread cannot absorb (see timerCallback); a fixed rate cap would
    // either throttle a machine with headroom or fail to save a loaded one.
    const double dt = juce::Time::getMillisecondCounterHiRes() - t0;
    lastPaintMs_ = lastPaintMs_ * 0.8 + dt * 0.2;
    if (measure)
    {
        const bool fresh = frame.isValid() && timingFrame != timingPresentedFrame_;
        timingPresentedFrame_ = timingFrame;
        if (paintTiming_.add(t0 + dt, dt, fresh))
            paintTiming_.report("present", t0 + dt);
    }
}

void VideoMixerComponent::currentView(int& w, int& h, bool& visible) const
{
    w = masterArea_.getWidth();
    h = masterArea_.getHeight();
    visible = isShowing() && ! masterArea_.isEmpty();
    if (window_ != nullptr && window_->isVisible())
        if (auto* c = window_->getContentComponent())
            if (c->getWidth() > 0 && c->getHeight() > 0)
            {
                w = c->getWidth();
                h = c->getHeight();
                visible = true;
            }
}

void VideoMixerComponent::requestOutputPreview(int slot)
{
    if (slot >= 0 && slot < ChainModel::kMaxVideoSlots)
        soloReqMs_[slot] = juce::Time::getMillisecondCounterHiRes();
}

juce::Image VideoMixerComponent::outputFrame(int slot) const
{
    return renderer_->soloImage(slot);
}

uint32_t VideoMixerComponent::frameCounter() const
{
    return renderer_->frameCounter();
}

juce::Point<int> VideoMixerComponent::viewSize() const
{
    int w = 0, h = 0;
    bool visible = false;
    currentView(w, h, visible);
    return { w, h };
}

//==============================================================================
// AUDIO FOLLOW — "mettre en avant ce que l'on entend".
//
// The presenter is the ONE place this is computed: it owns the chain model
// (message thread), it already ticks at the render cadence, and it feeds both
// consumers — the compositor (Renderer::setFollow, read per frame) and the
// toile (VideoMixRadar::setFollow, drawn on the polygon). One smoother, so
// the picture and the mix can never disagree by a frame.
//
// Disarmed it costs one atomic parameter read: the mask stays at 1 and no
// chain is walked. Arming SEEDS the envelope with the current levels (no
// fade-in from zero, the mix answers the button immediately).
void VideoMixerComponent::updateFollow(double nowMs)
{
    auto& apvts = processor_.getAPVTS();
    auto raw = [&apvts](const juce::String& id, float def)
    {
        auto* p = apvts.getRawParameterValue(id);
        return p != nullptr ? p->load() : def;
    };
    const bool armed = raw(VideoMixFollow::kArmId, 0.0f) >= 0.5f;
    const bool toAudio = raw(VideoMixFollow::kDirId, 1.0f) >= 0.5f;
    const int  n     = juce::jmin((int) activeSlots_.size(),
                                  (int) ChainModel::kMaxVideoSlots);

    const float dtMs = (float) juce::jlimit(1.0, 250.0, nowMs - lastFollowMs_);
    lastFollowMs_ = nowMs;

    // Release the video mask when disarmed or following VIDEO -> AUDIO.
    auto releaseVideo = [&]
    {
        for (int i = 0; i < n; ++i)
        {
            renderer_->setFollow(activeSlots_[(size_t) i].first, 1.0f);
            radar_->setFollow(i, 1.0f, false);
        }
    };


    if (! armed)
    {
        if (followArmed_)   // falling edge: release the UI-owned video mask
        {
            followArmed_ = false;
            releaseVideo();
        }
        return;
    }

    const float depth = raw(VideoMixFollow::kDepthId, 1.0f);
    const bool  seed  = ! followArmed_;
    followArmed_ = true;

    // ── VIDEO MIX → AUDIO ────────────────────────────────────────────────────
    // The PROJECTOR is the audio mix of the chains on the toile — a
    // crossfade, not a mask (VideoMixFocus::audioWeight): at the centre every
    // chain sits at 50 % of its send faders, pulled onto a spoke that chain
    // alone is heard and the others are silent, anywhere else the blend.
    // The handles (the PICTURE's levels), an output switched off and the
    // Amount say nothing here: one hand, one law. The video side is left
    // alone (a mask each way would be a loop).
    if (toAudio)
    {
        releaseVideo();

        // The processor drives the linked audio at block rate. This UI
        // timer owns only the opposite (audio -> video) envelope below.
        return;
    }

    // AUDIO MIX -> VIDEO
    float setLvl[ChainModel::kMaxChains], meter[ChainModel::kMaxChains];
    processor_.chainAudioLevels(setLvl, meter);
    const bool live = raw(VideoMixFollow::kModeId, 0.0f) >= 0.5f;

    // LIVE weighs each chain by what it is really staging, against the
    // loudest of the outputs ON THE TOILE — the peak is taken over the same
    // set the mask is applied to, never over chains one cannot see.
    float peakMeter = 0.0f;
    if (live)
        for (int i = 0; i < n; ++i)
        {
            const int c = activeSlots_[(size_t) i].second;
            if (c >= 0 && c < ChainModel::kMaxChains && setLvl[c] >= 0.0f)
                peakMeter = juce::jmax(peakMeter, meter[c]);
        }

    for (int i = 0; i < n; ++i)
    {
        const int c = activeSlots_[(size_t) i].second;
        const bool ok = c >= 0 && c < ChainModel::kMaxChains;
        const float target = ! ok ? VideoMixFollow::kExempt
                           : live ? VideoMixFollow::liveLevel(setLvl[c], meter[c], peakMeter)
                                  : setLvl[c];
        followExempt_[i] = target < 0.0f;
        followLvl_[i] = followExempt_[i]
                          ? 0.0f
                          : (seed ? target
                                  : VideoMixFollow::smooth(followLvl_[i], target, dtMs));
    }

    for (int i = 0; i < n; ++i)
    {
        const float w = followExempt_[i] ? 1.0f
                                         : VideoMixFollow::weight(followLvl_[i], depth);
        renderer_->setFollow(activeSlots_[(size_t) i].first, w);
        radar_->setFollow(i, w, ! followExempt_[i]);
    }
}

//==============================================================================
bool VideoMixerComponent::updatePresentationTarget()
{
    std::shared_ptr<VideoPresentationTarget> target;
#if JUCE_MAC
    const bool detached = isWindowOpen();
    if (nativePreview_)
    {
        nativePreview_->setVisible(! detached);
        if (! detached && isShowing()) target = nativePreview_->target();
    }
    if (detached)
        if (auto* view = dynamic_cast<MasterView*>(window_->getContentComponent()))
            target = view->target();
#endif
    renderer_->setPresentationTarget(target);
    return target != nullptr;
}

void VideoMixerComponent::timerCallback()
{
    // Presenter only: tell the renderer what to render (view size in LOGICAL px,
    // window content when open, else the column preview) and whether anything is
    // on screen, then invalidate the views ONLY when a new frame was published.
    // A frozen output publishes nothing → its static image is never repainted →
    // it can never be presented half-painted (the historical pause flicker).
    int w = 0, h = 0;
    bool visible = false;
    currentView(w, h, visible);
    renderer_->setViewState(w, h, visible);
    const bool nativePresentation = updatePresentationTarget();

    // Solo requests still alive → the outputs the renderer must also publish
    // alone (a pad that hid or died simply stops requesting).
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        uint32_t mask = 0;
        for (int s = 0; s < ChainModel::kMaxVideoSlots; ++s)
            if (soloReqMs_[s] > 0.0 && now - soloReqMs_[s] < kSoloHoldMs)
                mask |= 1u << s;
        renderer_->setSoloMask(mask);
    }

    // AUDIO FOLLOW — read the AUDIO MIX, smooth, publish the mask (both the
    // compositor and the toile read the SAME numbers, see updateFollow).
    updateFollow(juce::Time::getMillisecondCounterHiRes());

    // Flow pulses → the toile's handles (30 Hz is plenty for an LED; the
    // toile only repaints a handle whose reading actually moved).
    if ((++flowTick_ & 1) == 0)
        for (int i = 0; i < (int) activeSlots_.size(); ++i)
        {
            float fn = 0.0f, fp = 0.0f, pw = 0.0f;
            renderer_->flowOf(activeSlots_[(size_t) i].first, fn, fp, pw);
            radar_->setFlow(i, fn, fp, pw);
        }

    // Native presentation is submitted directly from the renderer. This timer
    // only maintains layout, pad requests and meters; no paint hop per frame.
    if (nativePresentation) return;
    const uint32_t fc = renderer_->frameCounter();
    if (fc == lastPresented_)
        return;

    // Adaptive presentation: when a single blit costs more than a frame slot
    // (a fullscreen master on a Retina display), invalidating on every published
    // frame just queues paints the message thread will never catch up with, and
    // the whole UI goes with it. Present as fast as the paint actually allows —
    // the RECORDING is unaffected, its frames come off the render thread.
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (! softwarePresentPacer_.due(nowMs,VideoDisplaySettings::fps())) return;
    constexpr double kSlotMs = 1000.0 / (double) kFps;
    if (lastPaintMs_ > kSlotMs && (nowMs - lastPresentMs_) < lastPaintMs_)
        return;
    lastPresentMs_ = nowMs;
    lastPresented_ = fc;

    // One view per frame: the detached window when open (the column shows a
    // placeholder then), else the column preview.
    if (isWindowOpen())
    {
        if (auto* c = window_->getContentComponent())
            c->repaint();
    }
    else
        repaint(masterArea_);
}

//==============================================================================
void VideoMixerComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0c0c10));

    // Master display — blit of the render thread's latest published composite.
    // While the detached window is open IT shows the master: the column keeps
    // a static placeholder instead of a second 60 Hz multi-megapixel blit of
    // the same frame (the presenter stops repainting this area, see
    // timerCallback) — user request 2026-08-30.
    const bool detached = isWindowOpen();
    if (detached)
    {
        g.setColour(juce::Colour(0xff14141c));
        g.fillRect(masterArea_);
        g.setColour(moduleColour(ModuleType::VideoScroll).withAlpha(0.6f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.drawText("VIDEO MIX is shown in its window",
                   masterArea_, juce::Justification::centred, true);
    }
    else
    {
#if JUCE_MAC
        if (nativePreview_) g.fillAll(juce::Colour(0xff0c0c10));
        else
#endif
            renderMaster(g, masterArea_);
    }

    if (activeSlots_.empty())
    {
        if (! detached)
        {
            // Dark text: the empty master area is WHITE (blank paper).
            g.setColour(juce::Colour(0xff5a6070));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText("Patch a VIDEO SCROLL output into a chain",
                       masterArea_, juce::Justification::centred, true);
        }
        return;
    }

    // Strip band background — the toile (radar_) paints its own frame on it.
    g.setColour(juce::Colour(0xff14141c));
    g.fillRect(stripArea_);
}

//==============================================================================
void VideoMixerComponent::setAllPaused(bool paused)
{
    auto& apvts = processor_.getAPVTS();
    const float v = paused ? 1.0f : 0.0f;
    for (const auto& [slot, chain] : activeSlots_)
        if (auto* p = apvts.getParameter(vsParam(slot, "paused")))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost(v);
            p->endChangeGesture();
        }
}

void VideoMixerComponent::stopAll()
{
    setAllPaused(true);
    renderer_->requestClear();   // consumed by the render thread on its next pass
}

//==============================================================================
bool VideoMixerComponent::beginRecording(const juce::File& out, int height, juce::String& err)
{
    if (isRecording()) { err = "Already recording."; return false; }

    // Record aspect = the current view aspect (detached window content if open,
    // else the square preview). `height` is the chosen vertical resolution.
    double aspect = 1.0;
    if (window_ != nullptr && window_->isVisible())
        if (auto* c = window_->getContentComponent())
            if (c->getWidth() > 0 && c->getHeight() > 0)
                aspect = (double) c->getWidth() / (double) c->getHeight();

    const int H = juce::jmax(2, height) & ~1;
    int       W = juce::jlimit(2, 7680, juce::roundToInt((double) H * aspect)) & ~1;

    // Arm the recorder FIRST (AVAssetWriter live), then let the render thread
    // start streaming the fixed-size composite.
    if (! processor_.startVideoRecording(out, W, H, 60.0, err))
        return false;
    renderer_->setRecordTarget(W, H, true);
    return true;
}

void VideoMixerComponent::endRecording()
{
    renderer_->setRecordTarget(0, 0, false);   // stop pushing frames first
    processor_.stopVideoRecording();           // finalise + close the file
}

bool VideoMixerComponent::isRecording() const noexcept
{
    return processor_.isVideoRecording();
}

//==============================================================================
void VideoMixerComponent::toggleDetachedWindow()
{
    if (window_ != nullptr)
    {
        window_.reset();
    }
    else
    {
        window_ = std::make_unique<MasterWindow>(*this);
        window_->onCloseRequested = [this]
        {
            juce::MessageManager::callAsync([this]
            {
                window_.reset();
                // Explicit close — do NOT reopen on the next launch.
                MachinePrefs::file().setValue("videoMixWin.open", false);
                repaint(masterArea_);   // placeholder → live preview again
                if (onWindowStateChanged) onWindowStateChanged();
            });
        };
    }
    // Explicit open/close (or the ctor reopen, which re-asserts true).
    MachinePrefs::file().setValue("videoMixWin.open", window_ != nullptr);
    repaint(masterArea_);   // live preview ⇄ placeholder
    if (onWindowStateChanged) onWindowStateChanged();
}

void VideoMixerComponent::requestFullscreenWindow()
{
    if (window_ == nullptr)
        toggleDetachedWindow();
    if (window_ != nullptr)
        window_->toggleFullscreen();
}

bool VideoMixerComponent::isWindowOpen() const noexcept
{
    return window_ != nullptr && window_->isVisible();
}
