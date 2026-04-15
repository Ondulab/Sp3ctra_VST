/**
 * @file ImagePageComponent.h
 * @brief Image Pipeline Page — 3-tab container (Sources / LuxStral / LuxSynth).
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │  [ SOURCES ]  [ LUXSTRAL ]  [ LUXSYNTH ]   ← sub-tabs          │
 * ├──────────────────────────────────────────────────────────────────┤
 * │                                                                  │
 * │   Active tab content (pipeline, controls, clickable nodes)      │
 * │                                                                  │
 * └──────────────────────────────────────────────────────────────────┘
 *
 * Each sub-tab contains clickable pipeline nodes. Clicking a node fires
 * onVisualizerModeChanged so the parent (PluginEditor) can update the
 * CisVisualizerComponent.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "VisualizerMode.h"
#include "SourcesTabComponent.h"
#include "LuxStralTabComponent.h"
#include "LuxSynthTabComponent.h"
#include <functional>

class ImagePageComponent : public juce::Component
{
public:
    explicit ImagePageComponent(Sp3ctraAudioProcessor& proc);
    ~ImagePageComponent() override = default;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Callback fired when a pipeline node is clicked in any sub-tab. */
    std::function<void(VisualizerMode)> onVisualizerModeChanged;

    /** Set the active visualizer mode (highlights the correct node). */
    void setActiveVisualizerMode(VisualizerMode m);

private:
    enum class SubTab { Sources, LuxStral, LuxSynth };

    void switchSubTab(SubTab tab);
    void handleNodeClicked(VisualizerMode m);

    Sp3ctraAudioProcessor& processor;

    // ── Sub-tab state ─────────────────────────────────────────────────────────
    SubTab currentSubTab { SubTab::Sources };

    // ── Tab buttons ───────────────────────────────────────────────────────────
    static constexpr int kSubTabH = 28;
    juce::TextButton sourcesBtn  { "SOURCES" };
    juce::TextButton luxstralBtn { "LUXSTRAL" };
    juce::TextButton luxsynthBtn { "LUXSYNTH" };

    // ── Tab content components ────────────────────────────────────────────────
    std::unique_ptr<SourcesTabComponent>   sourcesTab;
    std::unique_ptr<LuxStralTabComponent>  luxstralTab;
    std::unique_ptr<LuxSynthTabComponent>  luxsynthTab;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImagePageComponent)
};
