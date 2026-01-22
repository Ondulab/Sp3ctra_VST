#include "LuxStralSettingsTab.h"
#include "../Sp3ctraConstants.h"
#include <cmath>

//==============================================================================
LuxStralSettingsTab::LuxStralSettingsTab(Sp3ctraAudioProcessor& processor)
    : audioProcessor(processor),
      apvts(processor.getAPVTS())
{
    // Setup viewport for scrolling
    addAndMakeVisible(viewport);
    viewport.setViewedComponent(&contentComponent, false);
    viewport.setScrollBarsShown(true, false);

    // ========================================================================
    // Section: Musical Tuning (eliminates frequency jumps)
    // ========================================================================
    tuningRangeSectionLabel.setText("Musical Tuning", juce::dontSendNotification);
    tuningRangeSectionLabel.setFont(juce::Font(juce::FontOptions(15.0f)).boldened());
    tuningRangeSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(tuningRangeSectionLabel);

    // Tuning (A4 reference)
    tuningLabel.setText("Tuning (A4):", juce::dontSendNotification);
    tuningLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(tuningLabel);
    
    tuningSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    tuningSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    tuningSlider.setTextValueSuffix(" Hz");
    contentComponent.addAndMakeVisible(tuningSlider);
    tuningAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralTuning", tuningSlider);

    // Root Note (ComboBox)
    rootNoteLabel.setText("Root Note:", juce::dontSendNotification);
    rootNoteLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(rootNoteLabel);
    
    // Populate ComboBox with note names (C1 to B6)
    const char* noteLetters[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    for (int octave = 1; octave <= 6; octave++) {
        for (int note = 0; note < 12; note++) {
            rootNoteComboBox.addItem(juce::String(noteLetters[note]) + juce::String(octave), 
                                     (octave - 1) * 12 + note + 1);  // ItemID starts at 1
        }
    }
    contentComponent.addAndMakeVisible(rootNoteComboBox);
    rootNoteAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "luxstralRootNote", rootNoteComboBox);

    // Number of Octaves
    numOctavesLabel.setText("Octaves:", juce::dontSendNotification);
    numOctavesLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(numOctavesLabel);
    
    numOctavesSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    numOctavesSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    numOctavesSlider.setRange(1, 10, 1);  // Integer steps
    contentComponent.addAndMakeVisible(numOctavesSlider);
    numOctavesAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralNumOctaves", numOctavesSlider);

    // Frequency Range Info Label (read-only, displays calculated range)
    freqRangeInfoLabel.setText("Range: -- Hz to -- Hz", juce::dontSendNotification);
    freqRangeInfoLabel.setFont(juce::Font(juce::FontOptions(12.0f)).italicised());
    freqRangeInfoLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    contentComponent.addAndMakeVisible(freqRangeInfoLabel);

    // ========================================================================
    // Section: Envelope Parameters
    // ========================================================================
    envelopeSectionLabel.setText("Envelope Parameters", juce::dontSendNotification);
    envelopeSectionLabel.setFont(juce::Font(juce::FontOptions(15.0f)).boldened());
    envelopeSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(envelopeSectionLabel);

    attackLabel.setText("Attack Time:", juce::dontSendNotification);
    attackLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(attackLabel);
    
    attackSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    attackSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    attackSlider.setTextValueSuffix(" ms");
    contentComponent.addAndMakeVisible(attackSlider);
    attackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralAttackMs", attackSlider);

    releaseLabel.setText("Release Time:", juce::dontSendNotification);
    releaseLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(releaseLabel);
    
    releaseSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    releaseSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    releaseSlider.setTextValueSuffix(" ms");
    contentComponent.addAndMakeVisible(releaseSlider);
    releaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralReleaseMs", releaseSlider);

    // ========================================================================
    // Section: Image Processing
    // ========================================================================
    imageProcSectionLabel.setText("Image Processing", juce::dontSendNotification);
    imageProcSectionLabel.setFont(juce::Font(juce::FontOptions(15.0f)).boldened());
    imageProcSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(imageProcSectionLabel);

    gammaEnableLabel.setText("Gamma Correction:", juce::dontSendNotification);
    gammaEnableLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(gammaEnableLabel);
    
    gammaEnableToggle.setButtonText("Enable");
    contentComponent.addAndMakeVisible(gammaEnableToggle);
    gammaEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxstralGammaEnable", gammaEnableToggle);

    gammaValueLabel.setText("Gamma Value:", juce::dontSendNotification);
    gammaValueLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(gammaValueLabel);
    
    gammaValueSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gammaValueSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    contentComponent.addAndMakeVisible(gammaValueSlider);
    gammaValueAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralGammaValue", gammaValueSlider);

    contrastMinLabel.setText("Contrast Min:", juce::dontSendNotification);
    contrastMinLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(contrastMinLabel);
    
    contrastMinSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    contrastMinSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    contentComponent.addAndMakeVisible(contrastMinSlider);
    contrastMinAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralContrastMin", contrastMinSlider);

    // ========================================================================
    // Section: Stereo Processing
    // ========================================================================
    stereoSectionLabel.setText("Stereo Processing", juce::dontSendNotification);
    stereoSectionLabel.setFont(juce::Font(juce::FontOptions(15.0f)).boldened());
    stereoSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(stereoSectionLabel);

    stereoEnableLabel.setText("Stereo Mode:", juce::dontSendNotification);
    stereoEnableLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(stereoEnableLabel);
    
    stereoEnableToggle.setButtonText("Enable");
    contentComponent.addAndMakeVisible(stereoEnableToggle);
    stereoEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxstralStereoEnable", stereoEnableToggle);

    stereoTempAmpLabel.setText("Temperature Amp:", juce::dontSendNotification);
    stereoTempAmpLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(stereoTempAmpLabel);
    
    stereoTempAmpSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    stereoTempAmpSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    contentComponent.addAndMakeVisible(stereoTempAmpSlider);
    stereoTempAmpAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralStereoTempAmp", stereoTempAmpSlider);

    // ========================================================================
    // Section: Dynamics Processing
    // ========================================================================
    dynamicsSectionLabel.setText("Dynamics Processing", juce::dontSendNotification);
    dynamicsSectionLabel.setFont(juce::Font(juce::FontOptions(15.0f)).boldened());
    dynamicsSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(dynamicsSectionLabel);

    volumeWeightingLabel.setText("Volume Weighting:", juce::dontSendNotification);
    volumeWeightingLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(volumeWeightingLabel);
    
    volumeWeightingSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    volumeWeightingSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    contentComponent.addAndMakeVisible(volumeWeightingSlider);
    volumeWeightingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralVolumeWeightingExp", volumeWeightingSlider);

    softLimitThresholdLabel.setText("Soft Limit Threshold:", juce::dontSendNotification);
    softLimitThresholdLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(softLimitThresholdLabel);
    
    softLimitThresholdSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    softLimitThresholdSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    contentComponent.addAndMakeVisible(softLimitThresholdSlider);
    softLimitThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralSoftLimitThreshold", softLimitThresholdSlider);

    softLimitKneeLabel.setText("Soft Limit Knee:", juce::dontSendNotification);
    softLimitKneeLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(softLimitKneeLabel);
    
    softLimitKneeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    softLimitKneeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    contentComponent.addAndMakeVisible(softLimitKneeSlider);
    softLimitKneeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralSoftLimitKnee", softLimitKneeSlider);

    // ========================================================================
    // Section: Performance
    // ========================================================================
    performanceSectionLabel.setText("Performance", juce::dontSendNotification);
    performanceSectionLabel.setFont(juce::Font(juce::FontOptions(15.0f)).boldened());
    performanceSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(performanceSectionLabel);

    numWorkersLabel.setText("Worker Threads:", juce::dontSendNotification);
    numWorkersLabel.setJustificationType(juce::Justification::centredRight);
    contentComponent.addAndMakeVisible(numWorkersLabel);
    
    numWorkersSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    numWorkersSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 20);
    contentComponent.addAndMakeVisible(numWorkersSlider);
    numWorkersAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralNumWorkers", numWorkersSlider);

    // Add listeners for dynamic octave limitation
    rootNoteComboBox.addListener(this);
    tuningSlider.addListener(this);
    numOctavesSlider.addListener(this);
    
    // Initial update of octave range and info label
    updateOctavesSliderRange();
    updateFrequencyRangeInfo();

    layoutContentComponent();
}

LuxStralSettingsTab::~LuxStralSettingsTab()
{
}

void LuxStralSettingsTab::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Section title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(16.0f)).boldened());
    g.drawText("LuxStral Additive Synthesis", getLocalBounds().removeFromTop(30),
               juce::Justification::centred, true);
}

void LuxStralSettingsTab::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(35);  // Skip title
    
    viewport.setBounds(bounds);
    layoutContentComponent();
}

void LuxStralSettingsTab::layoutContentComponent()
{
    const int labelWidth = 140;
    const int sliderWidth = 200;
    const int rowHeight = 30;
    const int sectionSpacing = 15;
    const int itemSpacing = 5;
    const int padding = 20;

    int yPos = padding;
    int contentWidth = viewport.getWidth() - 40;

    // ========================================================================
    // Section: Musical Tuning
    // ========================================================================
    tuningRangeSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    tuningLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    tuningSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + itemSpacing;
    
    rootNoteLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    rootNoteComboBox.setBounds(padding + labelWidth + 10, yPos, 100, rowHeight);
    yPos += rowHeight + itemSpacing;
    
    numOctavesLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    numOctavesSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + itemSpacing;
    
    freqRangeInfoLabel.setBounds(padding, yPos, contentWidth, 20);
    yPos += 25 + sectionSpacing;

    // ========================================================================
    // Section: Envelope Parameters
    // ========================================================================
    envelopeSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    attackLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    attackSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + itemSpacing;
    
    releaseLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    releaseSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: Image Processing
    // ========================================================================
    imageProcSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    gammaEnableLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    gammaEnableToggle.setBounds(padding + labelWidth + 10, yPos, 100, rowHeight);
    yPos += rowHeight + itemSpacing;
    
    gammaValueLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    gammaValueSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + itemSpacing;
    
    contrastMinLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    contrastMinSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: Stereo Processing
    // ========================================================================
    stereoSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    stereoEnableLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    stereoEnableToggle.setBounds(padding + labelWidth + 10, yPos, 100, rowHeight);
    yPos += rowHeight + itemSpacing;
    
    stereoTempAmpLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    stereoTempAmpSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: Dynamics Processing
    // ========================================================================
    dynamicsSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    volumeWeightingLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    volumeWeightingSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + itemSpacing;
    
    softLimitThresholdLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    softLimitThresholdSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + itemSpacing;
    
    softLimitKneeLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    softLimitKneeSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: Performance
    // ========================================================================
    performanceSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    numWorkersLabel.setBounds(padding, yPos, labelWidth, rowHeight);
    numWorkersSlider.setBounds(padding + labelWidth + 10, yPos, sliderWidth, rowHeight);
    yPos += rowHeight + padding;

    // Set content component size for scrolling
    contentComponent.setSize(viewport.getWidth(), yPos);
}

//==============================================================================
// Listener implementations
//==============================================================================

void LuxStralSettingsTab::comboBoxChanged(juce::ComboBox* comboBoxThatHasChanged)
{
    if (comboBoxThatHasChanged == &rootNoteComboBox)
    {
        updateOctavesSliderRange();
        updateFrequencyRangeInfo();
    }
}

void LuxStralSettingsTab::sliderValueChanged(juce::Slider* slider)
{
    if (slider == &tuningSlider || slider == &numOctavesSlider)
    {
        // Also update when tuning changes (affects frequency calculations)
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

float LuxStralSettingsTab::getRootNoteFrequency() const
{
    // Get current tuning (A4 reference)
    float tuning = static_cast<float>(tuningSlider.getValue());
    
    // Get root note index (0-71 for C1-B6)
    int rootNoteIndex = rootNoteComboBox.getSelectedId() - 1;  // ItemID starts at 1
    if (rootNoteIndex < 0) rootNoteIndex = 0;
    
    // Calculate frequency using equal temperament
    // A4 is note index 45 (A1=9, A2=21, A3=33, A4=45)
    // Formula: freq = tuning * 2^((noteIndex - 45) / 12)
    float semitonesFromA4 = static_cast<float>(rootNoteIndex - 45);
    float rootFreq = tuning * std::pow(2.0f, semitonesFromA4 / 12.0f);
    
    return rootFreq;
}

int LuxStralSettingsTab::getMaxOctavesForRootNote() const
{
    constexpr float MAX_FREQUENCY = 20000.0f;  // Nyquist-safe limit
    
    float rootFreq = getRootNoteFrequency();
    if (rootFreq <= 0.0f) return 1;
    
    // Calculate max octaves: max_octaves = floor(log2(MAX_FREQUENCY / rootFreq))
    float maxOctavesFloat = std::log2(MAX_FREQUENCY / rootFreq);
    int maxOctaves = static_cast<int>(std::floor(maxOctavesFloat));
    
    // Clamp to valid range [1, 10]
    if (maxOctaves < 1) maxOctaves = 1;
    if (maxOctaves > 10) maxOctaves = 10;
    
    return maxOctaves;
}

void LuxStralSettingsTab::updateOctavesSliderRange()
{
    int maxOctaves = getMaxOctavesForRootNote();
    
    // Get current APVTS parameter value (the authoritative source)
    auto* param = apvts.getParameter("luxstralNumOctaves");
    int currentValue = static_cast<int>(param->convertFrom0to1(param->getValue()));
    
    // Update the slider range (this doesn't trigger attachment notification)
    numOctavesSlider.setRange(1, maxOctaves, 1);
    
    // Clamp current value if it exceeds new max
    if (currentValue > maxOctaves)
    {
        // CRITICAL FIX: Update BOTH the slider AND the APVTS parameter directly
        // The setValueNotifyingHost() ensures the processor gets the new value
        float normalizedValue = param->convertTo0to1(static_cast<float>(maxOctaves));
        param->setValueNotifyingHost(normalizedValue);
        
        // Also update slider (will be synced via attachment, but do it explicitly for UI)
        numOctavesSlider.setValue(maxOctaves, juce::dontSendNotification);
    }
    
    // Update label to show max
    numOctavesLabel.setText(juce::String("Octaves (max ") + juce::String(maxOctaves) + "):", 
                           juce::dontSendNotification);
}

void LuxStralSettingsTab::updateFrequencyRangeInfo()
{
    float rootFreq = getRootNoteFrequency();
    int numOctaves = static_cast<int>(numOctavesSlider.getValue());
    
    // Calculate high frequency
    float highFreq = rootFreq * std::pow(2.0f, static_cast<float>(numOctaves));
    
    // Clamp to 20kHz for display
    if (highFreq > 20000.0f) highFreq = 20000.0f;
    
    // Format the info label
    juce::String infoText = juce::String::formatted("Range: %.1f Hz to %.1f Hz", rootFreq, highFreq);
    
    // Add warning if approaching Nyquist
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
