/**
 * @file ImagePageComponent.cpp
 * @brief Image Pipeline Page — 3-tab container implementation.
 */
#include "ImagePageComponent.h"

// ── Colours for sub-tab highlights — sourced from theme tokens ──────────────
static const juce::Colour kSourcesColour (Sp3ctraTheme::kColSubTabAccentSrc);
static const juce::Colour kLuxStralColour(Sp3ctraTheme::kColSubTabAccentLux);
static const juce::Colour kLuxSynthColour(Sp3ctraTheme::kColSubTabAccentSyn);

// ============================================================================
ImagePageComponent::ImagePageComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc)
{
    // Create sub-tab content components
    sourcesTab  = std::make_unique<SourcesTabComponent>(proc);
    luxstralTab = std::make_unique<LuxStralTabComponent>(proc);
    luxsynthTab = std::make_unique<LuxSynthTabComponent>(proc);

    // Wire node-click callbacks to our handler
    sourcesTab->onNodeClicked  = [this](VisualizerMode m) { handleNodeClicked(m); };
    luxstralTab->onNodeClicked = [this](VisualizerMode m) { handleNodeClicked(m); };
    luxsynthTab->onNodeClicked = [this](VisualizerMode m) { handleNodeClicked(m); };

    addAndMakeVisible(sourcesTab.get());
    addChildComponent(luxstralTab.get());   // hidden initially
    addChildComponent(luxsynthTab.get());   // hidden initially

    // Tab buttons — marked as tabs so LookAndFeel renders transparent background
    sourcesBtn.getProperties().set("isTab", true);
    luxstralBtn.getProperties().set("isTab", true);
    luxsynthBtn.getProperties().set("isTab", true);
    addAndMakeVisible(sourcesBtn);
    addAndMakeVisible(luxstralBtn);
    addAndMakeVisible(luxsynthBtn);

    sourcesBtn.onClick  = [this] { switchSubTab(SubTab::Sources);  };
    luxstralBtn.onClick = [this] { switchSubTab(SubTab::LuxStral); };
    luxsynthBtn.onClick = [this] { switchSubTab(SubTab::LuxSynth); };

    // Initial state
    switchSubTab(SubTab::Sources);
}

// ============================================================================
void ImagePageComponent::switchSubTab(SubTab tab)
{
    currentSubTab = tab;

    sourcesTab->setVisible (tab == SubTab::Sources);
    luxstralTab->setVisible(tab == SubTab::LuxStral);
    luxsynthTab->setVisible(tab == SubTab::LuxSynth);

    // Update button colours — active tabs get accent tint, inactive use theme tokens
    auto setActive = [](juce::TextButton& btn, bool active, juce::Colour accent)
    {
        if (active)
        {
            btn.setColour(juce::TextButton::buttonColourId,
                          accent.withAlpha(0.18f));
            btn.setColour(juce::TextButton::textColourOffId, accent);
        }
        else
        {
            btn.setColour(juce::TextButton::buttonColourId,
                          juce::Colour(Sp3ctraTheme::kColSubTabInactiveBg));
            btn.setColour(juce::TextButton::textColourOffId,
                          juce::Colour(Sp3ctraTheme::kColSubTabInactiveText));
        }
    };

    setActive(sourcesBtn,  tab == SubTab::Sources,  kSourcesColour);
    setActive(luxstralBtn, tab == SubTab::LuxStral, kLuxStralColour);
    setActive(luxsynthBtn, tab == SubTab::LuxSynth, kLuxSynthColour);

    repaint();
}

// ============================================================================
void ImagePageComponent::handleNodeClicked(VisualizerMode m)
{
    // Update all tabs so only the clicked node is highlighted
    sourcesTab->setActiveMode(m);
    luxstralTab->setActiveMode(m);
    luxsynthTab->setActiveMode(m);

    // Propagate to parent (PluginEditor) for CisVisualizer update
    if (onVisualizerModeChanged)
        onVisualizerModeChanged(m);
}

// ============================================================================
void ImagePageComponent::setActiveVisualizerMode(VisualizerMode m)
{
    sourcesTab->setActiveMode(m);
    luxstralTab->setActiveMode(m);
    luxsynthTab->setActiveMode(m);
}

// ============================================================================
void ImagePageComponent::paint(juce::Graphics& g)
{
    // Full tab bar background
    g.setColour(juce::Colour(Sp3ctraTheme::kColSubTabBarBg));
    g.fillRect(0, 0, getWidth(), kSubTabH);

    // Bottom separator below entire sub-tab bar
    g.setColour(juce::Colour(Sp3ctraTheme::kColBorder).withAlpha(0.5f));
    g.fillRect(0, kSubTabH, getWidth(), 1);

    // Determine active accent colour
    juce::Colour accent;
    switch (currentSubTab)
    {
        case SubTab::Sources:  accent = kSourcesColour;  break;
        case SubTab::LuxStral: accent = kLuxStralColour; break;
        case SubTab::LuxSynth: accent = kLuxSynthColour; break;
    }

    // Draw each sub-tab background + accent behind the buttons
    const juce::TextButton* btns[]    = { &sourcesBtn, &luxstralBtn, &luxsynthBtn };
    const SubTab             subIds[] = { SubTab::Sources, SubTab::LuxStral, SubTab::LuxSynth };
    const juce::Colour       accents[] = { kSourcesColour, kLuxStralColour, kLuxSynthColour };

    for (int i = 0; i < 3; ++i)
    {
        const bool active = (currentSubTab == subIds[i]);
        const auto tbr = btns[i]->getBounds().toFloat();

        if (active)
        {
            // Active sub-tab background — rounded top, flat bottom
            juce::Path tabPath;
            const float r = Sp3ctraTheme::kTabCornerR;
            tabPath.addRoundedRectangle(tbr.getX(), tbr.getY(),
                                        tbr.getWidth(), tbr.getHeight() + r,
                                        r, r, true, true, false, false);

            // Tinted background with accent colour
            g.setColour(accents[i].withAlpha(0.15f));
            g.fillPath(tabPath);

            // Subtle side/top border glow in accent colour
            g.setColour(accents[i].withAlpha(0.3f));
            g.strokePath(tabPath, juce::PathStrokeType(1.0f));

            // Accent underline at the bottom of the active tab
            g.setColour(accents[i]);
            g.fillRect(tbr.getX(),
                       tbr.getBottom() - (float)Sp3ctraTheme::kTabUnderlineH,
                       tbr.getWidth(),
                       (float)Sp3ctraTheme::kTabUnderlineH);
        }
        else
        {
            // Inactive sub-tab — flat dark
            g.setColour(juce::Colour(Sp3ctraTheme::kColSubTabInactiveBg));
            g.fillRect(tbr);

            // Very subtle bottom border
            g.setColour(juce::Colour(Sp3ctraTheme::kColTabBorderInactive));
            g.fillRect(tbr.getX(), tbr.getBottom() - 1.f, tbr.getWidth(), 1.f);
        }
    }
}

// ============================================================================
void ImagePageComponent::resized()
{
    auto bounds = getLocalBounds();
    const int w = bounds.getWidth();
    const int tabW = w / 3;

    // Tab buttons
    sourcesBtn.setBounds (0,        0, tabW, kSubTabH);
    luxstralBtn.setBounds(tabW,     0, tabW, kSubTabH);
    luxsynthBtn.setBounds(tabW * 2, 0, w - tabW * 2, kSubTabH);

    // Content area — below the tab bar
    auto contentBounds = bounds.withTrimmedTop(kSubTabH + 2);
    sourcesTab->setBounds(contentBounds);
    luxstralTab->setBounds(contentBounds);
    luxsynthTab->setBounds(contentBounds);
}
