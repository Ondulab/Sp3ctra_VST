#include "VideoMixerComponent.h"
#include "ModuleCatalog.h"
#include "../session/MachinePrefs.h"   // detached-window state is machine-scoped
#include "../video/VideoScrollMode.h"  // videoScrollOutputLabels() (shared row labels)
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
    }

    // Driven by the mixer's presenter clock (owner repaints us whenever the
    // render thread publishes a new frame). paint() is a cheap image blit.
    void paint(juce::Graphics& g) override
    {
        owner_.renderMaster(g, getLocalBounds());
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (onFullscreenRequested) onFullscreenRequested();
    }

    std::function<void()> onFullscreenRequested;

private:
    VideoMixerComponent& owner_;
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

        for (int y = 0; y < h; ++y)
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
    }
}

//==============================================================================
// Renderer — the background render thread
//==============================================================================
VideoMixerComponent::Renderer::Renderer(Sp3ctraAudioProcessor& p)
    : juce::Thread("VideoMixRender"), processor_(p)
{
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
    double last = juce::Time::getMillisecondCounterHiRes();
    while (! threadShouldExit())
    {
        const double start = juce::Time::getMillisecondCounterHiRes();
        const double dt = start - last;
        last = start;

        renderFrame(start, dt);

        // Pace to ~60 fps: sleep whatever remains of the frame slot (min 1 ms so
        // an overloaded pass still yields; wait() returns early on stopThread).
        constexpr double kFrameMs = 1000.0 / 60.0;
        const double elapsed = juce::Time::getMillisecondCounterHiRes() - start;
        wait((int) juce::jlimit(1.0, kFrameMs, kFrameMs - elapsed));
    }
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
            const int rW = recW_.load(std::memory_order_acquire);
            const int rH = recH_.load(std::memory_order_acquire);
            if (rW > 0 && rH > 0
                && (lastRecPushMs_ < 0.0 || (nowMs - lastRecPushMs_) > 500.0))
            {
                juce::Image target = acquireTarget(rW, rH);
                if (target.isValid())
                {
                    target.clear(target.getBounds(), juce::Colours::white);
                    { const juce::ScopedLock fl(frontLock_); front_ = target; }
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
            { const juce::ScopedLock fl(frontLock_); front_ = juce::Image(); }
            haveFrame_ = false;
            frameCounter_.fetch_add(1, std::memory_order_release);
            return true;
        }
        return false;
    }

    const uint64_t vs = viewState_.load(std::memory_order_acquire);
    int W = (int) ((vs >> 25) & 0xffffff);
    int H = (int) ((vs >> 1)  & 0xffffff);

    // Outputs to publish ALONE (the VIEWPORT pads' thumbnails). A pad showing
    // is a view too: it keeps the warp/composite pass alive even when no
    // master view is on screen.
    const uint32_t solo = soloMask_.load(std::memory_order_acquire);
    auto wantsSolo = [solo](int slot)
    {
        return slot >= 0 && slot < ChainModel::kMaxVideoSlots && ((solo >> slot) & 1u) != 0;
    };
    bool visible = (vs & 1u) != 0 || solo != 0;

    // Recording overrides the view: render ONE fixed hi-res composite (no √N
    // budget) and force it visible so frames flow even when the preview is
    // hidden/collapsed. The preview downsamples the same front image.
    const bool rec = recOn_.load(std::memory_order_acquire);
    if (rec && ! lastRecOn_) { lastRecOn_ = true; recStartMs_ = nowMs; lastRecPushMs_ = -1.0e12; }
    else if (! rec && lastRecOn_) lastRecOn_ = false;
    if (rec) { W = recW_.load(std::memory_order_acquire);
               H = recH_.load(std::memory_order_acquire); visible = true; }

    if (W <= 0 || H <= 0)
        return false;

    if (! rec)
    {
        // Per-output render budget (see kMaxRenderDim above).
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

    if (! visible)
        return false;

    // Heavy warp ONCE per output per frame (cached while frozen).
    for (auto& l : layers_)
        changed |= l.core->buildWarp();

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
    std::vector<float> sig;
    sig.reserve(3 + layers_.size() * 8);
    sig.push_back((float) W);
    sig.push_back((float) H);
    sig.push_back((float) solo);   // a pad appearing/leaving → (re)publish once
    for (auto& l : layers_)
    {
        sig.push_back((float) l.slot);
        sig.push_back(rawOf(vsMixParam(l.slot, "level"), 1.0f));
        sig.push_back(rawOf(vsMixParam(l.slot, "blend"), 0.0f));
        sig.push_back(rawOf(vsParam(l.slot, "enabled"),  1.0f));
        sig.push_back(rawOf(vsParam(l.slot, "zoom"),     1.0f));
        sig.push_back(rawOf(vsParam(l.slot, "rotation"), 0.0f));
        sig.push_back(rawOf(vsParam(l.slot, "centerX"),  0.0f));
        sig.push_back(rawOf(vsParam(l.slot, "centerY"),  0.0f));
    }
    if (sig != lastSig_) { lastSig_ = std::move(sig); changed = true; }

    // Recording heartbeat: guarantee the first frame and ≥~2 fps so the video
    // track's duration tracks the (continuous) audio even while frozen.
    if (rec && (lastRecPushMs_ < 0.0 || (nowMs - lastRecPushMs_) > 500.0))
        changed = true;

    if (! changed && haveFrame_)
        return false;

    juce::Image target = acquireTarget(W, H);
    if (! target.isValid())
        return false;   // every pool buffer is momentarily referenced — retry next pass

    // Enabled outputs only (a disabled output is dropped from the composite).
    int numEnabled = 0;
    Layer* single = nullptr;
    for (auto& l : layers_)
    {
        if (rawOf(vsParam(l.slot, "enabled"), 1.0f) >= 0.5f)
        {
            ++numEnabled;
            single = &l;
        }
        // Solo pool released as soon as the slot is no longer requested.
        if (! wantsSolo(l.slot))
            for (auto& im : l.soloPool) im = juce::Image();
    }

    // Solo frames: an output drawn ALONE — full level, no blend, its own
    // paper — for the pads that asked. drawAlone() targets the layer's own
    // pool so the image a pad is painting is never written to. Invalid
    // result = pool momentarily busy → that slot keeps its previous solo.
    auto drawAlone = [&](Layer& l) -> juce::Image
    {
        juce::Image im = acquireFrom(l.soloPool, 2, W, H);
        if (im.isValid()) { juce::Graphics gs(im); l.core->drawWarp(gs, W, H); }
        return im;
    };
    juce::Image soloNew[ChainModel::kMaxVideoSlots];

    // Empty master (nothing enabled) = blank paper → WHITE. With layers the
    // compositing base stays black: Add/Screen accumulate light, so their
    // neutral element is black (a white base would saturate them).
    target.clear(target.getBounds(),
                 numEnabled == 0 ? juce::Colours::white : juce::Colours::black);

    if (numEnabled == 1 && rawOf(vsMixParam(single->slot, "level"), 1.0f) >= 0.999f)
    {
        // One output at full level: over a black base Mix/Add/Screen all reduce
        // to "just the source" — draw straight into the target, skip the blend.
        { juce::Graphics g(target); single->core->drawWarp(g, W, H); }
        // The composite IS that output alone: its solo shares the image (no
        // second warp draw). Any other requested output is disabled here.
        for (auto& l : layers_)
            if (wantsSolo(l.slot))
                soloNew[l.slot] = (&l == single) ? target : drawAlone(l);
    }
    else
    {
        for (auto& l : layers_)
        {
            const bool enabled = rawOf(vsParam(l.slot, "enabled"), 1.0f) >= 0.5f;
            const bool alone   = wantsSolo(l.slot);
            if (! enabled && ! alone)
                continue;
            // A soloed layer's solo image doubles as its blend source (no
            // extra warp draw); otherwise (or if its pool was busy) the
            // reusable scratch.
            juce::Image src;
            if (alone)
                src = soloNew[l.slot] = drawAlone(l);
            if (! enabled)
                continue;
            if (! src.isValid())
            {
                if (! l.scratch.isValid() || l.scratch.getWidth() != W || l.scratch.getHeight() != H)
                    l.scratch = juce::Image(juce::Image::ARGB, W, H, true, juce::SoftwareImageType());
                { juce::Graphics gs(l.scratch); l.core->drawWarp(gs, W, H); }
                src = l.scratch;
            }
            blendLayer(target, src,
                       rawOf(vsMixParam(l.slot, "level"), 1.0f),
                       (int) rawOf(vsMixParam(l.slot, "blend"), 0.0f));
        }
    }

    {
        const juce::ScopedLock fl(frontLock_);
        front_ = target;
        for (int s = 0; s < ChainModel::kMaxVideoSlots; ++s)
        {
            if (! wantsSolo(s))            soloFront_[s] = juce::Image();
            else if (soloNew[s].isValid()) soloFront_[s] = soloNew[s];
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
    if (slots == activeSlots_)
        return;                 // unchanged — keep existing cores/attachments
    activeSlots_ = slots;
    std::vector<int> slotList;
    slotList.reserve(activeSlots_.size());
    for (const auto& [slot, chain] : activeSlots_)
        slotList.push_back(slot);
    renderer_->setSlots(slotList);   // compositing follows the rack order too
    rebuildStrip();
}

void VideoMixerComponent::rebuildStrip()
{
    voices_.clear();
    auto& apvts = processor_.getAPVTS();

    // Row labels ("CHAIN n", suffixed a/b when a chain hosts SEVERAL probes)
    // come from the shared helper, so the zone-3 chain tabs and the ALL view
    // name every output exactly like this strip.
    const auto labels = videoScrollOutputLabels(activeSlots_);
    for (int i = 0; i < (int) activeSlots_.size(); ++i)
    {
        const int slot = activeSlots_[(size_t) i].first;
        auto v = std::make_unique<Voice>();
        v->slot  = slot;
        v->label = labels[i];

        v->level.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        v->level.setRange(0.0, 1.0, 0.01);
        addAndMakeVisible(v->level);

        v->blend.addItem("Mix",    1);
        v->blend.addItem("Add",    2);
        v->blend.addItem("Screen", 3);
        addAndMakeVisible(v->blend);

        v->levelAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, vsMixParam(slot, "level"), v->level);
        v->blendAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            apvts, vsMixParam(slot, "blend"), v->blend);
        v->levelLearn = std::make_unique<MidiLearnAttachment>(
            processor_.getMidiMap(), v->level, vsMixParam(slot, "level"));
        v->blendLearn = std::make_unique<MidiLearnAttachment>(
            processor_.getMidiMap(), v->blend, vsMixParam(slot, "blend"));

        voices_.push_back(std::move(v));
    }

    layoutStrip();
    repaint();
}

//==============================================================================
void VideoMixerComponent::resized()
{
    layoutStrip();
}

int VideoMixerComponent::stripHeight() const noexcept
{
    const int n = (int) voices_.size();
    return (n > 0) ? (kStripPad + n * (kRowH + kRowGap)) : 0;
}

int VideoMixerComponent::maxUsefulWidth(int height) const noexcept
{
    // layoutStrip(): avail = (bounds − strip).reduced(2), side = min(availW,
    // availH). Width only helps while availW <= availH, i.e. up to
    // height − stripHeight() (the ±4 of reduced(2) cancels on both axes).
    return juce::jmax(0, height - stripHeight());
}

void VideoMixerComponent::layoutStrip()
{
    auto r = getLocalBounds();

    stripArea_  = r.removeFromTop(stripHeight());
    // Fader rows follow the strip area, which stops stretching with a very
    // wide zone (paint() walks the same rect, so labels stay aligned).
    stripArea_.setWidth(juce::jmin(stripArea_.getWidth(), Sp3ctraTheme::kMaxContentW));

    // The preview is kept square so it reads correctly whatever the scroll
    // direction is. Fit the largest centred square inside the remaining area.
    auto avail = r.reduced(2);
    const int side = juce::jmin(avail.getWidth(), avail.getHeight());
    masterArea_ = juce::Rectangle<int>(0, 0, side, side).withCentre(avail.getCentre());

    // Lay out each fader row: [label kLabelW][level slider …][blend 72]
    auto strip = stripArea_.reduced(kStripPad, kStripPad / 2);
    for (auto& v : voices_)
    {
        auto row = strip.removeFromTop(kRowH);
        strip.removeFromTop(kRowGap);
        row.removeFromLeft(kLabelW);                 // label drawn in paint()
        v->blend.setBounds(row.removeFromRight(72).reduced(0, 1));
        row.removeFromRight(6);
        v->level.setBounds(row.reduced(0, 2));
    }
}

//==============================================================================
void VideoMixerComponent::renderMaster(juce::Graphics& g, juce::Rectangle<int> dest)
{
    if (dest.isEmpty()) return;
    g.setColour(juce::Colours::white);   // no frame yet → blank paper, not black
    g.fillRect(dest);

    const juce::Image frame = renderer_->frontImage();
    if (frame.isValid())
    {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        // Uniform fit (letterbox on white): the published frame's aspect can lag
        // the destination (resize in flight, square column preview vs detached
        // window) — never crop or distort it.
        g.drawImage(frame, dest.toFloat(), juce::RectanglePlacement::centred);
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

    const uint32_t fc = renderer_->frameCounter();
    if (fc == lastPresented_)
        return;
    lastPresented_ = fc;

    repaint(masterArea_);
    if (window_ != nullptr)
        if (auto* c = window_->getContentComponent())
            c->repaint();
}

//==============================================================================
void VideoMixerComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0c0c10));

    // Master display — blit of the render thread's latest published composite.
    renderMaster(g, masterArea_);

    if (voices_.empty())
    {
        // Dark text: the empty master area is now WHITE (blank paper).
        g.setColour(juce::Colour(0xff5a6070));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.drawText("Patch a VIDEO SCROLL output into a chain",
                   masterArea_, juce::Justification::centred, true);
        return;
    }

    // Strip background + per-row labels (aligned with the controls in layoutStrip).
    g.setColour(juce::Colour(0xff14141c));
    g.fillRect(stripArea_);

    // Labels are links to the output's chain tab (onOutputClicked): the
    // hovered one brightens + underlines.
    auto strip = stripArea_.reduced(kStripPad, kStripPad / 2);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
    const auto accent = moduleColour(ModuleType::VideoScroll);
    for (int i = 0; i < (int) voices_.size(); ++i)
    {
        auto& v  = voices_[(size_t) i];
        auto row = strip.removeFromTop(kRowH);
        strip.removeFromTop(kRowGap);
        const bool hov = (i == hoverRow_);
        const auto lab = row.removeFromLeft(kLabelW).reduced(2, 0);
        g.setColour(hov ? accent.brighter(0.5f) : accent);
        g.drawText(v->label, lab, juce::Justification::centredLeft, false);
        if (hov)
        {
            const int tw = (int) std::ceil(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), v->label));
            g.fillRect(lab.getX(), lab.getBottom() - 5, juce::jmin(tw, lab.getWidth()), 1);
        }
    }
}

int VideoMixerComponent::rowLabelAt(juce::Point<int> p) const noexcept
{
    auto strip = stripArea_.reduced(kStripPad, kStripPad / 2);
    for (int i = 0; i < (int) voices_.size(); ++i)
    {
        auto row = strip.removeFromTop(kRowH);
        strip.removeFromTop(kRowGap);
        if (row.removeFromLeft(kLabelW).contains(p))
            return i;
    }
    return -1;
}

void VideoMixerComponent::mouseMove(const juce::MouseEvent& e)
{
    const int r = rowLabelAt(e.getPosition());
    setMouseCursor(r >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    if (r != hoverRow_) { hoverRow_ = r; repaint(stripArea_); }
}

void VideoMixerComponent::mouseExit(const juce::MouseEvent&)
{
    if (hoverRow_ != -1) { hoverRow_ = -1; repaint(stripArea_); }
}

void VideoMixerComponent::mouseUp(const juce::MouseEvent& e)
{
    if (! e.mouseWasClicked() || e.mods.isPopupMenu()) return;
    const int r = rowLabelAt(e.getPosition());
    if (r >= 0 && onOutputClicked)
        onOutputClicked(voices_[(size_t) r]->slot);
}

//==============================================================================
void VideoMixerComponent::setAllPaused(bool paused)
{
    auto& apvts = processor_.getAPVTS();
    const float v = paused ? 1.0f : 0.0f;
    for (auto& voice : voices_)
        if (auto* p = apvts.getParameter(vsParam(voice->slot, "paused")))
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
                if (onWindowStateChanged) onWindowStateChanged();
            });
        };
    }
    // Explicit open/close (or the ctor reopen, which re-asserts true).
    MachinePrefs::file().setValue("videoMixWin.open", window_ != nullptr);
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
