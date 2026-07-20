/**
 * @file LuxGrainSetupPanel.h
 * @brief SETUP face of the → LUXGRAIN block (zone 3).
 *
 * Machine-level knobs of the granular engine — things you set once per
 * session, not per gesture:
 *   • Bands — log-frequency folding resolution (16..192). Fewer bands =
 *     sparser, more soloist cloud; more bands = denser, better separation
 *     of close material (Density is per band).
 *   • Grain material — LOAD SAMPLE publishes a WAV as the grains' content
 *     (NSDF root detection in the engine; the image stays the sole pilot).
 *     The PLAY page's Material selector switches SINE/SAMPLE.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"
#include "../../UITheme.h"

extern "C" {
    #include "synthesis/luxgrain/luxgrain_vst_adapter.h"
}

class LuxGrainSetupPanel : public juce::Component
{
public:
    LuxGrainSetupPanel(Sp3ctraAudioProcessor& p, juce::Colour accentColour)
        : processor(p), accent(accentColour)
    {
        auto& apvts = p.getAPVTS();

        bandsLabel.setText("Bands (fold resolution):", juce::dontSendNotification);
        bandsLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        bandsLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb8c4d0));
        addAndMakeVisible(bandsLabel);

        bandsSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        bandsSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52,
                                    Sp3ctraTheme::kControlH - 4);
        addAndMakeVisible(bandsSlider);
        bandsAttach = std::make_unique<
            juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, "luxgrainBands", bandsSlider);

        loadButton.setButtonText("LOAD SAMPLE...");
        loadButton.onClick = [this] { chooseSample(); };
        addAndMakeVisible(loadButton);

        clearButton.setButtonText("CLEAR");
        clearButton.onClick = [this]
        {
            processor.clearLuxGrainSample();
            refreshInfo();
        };
        addAndMakeVisible(clearButton);

        infoLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        infoLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8fd08f));
        addAndMakeVisible(infoLabel);

        refreshInfo();
    }

    static constexpr int kPreferredH = 148;

    void paint(juce::Graphics& g) override
    {
        g.setColour(accent);
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
        g.drawText("LUXGRAIN -- SETUP", 12, 8, getWidth() - 24, 22,
                   juce::Justification::centredLeft, false);
        g.setColour(juce::Colour(0xff2a2a40));
        g.drawLine(12.f, 34.f, (float) getWidth() - 12.f, 34.f, 1.f);

        g.setColour(juce::Colour(0xffb8c4d0));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        g.drawText("Grain material:", 12, 76, 160, Sp3ctraTheme::kControlH,
                   juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        const int w = getWidth();
        bandsLabel .setBounds(12, 42, 180, Sp3ctraTheme::kControlH);
        bandsSlider.setBounds(196, 42, juce::jmax(120, w - 208),
                              Sp3ctraTheme::kControlH);
        loadButton .setBounds(170, 76, 130, Sp3ctraTheme::kControlH);
        clearButton.setBounds(306, 76, 70, Sp3ctraTheme::kControlH);
        infoLabel  .setBounds(12, 104, w - 24, Sp3ctraTheme::kControlH);
    }

private:
    void chooseSample()
    {
        chooser = std::make_unique<juce::FileChooser>(
            "Select the grain material (audio file)",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a");
        chooser->launchAsync(
            juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f == juce::File{})
                    return;
                juce::String err;
                if (! processor.loadLuxGrainSampleFile(f, err))
                {
                    infoLabel.setColour(juce::Label::textColourId,
                                        juce::Colour(0xffe08f7a));
                    infoLabel.setText(err, juce::dontSendNotification);
                    return;
                }
                refreshInfo();
            });
    }

    void refreshInfo()
    {
        float root = 0.f, dur = 0.f;
        if (luxgrain_engine_sample_info(&g_luxgrain_engine, &root, &dur))
        {
            infoLabel.setColour(juce::Label::textColourId,
                                juce::Colour(0xff8fd08f));
            infoLabel.setText(
                juce::File(processor.luxgrainSamplePath()).getFileName()
                    + juce::String::fromUTF8(" — root ")
                    + juce::String(root, 1) + " Hz, "
                    + juce::String(dur, 1) + " s retained",
                juce::dontSendNotification);
        }
        else
        {
            infoLabel.setColour(juce::Label::textColourId,
                                juce::Colour(0xff7a8494));
            infoLabel.setText(
                juce::String::fromUTF8(
                    "No sample \xE2\x80\x94 grains play the internal sine."),
                juce::dontSendNotification);
        }
    }

    Sp3ctraAudioProcessor& processor;
    juce::Colour accent;

    juce::Label  bandsLabel, infoLabel;
    juce::Slider bandsSlider;
    juce::TextButton loadButton, clearButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bandsAttach;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxGrainSetupPanel)
};
