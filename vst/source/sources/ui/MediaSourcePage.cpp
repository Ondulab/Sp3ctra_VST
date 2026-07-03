/**
 * @file MediaSourcePage.cpp
 * @brief M9 — PLAY face for IMAGE / VIDEO / CAMERA SRC modules.
 */
#include "MediaSourcePage.h"
#include "../MediaSourceEngines.h"
#include "../../UITheme.h"

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

    // Engine playhead (thin, behind the param cursor)
    if (playheadFrac >= 0.0f)
    {
        const float py = area.getY() + playheadFrac * area.getHeight();
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        g.drawHorizontalLine((int) py, area.getX(), area.getRight());
    }

    // LINE cursor — the row injected into the chain
    const float y = area.getY() + lineFrac * area.getHeight();
    g.setColour(juce::Colour(0xffe0b84a));
    g.fillRect(area.getX(), y - 1.0f, area.getWidth(), 2.0f);
    // grab handles
    g.fillEllipse(area.getX() - 3.0f,      y - 4.0f, 8.0f, 8.0f);
    g.fillEllipse(area.getRight() - 5.0f,  y - 4.0f, 8.0f, 8.0f);
}

void MediaSourcePage::PreviewComponent::dragToLine(const juce::MouseEvent& e,
                                                   bool begin, bool end)
{
    const auto area = imageArea();
    if (area.getHeight() <= 0.0f)
        return;
    const float frac = juce::jlimit(0.0f, 1.0f,
        (e.position.y - area.getY()) / area.getHeight());
    owner.setLineParam(frac, begin, end);
    lineFrac = frac;
    repaint();
}

void MediaSourcePage::PreviewComponent::mouseDown(const juce::MouseEvent& e) { dragToLine(e, true,  false); }
void MediaSourcePage::PreviewComponent::mouseDrag(const juce::MouseEvent& e) { dragToLine(e, false, false); }
void MediaSourcePage::PreviewComponent::mouseUp  (const juce::MouseEvent& e) { dragToLine(e, false, true);  }

//==============================================================================
// MediaSourcePage
//==============================================================================
MediaSourcePage::MediaSourcePage(Sp3ctraAudioProcessor& p, Kind k)
    : processor(p), kind(k)
{
    auto& apvts = processor.getAPVTS();

    addAndMakeVisible(preview);
    preview.emptyHint = (kind == Kind::Image)  ? "No image loaded - use the SETUP face"
                      : (kind == Kind::Video)  ? "No video loaded - use the SETUP face"
                                               : "No camera open - use the SETUP face";

    statusLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.55f));
    statusLabel.setFont(juce::Font(12.0f));
    addAndMakeVisible(statusLabel);

    if (kind != Kind::Camera)
    {
        playButton.setClickingTogglesState(true);
        playButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3c8f4a));
        addAndMakeVisible(playButton);
        playAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            apvts, kind == Kind::Image ? "imgSrcPlay" : "vidSrcPlay", playButton);

        loopLabel.setText("MODE", juce::dontSendNotification);
        loopLabel.setFont(juce::Font(11.0f));
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
        speedLabel.setFont(juce::Font(11.0f));
        speedLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(speedLabel);

        speedSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        speedSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, kCtrlH);
        addAndMakeVisible(speedSlider);
        speedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, kind == Kind::Image ? "imgSrcDuration" : "vidSrcSpeed", speedSlider);
    }

    if (kind == Kind::Video)
    {
        positionLabel.setText("POSITION", juce::dontSendNotification);
        positionLabel.setFont(juce::Font(11.0f));
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
                if (auto* v = processor.getVideoSource())
                    v->seekFrac(positionSlider.getValue());
        };
        addAndMakeVisible(positionSlider);
    }
}

MediaSourcePage::~MediaSourcePage() = default;

juce::String MediaSourcePage::lineParamId() const
{
    switch (kind)
    {
        case Kind::Image:  return "imgSrcPos";
        case Kind::Video:  return "vidSrcLine";
        case Kind::Camera: return "camSrcLine";
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

void MediaSourcePage::visibilityChanged()
{
    if (isVisible()) startTimerHz(15);
    else             stopTimer();
}

void MediaSourcePage::timerCallback()
{
    juce::Image img;
    float head = -1.0f;
    juce::String status;

    if (kind == Kind::Image)
    {
        if (auto* e = processor.getImageSource())
        {
            img = e->getPreviewImage();
            if (e->isPlaying())
                head = e->getPlayheadFrac();
            if (e->isLoaded())
                status = e->getFile().getFileName()
                       + "   (" + juce::String(e->getRowCount()) + " lines)";
        }
    }
    else if (kind == Kind::Video)
    {
        if (auto* e = processor.getVideoSource())
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
        if (auto* e = processor.getCameraSource())
        {
            img = e->getPreviewImage();
            if (e->isOpen())
                status = e->getOpenDeviceName();
        }
    }

    preview.image        = img;
    preview.playheadFrac = head;
    preview.lineFrac     = juce::jlimit(0.0f, 1.0f, lineParamValue());
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
