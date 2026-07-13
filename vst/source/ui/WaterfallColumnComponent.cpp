#include "WaterfallColumnComponent.h"
#include <cmath>

//==============================================================================
// MiniButton — header glyphs drawn with paths
//==============================================================================
void WaterfallColumnComponent::MiniButton::paintButton(juce::Graphics& g,
                                                       bool isMouseOver,
                                                       bool isButtonDown)
{
    const auto b = getLocalBounds().toFloat().reduced(1.f);

    const juce::Colour bg(0xff222230);
    g.setColour(isButtonDown ? bg.brighter(0.30f)
              : isMouseOver  ? bg.brighter(0.12f)
              :                bg);
    g.fillRoundedRectangle(b, 3.f);

    const juce::Colour fg = isMouseOver ? juce::Colour(0xffe8eef8)
                                        : juce::Colour(0xff9aa6ba);
    g.setColour(fg);

    const auto inner = b.reduced(4.5f);

    switch (glyph)
    {
        case Glyph::Detach:   // ⧉ — two overlapping frames (pop-out window)
        {
            const float wq = inner.getWidth()  * 0.62f;
            const float hq = inner.getHeight() * 0.62f;
            g.drawRect(juce::Rectangle<float>(inner.getX(), inner.getBottom() - hq, wq, hq), 1.2f);
            g.drawRect(juce::Rectangle<float>(inner.getRight() - wq, inner.getY(), wq, hq), 1.2f);
            break;
        }

        case Glyph::Fullscreen:  // ⛶ — large frame with outward corner ticks
        {
            g.drawRect(inner, 1.2f);
            const float t = juce::jmin(inner.getWidth(), inner.getHeight()) * 0.30f;
            juce::Path p;
            // top-left corner ticks pointing out
            p.startNewSubPath(inner.getX(), inner.getY() + t);  p.lineTo(inner.getX(), inner.getY());  p.lineTo(inner.getX() + t, inner.getY());
            // top-right
            p.startNewSubPath(inner.getRight() - t, inner.getY());  p.lineTo(inner.getRight(), inner.getY());  p.lineTo(inner.getRight(), inner.getY() + t);
            // bottom-right
            p.startNewSubPath(inner.getRight(), inner.getBottom() - t);  p.lineTo(inner.getRight(), inner.getBottom());  p.lineTo(inner.getRight() - t, inner.getBottom());
            // bottom-left
            p.startNewSubPath(inner.getX() + t, inner.getBottom());  p.lineTo(inner.getX(), inner.getBottom());  p.lineTo(inner.getX(), inner.getBottom() - t);
            g.strokePath(p, juce::PathStrokeType(1.6f));
            break;
        }

        case Glyph::Collapse: // ✕
        {
            juce::Path p;
            p.startNewSubPath(inner.getX(), inner.getY());
            p.lineTo(inner.getRight(), inner.getBottom());
            p.startNewSubPath(inner.getRight(), inner.getY());
            p.lineTo(inner.getX(), inner.getBottom());
            g.strokePath(p, juce::PathStrokeType(1.6f));
            break;
        }

        case Glyph::Expand:   // ◀ — pointing into the column (expand back)
        {
            juce::Path p;
            p.addTriangle(inner.getRight(), inner.getY(),
                          inner.getRight(), inner.getBottom(),
                          inner.getX(),     inner.getCentreY());
            g.fillPath(p);
            break;
        }
    }
}

//==============================================================================
// IconToggleButton — toolbar display toggles drawn with paths
//==============================================================================
void WaterfallColumnComponent::IconToggleButton::paintButton(juce::Graphics& g,
                                                             bool isMouseOver,
                                                             bool isButtonDown)
{
    const auto b  = getLocalBounds().toFloat().reduced(1.f);
    const bool on = getToggleState();

    const juce::Colour accent(0xff66cc88);
    const juce::Colour bg = on ? juce::Colour(0xff223a2e) : juce::Colour(0xff222230);
    g.setColour(isButtonDown ? bg.brighter(0.30f)
              : isMouseOver  ? bg.brighter(0.12f)
              :                bg);
    g.fillRoundedRectangle(b, 3.f);
    if (on)
    {
        g.setColour(accent.withAlpha(0.7f));
        g.drawRoundedRectangle(b, 3.f, 1.f);
    }

    const juce::Colour fg = on          ? accent
                          : isMouseOver ? juce::Colour(0xffe8eef8)
                          :               juce::Colour(0xff9aa6ba);
    const auto inner = b.reduced(4.5f);

    switch (glyph)
    {
        case Glyph::Invert:   // ◐ — half-filled circle (negative image)
        {
            const auto circle = inner.withSizeKeepingCentre(
                juce::jmin(inner.getWidth(), inner.getHeight()),
                juce::jmin(inner.getWidth(), inner.getHeight()));
            g.setColour(fg);
            g.drawEllipse(circle, 1.2f);
            juce::Path half;
            half.addPieSegment(circle, 0.f, juce::MathConstants<float>::pi, 0.f);
            g.fillPath(half);
            break;
        }

        case Glyph::ColorMode: // ▥ — three vertical bars (R/G/B channels)
        {
            const float barW = inner.getWidth() / 4.f;
            const float gap  = barW * 0.5f;
            float x = inner.getX();
            const juce::Colour cols[3] = {
                on ? juce::Colour(0xffe06060) : fg,
                on ? juce::Colour(0xff60c860) : fg,
                on ? juce::Colour(0xff6080e0) : fg
            };
            for (auto col : cols)
            {
                g.setColour(col);
                g.fillRoundedRectangle(x, inner.getY(), barW, inner.getHeight(), 1.f);
                x += barW + gap;
            }
            break;
        }
    }
}

//==============================================================================
// TransportButton — scroll transport (Play/Pause toggle, momentary Stop)
//==============================================================================
void WaterfallColumnComponent::TransportButton::paintButton(juce::Graphics& g,
                                                            bool isMouseOver,
                                                            bool isButtonDown)
{
    const auto b = getLocalBounds().toFloat().reduced(1.f);

    const juce::Colour bg(0xff222230);
    g.setColour(isButtonDown ? bg.brighter(0.30f)
              : isMouseOver  ? bg.brighter(0.12f)
              :                bg);
    g.fillRoundedRectangle(b, 3.f);

    const auto inner = b.reduced(4.5f);

    if (glyph == Glyph::PlayPause)
    {
        const bool paused = getToggleState();
        if (paused)
        {
            // ▶ play (accent green = "press to resume")
            g.setColour(juce::Colour(0xff66cc88));
            juce::Path tri;
            tri.addTriangle(inner.getX(), inner.getY(),
                            inner.getX(), inner.getBottom(),
                            inner.getRight(), inner.getCentreY());
            g.fillPath(tri);
        }
        else
        {
            // ⏸ pause (two bars, neutral)
            g.setColour(isMouseOver ? juce::Colour(0xffe8eef8) : juce::Colour(0xff9aa6ba));
            const float bw = inner.getWidth() * 0.30f;
            g.fillRect(inner.getX(),            inner.getY(), bw, inner.getHeight());
            g.fillRect(inner.getRight() - bw,   inner.getY(), bw, inner.getHeight());
        }
    }
    else // Stop
    {
        // ⏹ filled square (reddish = halt + clear)
        g.setColour(isMouseOver ? juce::Colour(0xffe0a0a0) : juce::Colour(0xffb88a8a));
        const float s  = juce::jmin(inner.getWidth(), inner.getHeight());
        const auto  sq = inner.withSizeKeepingCentre(s, s);
        g.fillRoundedRectangle(sq, 1.5f);
    }
}

//==============================================================================
// WaterfallColumnComponent
//==============================================================================
WaterfallColumnComponent::WaterfallColumnComponent(Sp3ctraAudioProcessor& p)
    : processor(p)
{
    videoTab = std::make_unique<VideoScrollTab>(processor);

    viewport.setViewedComponent(videoTab.get(), false);
    viewport.setScrollBarsShown(true, false);
    viewport.setScrollBarThickness(8);
    addAndMakeVisible(viewport);

    // Window-open state drives the green status dot — refresh it (and repaint)
    // whenever the detached window opens or closes (by any route, incl. its own
    // [✕] close button).
    videoTab->onWindowStateChanged = [this]
    {
        const bool open = videoTab && videoTab->isVideoWindowOpen();
        if (open != windowOpen) { windowOpen = open; repaint(); }
    };
    windowOpen = videoTab->isVideoWindowOpen();

    detachBtn.setTooltip("Open / close the detached video window");
    detachBtn.onClick = [this]
    {
        if (videoTab) videoTab->toggleDetachedWindow();
    };
    addAndMakeVisible(detachBtn);

    fullscreenBtn.setTooltip("Open the detached video window in full screen");
    fullscreenBtn.onClick = [this]
    {
        if (videoTab) videoTab->requestFullscreenWindow();
    };
    addAndMakeVisible(fullscreenBtn);

    // Collapsed-grip copies of the window-display controls (same actions).
    detachGrip.setTooltip(detachBtn.getTooltip());
    detachGrip.onClick = [this] { if (videoTab) videoTab->toggleDetachedWindow(); };
    addChildComponent(detachGrip);

    fullscreenGrip.setTooltip(fullscreenBtn.getTooltip());
    fullscreenGrip.onClick = [this] { if (videoTab) videoTab->requestFullscreenWindow(); };
    addChildComponent(fullscreenGrip);

    collapseBtn.setTooltip("Collapse column");
    collapseBtn.onClick = [this] { setCollapsed(true, true); };
    addAndMakeVisible(collapseBtn);

    expandBtn.setTooltip("Expand video scroll column");
    expandBtn.onClick = [this] { setCollapsed(false, true); };
    addChildComponent(expandBtn);

    auto& apvts = processor.getAPVTS();

    // ── Transport (play/pause/stop) — drives the scroll engine ────────────────
    // Play/Pause is a toggle bound to "videoScrollPaused" (shared with the grip
    // copy and any other view).  Stop freezes AND clears the waterfall.
    auto doStop = [this]
    {
        if (auto* prm = processor.getAPVTS().getParameter("videoScrollPaused"))
            prm->setValueNotifyingHost(1.0f);   // freeze
        processor.requestVideoScrollClear();     // blank the image
    };

    // Pressing PLAY (un-pausing) must guarantee the waterfall is on screen: if
    // the detached window is closed, open it. Covers the case where the engine
    // reads as "playing" yet no window is showing. The play/pause buttons keep
    // their APVTS attachment (a Listener); onClick fires AFTER it, so the toggle
    // state / param already reflect the new value when we read it here.
    auto openWindowWhenPlaying = [this](const juce::Button& b)
    {
        const bool paused = b.getToggleState();
        if (!paused && videoTab && !videoTab->isVideoWindowOpen())
            videoTab->openDetachedWindow();
    };

    playPauseBtn.setTooltip("Play / Pause the scroll (freezes the waterfall in place).\n"
                            "Pressing Play also opens the video window if it is closed.");
    addAndMakeVisible(playPauseBtn);
    playPauseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "videoScrollPaused", playPauseBtn);
    playPauseBtn.onClick = [this, openWindowWhenPlaying]
    {
        openWindowWhenPlaying(playPauseBtn);
    };

    stopBtn.setTooltip("Stop: freeze and clear the waterfall (restart blank).");
    stopBtn.onClick = doStop;
    addAndMakeVisible(stopBtn);

    // Collapsed-grip copies (same param / action; visible only when collapsed).
    playPauseGrip.setTooltip(playPauseBtn.getTooltip());
    addChildComponent(playPauseGrip);
    playPauseGripAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "videoScrollPaused", playPauseGrip);
    playPauseGrip.onClick = [this, openWindowWhenPlaying]
    {
        openWindowWhenPlaying(playPauseGrip);
    };

    stopGrip.setTooltip(stopBtn.getTooltip());
    stopGrip.onClick = doStop;
    addChildComponent(stopGrip);

    // ── Display toggles (M5 — migrated from the gear "Video Scroll" tab) ──────
    invertBtn.setTooltip("Invert RGB values of each pixel (negative image).");
    addAndMakeVisible(invertBtn);
    invertAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "videoInvertColor", invertBtn);

    colorModeBtn.setTooltip("On: full RGB render from CIS R/G/B channels.\n"
                            "Off: grayscale (luma BT.601).");
    addAndMakeVisible(colorModeBtn);
    colorModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "videoColorMode", colorModeBtn);

    // The column is visible by default. The detached window is no longer
    // auto-opened — it opens on demand from the header window icons.
    videoTab->onTabActivated();
}

//==============================================================================
void WaterfallColumnComponent::setCollapsed(bool shouldCollapse, bool notify)
{
    if (collapsed == shouldCollapse)
        return;

    collapsed = shouldCollapse;

    viewport     .setVisible(!collapsed);
    detachBtn    .setVisible(!collapsed);
    fullscreenBtn.setVisible(!collapsed);
    collapseBtn  .setVisible(!collapsed);
    invertBtn  .setVisible(!collapsed);
    colorModeBtn.setVisible(!collapsed);
    playPauseBtn.setVisible(!collapsed);
    stopBtn    .setVisible(!collapsed);
    expandBtn  .setVisible(collapsed);
    playPauseGrip.setVisible(collapsed);
    stopGrip   .setVisible(collapsed);
    detachGrip    .setVisible(collapsed);
    fullscreenGrip.setVisible(collapsed);

    if (videoTab)
    {
        if (collapsed) videoTab->onTabDeactivated();
        else           videoTab->onTabActivated();
    }

    resized();
    repaint();

    if (notify && onCollapseToggled)
        onCollapseToggled(collapsed);
}

//==============================================================================
void WaterfallColumnComponent::resized()
{
    const int W = getWidth();
    const int H = getHeight();

    if (collapsed)
    {
        // Vertical grip: expand triangle, then stacked transport (▶/⏸ then ⏹),
        // then the window-display controls (⧉ detach, ⛶ fullscreen), then (in
        // paint) the dotted spine + centred rotated caption.
        const int gw = juce::jmax(12, W - 6);     // square side (~18 at kGripW=24)
        expandBtn     .setBounds(2, 4, W - 4, 18);
        playPauseGrip .setBounds(3, 26,                gw, gw);
        stopGrip      .setBounds(3, 26 + 1 * (gw + 4), gw, gw);
        detachGrip    .setBounds(3, 26 + 2 * (gw + 4), gw, gw);
        fullscreenGrip.setBounds(3, 26 + 3 * (gw + 4), gw, gw);
        return;
    }

    // Header mini buttons: only the collapse button remains on the right. The
    // centred "● VIDEO SCROLL" title is drawn in paint(); the window-display
    // icons moved down into the control band.
    const int btn = kHeaderH - 4;
    collapseBtn  .setBounds(W - btn - 2,    2, btn, btn);

    // Combined control band — one bandeau grouping (left → right):
    //   window-display [⧉][⛶] · transport [▶/⏸][⏹] CENTRED · colorimetry [◐][▥]
    {
        const int top  = kHeaderH;
        const int bsz  = kToolbarH - 8;
        const int ty   = top + (kToolbarH - bsz) / 2;

        // Window-display controls on the LEFT.
        detachBtn    .setBounds(6,             ty, bsz, bsz);
        fullscreenBtn.setBounds(6 + bsz + 4,   ty, bsz, bsz);

        // Transport CENTRED (under the title).
        const int pairW = 2 * bsz + 4;                 // play/pause + stop
        const int px    = juce::jmax(6, (W - pairW) / 2);
        playPauseBtn.setBounds(px,           ty, bsz, bsz);
        stopBtn     .setBounds(px + bsz + 4, ty, bsz, bsz);

        // Colorimetry toggles on the RIGHT.
        colorModeBtn.setBounds(W - bsz - 4,      ty, bsz, bsz);
        invertBtn   .setBounds(W - 2 * bsz - 8,  ty, bsz, bsz);
    }

    // Content viewport
    const int contentY = kHeaderH + kToolbarH;
    viewport.setBounds(0, contentY, W, juce::jmax(0, H - contentY));

    if (videoTab)
    {
        const int cw = juce::jmax(120, viewport.getWidth() - viewport.getScrollBarThickness());
        const int ch = juce::jmax(kContentMinH, viewport.getHeight());
        videoTab->setSize(cw, ch);
    }
}

//==============================================================================
void WaterfallColumnComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff181820));

    const int W = getWidth();

    if (collapsed)
    {
        // Vertical grip: stacked transport at the top (laid out in resized), a
        // dotted spine, and the "VIDEO SCROLL" caption rotated and CENTRED in
        // the column.
        const float cx = W * 0.5f;
        const float cy = getHeight() * 0.5f;

        // Dotted spine starts below the four stacked grip buttons (transport +
        // window-display), mirroring the layout in resized().
        const int gw       = juce::jmax(12, W - 6);
        const int spineTop = 26 + 3 * (gw + 4) + gw + 8;

        g.setColour(juce::Colour(0xff2c2c3a));
        for (int y = spineTop; y < getHeight() - 12; y += 9)
            g.fillEllipse(cx - 1.5f, (float) y, 3.f, 3.f);

        g.setColour(juce::Colour(0xff7a86a0));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        g.saveState();
        g.addTransform(juce::AffineTransform::rotation(
            juce::MathConstants<float>::halfPi, cx, cy));
        g.drawText("VIDEO SCROLL",
                   juce::Rectangle<float>(cx - 130.f, cy - 9.f, 260.f, 18.f),
                   juce::Justification::centred, false);
        g.restoreState();

        g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
        g.fillRect(0, 0, 1, getHeight());
        return;
    }

    // Header strip — centred "● VIDEO SCROLL" between the window icons (left)
    // and the collapse button (right). The dot is green while the detached
    // window is open, grey when closed.
    g.setColour(juce::Colour(0xff1f1f2c));
    g.fillRect(0, 0, W, kHeaderH);

    {
        const int btn   = kHeaderH - 4;
        const int zoneX = 6;                          // left margin (icons moved to band)
        const int zoneR = W - btn - 4;                // left edge of collapse btn
        const int zoneW = juce::jmax(0, zoneR - zoneX);

        const juce::Font font = juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened();
        const float textW = juce::GlyphArrangement::getStringWidth(font, "VIDEO SCROLL");
        const float dotD  = 8.f;
        const float gap   = 6.f;
        const float total = dotD + gap + textW;
        const float startX = (float) zoneX + juce::jmax(0.f, (zoneW - total) * 0.5f);
        const float cy     = kHeaderH * 0.5f;

        g.setColour(windowOpen ? juce::Colour(0xff44cc66) : juce::Colour(0xff555a66));
        g.fillEllipse(startX, cy - dotD * 0.5f, dotD, dotD);

        g.setColour(juce::Colour(0xff66cc88));
        g.setFont(font);
        g.drawText("VIDEO SCROLL",
                   juce::roundToInt(startX + dotD + gap), 0,
                   juce::roundToInt(textW) + 4, kHeaderH,
                   juce::Justification::centredLeft, true);
    }

    g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
    g.fillRect(0, kHeaderH - 1, W, 1);

    // Combined control band (transport + display toggles) — one bandeau.
    g.setColour(juce::Colour(0xff181822));
    g.fillRect(0, kHeaderH, W, kToolbarH);
    g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
    g.fillRect(0, kHeaderH + kToolbarH - 1, W, 1);

    g.fillRect(0, 0, 1, getHeight());   // left border (separation from splitter)
}
