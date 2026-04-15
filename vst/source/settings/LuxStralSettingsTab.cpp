#include "LuxStralSettingsTab.h"
#include "../Sp3ctraConstants.h"
#include "../UITheme.h"
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
    tuningRangeSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    tuningRangeSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(tuningRangeSectionLabel);

    // Tuning (A4 reference)
    tuningLabel.setText("Tuning (A4):", juce::dontSendNotification);
    tuningLabel.setJustificationType(juce::Justification::centredRight);
    tuningLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(tuningLabel);
    
    tuningSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    tuningSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    tuningSlider.setTextValueSuffix(" Hz");
    contentComponent.addAndMakeVisible(tuningSlider);
    tuningAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralTuning", tuningSlider);

    // Root Note (ComboBox)
    rootNoteLabel.setText("Root Note:", juce::dontSendNotification);
    rootNoteLabel.setJustificationType(juce::Justification::centredRight);
    rootNoteLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
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
    numOctavesLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(numOctavesLabel);
    
    numOctavesSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    numOctavesSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbNarrow, Sp3ctraTheme::kTextBoxH);
    numOctavesSlider.setRange(1, 10, 1);  // Integer steps
    contentComponent.addAndMakeVisible(numOctavesSlider);
    numOctavesAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralNumOctaves", numOctavesSlider);

    // Frequency Range Info Label (read-only, displays calculated range)
    freqRangeInfoLabel.setText("Range: -- Hz to -- Hz", juce::dontSendNotification);
    freqRangeInfoLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).italicised());
    freqRangeInfoLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    contentComponent.addAndMakeVisible(freqRangeInfoLabel);

    // Equal-Loudness Compensation (Physiological Filter)
    physiologicalFilterLabel.setText("Equal-Loudness:", juce::dontSendNotification);
    physiologicalFilterLabel.setJustificationType(juce::Justification::centredRight);
    physiologicalFilterLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(physiologicalFilterLabel);
    
    physiologicalFilterToggle.setButtonText("Compensate (A-weighting)");
    physiologicalFilterToggle.setTooltip(
        "Compensates for human hearing sensitivity (ISO 226 / A-weighting).\n"
        "Boosts bass frequencies and attenuates mid frequencies (~1-5 kHz)\n"
        "so all frequencies are perceived at equal loudness.\n\n"
        "Regenerates wavetables automatically when toggled.");
    contentComponent.addAndMakeVisible(physiologicalFilterToggle);
    physiologicalFilterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxstralPhysiologicalFilter", physiologicalFilterToggle);

    // Correction depth (0.0 = flat, 1.0 = full A-weighting inverse)
    physiologicalDepthLabel.setText("Correction Depth:", juce::dontSendNotification);
    physiologicalDepthLabel.setJustificationType(juce::Justification::centredRight);
    physiologicalDepthLabel.setTooltip(
        "Intensity of the equal-loudness correction.\n"
        "0.0 = flat (no correction), 1.0 = full inverse A-weighting.\n"
        "0.5 is a balanced starting point.");
    contentComponent.addAndMakeVisible(physiologicalDepthLabel);

    physiologicalDepthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    physiologicalDepthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbXNarrow, Sp3ctraTheme::kTextBoxH);
    physiologicalDepthSlider.setTooltip(physiologicalDepthLabel.getTooltip());
    contentComponent.addAndMakeVisible(physiologicalDepthSlider);
    physiologicalDepthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralPhysiologicalDepth", physiologicalDepthSlider);

    // ========================================================================
    // Section: Envelope Parameters
    // ========================================================================
    envelopeSectionLabel.setText("Envelope Parameters", juce::dontSendNotification);
    envelopeSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    envelopeSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(envelopeSectionLabel);

    attackLabel.setText("Attack Time:", juce::dontSendNotification);
    attackLabel.setJustificationType(juce::Justification::centredRight);
    attackLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(attackLabel);
    
    attackSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    attackSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    attackSlider.setTextValueSuffix(" ms");
    contentComponent.addAndMakeVisible(attackSlider);
    attackAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralAttackMs", attackSlider);

    releaseLabel.setText("Release Time:", juce::dontSendNotification);
    releaseLabel.setJustificationType(juce::Justification::centredRight);
    releaseLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(releaseLabel);
    
    releaseSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    releaseSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    releaseSlider.setTextValueSuffix(" ms");
    contentComponent.addAndMakeVisible(releaseSlider);
    releaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralReleaseMs", releaseSlider);

    // ========================================================================
    // Section: Image Processing
    // ========================================================================
    imageProcSectionLabel.setText("Image Processing", juce::dontSendNotification);
    imageProcSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    imageProcSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(imageProcSectionLabel);

    gammaEnableLabel.setText("Gamma Correction:", juce::dontSendNotification);
    gammaEnableLabel.setJustificationType(juce::Justification::centredRight);
    gammaEnableLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(gammaEnableLabel);
    
    gammaEnableToggle.setButtonText("Enable");
    contentComponent.addAndMakeVisible(gammaEnableToggle);
    gammaEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxstralGammaEnable", gammaEnableToggle);

    gammaValueLabel.setText("Gamma Value:", juce::dontSendNotification);
    gammaValueLabel.setJustificationType(juce::Justification::centredRight);
    gammaValueLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(gammaValueLabel);
    
    gammaValueSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gammaValueSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    contentComponent.addAndMakeVisible(gammaValueSlider);
    gammaValueAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralGammaValue", gammaValueSlider);

    contrastMinLabel.setText("Contrast Min:", juce::dontSendNotification);
    contrastMinLabel.setJustificationType(juce::Justification::centredRight);
    contrastMinLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(contrastMinLabel);
    
    contrastMinSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    contrastMinSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    contentComponent.addAndMakeVisible(contrastMinSlider);
    contrastMinAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralContrastMin", contrastMinSlider);

    // ========================================================================
    // Section: Stereo Processing
    // ========================================================================
    stereoSectionLabel.setText("Stereo Processing", juce::dontSendNotification);
    stereoSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    stereoSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(stereoSectionLabel);

    stereoEnableLabel.setText("Stereo Mode:", juce::dontSendNotification);
    stereoEnableLabel.setJustificationType(juce::Justification::centredRight);
    stereoEnableLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(stereoEnableLabel);
    
    stereoEnableToggle.setButtonText("Enable");
    contentComponent.addAndMakeVisible(stereoEnableToggle);
    stereoEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxstralStereoEnable", stereoEnableToggle);

    stereoTempAmpLabel.setText("Temperature Amp:", juce::dontSendNotification);
    stereoTempAmpLabel.setJustificationType(juce::Justification::centredRight);
    stereoTempAmpLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(stereoTempAmpLabel);
    
    stereoTempAmpSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    stereoTempAmpSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    contentComponent.addAndMakeVisible(stereoTempAmpSlider);
    stereoTempAmpAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralStereoTempAmp", stereoTempAmpSlider);

    // ========================================================================
    // Section: Dynamics Processing
    // ========================================================================
    dynamicsSectionLabel.setText("Dynamics Processing", juce::dontSendNotification);
    dynamicsSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    dynamicsSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(dynamicsSectionLabel);

    volumeWeightingLabel.setText("Volume Weighting:", juce::dontSendNotification);
    volumeWeightingLabel.setJustificationType(juce::Justification::centredRight);
    volumeWeightingLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(volumeWeightingLabel);
    
    volumeWeightingSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    volumeWeightingSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    contentComponent.addAndMakeVisible(volumeWeightingSlider);
    volumeWeightingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralVolumeWeightingExp", volumeWeightingSlider);

    softLimitThresholdLabel.setText("Soft Limit Threshold:", juce::dontSendNotification);
    softLimitThresholdLabel.setJustificationType(juce::Justification::centredRight);
    softLimitThresholdLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(softLimitThresholdLabel);
    
    softLimitThresholdSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    softLimitThresholdSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    contentComponent.addAndMakeVisible(softLimitThresholdSlider);
    softLimitThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralSoftLimitThreshold", softLimitThresholdSlider);

    softLimitKneeLabel.setText("Soft Limit Knee:", juce::dontSendNotification);
    softLimitKneeLabel.setJustificationType(juce::Justification::centredRight);
    softLimitKneeLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(softLimitKneeLabel);
    
    softLimitKneeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    softLimitKneeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    contentComponent.addAndMakeVisible(softLimitKneeSlider);
    softLimitKneeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralSoftLimitKnee", softLimitKneeSlider);

    // ========================================================================
    // Section: Performance
    // ========================================================================
    performanceSectionLabel.setText("Performance", juce::dontSendNotification);
    performanceSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    performanceSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    contentComponent.addAndMakeVisible(performanceSectionLabel);

    numWorkersLabel.setText("Worker Threads:", juce::dontSendNotification);
    numWorkersLabel.setJustificationType(juce::Justification::centredRight);
    numWorkersLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(numWorkersLabel);
    
    numWorkersSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    numWorkersSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    contentComponent.addAndMakeVisible(numWorkersSlider);
    numWorkersAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "luxstralNumWorkers", numWorkersSlider);

    // ========================================================================
    // Section: StrokeForge — Sine → Square Waveform Morphing
    // ========================================================================
    sfSectionLabel.setText("StrokeForge — Waveform Morphing (Sine \xe2\x86\x92 Square)", juce::dontSendNotification);
    sfSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    sfSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightyellow);
    contentComponent.addAndMakeVisible(sfSectionLabel);

    // Enable
    sfEnabledLabel.setText("Enable:", juce::dontSendNotification);
    sfEnabledLabel.setJustificationType(juce::Justification::centredRight);
    sfEnabledLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    contentComponent.addAndMakeVisible(sfEnabledLabel);
    sfEnabledToggle.setButtonText("StrokeForge Active");
    sfEnabledToggle.setTooltip("When enabled, stroke width controls waveform morphing:\n"
                               "  narrow stroke → pure sine\n"
                               "  wide stroke   → pure square (bandlimited)");
    contentComponent.addAndMakeVisible(sfEnabledToggle);
    sfEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "sfEnabled", sfEnabledToggle);

    // Blob Threshold — detection sensitivity
    sfBlobThresholdLabel.setText("Detect Threshold:", juce::dontSendNotification);
    sfBlobThresholdLabel.setJustificationType(juce::Justification::centredRight);
    sfBlobThresholdLabel.setTooltip("Minimum brightness to consider a region as a stroke.");
    contentComponent.addAndMakeVisible(sfBlobThresholdLabel);
    sfBlobThresholdSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    sfBlobThresholdSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    sfBlobThresholdSlider.setTooltip(sfBlobThresholdLabel.getTooltip());
    contentComponent.addAndMakeVisible(sfBlobThresholdSlider);
    sfBlobThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobBaseThreshold", sfBlobThresholdSlider);

    // Min Blob Width — anti-noise filter
    sfMinWidthLabel.setText("Min Stroke Width:", juce::dontSendNotification);
    sfMinWidthLabel.setJustificationType(juce::Justification::centredRight);
    sfMinWidthLabel.setTooltip("Minimum width (notes) for a detected region to count as a stroke.\n"
                               "Increase to reject sensor noise.");
    contentComponent.addAndMakeVisible(sfMinWidthLabel);
    sfMinWidthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    sfMinWidthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    sfMinWidthSlider.setTooltip(sfMinWidthLabel.getTooltip());
    contentComponent.addAndMakeVisible(sfMinWidthSlider);
    sfMinWidthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobMinWidth", sfMinWidthSlider);

    // Merge Gap — join nearby segments
    sfMergeGapLabel.setText("Merge Gap:", juce::dontSendNotification);
    sfMergeGapLabel.setJustificationType(juce::Justification::centredRight);
    sfMergeGapLabel.setTooltip("Maximum gap (notes) between two segments to merge them into one stroke.");
    contentComponent.addAndMakeVisible(sfMergeGapLabel);
    sfMergeGapSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    sfMergeGapSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    sfMergeGapSlider.setTooltip(sfMergeGapLabel.getTooltip());
    contentComponent.addAndMakeVisible(sfMergeGapSlider);
    sfMergeGapAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobMergeGap", sfMergeGapSlider);

    // Morph Width Scale — maps physical stroke width to morph=1.0 (full square)
    sfMorphWidthLabel.setText("Square at Width:", juce::dontSendNotification);
    sfMorphWidthLabel.setJustificationType(juce::Justification::centredRight);
    sfMorphWidthLabel.setTooltip("Stroke width (in notes) that produces 100% square wave.\n"
                                 "Smaller value = square wave with thinner strokes.\n"
                                 "morph = stroke_width / this_value  (clamped to 1.0)");
    contentComponent.addAndMakeVisible(sfMorphWidthLabel);
    sfMorphWidthSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    sfMorphWidthSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    sfMorphWidthSlider.setTooltip(sfMorphWidthLabel.getTooltip());
    contentComponent.addAndMakeVisible(sfMorphWidthSlider);
    sfMorphWidthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfMorphWidthScale", sfMorphWidthSlider);

    // Focus Sigma — Gaussian half-width controlling active oscillators per blob
    sfFocusSigmaLabel.setText("Focus Sigma:", juce::dontSendNotification);
    sfFocusSigmaLabel.setJustificationType(juce::Justification::centredRight);
    sfFocusSigmaLabel.setTooltip(
        "Gaussian focus sigma (notes): controls active oscillators per stroke.\n"
        "  0.5-5   = pure tone (1-2 notes ring per stroke)\n"
        "  10-20   = soft focus (~1 semitone bandwidth)\n"
        "  50-100  = spectral cloud (many notes active)\n"
        "Tip: set 1-5 to get a precise-pitch square wave.");
    contentComponent.addAndMakeVisible(sfFocusSigmaLabel);
    sfFocusSigmaSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    sfFocusSigmaSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    sfFocusSigmaSlider.setTextValueSuffix(" notes");
    sfFocusSigmaSlider.setTooltip(sfFocusSigmaLabel.getTooltip());
    contentComponent.addAndMakeVisible(sfFocusSigmaSlider);
    sfFocusSigmaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobFocusSigma", sfFocusSigmaSlider);

    // ── Spectral Width Threshold ──────────────────────────────────────────────
    sfSpectralWidthThresholdLabel.setText("Spectral Threshold:", juce::dontSendNotification);
    sfSpectralWidthThresholdLabel.setJustificationType(juce::Justification::centredRight);
    sfSpectralWidthThresholdLabel.setTooltip(
        "Width threshold (notes) above which a blob bypasses Gaussian focus\n"
        "and lets raw pixel intensities flow through unchanged (spectral passthrough).\n"
        "0 = disabled: all blobs always use Gaussian focus.");
    contentComponent.addAndMakeVisible(sfSpectralWidthThresholdLabel);

    sfSpectralWidthThresholdSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    sfSpectralWidthThresholdSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
    sfSpectralWidthThresholdSlider.setTextValueSuffix(" notes");
    sfSpectralWidthThresholdSlider.setTooltip(sfSpectralWidthThresholdLabel.getTooltip());
    contentComponent.addAndMakeVisible(sfSpectralWidthThresholdSlider);

    sfSpectralWidthThresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfSpectralWidthThreshold", sfSpectralWidthThresholdSlider);

    // ── Focus-Only Mode ───────────────────────────────────────────────────────
    sfFocusOnlyLabel.setText("Focus Only (spectral):", juce::dontSendNotification);
    sfFocusOnlyLabel.setJustificationType(juce::Justification::centredRight);
    sfFocusOnlyLabel.setTooltip(
        "Gaussian pitch focus without sine\xe2\x86\x92square morphing.\n"
        "Enabled when StrokeForge is OFF:\n"
        "  stroke drawn  \xe2\x86\x92 only the drawn frequency rings (Gaussian focus)\n"
        "  waveform stays pure sine (no timbre change)\n\n"
        "Tip: use with a tight sigma (1-5) for a focused sine instrument\n"
        "that responds precisely to the drawn frequency.");
    contentComponent.addAndMakeVisible(sfFocusOnlyLabel);

    sfFocusOnlyToggle.setButtonText("Focus Without Morph");
    sfFocusOnlyToggle.setTooltip(sfFocusOnlyLabel.getTooltip());
    contentComponent.addAndMakeVisible(sfFocusOnlyToggle);
    sfFocusOnlyAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "sfFocusOnly", sfFocusOnlyToggle);

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
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
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
    constexpr int labelWidth = Sp3ctraTheme::kLabelWide;
    const int sliderWidth = 200;
    constexpr int rowHeight = Sp3ctraTheme::kRowStep;
    const int sectionSpacing = 15;
    constexpr int itemSpacing = Sp3ctraTheme::kRowGap;
    constexpr int padding = Sp3ctraTheme::kHPad;
    constexpr int ctrlH = Sp3ctraTheme::kControlH;
    constexpr int vc = (rowHeight - ctrlH) / 2;

    int yPos = padding;
    int contentWidth = viewport.getWidth() - 40;

    // ========================================================================
    // Section: Musical Tuning
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
    // Section: Envelope Parameters
    // ========================================================================
    envelopeSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    attackLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    attackSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;
    
    releaseLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    releaseSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: Image Processing
    // ========================================================================
    imageProcSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    gammaEnableLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    gammaEnableToggle.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, 100, ctrlH);
    yPos += rowHeight + itemSpacing;
    
    gammaValueLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    gammaValueSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;
    
    contrastMinLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    contrastMinSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: Stereo Processing
    // ========================================================================
    stereoSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    stereoEnableLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    stereoEnableToggle.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, 100, ctrlH);
    yPos += rowHeight + itemSpacing;
    
    stereoTempAmpLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    stereoTempAmpSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: Dynamics Processing
    // ========================================================================
    dynamicsSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    volumeWeightingLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    volumeWeightingSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;
    
    softLimitThresholdLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    softLimitThresholdSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;
    
    softLimitKneeLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    softLimitKneeSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: Performance
    // ========================================================================
    performanceSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;
    
    numWorkersLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    numWorkersSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + sectionSpacing;

    // ========================================================================
    // Section: StrokeForge — Waveform Morphing (5 params only)
    // ========================================================================
    sfSectionLabel.setBounds(padding, yPos, contentWidth, 25);
    yPos += 30;

    sfEnabledLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    sfEnabledToggle.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    sfBlobThresholdLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    sfBlobThresholdSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    sfMinWidthLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    sfMinWidthSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    sfMergeGapLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    sfMergeGapSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    sfMorphWidthLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    sfMorphWidthSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    sfFocusSigmaLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    sfFocusSigmaSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    sfSpectralWidthThresholdLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    sfSpectralWidthThresholdSlider.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
    yPos += rowHeight + itemSpacing;

    sfFocusOnlyLabel.setBounds(padding, yPos + vc, labelWidth, ctrlH);
    sfFocusOnlyToggle.setBounds(padding + labelWidth + Sp3ctraTheme::kGap, yPos + vc, sliderWidth, ctrlH);
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
