/**
 * @file MediaSourceSetupPanel.h
 * @brief M9 — ZONE 3 (SETUP face) for the IMAGE / VIDEO / CAMERA SRC modules.
 *
 * IMAGE / VIDEO — pick the media file (resized to the chain line width on
 * load); shows the decoded characteristics. CAMERA — pick the capture device.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../PluginProcessor.h"

class MediaSourceSetupPanel : public juce::Component
{
public:
    enum class Kind { Image, Video, Camera };

    MediaSourceSetupPanel(Sp3ctraAudioProcessor& p, Kind k, juce::Colour accent);

    static constexpr int kPreferredH = 200;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

private:
    void chooseMedia();
    void clearMedia();
    void refreshDevices();
    void openSelectedDevice();
    void refreshInfo();

    Sp3ctraAudioProcessor& processor;
    const Kind   kind;
    juce::Colour accent;

    juce::Label      titleLabel, pathLabel, infoLabel;
    juce::TextButton loadButton  { "LOAD..." };
    juce::TextButton clearButton { "CLEAR" };

    juce::ComboBox   deviceCombo;                 // CAMERA only
    juce::TextButton refreshButton { "REFRESH" };

    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MediaSourceSetupPanel)
};
