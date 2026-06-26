#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "settings/SystemSettingsTab.h"

//==============================================================================
/**
 * @brief Settings window for Sp3ctra machine-level parameters (M5 / C9).
 *
 * Holds the System tab — Log level, LuxStral worker threads, detached video
 * window size. The UDP / Sensor-DPI network configuration moved to the SP3CTRA
 * source block's zone-3 SETUP face (so it is no longer duplicated here).
 *
 * Per-block musical settings (MIDI channel, octave, tuning, sampler slots,
 * bindings…) live in the zone-3 SETUP face of each block; the waterfall
 * display settings (invert / colour) live in the zone-4 toolbar.
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
    SystemSettingsTab*  systemTab;

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
