#include "VideoScrollSettingsTab.h"
#include "../UITheme.h"

//==============================================================================
namespace
{
    constexpr int kLW     = Sp3ctraTheme::kLabelWide;
    constexpr int kGap    = Sp3ctraTheme::kGap;
    constexpr int kHP     = Sp3ctraTheme::kHPad;
    constexpr int kCtrlH  = Sp3ctraTheme::kControlH;
    constexpr int kStep   = Sp3ctraTheme::kRowStep;
    constexpr int kSecH   = Sp3ctraTheme::kSectionH;
    constexpr int kSecGap = Sp3ctraTheme::kSectionGap;
    constexpr int kTbW    = Sp3ctraTheme::kTbStd;
    constexpr int kTbH    = Sp3ctraTheme::kTextBoxH;

    // Total scrollable content height (3 sections)
    constexpr int kContentH = 420;

    void styleSectionLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
        lbl.setColour(juce::Label::textColourId, juce::Colour(0xff66cc88u));
        lbl.setJustificationType(juce::Justification::centredLeft);
    }

    void styleLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        lbl.setColour(juce::Label::textColourId, juce::Colour(Sp3ctraTheme::kColText));
        lbl.setJustificationType(juce::Justification::centredRight);
    }

    void styleSlider(juce::Slider& s, const juce::String& suffix = "")
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, kTbW, kTbH);
        if (suffix.isNotEmpty()) s.setTextValueSuffix(suffix);
    }
} // namespace

//==============================================================================
VideoScrollSettingsTab::VideoScrollSettingsTab(Sp3ctraAudioProcessor& processor)
    : apvts_(processor.getAPVTS())
{
    // ── Viewport setup ────────────────────────────────────────────────────────
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);

    // ── Section: Display ──────────────────────────────────────────────────────
    styleSectionLabel(displaySectionLabel_, "Display");
    content_.addAndMakeVisible(displaySectionLabel_);

    styleLabel(brightnessLabel_, "Brightness");
    content_.addAndMakeVisible(brightnessLabel_);
    styleSlider(brightnessSlider_, " x");
    brightnessSlider_.setTooltip(
        "Brightness multiplier applied before display.\n"
        "1.0 = neutral, >1.0 = brighter, <1.0 = darker.");
    content_.addAndMakeVisible(brightnessSlider_);
    brightnessAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts_, "videoScrollBrightness", brightnessSlider_);

    styleLabel(invertLabel_, "Invert");
    content_.addAndMakeVisible(invertLabel_);
    invertToggle_.setButtonText("Invert colors (negative)");
    invertToggle_.setTooltip("Invert RGB values of each pixel (negative image).");
    content_.addAndMakeVisible(invertToggle_);
    invertAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts_, "videoInvertColor", invertToggle_);

    styleLabel(colorModeLabel_, "Color mode");
    content_.addAndMakeVisible(colorModeLabel_);
    colorModeToggle_.setButtonText("RGB (off = grayscale)");
    colorModeToggle_.setTooltip(
        "On: full RGB render from CIS R/G/B channels.\n"
        "Off: grayscale (luma BT.601).");
    content_.addAndMakeVisible(colorModeToggle_);
    colorModeAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts_, "videoColorMode", colorModeToggle_);

    // ── Section: Sequencer ────────────────────────────────────────────────────
    styleSectionLabel(seqSectionLabel_, "Sequencer");
    content_.addAndMakeVisible(seqSectionLabel_);

    styleLabel(bpmLabel_, "BPM");
    content_.addAndMakeVisible(bpmLabel_);
    styleSlider(bpmSlider_, " BPM");
    bpmSlider_.setTooltip(
        "Tempo used for MIDI-clock-synced scroll modes.\n"
        "Only active when MIDI Sync is enabled.");
    content_.addAndMakeVisible(bpmSlider_);
    bpmAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts_, "videoScrollBpm", bpmSlider_);

    styleLabel(midiSyncLabel_, "MIDI Sync");
    content_.addAndMakeVisible(midiSyncLabel_);
    midiSyncToggle_.setButtonText("Sync to MIDI clock");
    midiSyncToggle_.setTooltip(
        "When enabled, scroll speed is quantized to the MIDI clock\n"
        "received from the DAW or an external device.");
    content_.addAndMakeVisible(midiSyncToggle_);
    midiSyncAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
        apvts_, "videoScrollMidiSync", midiSyncToggle_);

    styleLabel(maxDurLabel_, "Max duration");
    content_.addAndMakeVisible(maxDurLabel_);
    styleSlider(maxDurSlider_, " s");
    maxDurSlider_.setTooltip(
        "Maximum sequence recording duration (seconds).\n"
        "Higher values consume more memory (10 KB/frame * fps * duration).");
    content_.addAndMakeVisible(maxDurSlider_);
    maxDurAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts_, "videoScrollMaxDuration", maxDurSlider_);

    // ── Section: Window ───────────────────────────────────────────────────────
    styleSectionLabel(windowSectionLabel_, "Video Window");
    content_.addAndMakeVisible(windowSectionLabel_);

    styleLabel(windowWLabel_, "Default width");
    content_.addAndMakeVisible(windowWLabel_);
    styleSlider(windowWSlider_, " px");
    windowWSlider_.setTooltip("Default window width when opened (pixels).");
    content_.addAndMakeVisible(windowWSlider_);
    windowWAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts_, "videoWindowWidth", windowWSlider_);

    styleLabel(windowHLabel_, "Default height");
    content_.addAndMakeVisible(windowHLabel_);
    styleSlider(windowHSlider_, " px");
    windowHSlider_.setTooltip("Default window height when opened (pixels).");
    content_.addAndMakeVisible(windowHSlider_);
    windowHAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        apvts_, "videoWindowHeight", windowHSlider_);
}

VideoScrollSettingsTab::~VideoScrollSettingsTab() = default;

//==============================================================================
void VideoScrollSettingsTab::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(Sp3ctraTheme::kColBg));
    g.fillAll();
}

void VideoScrollSettingsTab::resized()
{
    viewport_.setBounds(getLocalBounds());
    content_.setSize(juce::jmax(getWidth() - viewport_.getScrollBarThickness(), 300),
                     kContentH);
    layoutContent();
}

void VideoScrollSettingsTab::layoutContent()
{
    const int W     = content_.getWidth();
    const int ctrlX = kHP + kLW + kGap;
    const int ctrlW = juce::jmax(100, W - ctrlX - kHP);

    int y = kHP;

    auto placeRow = [&](juce::Label& lbl, juce::Component& ctrl)
    {
        lbl.setBounds(kHP, y, kLW, kCtrlH);
        ctrl.setBounds(ctrlX, y, ctrlW, kCtrlH);
        y += kStep;
    };
    auto placeToggle = [&](juce::Label& lbl, juce::ToggleButton& btn)
    {
        lbl.setBounds(kHP, y, kLW, kCtrlH);
        btn.setBounds(ctrlX, y, ctrlW, kCtrlH);
        y += kStep;
    };

    // ── Display ───────────────────────────────────────────────────────────────
    displaySectionLabel_.setBounds(kHP, y, W - 2*kHP, kSecH);
    y += kSecH + kSecGap;

    placeRow   (brightnessLabel_, brightnessSlider_);
    placeToggle(invertLabel_,     invertToggle_);
    placeToggle(colorModeLabel_,  colorModeToggle_);

    y += kStep / 2;

    // ── Sequencer ─────────────────────────────────────────────────────────────
    seqSectionLabel_.setBounds(kHP, y, W - 2*kHP, kSecH);
    y += kSecH + kSecGap;

    placeRow   (bpmLabel_,     bpmSlider_);
    placeToggle(midiSyncLabel_, midiSyncToggle_);
    placeRow   (maxDurLabel_,  maxDurSlider_);

    y += kStep / 2;

    // ── Window ────────────────────────────────────────────────────────────────
    windowSectionLabel_.setBounds(kHP, y, W - 2*kHP, kSecH);
    y += kSecH + kSecGap;

    placeRow(windowWLabel_, windowWSlider_);
    placeRow(windowHLabel_, windowHSlider_);
}
