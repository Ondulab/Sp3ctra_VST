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
    // ── Section: StrokeForge Blob Detection (advanced) ──────────────────────────
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

    constexpr int pad  = 10;
    constexpr int secH = 28;

    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
    g.setColour(juce::Colour(Sp3ctraTheme::kColText).brighter(0.2f));

    g.drawText("StrokeForge — Advanced Blob Detection",
               juce::Rectangle<int>(pad, pad, getWidth() - 2 * pad, secH),
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

    int cy = pad + secH;
    contrastAdaptiveLabel .setBounds(pad,   cy, labelW, rowH);
    contrastAdaptiveToggle.setBounds(ctrlX, cy, 80,     rowH);
    cy += rowSt;
    contrastSensLabel .setBounds(pad,   cy, labelW, rowH);
    contrastSensSlider.setBounds(ctrlX, cy, ctrlW,  rowH);
}
