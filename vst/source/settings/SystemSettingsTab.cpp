#include "SystemSettingsTab.h"
#include "../Sp3ctraConstants.h"
#include "../UITheme.h"

//==============================================================================
SystemSettingsTab::SystemSettingsTab(Sp3ctraAudioProcessor& processor)
    : apvts(processor.getAPVTS())
{
    // ── Log Level (from the former GeneralSettingsTab) ────────────────────────
    logLevelLabel.setText("Log Level:", juce::dontSendNotification);
    logLevelLabel.setJustificationType(juce::Justification::centredRight);
    logLevelLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(logLevelLabel);

    logLevelCombo.addItem("Error",   1);
    logLevelCombo.addItem("Warning", 2);
    logLevelCombo.addItem("Info",    3);
    logLevelCombo.addItem("Debug",   4);
    addAndMakeVisible(logLevelCombo);

    logLevelAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            apvts, "logLevel", logLevelCombo);

    // ── LuxStral worker threads (from the former LuxStralSettingsTab) ─────────
    numWorkersLabel.setText("Worker Threads:", juce::dontSendNotification);
    numWorkersLabel.setJustificationType(juce::Justification::centredRight);
    numWorkersLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    numWorkersLabel.setTooltip("Number of LuxStral synthesis worker threads.");
    addAndMakeVisible(numWorkersLabel);

    numWorkersSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    numWorkersSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                     Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    addAndMakeVisible(numWorkersSlider);
    numWorkersAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, "luxstralNumWorkers", numWorkersSlider);

    // ── MIDI — auto-navigate to the module a controller edits ─────────────────
    midiSectionLabel.setText("MIDI", juce::dontSendNotification);
    midiSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    midiSectionLabel.setColour(juce::Label::textColourId, juce::Colour(0xff66cc88u));
    midiSectionLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(midiSectionLabel);

    midiFollowLabel.setText("Follow control:", juce::dontSendNotification);
    midiFollowLabel.setJustificationType(juce::Justification::centredRight);
    midiFollowLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(midiFollowLabel);

    midiFollowToggle.setButtonText("Show the module a MIDI control edits");
    midiFollowToggle.setTooltip("When a mapped MIDI controller changes a parameter "
                               "(played notes excluded), jump to that module's page.");
    addAndMakeVisible(midiFollowToggle);
    midiFollowAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            apvts, "midiFollowParam", midiFollowToggle);

    // ── Detached video window default size (from VideoScrollSettingsTab) ──────
    videoWindowSectionLabel.setText("Detached Video Window", juce::dontSendNotification);
    videoWindowSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    videoWindowSectionLabel.setColour(juce::Label::textColourId, juce::Colour(0xff66cc88u));
    videoWindowSectionLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(videoWindowSectionLabel);

    windowWLabel.setText("Default width:", juce::dontSendNotification);
    windowWLabel.setJustificationType(juce::Justification::centredRight);
    windowWLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(windowWLabel);

    windowWSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    windowWSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                  Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    windowWSlider.setTextValueSuffix(" px");
    windowWSlider.setTooltip("Default window width when opened (pixels).");
    addAndMakeVisible(windowWSlider);
    windowWAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, "videoWindowWidth", windowWSlider);

    windowHLabel.setText("Default height:", juce::dontSendNotification);
    windowHLabel.setJustificationType(juce::Justification::centredRight);
    windowHLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(windowHLabel);

    windowHSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    windowHSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                                  Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    windowHSlider.setTextValueSuffix(" px");
    windowHSlider.setTooltip("Default window height when opened (pixels).");
    addAndMakeVisible(windowHSlider);
    windowHAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, "videoWindowHeight", windowHSlider);
}

SystemSettingsTab::~SystemSettingsTab() = default;

//==============================================================================
void SystemSettingsTab::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Section title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    g.drawText("System Configuration",
               getLocalBounds().removeFromTop(30),
               juce::Justification::centred, true);
}

//==============================================================================
void SystemSettingsTab::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(40); // skip title
    bounds.reduce(20, 10);

    constexpr int labelW  = Sp3ctraTheme::kLabelW;      // 110
    constexpr int rowH    = Sp3ctraTheme::kRowStep;      // 32
    constexpr int ctrlH   = Sp3ctraTheme::kControlH;     // 22
    constexpr int padding = Sp3ctraTheme::kGap;          // 6

    auto placeRow = [&](juce::Label& lbl, juce::Component& ctrl)
    {
        auto row = bounds.removeFromTop(rowH);
        lbl .setBounds(row.removeFromLeft(labelW).withSizeKeepingCentre(labelW, ctrlH));
        row .removeFromLeft(padding);
        ctrl.setBounds(row.withSizeKeepingCentre(row.getWidth(), ctrlH));
    };

    placeRow(logLevelLabel,   logLevelCombo);
    bounds.removeFromTop(Sp3ctraTheme::kSectionGap);
    placeRow(numWorkersLabel, numWorkersSlider);

    bounds.removeFromTop(Sp3ctraTheme::kSectionGap * 3);
    midiSectionLabel.setBounds(bounds.removeFromTop(Sp3ctraTheme::kSectionH));
    bounds.removeFromTop(Sp3ctraTheme::kSectionGap);
    placeRow(midiFollowLabel, midiFollowToggle);

    bounds.removeFromTop(Sp3ctraTheme::kSectionGap * 3);
    videoWindowSectionLabel.setBounds(bounds.removeFromTop(Sp3ctraTheme::kSectionH));
    bounds.removeFromTop(Sp3ctraTheme::kSectionGap);

    placeRow(windowWLabel, windowWSlider);
    placeRow(windowHLabel, windowHSlider);
}
