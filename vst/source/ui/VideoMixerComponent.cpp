#include "VideoMixerComponent.h"

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
        // window content from flickering under the 60 fps repaint driven by owner_.
        setOpaque(true);
    }

    // Driven by the mixer's 60 fps clock (owner repaints us after each tick), so
    // the fullscreen view scrolls at full rate without its own timer.
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
        setVisible(true);
    }

    void closeButtonPressed() override { if (onCloseRequested) onCloseRequested(); }

    void toggleFullscreen() { setFullScreen(! isFullScreen()); }

    std::function<void()> onCloseRequested;
};

//==============================================================================
VideoMixerComponent::VideoMixerComponent(Sp3ctraAudioProcessor& proc)
    : processor_(proc)
{
    // paint() fills every pixel (dark bg + master + strip). Marking the component
    // opaque stops JUCE from repainting the (non-opaque) parent behind it on every
    // 60 fps repaint — a non-opaque fast-repainting component is a classic flicker
    // source. The original VideoDisplayComponent did the same (setOpaque(true)).
    setOpaque(true);
    refreshActiveSlots();
    startTimerHz(kFps);
}

VideoMixerComponent::~VideoMixerComponent()
{
    stopTimer();
    window_.reset();
}

//==============================================================================
void VideoMixerComponent::refreshActiveSlots()
{
    auto slots = processor_.activeVideoSlots();
    if (slots == activeSlots_)
        return;                 // unchanged — keep existing cores/attachments
    activeSlots_ = slots;
    rebuildStrip();
}

void VideoMixerComponent::rebuildStrip()
{
    voices_.clear();
    auto& apvts = processor_.getAPVTS();

    for (int slot : activeSlots_)
    {
        auto v = std::make_unique<Voice>();
        v->slot = slot;
        v->core = std::make_unique<VideoScrollRenderCore>(processor_, slot);

        v->level.setSliderStyle(juce::Slider::LinearHorizontal);
        v->level.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
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

void VideoMixerComponent::layoutStrip()
{
    auto r = getLocalBounds();
    const int n = (int) voices_.size();
    const int stripH = (n > 0) ? (kStripPad + n * (kRowH + kRowGap)) : 0;

    stripArea_  = r.removeFromBottom(stripH);

    // The preview is kept square so it reads correctly whatever the scroll
    // direction is. Fit the largest centred square inside the remaining area.
    auto avail = r.reduced(2);
    const int side = juce::jmin(avail.getWidth(), avail.getHeight());
    masterArea_ = juce::Rectangle<int>(0, 0, side, side).withCentre(avail.getCentre());

    // Lay out each fader row: [label 38][level slider …][blend 72]
    auto strip = stripArea_.reduced(kStripPad, kStripPad / 2);
    for (auto& v : voices_)
    {
        auto row = strip.removeFromTop(kRowH);
        strip.removeFromTop(kRowGap);
        row.removeFromLeft(40);                      // label drawn in paint()
        v->blend.setBounds(row.removeFromRight(72).reduced(0, 1));
        row.removeFromRight(6);
        v->level.setBounds(row.reduced(0, 2));
    }
}

//==============================================================================
namespace
{
    // Composite one rendered layer into the master with level + blend mode.
    // Opaque ARGB line-pointer blend (fast enough for 60 fps at window resolution).
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

bool VideoMixerComponent::singleDirect() const
{
    if (voices_.size() != 1) return false;
    auto& apvts = processor_.getAPVTS();
    const int slot = voices_[0]->slot;
    // Disabled output → route through the offscreen path so composite() drops it
    // and the master stays black (no crisp direct paint of a muted output).
    if (auto* ep = apvts.getRawParameterValue(vsParam(slot, "enabled")))
        if (ep->load() < 0.5f) return false;
    float level = 1.0f;
    if (auto* lp = apvts.getRawParameterValue(vsMixParam(slot, "level"))) level = lp->load();
    // Blend mode is irrelevant with a SINGLE layer over a black base: Mix, Add and
    // Screen at full level all reduce to "just the source". Requiring blend==Mix
    // here needlessly forced the offscreen composite path (logical-res → upscaled
    // to retina = soft), losing the crisp direct-into-retina render. Ignore blend.
    return level >= 0.999f;
}

bool VideoMixerComponent::composite()
{
    // Engine (history) resolution = the view's LOGICAL size. The detached window
    // (when open) is the biggest consumer, so size the history to it; otherwise to
    // the in-column master area. Like the original renderer, the warp is built at
    // this logical size and upscaled ONCE to physical by the destination's
    // retina-backed Graphics — no extra offscreen resample.
    int W = masterArea_.getWidth();
    int H = masterArea_.getHeight();
    if (window_ != nullptr && window_->isVisible())
        if (auto* c = window_->getContentComponent())
            if (c->getWidth() > 0 && c->getHeight() > 0) { W = c->getWidth(); H = c->getHeight(); }
    if (W <= 0 || H <= 0) return false;

    // Render-resolution ceiling. The whole warp is a per-pixel scalar pass on the
    // message thread, so its cost is O(W×H) every tick — a 2560×1440 fullscreen
    // window collapsed the frame-rate to ~8 fps. 1600 keeps the warp affordable
    // while the final medium-quality blit upscales to the window's physical size
    // with no visible loss for a waterfall. The small in-column preview is far
    // below this ceiling, so it is unaffected.
    constexpr int kMaxRenderDim = 1600;
    const int big = juce::jmax(W, H);
    if (big > kMaxRenderDim) { W = juce::jmax(1, W * kMaxRenderDim / big);
                               H = juce::jmax(1, H * kMaxRenderDim / big); }

    // Advance every output's waterfall once per 60 fps tick, then build the heavy
    // warp ONCE here (shared by the column preview AND the detached window). The
    // per-view draw is a cheap blit (drawWarp) — this is what keeps the message
    // thread from melting when a large/fullscreen window is open.
    bool changed = false;
    for (auto& v : voices_)
    {
        v->core->setDisplaySize(W, H);
        changed |= v->core->tick();
        changed |= v->core->buildWarp();
    }

    auto& apvts = processor_.getAPVTS();
    auto levelOf   = [&](int slot) { auto* p = apvts.getRawParameterValue(vsMixParam(slot, "level")); return p ? p->load() : 1.0f; };
    auto modeOf    = [&](int slot) { auto* p = apvts.getRawParameterValue(vsMixParam(slot, "blend")); return p ? (int) p->load() : 0; };
    auto enabledOf = [&](int slot) { auto* p = apvts.getRawParameterValue(vsParam(slot, "enabled")); return p ? p->load() >= 0.5f : true; };

    // Signature of everything the views depend on OUTSIDE the warp itself: the
    // render size, the per-voice mix controls, and the drawWarp-time params
    // (zoom/mode are applied at draw time, not baked into warpBuf_). A frozen
    // (paused) output with untouched controls yields an identical signature →
    // composite() reports "unchanged" and the mixer skips the repaint. Without
    // this, the 60 fps repaint of a static image occasionally gets presented
    // half-painted by the OS → the pause "blinking".
    std::vector<float> sig;
    sig.reserve(2 + voices_.size() * 6);
    sig.push_back((float) W);
    sig.push_back((float) H);
    auto zoomOf = [&](int slot) { auto* p = apvts.getRawParameterValue(vsParam(slot, "zoom")); return p ? p->load() : 1.0f; };
    auto rotOf  = [&](int slot) { auto* p = apvts.getRawParameterValue(vsParam(slot, "mode")); return p ? p->load() : 0.0f; };
    for (auto& v : voices_)
    {
        sig.push_back((float) v->slot);
        sig.push_back(levelOf(v->slot));
        sig.push_back((float) modeOf(v->slot));
        sig.push_back(enabledOf(v->slot) ? 1.f : 0.f);
        sig.push_back(zoomOf(v->slot));
        sig.push_back(rotOf(v->slot));
    }
    if (sig != mixSig_) { mixSig_ = std::move(sig); changed = true; }

    // Single full-level Mix output → painted DIRECTLY by renderMaster() into the
    // destination Graphics (no offscreen image, no blend, crisp). Skip the composite.
    if (singleDirect())
        return changed;

    // Nothing changed → master_ already holds the current composite; skip the
    // per-layer blend pass entirely (and tell the caller not to repaint).
    if (! changed && master_.isValid() && master_.getWidth() == W && master_.getHeight() == H)
        return false;

    // Multi-output → composite into the offscreen master_ at the engine resolution.
    if (! master_.isValid() || master_.getWidth() != W || master_.getHeight() != H)
        master_ = juce::Image(juce::Image::ARGB, W, H, true);
    master_.clear(master_.getBounds(), juce::Colours::black);

    for (auto& v : voices_)
    {
        if (! enabledOf(v->slot))
            continue;   // output disabled → not composited into the master
        if (! v->scratch.isValid() || v->scratch.getWidth() != W || v->scratch.getHeight() != H)
            v->scratch = juce::Image(juce::Image::ARGB, W, H, true);
        { juce::Graphics gs(v->scratch); v->core->drawWarp(gs, W, H); }  // warp already built above
        blendLayer(master_, v->scratch, levelOf(v->slot), modeOf(v->slot));
    }
    return true;
}

void VideoMixerComponent::renderMaster(juce::Graphics& g, juce::Rectangle<int> dest)
{
    if (dest.isEmpty()) return;
    g.setColour(juce::Colours::black);
    g.fillRect(dest);
    if (voices_.empty()) return;

    if (singleDirect())
    {
        // Paint the single output straight into g at the destination's own
        // resolution (retina-backed for the fullscreen window → one resample).
        juce::Graphics::ScopedSaveState s(g);
        g.reduceClipRegion(dest);
        g.setOrigin(dest.getX(), dest.getY());
        voices_[0]->core->drawWarp(g, dest.getWidth(), dest.getHeight());  // warp built in composite()
        return;
    }

    if (master_.isValid())
    {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        g.drawImage(master_, dest.toFloat(), juce::RectanglePlacement::stretchToFit);
    }
}

void VideoMixerComponent::timerCallback()
{
    // Only invalidate the views when this tick actually produced a new frame.
    // While paused (or otherwise frozen) the previous — complete — frame stays on
    // screen untouched: invalidating a static area 60×/s occasionally got a
    // half-painted buffer presented (black flash over the frozen image).
    if (! composite())
        return;
    repaint(masterArea_);
    if (window_ != nullptr)
        if (auto* c = window_->getContentComponent())
            c->repaint();   // drive the detached/fullscreen view at the same 60 fps
}

//==============================================================================
void VideoMixerComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0c0c10));

    // Master display — shared render path (direct-paint single output, or scale
    // the offscreen composite).
    renderMaster(g, masterArea_);

    if (voices_.empty())
    {
        g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.55f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.drawText("Patch a VIDEO SCROLL output into a chain",
                   masterArea_, juce::Justification::centred, true);
        return;
    }

    // Strip background + per-row labels (aligned with the controls in layoutStrip).
    g.setColour(juce::Colour(0xff14141c));
    g.fillRect(stripArea_);

    auto strip = stripArea_.reduced(kStripPad, kStripPad / 2);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
    for (auto& v : voices_)
    {
        auto row = strip.removeFromTop(kRowH);
        strip.removeFromTop(kRowGap);
        g.setColour(juce::Colour(0xff5ad0c8));
        g.drawText("OUT " + juce::String(v->slot + 1),
                   row.removeFromLeft(40).reduced(2, 0),
                   juce::Justification::centredLeft, false);
    }
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
    for (auto& voice : voices_)
        voice->core->clear();
    composite();
    repaint();
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
                if (onWindowStateChanged) onWindowStateChanged();
            });
        };
    }
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
