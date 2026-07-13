#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../UITheme.h"
#include "SlotGridComponent.h"
#include "SlotEditorComponent.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Master container for the LuxSampler sampler page.
 *
 * Layout — four full-width zones stacked vertically:
 *   1. SlotGridComponent     (h=66,  fixed)  — 12 slot cells (sample bank)
 *   2. SlotEditorComponent   (h=210, fixed)  — edit panel:
 *        left  (~63 %) : REC/PLAY/CLEAR buttons + large timeline
 *        right (~37 %) : Speed / Loop / Resume controls
 *   3. Session toolbar       (NEW / SAVE / LOAD session)
 *
 * The step sequencer now lives in its own SEQUENCER module page.
 * Manages selectedSlot state shared between SlotGrid and SlotEditor.
 */
class SamplerPageComponent : public juce::Component
{
public:
    explicit SamplerPageComponent(Sp3ctraAudioProcessor& proc);
    ~SamplerPageComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // ── Layout constants (single source of truth for resized() + kPreferredH) ──
    static constexpr int kGridH = SlotGridComponent::kPreferredH; // bank tiles + per-bank mixer
    static constexpr int kEditH = 500;  // slot editor: 2 param cols + image + EQ

    // Natural height — must match the layout in resized(): pad + grid + gap +
    // editor + gap + session toolbar + pad.  PluginEditor uses this to size the
    // zone-3 viewport content so the whole page scrolls into view at min size.
    static constexpr int kPreferredH = Sp3ctraTheme::kPad + kGridH + Sp3ctraTheme::kGap
                                     + kEditH + Sp3ctraTheme::kGap + Sp3ctraTheme::kControlH
                                     + Sp3ctraTheme::kPad;

    /** Bind this page (grid + editor + session I/O) to engine 0 (A) or 1 (B). */
    void setSamplerIndex(int i)
    {
        samplerIndex_ = i;
        slotGrid.setSamplerIndex(i);
        slotEditor.setSamplerIndex(i);
    }

private:
    void onSlotSelected(int idx);

    Sp3ctraAudioProcessor& processor;
    int  samplerIndex_ = 0;   // 0 = engine A, 1 = engine B

    SlotGridComponent     slotGrid;
    SlotEditorComponent   slotEditor;

    // ── Session toolbar ───────────────────────────────────────────────────────
    juce::TextButton newSessionBtn  { "NEW SESSION"  };
    juce::TextButton saveSessionBtn { "SAVE SESSION" };
    juce::TextButton loadSessionBtn { "LOAD SESSION" };

    std::unique_ptr<juce::FileChooser> fileChooser;

    // ── Session helpers (Non-RT, message thread only) ─────────────────────────
    void doSaveSession(const juce::File& sessionFile);
    /** @p isAutoRestore true when triggered by the startup auto-reload: the
     *  slot params / sequencer pattern freshly restored from the DAW state
     *  (newer than the session's copies) are then preserved. */
    void doLoadSession(const juce::File& sessionFile, bool isAutoRestore = false);

    /** Path of the last loaded or saved session.
     *  When set, SAVE SESSION writes directly to this file (no file dialog). */
    juce::File currentSessionFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerPageComponent)
};
