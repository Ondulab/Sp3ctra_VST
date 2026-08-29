#include "CentroSetupPanel.h"
#include "SetupHeader.h"
#include "../../UITheme.h"
#include "../ModuleParamManifest.h"   // ctParam — luxcentro{slot}_* bank ids

CentroSetupPanel::CentroSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour)
    : apvts(processor.getAPVTS()), accent(accentColour)
{
    // ── Width law (PX / ERB) ────────────────────────────────────────────
    lawLabel.setText("Width law", juce::dontSendNotification);
    lawLabel.setJustificationType(juce::Justification::centredRight);
    lawLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(lawLabel);

    lawCombo.addItem("PX  - constant interval",    1);
    lawCombo.addItem("ERB - constant sensation",   2);
    addAndMakeVisible(lawCombo);

    setSlot(0);   // bind to bank 0 until a block is selected
}

void CentroSetupPanel::setSlot(int slot)
{
    slot_ = juce::jlimit(0, 7, slot);

    lawAttachment.reset();
    lawAttachment.reset(
        new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, ctParam(slot_, "WidthLaw"), lawCombo));
}

CentroSetupPanel::~CentroSetupPanel() {}

void CentroSetupPanel::paint(juce::Graphics& g)
{
    SetupUI::paintHeader(g, *this, "CENTROID -- SETUP", accent);

    // ── Explanation — why a width needs a LAW on this axis ──────────────
    // (The pixel axis is log-frequency: what a thickness IS depends on
    // where it sits, so the choice is structural and lives on SETUP.)
    const juce::String text =
        "Each mass is redrawn as one line of Thickness pixels. The axis is "
        "log-frequency, so a fixed pixel width is a fixed musical interval "
        "- but NOT a fixed sensation: the same width sounds like a slow "
        "chorus in the bass, roughness in the mids and a noisy band in the "
        "treble.\n\n"
        "PX keeps the width uniform (constant interval, the raw geometry).\n\n"
        "ERB (default) follows the ear's critical band: wider strokes in "
        "the bass, narrower in the treble, so a line sounds equally wide "
        "everywhere. Thickness keeps its exact value at the axis centre.\n\n"
        "Width Tilt (PLAY face, WIDTH LAW view) adds a free slope on top "
        "of the chosen law: + widens toward the treble, - toward the bass.";

    g.setColour(accent.withAlpha(0.60f));
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    g.drawFittedText(text, helpArea_, juce::Justification::topLeft, 20, 1.0f);
}

void CentroSetupPanel::resized()
{
    auto area = getLocalBounds().reduced(Sp3ctraTheme::kHPad, 0);
    area.removeFromTop(SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap);

    constexpr int rowH   = Sp3ctraTheme::kControlH;
    constexpr int gap    = Sp3ctraTheme::kRowGap * 2;
    constexpr int labelW = Sp3ctraTheme::kLabelW;
    const int ctrlW      = juce::jmin(260, area.getWidth() - labelW - Sp3ctraTheme::kGap);

    // Row 1: Width law
    {
        auto r = area.removeFromTop(rowH);
        area.removeFromTop(gap);
        lawLabel.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        lawCombo.setBounds(r.removeFromLeft(ctrlW));
    }

    // The rest is the painted explanation block.
    helpArea_ = area.reduced(2, 2);
}
