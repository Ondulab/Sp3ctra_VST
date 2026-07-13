/**
 * @file MediaSourceSetupPanel.cpp
 * @brief M9 — SETUP face for IMAGE / VIDEO / CAMERA SRC modules.
 */
#include "MediaSourceSetupPanel.h"
#include "../MediaSourceEngines.h"

namespace
{
    constexpr int kPad   = 12;
    constexpr int kCtrlH = 26;
    constexpr int kGap   = 8;
}

MediaSourceSetupPanel::MediaSourceSetupPanel(Sp3ctraAudioProcessor& p, Kind k,
                                             juce::Colour accentColour)
    : processor(p), kind(k), accent(accentColour)
{
    titleLabel.setText(kind == Kind::Image  ? "IMAGE SOURCE"
                     : kind == Kind::Video  ? "VIDEO SOURCE"
                                            : "CAMERA SOURCE",
                       juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(13.0f)).boldened());
    titleLabel.setColour(juce::Label::textColourId, accent);
    addAndMakeVisible(titleLabel);

    pathLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    pathLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.8f));
    pathLabel.setMinimumHorizontalScale(0.6f);
    addAndMakeVisible(pathLabel);

    infoLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    infoLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.55f));
    infoLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(infoLabel);

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

    refreshInfo();
}

void MediaSourceSetupPanel::visibilityChanged()
{
    if (isVisible())
    {
        if (kind == Kind::Camera)
            refreshDevices();
        refreshInfo();
    }
}

void MediaSourceSetupPanel::chooseMedia()
{
    const bool isImage = (kind == Kind::Image);
    chooser_ = std::make_unique<juce::FileChooser>(
        isImage ? "Choose an image" : "Choose a video",
        juce::File::getSpecialLocation(juce::File::userPicturesDirectory),
        isImage ? "*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tiff;*.tif"
                : "*.mov;*.mp4;*.m4v;*.avi;*.mpg;*.mpeg");

    juce::Component::SafePointer<MediaSourceSetupPanel> safe(this);
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
                if (auto* e = safe->processor.getImageSource())
                    ok = e->loadFile(file, err);
            }
            else
            {
                if (auto* e = safe->processor.getVideoSource())
                    ok = e->loadFile(file, err);
            }
            if (! ok && err.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon, "Load failed", err);
            safe->refreshInfo();
        });
}

void MediaSourceSetupPanel::clearMedia()
{
    switch (kind)
    {
        case Kind::Image:
            if (auto* e = processor.getImageSource()) e->unload();
            break;
        case Kind::Video:
            if (auto* e = processor.getVideoSource()) e->unload();
            break;
        case Kind::Camera:
            if (auto* e = processor.getCameraSource()) e->closeDevice();
            processor.setCameraDeviceName({});
            deviceCombo.setSelectedId(0, juce::dontSendNotification);
            break;
    }
    refreshInfo();
}

void MediaSourceSetupPanel::refreshDevices()
{
    const auto names   = CameraSourceEngine::getDeviceNames();
    const auto current = processor.getCameraSource() != nullptr
                       ? processor.getCameraSource()->getOpenDeviceName()
                       : juce::String();

    deviceCombo.clear(juce::dontSendNotification);
    for (int i = 0; i < names.size(); ++i)
        deviceCombo.addItem(names[i], i + 1);

    const int cur = names.indexOf(current);
    if (cur >= 0)
        deviceCombo.setSelectedId(cur + 1, juce::dontSendNotification);
}

void MediaSourceSetupPanel::openSelectedDevice()
{
    const int idx = deviceCombo.getSelectedId() - 1;
    if (idx < 0)
        return;
    if (auto* e = processor.getCameraSource())
    {
        // Already open on this device → nothing to do.
        if (e->isOpen() && e->getOpenDeviceIndex() == idx)
            return;
        juce::String err;
        if (e->openDevice(idx, err))
            processor.setCameraDeviceName(deviceCombo.getText());
        else
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Camera", err);
    }
    refreshInfo();
}

void MediaSourceSetupPanel::refreshInfo()
{
    juce::String path, info;

    switch (kind)
    {
        case Kind::Image:
            if (auto* e = processor.getImageSource(); e != nullptr && e->isLoaded())
            {
                path = e->getFile().getFullPathName();
                info << "Resampled to the chain line width (" << INTERNAL_SRC_MAX_PIXELS
                     << " px) - " << e->getRowCount() << " scannable lines.\n"
                     << "Drag the line in PLAY, or automate \"Image Src Line\".";
            }
            else
                info = "Load an image; one horizontal line of it feeds the chain,\n"
                       "movable and scannable like a sampler.";
            break;

        case Kind::Video:
            if (auto* e = processor.getVideoSource(); e != nullptr && e->isLoaded())
            {
                path = e->getFile().getFullPathName();
                info << juce::String(e->getDurationS(), 1) << " s - line resampled to "
                     << INTERNAL_SRC_MAX_PIXELS << " px.\n"
                     << (e->canPlayReverse()
                            ? "Reverse playback: native."
                            : "Reverse playback: step mode (codec without reverse support).");
            }
            else
                info = "Load a video; one chosen line of the running picture\n"
                       "feeds the chain (play once / loop / reverse / ping-pong).";
            break;

        case Kind::Camera:
            if (auto* e = processor.getCameraSource(); e != nullptr && e->isOpen())
            {
                path = e->getOpenDeviceName();
                info = "Live capture running - one chosen line of the camera\n"
                       "picture feeds the chain (\"Camera Src Line\").";
            }
            else
                info = "Select a capture device (webcam...). One chosen line of\n"
                       "the live picture feeds the chain.";
            break;
    }

    pathLabel.setText(path.isEmpty() ? "-" : path, juce::dontSendNotification);
    infoLabel.setText(info, juce::dontSendNotification);
}

void MediaSourceSetupPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff141821));
    g.setColour(accent.withAlpha(0.25f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 6.0f, 1.0f);
}

void MediaSourceSetupPanel::resized()
{
    auto b = getLocalBounds().reduced(kPad);
    titleLabel.setBounds(b.removeFromTop(20));
    b.removeFromTop(kGap);

    auto row = b.removeFromTop(kCtrlH);
    if (kind == Kind::Camera)
    {
        deviceCombo.setBounds(row.removeFromLeft(juce::jmax(180, row.getWidth() - 190)));
        row.removeFromLeft(kGap);
        refreshButton.setBounds(row.removeFromLeft(84));
        row.removeFromLeft(kGap);
        clearButton.setBounds(row.removeFromLeft(84));
    }
    else
    {
        loadButton.setBounds(row.removeFromLeft(110));
        row.removeFromLeft(kGap);
        clearButton.setBounds(row.removeFromLeft(84));
    }
    b.removeFromTop(kGap);

    pathLabel.setBounds(b.removeFromTop(18));
    b.removeFromTop(4);
    infoLabel.setBounds(b);
}
