#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "settings/GeneralSettingsTab.h"
#include "settings/NetworkSettingsTab.h"
#include "settings/ImageSettingsTab.h"
#include "settings/LuxStralSettingsTab.h"
#include "settings/LuxSamplerSettingsTab.h"
#include "settings/LuxSynthSettingsTab.h"

//==============================================================================
/**
 * @brief Settings window for Sp3ctra VST parameters
 *
 * Organised in 5 tabs (ordered by pipeline stage):
 *   General      — Visualizer Mode, Log Level
 *   Network      — UDP configuration, Sensor DPI
 *   Image        — Image pipeline flags, stream opacities, advanced blob params
 *   LuxStral     — Additive synthesis parameters + StrokeForge waveform morphing
 *   LuxSampler — Slot configuration and MIDI mapping
 *
 * All changes are automatically saved via APVTS to DAW projects.
 */
class SettingsComponent : public juce::Component
{
public:
    SettingsComponent(Sp3ctraAudioProcessor& processor);
    ~SettingsComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // Tabbed interface
    juce::TabbedComponent tabbedComponent;

    // Tab content (owned by TabbedComponent — raw pointers intentional)
    GeneralSettingsTab*      generalTab;
    NetworkSettingsTab*      networkTab;
    ImageSettingsTab*        imageTab;
    LuxStralSettingsTab*     luxstralTab;
    LuxSynthSettingsTab*     luxsynthTab;
    LuxSamplerSettingsTab* luxSamplerTab;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsComponent)
};

//==============================================================================
/**
 * @brief Settings window wrapper
 *
 * A DocumentWindow that contains the SettingsComponent.
 * Can be shown/hidden without destroying the component.
 */
class SettingsWindow : public juce::DocumentWindow
{
public:
    SettingsWindow(Sp3ctraAudioProcessor& processor);
    ~SettingsWindow() override;

    void closeButtonPressed() override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsWindow)
};
