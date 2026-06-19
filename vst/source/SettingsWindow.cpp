#include "SettingsWindow.h"
#include "Sp3ctraConstants.h"
#include "UITheme.h"

//==============================================================================
// SettingsComponent Implementation
//==============================================================================

SettingsComponent::SettingsComponent(Sp3ctraAudioProcessor& processor)
    : tabbedComponent(juce::TabbedButtonBar::TabsAtTop)
{
    // Machine-level settings only (M5 / C9): everything musical moved to the
    // per-block SETUP faces (zone 3) and the waterfall toolbar (zone 4).
    networkTab = new NetworkSettingsTab(processor);
    systemTab  = new SystemSettingsTab(processor);

    tabbedComponent.addTab("Network", juce::Colours::darkgrey, networkTab, false);
    tabbedComponent.addTab("System",  juce::Colours::darkgrey, systemTab,  false);

    addAndMakeVisible(tabbedComponent);

    setSize(560, 420);
}

SettingsComponent::~SettingsComponent()
{
    // Tabs are owned and deleted by TabbedComponent
}

void SettingsComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Title
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontWindowTitle)).boldened());
    g.drawText("Sp3ctra Configuration", getLocalBounds().removeFromTop(40),
               juce::Justification::centred, true);
}

void SettingsComponent::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop(40);  // Skip title area
    bounds.reduce(10, 5);

    tabbedComponent.setBounds(bounds);
}

//==============================================================================
// SettingsWindow Implementation
//==============================================================================

SettingsWindow::SettingsWindow(Sp3ctraAudioProcessor& processor)
    : DocumentWindow("Sp3ctra Settings",
                     juce::Desktop::getInstance().getDefaultLookAndFeel()
                         .findColour(juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new SettingsComponent(processor), true);

    #if JUCE_IOS || JUCE_ANDROID
        setFullScreen(true);
    #else
        setResizable(false, false);
        centreWithSize(getWidth(), getHeight());
    #endif

    setVisible(true);
}

SettingsWindow::~SettingsWindow()
{
}

void SettingsWindow::closeButtonPressed()
{
    // Just hide the window, don't delete it
    setVisible(false);
}
