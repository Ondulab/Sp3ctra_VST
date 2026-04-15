#include "ImageSettingsTab.h"
#include "../UITheme.h"

// ── File-local helpers ────────────────────────────────────────────────────────
namespace
{
    void setupSlider(juce::Slider& s, const char* suffix = nullptr)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                          Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
        if (suffix) s.setTextValueSuffix(suffix);
    }

    void setupLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        lbl.setColour(juce::Label::textColourId, juce::Colour(Sp3ctraTheme::kColText));
    }
} // namespace

//==============================================================================
ImageSettingsTab::ImageSettingsTab(Sp3ctraAudioProcessor& processor)
    : apvts(processor.getAPVTS())
{
    // ── Section: Image Processing Flags ───────────────────────────────────────
    setupLabel(gammaEnableLabel, "Gamma Enable");
    gammaEnableToggle.setButtonText("On");
    addAndMakeVisible(gammaEnableLabel);
    addAndMakeVisible(gammaEnableToggle);
    gammaEnableAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxstralGammaEnable", gammaEnableToggle);

    setupLabel(invertLabel, "Invert Intensity");
    invertToggle.setButtonText("On");
    addAndMakeVisible(invertLabel);
    addAndMakeVisible(invertToggle);
    invertAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "luxstralInvertIntensity", invertToggle);

    // ── Section: Stream Settings ──────────────────────────────────────────────
    // Note: Live/Sampler opacity sliders removed — now driven by Mix Balance
    // crossfader in SourcesTabComponent.

    setupLabel(fadeInLabel, "Fade-In");
    setupSlider(fadeInSlider, " ms");
    addAndMakeVisible(fadeInLabel);
    addAndMakeVisible(fadeInSlider);
    fadeInAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "imageFadeInMs", fadeInSlider);

    // ── Section: Advanced Blob Detection ──────────────────────────────────────
    setupLabel(blobMinWidthLabel, "Blob Min Width");
    setupSlider(blobMinWidthSlider, " pix");
    addAndMakeVisible(blobMinWidthLabel);
    addAndMakeVisible(blobMinWidthSlider);
    blobMinWidthAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobMinWidth", blobMinWidthSlider);

    setupLabel(contrastAdaptiveLabel, "Contrast Adaptive");
    contrastAdaptiveToggle.setButtonText("On");
    addAndMakeVisible(contrastAdaptiveLabel);
    addAndMakeVisible(contrastAdaptiveToggle);
    contrastAdaptiveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts, "sfBlobContrastAdaptive", contrastAdaptiveToggle);

    setupLabel(contrastSensLabel, "Contrast Sensitivity");
    setupSlider(contrastSensSlider);
    addAndMakeVisible(contrastSensLabel);
    addAndMakeVisible(contrastSensSlider);
    contrastSensAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts, "sfBlobContrastSensitivity", contrastSensSlider);
}

ImageSettingsTab::~ImageSettingsTab() = default;

//==============================================================================
void ImageSettingsTab::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1e1e));

    constexpr int pad   = 10;
    constexpr int secH  = 28;
    constexpr int rowSt = Sp3ctraTheme::kRowStep;

    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    g.setColour(juce::Colour(Sp3ctraTheme::kColText).brighter(0.2f));

    // Section heading Y positions mirror resized() layout
    g.drawText("Image Processing",
               juce::Rectangle<int>(pad, pad, getWidth() - 2 * pad, secH),
               juce::Justification::centredLeft, true);

    const int streamSectionY = pad + secH + 2 * rowSt + 6;
    g.drawText("Stream Settings",
               juce::Rectangle<int>(pad, streamSectionY, getWidth() - 2 * pad, secH),
               juce::Justification::centredLeft, true);

    const int blobSectionY = streamSectionY + secH + 1 * rowSt + 6;
    g.drawText("Advanced Blob Detection",
               juce::Rectangle<int>(pad, blobSectionY, getWidth() - 2 * pad, secH),
               juce::Justification::centredLeft, true);
}

//==============================================================================
void ImageSettingsTab::resized()
{
    constexpr int pad    = 10;
    constexpr int secH   = 28;
    constexpr int rowSt  = Sp3ctraTheme::kRowStep;
    constexpr int rowH   = Sp3ctraTheme::kControlH;
    constexpr int labelW = Sp3ctraTheme::kLabelWide;
    constexpr int ctrlW  = 260;
    constexpr int ctrlX  = pad + labelW + 8;

    // ── Image Processing Flags ────────────────────────────────────────────────
    int cy = pad + secH;
    gammaEnableLabel .setBounds(pad,   cy, labelW, rowH);
    gammaEnableToggle.setBounds(ctrlX, cy, 80,     rowH);
    cy += rowSt;
    invertLabel .setBounds(pad,   cy, labelW, rowH);
    invertToggle.setBounds(ctrlX, cy, 80,     rowH);
    cy += rowSt;

    // ── Stream Settings ───────────────────────────────────────────────────────
    cy += secH + 6;
    fadeInLabel .setBounds(pad,   cy, labelW, rowH);
    fadeInSlider.setBounds(ctrlX, cy, ctrlW,  rowH);
    cy += rowSt;

    // ── Advanced Blob Detection ───────────────────────────────────────────────
    cy += secH + 6;
    blobMinWidthLabel .setBounds(pad,   cy, labelW, rowH);
    blobMinWidthSlider.setBounds(ctrlX, cy, ctrlW,  rowH);
    cy += rowSt;
    contrastAdaptiveLabel .setBounds(pad,   cy, labelW, rowH);
    contrastAdaptiveToggle.setBounds(ctrlX, cy, 80,     rowH);
    cy += rowSt;
    contrastSensLabel .setBounds(pad,   cy, labelW, rowH);
    contrastSensSlider.setBounds(ctrlX, cy, ctrlW,  rowH);
}
