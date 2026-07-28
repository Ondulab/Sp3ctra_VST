#include "VideoScrollSetupPanel.h"
#include "SetupHeader.h"
#include "../../UITheme.h"

//==============================================================================
VideoScrollSetupPanel::VideoScrollSetupPanel(Sp3ctraAudioProcessor& processor,
                                             juce::Colour accentColour)
    : processor_(processor), apvts_(processor.getAPVTS()), accent_(accentColour)
{
    selector_.setName("Background");
    addAndMakeVisible(selector_);
    selector_.addChangeListener(this);
}

VideoScrollSetupPanel::~VideoScrollSetupPanel()
{
    selector_.removeChangeListener(this);
}

//==============================================================================
float VideoScrollSetupPanel::readParam(const char* suffix, float def) const
{
    if (slot_ < 0) return def;
    if (auto* v = apvts_.getRawParameterValue(vsParam(slot_, suffix)))
        return v->load();
    return def;
}

void VideoScrollSetupPanel::writeParam(const char* suffix, float value)
{
    if (slot_ < 0) return;
    if (auto* p = apvts_.getParameter(vsParam(slot_, suffix)))
        p->setValueNotifyingHost(p->convertTo0to1(juce::jlimit(0.f, 1.f, value)));
}

//==============================================================================
void VideoScrollSetupPanel::setSlot(int slot)
{
    slot_ = slot;
    if (slot_ >= 0)
    {
        // Push the stored colour into the picker WITHOUT triggering a write-back.
        const juce::ScopedValueSetter<bool> guard(updating_, true);
        selector_.setCurrentColour(
            juce::Colour::fromFloatRGBA(readParam("bgR", 1.f),
                                        readParam("bgG", 1.f),
                                        readParam("bgB", 1.f), 1.f),
            juce::dontSendNotification);
    }
    repaint();
}

//==============================================================================
void VideoScrollSetupPanel::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    if (updating_ || slot_ < 0 || source != &selector_) return;
    const auto c = selector_.getCurrentColour();
    writeParam("bgR", c.getFloatRed());
    writeParam("bgG", c.getFloatGreen());
    writeParam("bgB", c.getFloatBlue());
}

//==============================================================================
void VideoScrollSetupPanel::paint(juce::Graphics& g)
{
    SetupUI::paintHeader(g, *this, "VIDEO SCROLL -- SETUP", accent_);

    g.setColour(juce::Colour(Sp3ctraTheme::kColText));
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    const int y = SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap;
    g.drawText("Frame background (negative-zoom border):",
               Sp3ctraTheme::kHPad, y,
               getWidth() - 2 * Sp3ctraTheme::kHPad, Sp3ctraTheme::kControlH,
               juce::Justification::centredLeft, true);

    if (slot_ < 0)
    {
        g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.5f));
        g.drawText("Select a VIDEO SCROLL block",
                   getLocalBounds().removeFromBottom(28),
                   juce::Justification::centred, true);
    }
}

//==============================================================================
void VideoScrollSetupPanel::resized()
{
    auto r = getLocalBounds().reduced(Sp3ctraTheme::kHPad, 0);
    r.removeFromTop(SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap
                    + Sp3ctraTheme::kControlH + Sp3ctraTheme::kSectionGap);
    selector_.setBounds(r);
}
