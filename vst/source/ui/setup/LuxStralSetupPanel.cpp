#include "LuxStralSetupPanel.h"
#include "SetupHeader.h"
#include "../../Sp3ctraConstants.h"
#include "../../UITheme.h"
#include <cmath>

//==============================================================================
LuxStralSetupPanel::LuxStralSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour)
    : apvts(processor.getAPVTS()), accent(accentColour)
{
    // Engine Enable moved to the rack LED + zone-3 header power switch.

    // ========================================================================
    // Section: Musical Tuning (eliminates frequency jumps)
    // ========================================================================
    tuningRangeSectionLabel.setText("Musical Tuning", juce::dontSendNotification);
    tuningRangeSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    tuningRangeSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    addAndMakeVisible(tuningRangeSectionLabel);

    // Tuning (A4 reference)
    tuningLabel.setText("Tuning (A4):", juce::dontSendNotification);
    tuningLabel.setJustificationType(juce::Justification::centredRight);
    tuningLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(tuningLabel);

    tuningSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    tuningSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    tuningSlider.setTextValueSuffix(" Hz");
    addAndMakeVisible(tuningSlider);
    tuningAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralTuning", tuningSlider);

    // Root Note (ComboBox)
    rootNoteLabel.setText("Root Note:", juce::dontSendNotification);
    rootNoteLabel.setJustificationType(juce::Justification::centredRight);
    rootNoteLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(rootNoteLabel);

    const char* noteLetters[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    for (int octave = 1; octave <= 6; octave++) {
        for (int note = 0; note < 12; note++) {
            rootNoteComboBox.addItem(juce::String(noteLetters[note]) + juce::String(octave),
                                     (octave - 1) * 12 + note + 1);
        }
    }
    addAndMakeVisible(rootNoteComboBox);
    rootNoteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "luxstralRootNote", rootNoteComboBox);

    // Number of Octaves
    numOctavesLabel.setText("Octaves:", juce::dontSendNotification);
    numOctavesLabel.setJustificationType(juce::Justification::centredRight);
    numOctavesLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(numOctavesLabel);

    numOctavesSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    numOctavesSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbNarrow, Sp3ctraTheme::kTextBoxH);
    numOctavesSlider.setRange(1, 10, 1);
    addAndMakeVisible(numOctavesSlider);
    numOctavesAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralNumOctaves", numOctavesSlider);

    // Frequency Range Info Label
    freqRangeInfoLabel.setText("Range: -- Hz to -- Hz", juce::dontSendNotification);
    freqRangeInfoLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).italicised());
    freqRangeInfoLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible(freqRangeInfoLabel);

    // Equal-Loudness Compensation (Physiological Filter)
    physiologicalFilterLabel.setText("Equal-Loudness:", juce::dontSendNotification);
    physiologicalFilterLabel.setJustificationType(juce::Justification::centredRight);
    physiologicalFilterLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(physiologicalFilterLabel);

    physiologicalFilterToggle.setButtonText("Compensate (A-weighting)");
    physiologicalFilterToggle.setTooltip(
        "Compensates for human hearing sensitivity (ISO 226 / A-weighting).\n"
        "Boosts bass frequencies and attenuates mid frequencies (~1-5 kHz)\n"
        "so all frequencies are perceived at equal loudness.\n\n"
        "Regenerates wavetables automatically when toggled.");
    addAndMakeVisible(physiologicalFilterToggle);
    physiologicalFilterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxstralPhysiologicalFilter", physiologicalFilterToggle);

    // Correction depth
    physiologicalDepthLabel.setText("Correction Depth:", juce::dontSendNotification);
    physiologicalDepthLabel.setJustificationType(juce::Justification::centredRight);
    physiologicalDepthLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    physiologicalDepthLabel.setTooltip(
        "Intensity of the equal-loudness correction.\n"
        "0.0 = flat (no correction), 1.0 = full inverse A-weighting.\n"
        "0.5 is a balanced starting point.");
    addAndMakeVisible(physiologicalDepthLabel);

    physiologicalDepthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    physiologicalDepthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbXNarrow, Sp3ctraTheme::kTextBoxH);
    physiologicalDepthSlider.setTooltip(physiologicalDepthLabel.getTooltip());
    addAndMakeVisible(physiologicalDepthSlider);
    physiologicalDepthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralPhysiologicalDepth", physiologicalDepthSlider);

    // ========================================================================
    // Section: Dynamics Processing (Soft Limit only)
    // ========================================================================
    dynamicsSectionLabel.setText("Dynamics Processing", juce::dontSendNotification);
    dynamicsSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    dynamicsSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    addAndMakeVisible(dynamicsSectionLabel);

    softLimitThresholdLabel.setText("Soft Limit Threshold:", juce::dontSendNotification);
    softLimitThresholdLabel.setJustificationType(juce::Justification::centredRight);
    softLimitThresholdLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(softLimitThresholdLabel);

    softLimitThresholdSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    softLimitThresholdSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    addAndMakeVisible(softLimitThresholdSlider);
    softLimitThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralSoftLimitThreshold", softLimitThresholdSlider);

    softLimitKneeLabel.setText("Soft Limit Knee:", juce::dontSendNotification);
    softLimitKneeLabel.setJustificationType(juce::Justification::centredRight);
    softLimitKneeLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(softLimitKneeLabel);

    softLimitKneeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    softLimitKneeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    addAndMakeVisible(softLimitKneeSlider);
    softLimitKneeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralSoftLimitKnee", softLimitKneeSlider);

    // ========================================================================
    // Section: StrokeForge Advanced Blob Detection
    // ========================================================================
    sfBlobSectionLabel.setText(juce::String::fromUTF8("StrokeForge — Advanced Blob Detection"), juce::dontSendNotification);
    sfBlobSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    sfBlobSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightyellow);
    addAndMakeVisible(sfBlobSectionLabel);

    contrastAdaptiveLabel.setText("Contrast Adaptive:", juce::dontSendNotification);
    contrastAdaptiveLabel.setJustificationType(juce::Justification::centredRight);
    contrastAdaptiveLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(contrastAdaptiveLabel);

    contrastAdaptiveToggle.setButtonText("Enable");
    addAndMakeVisible(contrastAdaptiveToggle);
    contrastAdaptiveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "sfBlobContrastAdaptive", contrastAdaptiveToggle);

    contrastSensLabel.setText("Contrast Sensitivity:", juce::dontSendNotification);
    contrastSensLabel.setJustificationType(juce::Justification::centredRight);
    contrastSensLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(contrastSensLabel);

    contrastSensSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    contrastSensSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    addAndMakeVisible(contrastSensSlider);
    contrastSensAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobContrastSensitivity", contrastSensSlider);

    // NOTE: the former "Performance / Worker Threads" section intentionally
    // stays in the gear-wheel SYSTEM tab (luxstralNumWorkers is machine-level).

    // Add listeners for dynamic octave limitation
    rootNoteComboBox.addListener(this);
    tuningSlider.addListener(this);
    numOctavesSlider.addListener(this);

    // Initial update
    updateOctavesSliderRange();
    updateFrequencyRangeInfo();
}

LuxStralSetupPanel::~LuxStralSetupPanel()
{
}

void LuxStralSetupPanel::paint(juce::Graphics& g)
{
    SetupUI::paintHeader(g, *this, "LUXSTRAL -- SETUP", accent);
}

void LuxStralSetupPanel::resized()
{
    constexpr int labelWidth = Sp3ctraTheme::kLabelWide;
    const int sliderWidth = juce::jmin(280, juce::jmax(160, getWidth() - labelWidth - 3 * Sp3ctraTheme::kHPad));
    constexpr int rowHeight = Sp3ctraTheme::kRowStep;
    const int sectionSpacing = 15;
    constexpr int itemSpacing = Sp3ctraTheme::kRowGap;
    constexpr int padding = Sp3ctraTheme::kHPad;
    constexpr int ctrlH = Sp3ctraTheme::kControlH;
    constexpr int vc = (rowHeight - ctrlH) / 2;

    int yPos = SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap;
    const int contentWidth = juce::jmax(120, getWidth() - 2 * padding);

    // ========================================================================
    // Section: Musical Tuning  (Engine Enable removed — power lives in rack/header)
    // ========================================================================
    tuningRangeSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;

    tuningLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    tuningSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    rootNoteLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    rootNoteComboBox.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, 70, ctrlH);
    yPos += rowHeight + itemSpacing;

    numOctavesLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    numOctavesSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    freqRangeInfoLabel.setBounds(padding, yPos, contentWidth, 20);
    yPos += 25 + itemSpacing;

    physiologicalFilterLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    physiologicalFilterToggle.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    physiologicalDepthLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    physiologicalDepthSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: Dynamics Processing (Soft Limit only)
    // ========================================================================
    dynamicsSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;

    softLimitThresholdLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    softLimitThresholdSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    softLimitKneeLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    softLimitKneeSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: StrokeForge Advanced Blob Detection
    // ========================================================================
    sfBlobSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;

    contrastAdaptiveLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    contrastAdaptiveToggle.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, 100, ctrlH);
    yPos += rowHeight + itemSpacing;

    contrastSensLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    contrastSensSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
}

//==============================================================================
// Listener implementations
//==============================================================================

void LuxStralSetupPanel::comboBoxChanged(juce::ComboBox* comboBoxThatHasChanged)
{
    if (comboBoxThatHasChanged == &rootNoteComboBox)
    {
        updateOctavesSliderRange();
        updateFrequencyRangeInfo();
    }
}

void LuxStralSetupPanel::sliderValueChanged(juce::Slider* slider)
{
    if (slider == &tuningSlider || slider == &numOctavesSlider)
    {
        if (slider == &tuningSlider)
        {
            updateOctavesSliderRange();
        }
        updateFrequencyRangeInfo();
    }
}

//==============================================================================
// Helper functions for dynamic octave limitation
//==============================================================================

float LuxStralSetupPanel::getRootNoteFrequency() const
{
    float tuning = static_cast<float>(tuningSlider.getValue());
    int rootNoteIndex = rootNoteComboBox.getSelectedId() - 1;
    if (rootNoteIndex < 0) rootNoteIndex = 0;

    float semitonesFromA4 = static_cast<float>(rootNoteIndex - 45);
    float rootFreq = tuning * std::pow(2.0f, semitonesFromA4 / 12.0f);
    return rootFreq;
}

int LuxStralSetupPanel::getMaxOctavesForRootNote() const
{
    constexpr float MAX_FREQUENCY = 20000.0f;
    float rootFreq = getRootNoteFrequency();
    if (rootFreq <= 0.0f) return 1;

    float maxOctavesFloat = std::log2(MAX_FREQUENCY / rootFreq);
    int maxOctaves = static_cast<int>(std::floor(maxOctavesFloat));

    if (maxOctaves < 1) maxOctaves = 1;
    if (maxOctaves > 10) maxOctaves = 10;
    return maxOctaves;
}

void LuxStralSetupPanel::updateOctavesSliderRange()
{
    int maxOctaves = getMaxOctavesForRootNote();

    auto* param = apvts.getParameter("luxstralNumOctaves");
    int currentValue = static_cast<int>(param->convertFrom0to1(param->getValue()));

    numOctavesSlider.setRange(1, maxOctaves, 1);

    if (currentValue > maxOctaves)
    {
        float normalizedValue = param->convertTo0to1(static_cast<float>(maxOctaves));
        param->setValueNotifyingHost(normalizedValue);
        numOctavesSlider.setValue(maxOctaves, juce::dontSendNotification);
    }

    numOctavesLabel.setText(juce::String("Octaves (max ") + juce::String(maxOctaves) + "):",
                           juce::dontSendNotification);
}

void LuxStralSetupPanel::updateFrequencyRangeInfo()
{
    float rootFreq = getRootNoteFrequency();
    int numOctaves = static_cast<int>(numOctavesSlider.getValue());

    float highFreq = rootFreq * std::pow(2.0f, static_cast<float>(numOctaves));
    if (highFreq > 20000.0f) highFreq = 20000.0f;

    juce::String infoText = juce::String::formatted("Range: %.1f Hz to %.1f Hz", rootFreq, highFreq);

    if (highFreq >= 19000.0f)
    {
        infoText += " (near Nyquist limit)";
        freqRangeInfoLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    }
    else
    {
        freqRangeInfoLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    }

    freqRangeInfoLabel.setText(infoText, juce::dontSendNotification);
}
