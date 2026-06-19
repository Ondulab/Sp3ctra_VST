#include "WaterfallColumnComponent.h"

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

    detachBtn.setTooltip("Open detached video window");
    detachBtn.onClick = [this]
    {
        if (videoTab) videoTab->openDetachedWindow();
    };
    addAndMakeVisible(detachBtn);

    collapseBtn.setTooltip("Collapse column");
    collapseBtn.onClick = [this] { setCollapsed(true, true); };
    addAndMakeVisible(collapseBtn);

    expandBtn.setTooltip("Expand video scroll column");
    expandBtn.onClick = [this] { setCollapsed(false, true); };
    addChildComponent(expandBtn);

    // ── Display toolbar (M5 — migrated from the gear "Video Scroll" tab) ──────
    auto& apvts = processor.getAPVTS();

    brightnessSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    brightnessSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    brightnessSlider.setPopupDisplayEnabled(true, true, this);
    brightnessSlider.setTooltip(
        "Brightness multiplier applied before display.\n"
        "1.0 = neutral, >1.0 = brighter, <1.0 = darker.");
    addAndMakeVisible(brightnessSlider);
    brightnessAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "videoScrollBrightness", brightnessSlider);

    invertBtn.setTooltip("Invert RGB values of each pixel (negative image).");
    addAndMakeVisible(invertBtn);
    invertAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "videoInvertColor", invertBtn);

    colorModeBtn.setTooltip("On: full RGB render from CIS R/G/B channels.\n"
                            "Off: grayscale (luma BT.601).");
    addAndMakeVisible(colorModeBtn);
    colorModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "videoColorMode", colorModeBtn);

    // The column is visible by default ⇒ behave like entering the old VIDEO
    // tab (re-opens the detached window when "videoScrollEnabled" is on).
    videoTab->onTabActivated();
}

//==============================================================================
void WaterfallColumnComponent::setCollapsed(bool shouldCollapse, bool notify)
{
    if (collapsed == shouldCollapse)
        return;

    collapsed = shouldCollapse;

    viewport   .setVisible(!collapsed);
    detachBtn  .setVisible(!collapsed);
    collapseBtn.setVisible(!collapsed);
    brightnessSlider.setVisible(!collapsed);
    invertBtn  .setVisible(!collapsed);
    colorModeBtn.setVisible(!collapsed);
    expandBtn  .setVisible(collapsed);

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
        expandBtn.setBounds(2, 4, W - 4, 18);
        return;
    }

    // Header mini buttons (top-right)
    const int btn = kHeaderH - 4;
    collapseBtn.setBounds(W - btn - 2,       2, btn, btn);
    detachBtn  .setBounds(W - 2 * btn - 6,   2, btn, btn);

    // Display toolbar: [brightness ────] [◐] [▥]
    {
        const int tbBtn = kToolbarH - 6;
        const int ty    = kHeaderH + (kToolbarH - tbBtn) / 2;
        colorModeBtn.setBounds(W - tbBtn - 4,          ty, tbBtn, tbBtn);
        invertBtn   .setBounds(W - 2 * tbBtn - 8,      ty, tbBtn, tbBtn);
        brightnessSlider.setBounds(4, kHeaderH + 2,
                                   juce::jmax(40, W - 2 * tbBtn - 16),
                                   kToolbarH - 4);
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

    if (collapsed)
    {
        // Vertical grip: dotted spine + rotated caption
        g.setColour(juce::Colour(0xff2c2c3a));
        const float cx = getWidth() * 0.5f;
        for (int y = 30; y < getHeight() - 12; y += 9)
            g.fillEllipse(cx - 1.5f, (float)y, 3.f, 3.f);

        g.setColour(juce::Colour(0xff7a86a0));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        juce::GlyphArrangement ga;
        ga.addLineOfText(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened(),
                         "VIDEO SCROLL", 0.f, 0.f);
        const auto t = juce::AffineTransform::rotation(juce::MathConstants<float>::halfPi)
                           .translated(cx + 4.f, 36.f);
        ga.draw(g, t);

        g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
        g.fillRect(0, 0, 1, getHeight());
        return;
    }

    // Header strip
    g.setColour(juce::Colour(0xff1f1f2c));
    g.fillRect(0, 0, getWidth(), kHeaderH);
    g.setColour(juce::Colour(0xff66cc88));
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
    g.drawText("VIDEO SCROLL", 8, 0, getWidth() - 60, kHeaderH,
               juce::Justification::centredLeft, true);

    g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
    g.fillRect(0, kHeaderH - 1, getWidth(), 1);

    // Display toolbar strip (brightness / invert / colour mode)
    g.setColour(juce::Colour(0xff1b1b26));
    g.fillRect(0, kHeaderH, getWidth(), kToolbarH);
    g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
    g.fillRect(0, kHeaderH + kToolbarH - 1, getWidth(), 1);

    g.fillRect(0, 0, 1, getHeight());   // left border (separation from splitter)
}
