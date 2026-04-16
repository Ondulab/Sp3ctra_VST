#include "LuxSynthSettingsTab.h"

//==============================================================================
LuxSynthSettingsTab::LuxSynthSettingsTab(Sp3ctraAudioProcessor& processor)
    : apvts(processor.getAPVTS())
{
    auto setupSectionLabel = [this](juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(16.0f));
        label.setColour(juce::Label::textColourId, juce::Colour(0xFF88CCFF));
        label.setJustificationType(juce::Justification::centredLeft);
        contentComponent.addAndMakeVisible(label);
    };

    auto setupLabel = [this](juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::right);
        contentComponent.addAndMakeVisible(label);
    };

    auto setupSlider = [this](juce::Slider& slider, const juce::String& paramId,
                               std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& attachment)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, 22);
        slider.addListener(this);
        contentComponent.addAndMakeVisible(slider);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, paramId, slider);
    };

    // ── Engine Enable ──
    setupSectionLabel(enableSectionLabel, "ENGINE");
    setupLabel(enableLabel, "LuxSynth Enabled");
    contentComponent.addAndMakeVisible(enableToggle);
    enableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxsynthEnabled", enableToggle);

    setupLabel(midiChannelLabel, "MIDI Channel");
    contentComponent.addAndMakeVisible(midiChannelCombo);
    midiChannelAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "luxsynthMidiChannel", midiChannelCombo);

    setupLabel(octaveOffsetLabel, "Octave Offset");
    contentComponent.addAndMakeVisible(octaveOffsetCombo);
    octaveOffsetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, "luxsynthOctaveOffset", octaveOffsetCombo);

    // ── Volume ADSR ──
    setupSectionLabel(volAdsrSectionLabel, "VOLUME ADSR");
    setupLabel(attackLabel, "Attack");
    setupSlider(attackSlider, "luxsynthAttackMs", attackAttachment);
    setupLabel(decayLabel, "Decay");
    setupSlider(decaySlider, "luxsynthDecayMs", decayAttachment);
    setupLabel(sustainLabel, "Sustain");
    setupSlider(sustainSlider, "luxsynthSustainLevel", sustainAttachment);
    setupLabel(releaseLabel, "Release");
    setupSlider(releaseSlider, "luxsynthReleaseMs", releaseAttachment);

    // ── Filter ADSR ──
    setupSectionLabel(fltAdsrSectionLabel, "FILTER ADSR");
    setupLabel(fltAttackLabel, "Filter Attack");
    setupSlider(fltAttackSlider, "luxsynthFilterAttackMs", fltAttackAttachment);
    setupLabel(fltDecayLabel, "Filter Decay");
    setupSlider(fltDecaySlider, "luxsynthFilterDecayMs", fltDecayAttachment);
    setupLabel(fltSustainLabel, "Filter Sustain");
    setupSlider(fltSustainSlider, "luxsynthFilterSustain", fltSustainAttachment);
    setupLabel(fltReleaseLabel, "Filter Release");
    setupSlider(fltReleaseSlider, "luxsynthFilterReleaseMs", fltReleaseAttachment);
    setupLabel(fltCutoffLabel, "Cutoff");
    setupSlider(fltCutoffSlider, "luxsynthFilterCutoff", fltCutoffAttachment);
    setupLabel(fltDepthLabel, "Env Depth");
    setupSlider(fltDepthSlider, "luxsynthFilterEnvDepth", fltDepthAttachment);

    // ── Spectral ──
    setupSectionLabel(spectralSectionLabel, "SPECTRAL");
    setupLabel(gammaLabel, "Gamma");
    setupSlider(gammaSlider, "luxsynthGamma", gammaAttachment);
    setupLabel(numOscLabel, "Oscillators");
    setupSlider(numOscSlider, "luxsynthNumOscillators", numOscAttachment);

    // ── LFO ──
    setupSectionLabel(lfoSectionLabel, "LFO (VIBRATO)");
    setupLabel(lfoRateLabel, "Rate");
    setupSlider(lfoRateSlider, "luxsynthLfoRate", lfoRateAttachment);
    setupLabel(lfoDepthLabel, "Depth");
    setupSlider(lfoDepthSlider, "luxsynthLfoDepth", lfoDepthAttachment);

    viewport.setViewedComponent(&contentComponent, false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);
}

LuxSynthSettingsTab::~LuxSynthSettingsTab() = default;

void LuxSynthSettingsTab::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void LuxSynthSettingsTab::resized()
{
    viewport.setBounds(getLocalBounds());
    layoutContentComponent();
}

void LuxSynthSettingsTab::layoutContentComponent()
{
    const int w = viewport.getMaximumVisibleWidth();
    const int rowH = 28;
    const int sectionH = 32;
    const int labelW = 130;
    const int gap = 6;
    int y = 10;

    auto addSection = [&](juce::Label& label)
    {
        label.setBounds(10, y, w - 20, sectionH);
        y += sectionH + gap;
    };

    auto addRow = [&](juce::Label& label, juce::Component& ctrl)
    {
        label.setBounds(10, y, labelW, rowH);
        ctrl.setBounds(labelW + 10, y, w - labelW - 30, rowH);
        y += rowH + gap;
    };

    addSection(enableSectionLabel);
    addRow(enableLabel, enableToggle);
    addRow(midiChannelLabel, midiChannelCombo);
    addRow(octaveOffsetLabel, octaveOffsetCombo);

    addSection(volAdsrSectionLabel);
    addRow(attackLabel, attackSlider);
    addRow(decayLabel, decaySlider);
    addRow(sustainLabel, sustainSlider);
    addRow(releaseLabel, releaseSlider);

    addSection(fltAdsrSectionLabel);
    addRow(fltAttackLabel, fltAttackSlider);
    addRow(fltDecayLabel, fltDecaySlider);
    addRow(fltSustainLabel, fltSustainSlider);
    addRow(fltReleaseLabel, fltReleaseSlider);
    addRow(fltCutoffLabel, fltCutoffSlider);
    addRow(fltDepthLabel, fltDepthSlider);

    addSection(spectralSectionLabel);
    addRow(gammaLabel, gammaSlider);
    addRow(numOscLabel, numOscSlider);

    addSection(lfoSectionLabel);
    addRow(lfoRateLabel, lfoRateSlider);
    addRow(lfoDepthLabel, lfoDepthSlider);

    contentComponent.setSize(w, y + 20);
}

void LuxSynthSettingsTab::sliderValueChanged(juce::Slider* /*slider*/)
{
    /* All parameter changes are handled via APVTS attachments.
     * The LuxSynth engine reads these atomically in its processing loop. */
}
