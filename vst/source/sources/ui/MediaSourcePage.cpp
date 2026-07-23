/**
 * @file MediaSourcePage.cpp
 * @brief M9 — PLAY face for IMAGE / VIDEO / CAMERA SRC modules.
 */
#include "MediaSourcePage.h"
#include "../MediaSourceEngines.h"
#include "../../UITheme.h"

#include <cmath>

namespace
{
    constexpr int kCtrlH   = 24;
    constexpr int kRowGap  = 8;
    constexpr int kPad     = 10;
}

//==============================================================================
// PreviewComponent
//==============================================================================
juce::Rectangle<float> MediaSourcePage::PreviewComponent::imageArea() const
{
    auto b = getLocalBounds().toFloat().reduced(1.0f);
    if (! image.isValid())
        return b;
    const float scale = juce::jmin(b.getWidth()  / (float) image.getWidth(),
                                   b.getHeight() / (float) image.getHeight());
    const float w = image.getWidth()  * scale;
    const float h = image.getHeight() * scale;
    return { b.getCentreX() - w * 0.5f, b.getCentreY() - h * 0.5f, w, h };
}

void MediaSourcePage::PreviewComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff11151d));
    g.fillRoundedRectangle(b, 5.0f);
    g.setColour(juce::Colour(0xff2a3242));
    g.drawRoundedRectangle(b.reduced(0.5f), 5.0f, 1.0f);

    if (! image.isValid())
    {
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.setFont(14.0f);
        g.drawText(emptyHint, getLocalBounds(), juce::Justification::centred);
        return;
    }

    const auto area = imageArea();
    g.drawImage(image, area, juce::RectanglePlacement::stretchToFit);

    // IMAGE scan bounds — the transport only reads [start, end]; the excluded
    // regions are dimmed and each bound is a draggable cyan marker.
    if (scanStartFrac >= 0.0f && scanEndFrac >= 0.0f)
    {
        const float lo  = juce::jmin(scanStartFrac, scanEndFrac);
        const float hi  = juce::jmax(scanStartFrac, scanEndFrac);
        const float yLo = area.getY() + lo * area.getHeight();
        const float yHi = area.getY() + hi * area.getHeight();

        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRect(area.getX(), area.getY(), area.getWidth(), yLo - area.getY());
        g.fillRect(area.getX(), yHi, area.getWidth(), area.getBottom() - yHi);

        const auto boundCol = juce::Colour(0xff4ab8e0);
        g.setColour(boundCol);
        g.fillRect(area.getX(), yLo - 1.0f, area.getWidth(), 2.0f);
        g.fillRect(area.getX(), yHi - 1.0f, area.getWidth(), 2.0f);
        // edge handles pointing into the readable region
        juce::Path h;
        h.addTriangle(area.getX(), yLo - 5.0f, area.getX(), yLo + 5.0f, area.getX() + 9.0f, yLo);
        h.addTriangle(area.getRight(), yLo - 5.0f, area.getRight(), yLo + 5.0f, area.getRight() - 9.0f, yLo);
        h.addTriangle(area.getX(), yHi - 5.0f, area.getX(), yHi + 5.0f, area.getX() + 9.0f, yHi);
        h.addTriangle(area.getRight(), yHi - 5.0f, area.getRight(), yHi + 5.0f, area.getRight() - 9.0f, yHi);
        g.fillPath(h);
    }

    // Engine playhead (behind the param cursor) — same yellow as the LINE
    // cursor (it IS the line being read while the transport runs), over a
    // dark halo so it reads on white material as well as black. No grab
    // handles: that is what still tells it apart from the LINE cursor.
    if (playheadFrac >= 0.0f)
    {
        const float py = area.getY() + playheadFrac * area.getHeight();
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.fillRect(area.getX(), py - 2.0f, area.getWidth(), 4.0f);
        g.setColour(juce::Colour(0xffe0b84a));
        g.fillRect(area.getX(), py - 1.0f, area.getWidth(), 2.0f);
    }

    // LINE cursor — the row injected into the chain (same dark halo: the
    // yellow alone vanished on white images)
    const float y = area.getY() + lineFrac * area.getHeight();
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    g.fillRect(area.getX(), y - 2.0f, area.getWidth(), 4.0f);
    g.setColour(juce::Colour(0xffe0b84a));
    g.fillRect(area.getX(), y - 1.0f, area.getWidth(), 2.0f);
    // grab handles
    g.fillEllipse(area.getX() - 3.0f,      y - 4.0f, 8.0f, 8.0f);
    g.fillEllipse(area.getRight() - 5.0f,  y - 4.0f, 8.0f, 8.0f);
}

void MediaSourcePage::PreviewComponent::dragTo(const juce::MouseEvent& e,
                                               bool begin, bool end)
{
    const auto area = imageArea();
    if (area.getHeight() <= 0.0f)
        return;
    const float frac = juce::jlimit(0.0f, 1.0f,
        (e.position.y - area.getY()) / area.getHeight());
    switch (drag_)
    {
        case DragTarget::Line:
            owner.setLineParam(frac, begin, end);
            lineFrac = frac;
            break;
        case DragTarget::ScanStart:
            owner.setScanParam(true, frac, begin, end);
            scanStartFrac = frac;
            break;
        case DragTarget::ScanEnd:
            owner.setScanParam(false, frac, begin, end);
            scanEndFrac = frac;
            break;
    }
    repaint();
}

void MediaSourcePage::PreviewComponent::mouseDown(const juce::MouseEvent& e)
{
    // A click near a scan bound grabs THAT bound; the LINE cursor keeps
    // priority when it sits closer (it is the primary control).
    drag_ = DragTarget::Line;
    const auto area = imageArea();
    if (scanStartFrac >= 0.0f && scanEndFrac >= 0.0f && area.getHeight() > 0.0f)
    {
        constexpr float grab = 6.0f;
        const float dL = std::abs(e.position.y - (area.getY() + lineFrac      * area.getHeight()));
        const float dS = std::abs(e.position.y - (area.getY() + scanStartFrac * area.getHeight()));
        const float dE = std::abs(e.position.y - (area.getY() + scanEndFrac   * area.getHeight()));
        if (dS <= grab && dS <= dE && dS < dL)
            drag_ = DragTarget::ScanStart;
        else if (dE <= grab && dE < dL)
            drag_ = DragTarget::ScanEnd;
    }
    dragTo(e, true, false);
}

void MediaSourcePage::PreviewComponent::mouseDrag(const juce::MouseEvent& e) { dragTo(e, false, false); }
void MediaSourcePage::PreviewComponent::mouseUp  (const juce::MouseEvent& e) { dragTo(e, false, true);  }

//==============================================================================
// MediaSourcePage
//==============================================================================
MediaSourcePage::MediaSourcePage(Sp3ctraAudioProcessor& p, Kind k)
    : processor(p), kind(k)
{
    auto& apvts = processor.getAPVTS();

    addAndMakeVisible(preview);
    preview.emptyHint = (kind == Kind::Image)  ? "No image loaded - click LOAD..."
                      : (kind == Kind::Video)  ? "No video loaded - click LOAD..."
                                               : "No camera open - pick a device above";

    // ── Source picker row (formerly the SETUP face) ──────────────────────────
    if (kind == Kind::Camera)
    {
        deviceCombo.setTextWhenNoChoicesAvailable("No camera found");
        deviceCombo.setTextWhenNothingSelected("Select a camera...");
        deviceCombo.onChange = [this] { openSelectedDevice(); };
        addAndMakeVisible(deviceCombo);

        refreshButton.onClick = [this] { refreshDevices(); };
        addAndMakeVisible(refreshButton);

        clearButton.setButtonText("CLOSE");
        clearButton.onClick = [this] { clearMedia(); };
        addAndMakeVisible(clearButton);
    }
    else
    {
        loadButton.onClick  = [this] { chooseMedia(); };
        addAndMakeVisible(loadButton);
        clearButton.onClick = [this] { clearMedia(); };
        addAndMakeVisible(clearButton);
    }

    if (kind == Kind::Image)
    {
        // Orientation — cycles 0° → 90° → 180° → 270° (imgSrcRotate, per slot);
        // the engine rebuilds the strip so the scan reads the rotated image.
        rotateButton.onClick = [this] { cycleRotation(); };
        addAndMakeVisible(rotateButton);
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
            processor.getMidiMap(), rotateButton, imgSrcParam(0, "Rotate")));
    }

    // ── ACTIVE toggle (all kinds) — the source's on/off ──────────────────────
    // Off: the source feeds NOTHING (its chain streams blank paper). Media,
    // transport and params are kept; switching back on resumes instantly.
    {
        activeButton.setClickingTogglesState(true);
        activeButton.setColour(juce::TextButton::buttonOnColourId,
                               juce::Colour(0xff3c8f4a));
        addAndMakeVisible(activeButton);
        const char* enabledId = kind == Kind::Image ? "imgSrcEnabled"
                              : kind == Kind::Video ? "vidSrcEnabled"
                                                    : "camSrcEnabled";
        activeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            apvts, enabledId, activeButton);
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
            processor.getMidiMap(), activeButton, enabledId));
    }

    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.55f));
    statusLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    addAndMakeVisible(statusLabel);

    if (kind != Kind::Camera)
    {
        playButton.setClickingTogglesState(true);
        playButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3c8f4a));
        addAndMakeVisible(playButton);
        playAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            apvts, kind == Kind::Image ? "imgSrcPlay" : "vidSrcPlay", playButton);

        loopLabel.setText("MODE", juce::dontSendNotification);
        loopLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
        loopLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(loopLabel);

        loopCombo.addItem("Once",      1);
        loopCombo.addItem("Loop",      2);
        loopCombo.addItem("Reverse",   3);
        loopCombo.addItem("Ping-Pong", 4);
        addAndMakeVisible(loopCombo);
        loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            apvts, kind == Kind::Image ? "imgSrcLoop" : "vidSrcLoop", loopCombo);

        speedLabel.setText(kind == Kind::Image ? "SCAN TIME" : "SPEED",
                           juce::dontSendNotification);
        speedLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
        speedLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(speedLabel);

        speedSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        speedSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, kCtrlH);
        addAndMakeVisible(speedSlider);
        speedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, kind == Kind::Image ? "imgSrcDuration" : "vidSrcSpeed", speedSlider);

        // Right-click MIDI Learn (media sources are engine singletons).
        auto& mm = processor.getMidiMap();
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
            mm, playButton,  kind == Kind::Image ? "imgSrcPlay" : "vidSrcPlay"));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
            mm, speedSlider, kind == Kind::Image ? "imgSrcDuration" : "vidSrcSpeed"));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
            mm, loopCombo,   kind == Kind::Image ? "imgSrcLoop" : "vidSrcLoop"));
    }

    if (kind == Kind::Video)
    {
        positionLabel.setText("POSITION", juce::dontSendNotification);
        positionLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
        positionLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(positionLabel);

        positionSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        positionSlider.setRange(0.0, 1.0, 0.0001);
        positionSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        positionSlider.onDragStart = [this] { scrubbing_ = true; };
        positionSlider.onDragEnd   = [this] { scrubbing_ = false; };
        positionSlider.onValueChange = [this]
        {
            if (scrubbing_)
                if (auto* v = processor.getVideoSource(slot_))
                    v->seekFrac(positionSlider.getValue());
        };
        addAndMakeVisible(positionSlider);
    }
}

MediaSourcePage::~MediaSourcePage() = default;

//==============================================================================
// Source picking (moved here from the former SETUP face)
//==============================================================================
void MediaSourcePage::chooseMedia()
{
    const bool isImage = (kind == Kind::Image);
    chooser_ = std::make_unique<juce::FileChooser>(
        isImage ? "Choose an image" : "Choose a video",
        juce::File::getSpecialLocation(juce::File::userPicturesDirectory),
        isImage ? "*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tiff;*.tif"
                : "*.mov;*.mp4;*.m4v;*.avi;*.mpg;*.mpeg");

    juce::Component::SafePointer<MediaSourcePage> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
        [safe](const juce::FileChooser& fc)
        {
            if (safe == nullptr)
                return;
            const auto file = fc.getResult();
            if (file == juce::File{})
                return;

            juce::String err;
            bool ok = false;
            if (safe->kind == Kind::Image)
            {
                if (auto* e = safe->processor.getImageSource(safe->slot_))
                    ok = e->loadFile(file, err);
            }
            else
            {
                if (auto* e = safe->processor.getVideoSource(safe->slot_))
                    ok = e->loadFile(file, err);
            }
            if (! ok && err.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon, "Load failed", err);
        });
}

void MediaSourcePage::clearMedia()
{
    switch (kind)
    {
        case Kind::Image:
            if (auto* e = processor.getImageSource(slot_)) e->unload();
            break;
        case Kind::Video:
            if (auto* e = processor.getVideoSource(slot_)) e->unload();
            break;
        case Kind::Camera:
            if (auto* e = processor.getCameraSource(slot_)) e->closeDevice();
            processor.setCameraDeviceName(slot_, {});
            deviceCombo.setSelectedId(0, juce::dontSendNotification);
            break;
    }
}

void MediaSourcePage::refreshDevices()
{
    const auto names   = CameraSourceEngine::getDeviceNames();
    const auto current = processor.getCameraSource(slot_) != nullptr
                       ? processor.getCameraSource(slot_)->getOpenDeviceName()
                       : juce::String();

    deviceCombo.clear(juce::dontSendNotification);
    for (int i = 0; i < names.size(); ++i)
        deviceCombo.addItem(names[i], i + 1);

    const int cur = names.indexOf(current);
    if (cur >= 0)
        deviceCombo.setSelectedId(cur + 1, juce::dontSendNotification);
}

void MediaSourcePage::openSelectedDevice()
{
    const int idx = deviceCombo.getSelectedId() - 1;
    if (idx < 0)
        return;
    if (auto* e = processor.getCameraSource(slot_))
    {
        // Already open on this device → nothing to do.
        if (e->isOpen() && e->getOpenDeviceIndex() == idx)
            return;
        juce::String err;
        if (e->openDevice(idx, err))
            processor.setCameraDeviceName(slot_, deviceCombo.getText());
        else
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Camera", err);
    }
}

void MediaSourcePage::cycleRotation()
{
    if (kind != Kind::Image)
        return;
    if (auto* param = processor.getAPVTS().getParameter(imgSrcParam(slot_, "Rotate")))
    {
        const int cur  = (int) std::lround(param->convertFrom0to1(param->getValue()));
        const int next = (cur + 1) & 3;
        param->beginChangeGesture();
        param->setValueNotifyingHost(param->convertTo0to1((float) next));
        param->endChangeGesture();
    }
}

juce::String MediaSourcePage::lineParamId() const
{
    switch (kind)
    {
        case Kind::Image:  return imgSrcParam(slot_, "Pos");
        case Kind::Video:  return vidSrcParam(slot_, "Line");
        case Kind::Camera: return camSrcParam(slot_, "Line");
    }
    return "imgSrcPos";
}

float MediaSourcePage::lineParamValue() const
{
    if (auto* raw = processor.getAPVTS().getRawParameterValue(lineParamId()))
        return raw->load();
    return 0.5f;
}

void MediaSourcePage::setLineParam(float v, bool gestureBegin, bool gestureEnd)
{
    if (auto* param = processor.getAPVTS().getParameter(lineParamId()))
    {
        if (gestureBegin) param->beginChangeGesture();
        param->setValueNotifyingHost(param->convertTo0to1(v));
        if (gestureEnd)   param->endChangeGesture();
    }
}

float MediaSourcePage::scanParamValue(bool start) const
{
    if (kind != Kind::Image)
        return start ? 0.0f : 1.0f;
    if (auto* raw = processor.getAPVTS().getRawParameterValue(
            imgSrcParam(slot_, start ? "ScanStart" : "ScanEnd")))
        return raw->load();
    return start ? 0.0f : 1.0f;
}

void MediaSourcePage::setScanParam(bool start, float v, bool gestureBegin, bool gestureEnd)
{
    if (kind != Kind::Image)
        return;
    if (auto* param = processor.getAPVTS().getParameter(
            imgSrcParam(slot_, start ? "ScanStart" : "ScanEnd")))
    {
        if (gestureBegin) param->beginChangeGesture();
        param->setValueNotifyingHost(param->convertTo0to1(v));
        if (gestureEnd)   param->endChangeGesture();
    }
}

void MediaSourcePage::visibilityChanged()
{
    if (isVisible())
    {
        if (kind == Kind::Camera)
            refreshDevices();
        startTimerHz(15);
    }
    else
        stopTimer();
}

void MediaSourcePage::timerCallback()
{
    juce::Image img;
    float head = -1.0f;
    juce::String status;

    if (kind == Kind::Image)
    {
        if (auto* e = processor.getImageSource(slot_))
        {
            img = e->getPreviewImage();
            if (e->isPlaying())
                head = e->getPlayheadFrac();
            if (e->isLoaded())
                status = e->getFile().getFileName()
                       + "   (" + juce::String(e->getRowCount()) + " lines)";
        }
        if (auto* raw = processor.getAPVTS().getRawParameterValue(
                imgSrcParam(slot_, "Rotate")))
            rotateButton.setButtonText(
                "ROT " + juce::String(((int) raw->load()) * 90)
                       + juce::String::fromUTF8("\xc2\xb0"));
    }
    else if (kind == Kind::Video)
    {
        if (auto* e = processor.getVideoSource(slot_))
        {
            img = e->getPreviewImage();
            if (e->isLoaded())
            {
                const double dur = e->getDurationS();
                status = e->getFile().getFileName()
                       + "   " + juce::String(dur, 1) + " s"
                       + (e->canPlayReverse() ? "" : "   (reverse: step mode)");
                if (! scrubbing_)
                    positionSlider.setValue(e->getPositionFrac(),
                                            juce::dontSendNotification);
            }
        }
    }
    else
    {
        if (auto* e = processor.getCameraSource(slot_))
        {
            img = e->getPreviewImage();
            if (e->isOpen())
                status = e->getOpenDeviceName();
        }
    }

    preview.image        = img;
    preview.playheadFrac = head;
    preview.lineFrac     = juce::jlimit(0.0f, 1.0f, lineParamValue());
    if (kind == Kind::Image)
    {
        preview.scanStartFrac = juce::jlimit(0.0f, 1.0f, scanParamValue(true));
        preview.scanEndFrac   = juce::jlimit(0.0f, 1.0f, scanParamValue(false));
    }
    statusLabel.setText(status, juce::dontSendNotification);
    preview.repaint();
}

void MediaSourcePage::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff141821));
}

void MediaSourcePage::resized()
{
    auto b = getLocalBounds().reduced(kPad);

    // Source picker row at the top (LOAD/CLEAR — or device combo for CAMERA),
    // with the ACTIVE toggle pinned at the right edge.
    {
        auto row = b.removeFromTop(kCtrlH);
        activeButton.setBounds(row.removeFromRight(74));
        row.removeFromRight(kRowGap);
        if (kind == Kind::Camera)
        {
            deviceCombo.setBounds(row.removeFromLeft(juce::jmax(180, row.getWidth() - 190)));
            row.removeFromLeft(kRowGap);
            refreshButton.setBounds(row.removeFromLeft(84));
            row.removeFromLeft(kRowGap);
            clearButton.setBounds(row.removeFromLeft(84));
        }
        else
        {
            loadButton.setBounds(row.removeFromLeft(110));
            row.removeFromLeft(kRowGap);
            clearButton.setBounds(row.removeFromLeft(84));
            if (kind == Kind::Image)
            {
                row.removeFromLeft(kRowGap);
                rotateButton.setBounds(row.removeFromLeft(84));
            }
        }
        b.removeFromTop(kRowGap);
    }

    // Transport row(s) at the bottom
    if (kind != Kind::Camera)
    {
        auto row = b.removeFromBottom(kCtrlH);
        playButton.setBounds(row.removeFromLeft(74));
        row.removeFromLeft(kRowGap);
        loopLabel.setBounds(row.removeFromLeft(38));
        loopCombo.setBounds(row.removeFromLeft(110));
        row.removeFromLeft(kRowGap);
        speedLabel.setBounds(row.removeFromLeft(kind == Kind::Image ? 66 : 44));
        speedSlider.setBounds(row);
        b.removeFromBottom(kRowGap);

        if (kind == Kind::Video)
        {
            auto row2 = b.removeFromBottom(kCtrlH);
            positionLabel.setBounds(row2.removeFromLeft(60));
            positionSlider.setBounds(row2);
            b.removeFromBottom(kRowGap);
        }
    }

    statusLabel.setBounds(b.removeFromBottom(18));
    b.removeFromBottom(4);
    preview.setBounds(b);
}

//==============================================================================
// P5-M3 — bind the page to ONE IMAGE instance (pool slot).
// Attachment rebind trap (memory 2026-07-10): reset FIRST, then recreate —
// resetting via assignment leaks the previous bank's value into the new one.
//==============================================================================
void MediaSourcePage::setSlot(int slot)
{
    slot = juce::jlimit(0, 7, slot);
    if (slot == slot_)
        return;
    slot_ = slot;

    auto& apvts = processor.getAPVTS();
    auto& mm    = processor.getMidiMap();

    activeAttach.reset();
    playAttach.reset();
    loopAttach.reset();
    speedAttach.reset();
    learnAtts_.clear();

    const auto enabledId = kind == Kind::Image ? imgSrcParam(slot_, "Enabled")
                         : kind == Kind::Video ? vidSrcParam(slot_, "Enabled")
                                               : camSrcParam(slot_, "Enabled");
    activeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, enabledId, activeButton);
    learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
        mm, activeButton, enabledId));

    if (kind != Kind::Camera)
    {
        const auto playId  = kind == Kind::Image ? imgSrcParam(slot_, "Play")
                                                 : vidSrcParam(slot_, "Play");
        const auto loopId  = kind == Kind::Image ? imgSrcParam(slot_, "Loop")
                                                 : vidSrcParam(slot_, "Loop");
        const auto speedId = kind == Kind::Image ? imgSrcParam(slot_, "Duration")
                                                 : vidSrcParam(slot_, "Speed");
        playAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            apvts, playId, playButton);
        loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            apvts, loopId, loopCombo);
        speedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, speedId, speedSlider);
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
            mm, playButton,  playId));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
            mm, speedSlider, speedId));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
            mm, loopCombo,   loopId));
        if (kind == Kind::Image)
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(
                mm, rotateButton, imgSrcParam(slot_, "Rotate")));
    }
    else
        refreshDevices();   // combo mirrors THIS instance's open device

    repaint();
}
