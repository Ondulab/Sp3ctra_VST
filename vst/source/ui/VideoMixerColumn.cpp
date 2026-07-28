#include "VideoMixerColumn.h"
#include "../licensing/ActivationDialog.h"

//==============================================================================
// MiniButton — header glyphs drawn with paths (ported from WaterfallColumnComponent).
//==============================================================================
void VideoMixerColumn::MiniButton::paintButton(juce::Graphics& g,
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
            p.startNewSubPath(inner.getX(), inner.getY() + t);  p.lineTo(inner.getX(), inner.getY());  p.lineTo(inner.getX() + t, inner.getY());
            p.startNewSubPath(inner.getRight() - t, inner.getY());  p.lineTo(inner.getRight(), inner.getY());  p.lineTo(inner.getRight(), inner.getY() + t);
            p.startNewSubPath(inner.getRight(), inner.getBottom() - t);  p.lineTo(inner.getRight(), inner.getBottom());  p.lineTo(inner.getRight() - t, inner.getBottom());
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
// TransportButton — Play/Pause toggle + momentary Stop (ported).
//==============================================================================
void VideoMixerColumn::TransportButton::paintButton(juce::Graphics& g,
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
            g.fillRect(inner.getX(),          inner.getY(), bw, inner.getHeight());
            g.fillRect(inner.getRight() - bw, inner.getY(), bw, inner.getHeight());
        }
    }
    else // Stop — filled square (reddish = halt + clear)
    {
        g.setColour(isMouseOver ? juce::Colour(0xffe0a0a0) : juce::Colour(0xffb88a8a));
        const float s  = juce::jmin(inner.getWidth(), inner.getHeight());
        const auto  sq = inner.withSizeKeepingCentre(s, s);
        g.fillRoundedRectangle(sq, 1.5f);
    }
}

//==============================================================================
// RecordButton — red REC dot (idle) / blinking red square (recording).
//==============================================================================
void VideoMixerColumn::RecordButton::paintButton(juce::Graphics& g,
                                                 bool isMouseOver,
                                                 bool isButtonDown)
{
    const auto b = getLocalBounds().toFloat().reduced(1.f);

    const juce::Colour bg(0xff222230);
    g.setColour(isButtonDown ? bg.brighter(0.30f)
              : isMouseOver  ? bg.brighter(0.12f)
              :                bg);
    g.fillRoundedRectangle(b, 3.f);

    const auto  inner = b.reduced(4.5f);
    const float s     = juce::jmin(inner.getWidth(), inner.getHeight());

    if (recording)
    {
        // Blinking filled red square — "recording, click to stop".
        g.setColour(juce::Colour(0xffff3b30).withAlpha(blinkOn ? 1.0f : 0.30f));
        g.fillRoundedRectangle(inner.withSizeKeepingCentre(s, s), 1.5f);
    }
    else
    {
        // Idle red REC dot.
        g.setColour(isMouseOver ? juce::Colour(0xffff5b52) : juce::Colour(0xffd9433a));
        g.fillEllipse(inner.withSizeKeepingCentre(s, s));
    }
}

//==============================================================================
VideoMixerColumn::VideoMixerColumn(Sp3ctraAudioProcessor& p)
    : processor_(p), mixer_(p)
{
    // paint() fills the whole column (bg + header). Opaque so the mixer's 60 fps
    // repaints don't cascade a parent-background repaint through this container.
    setOpaque(true);
    addAndMakeVisible(mixer_);

    // Outputs run by default → PlayPause shows ⏸ (toggle OFF = not paused).
    playBtn_.setToggleState(false, juce::dontSendNotification);
    playBtn_.setTooltip("Play / pause all video outputs");
    playBtn_.onClick = [this]
    {
        mixer_.setAllPaused(playBtn_.getToggleState());   // state ON = paused
    };
    addAndMakeVisible(playBtn_);

    stopBtn_.setTooltip(juce::String::fromUTF8("Stop — freeze and clear all video outputs"));
    stopBtn_.onClick = [this]
    {
        mixer_.stopAll();
        playBtn_.setToggleState(true, juce::dontSendNotification);   // → ▶
    };
    addAndMakeVisible(stopBtn_);

    recBtn_.setTooltip(juce::String::fromUTF8(
        "Record VIDEO MIX + master audio to a high-quality .mov\n"
        "Left-click: start / stop    Right-click: resolution"));
    recBtn_.onClick = [this]
    {
        if (mixer_.isRecording())
        {
            mixer_.endRecording();
            recBtn_.setRecording(false);
        }
        else
        {
            startRecordingFlow();
        }
    };
    recBtn_.onRightClick = [this]
    {
        juce::PopupMenu m;
        m.addSectionHeader("Recording resolution");
        for (int hgt : { 1080, 1440, 2160 })
            m.addItem(juce::String(hgt) + "p", true, recordHeight_ == hgt,
                      [this, hgt] { recordHeight_ = hgt; });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&recBtn_));
    };
    addAndMakeVisible(recBtn_);

    detachBtn_.setTooltip("Open / close the detached master window");
    detachBtn_.onClick = [this] { mixer_.toggleDetachedWindow(); };
    addAndMakeVisible(detachBtn_);

    fullscreenBtn_.setTooltip("Open the master window in full screen");
    fullscreenBtn_.onClick = [this] { mixer_.requestFullscreenWindow(); };
    addAndMakeVisible(fullscreenBtn_);

    collapseBtn_.setTooltip("Collapse column");
    collapseBtn_.onClick = [this] { setCollapsed(true, true); };
    addAndMakeVisible(collapseBtn_);

    expandBtn_.setTooltip("Expand column");
    expandBtn_.onClick = [this] { setCollapsed(false, true); };
    addChildComponent(expandBtn_);
}

void VideoMixerColumn::setCollapsed(bool shouldCollapse, bool notify)
{
    if (collapsed_ == shouldCollapse) { resized(); return; }
    collapsed_ = shouldCollapse;
    resized();
    if (notify && onCollapseToggled) onCollapseToggled(collapsed_);
}

void VideoMixerColumn::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0c0c10));

    if (collapsed_)
        return;

    g.setColour(juce::Colour(0xff5ad0c8));
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
    g.drawText("VIDEO MIX", 8, 0, getWidth() - 16, kHeaderH,
               juce::Justification::centredLeft, false);
}

void VideoMixerColumn::resized()
{
    auto r = getLocalBounds();

    const bool showFull = ! collapsed_;
    // Transport stays reachable in the collapsed band (stacked under the
    // expand grip) — the video outputs keep running while ZONE 4 is folded.
    playBtn_.setVisible(true);
    stopBtn_.setVisible(true);
    recBtn_.setVisible(true);
    detachBtn_.setVisible(showFull);
    fullscreenBtn_.setVisible(showFull);
    collapseBtn_.setVisible(showFull);
    mixer_.setVisible(showFull);
    expandBtn_.setVisible(collapsed_);

    if (collapsed_)
    {
        expandBtn_.setBounds(0, 2, kGripW, kGripW);
        playBtn_.setBounds(2, 2 + kGripW + 6,               kGripW - 4, kGripW - 4);
        stopBtn_.setBounds(2, 2 + kGripW + 6 + kGripW,      kGripW - 4, kGripW - 4);
        recBtn_ .setBounds(2, 2 + kGripW + 6 + kGripW * 2,  kGripW - 4, kGripW - 4);
        return;
    }

    auto header = r.removeFromTop(kHeaderH);
    // Right-aligned: [collapse][fullscreen][detach]  …  [rec][play][stop]
    const int bw = 22;
    collapseBtn_.setBounds(header.removeFromRight(bw).reduced(1));
    fullscreenBtn_.setBounds(header.removeFromRight(bw).reduced(1));
    detachBtn_.setBounds(header.removeFromRight(bw).reduced(1));
    header.removeFromRight(8);
    stopBtn_.setBounds(header.removeFromRight(bw).reduced(1));
    playBtn_.setBounds(header.removeFromRight(bw).reduced(1));
    recBtn_.setBounds(header.removeFromRight(bw).reduced(1));

    mixer_.setBounds(r);
}

//==============================================================================
void VideoMixerColumn::startRecordingFlow()
{
    if (LicenseGate::blockIfDemo(this, "Record VIDEO MIX"))
        return;
    auto* sessions = processor_.sessions();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");

    // Named session active (Standalone): recordings belong to the session —
    // start capturing straight into <session>/exports/ with an auto-stamped
    // name, no chooser. Only the Global session (and the DAW builds, which
    // have no session folder) still ask where to save.
    if (sessions != nullptr && sessions->isStandalone() && ! sessions->isGlobal())
    {
        sessions->exportsDir().createDirectory();
        const auto file = sessions->exportsDir()
                              .getChildFile("Sp3ctra_" + stamp + ".mov");
        juce::String err;
        if (mixer_.beginRecording(file, recordHeight_, err))
            recBtn_.setRecording(true);
        else
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Recording failed",
                err.isNotEmpty() ? err : juce::String("Could not start the recorder."),
                "OK");
        return;
    }

    // Global / DAW: chooser, seeded at the exports folder (Global) or the
    // last capture directory used.
    auto fallback = juce::File::getSpecialLocation(juce::File::userMoviesDirectory);
    if (! fallback.isDirectory())
        fallback = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
    const auto dir = sessions->startDirFor(
        PathKeys::videoCapture, fallback, /*isExport*/ true);

    const auto suggested = dir.getChildFile("Sp3ctra_" + stamp + ".mov");

    fileChooser_ = std::make_unique<juce::FileChooser>(
        juce::String::fromUTF8("Record VIDEO MIX + master audio to…"),
        suggested, "*.mov");

    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::warnAboutOverwriting;

    fileChooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        auto file = fc.getResult();
        if (file == juce::File{}) return;                       // cancelled
        if (file.getFileExtension().isEmpty())
            file = file.withFileExtension("mov");
        processor_.sessions()->rememberDirFor(PathKeys::videoCapture, file);

        juce::String err;
        if (mixer_.beginRecording(file, recordHeight_, err))
            recBtn_.setRecording(true);
        else
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Recording failed",
                err.isNotEmpty() ? err : juce::String("Could not start the recorder."),
                "OK");
    });
}
