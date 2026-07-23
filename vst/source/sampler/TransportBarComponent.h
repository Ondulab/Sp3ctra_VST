#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../midi/MidiLearnAttachment.h"
#include <vector>

class Sp3ctraAudioProcessor;

// ─────────────────────────────────────────────────────────────────────────────
// IconTextButton — Borderless TextButton with a centred juce::Path icon.
// Path coordinates are normalised [0,1]; the icon is scaled to 60 % of the
// shortest button dimension and centred both horizontally and vertically.
// ─────────────────────────────────────────────────────────────────────────────
class IconTextButton : public juce::TextButton
{
public:
    IconTextButton() = default;
    explicit IconTextButton(const juce::String& t) : juce::TextButton(t) {}

    void setIconPath(const juce::Path& p) { iconPath = p; hasIcon = true; repaint(); }

    void paintButton(juce::Graphics& g, bool isHighlighted, bool isDown) override;

private:
    juce::Path iconPath;
    bool       hasIcon = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IconTextButton)
};

// ─────────────────────────────────────────────────────────────────────────────
// TransportBarComponent
//
// Full-width bar at the bottom of the sampler page's sequencer section.
// Bound to ONE sampler engine's sequencer bank (fsEngineParam ids):
// Seq BPM / NumSteps / Loop / DawSync + the SeqTransport choice param that
// Play/hold/stop drive (DAW-automatable/MIDI-mappable); the processor relays
// it to that engine's FrameSequencer. Steps is a draggable value bar covering
// the full 2..16 param range. 200 ms Timer disables BPM slider when DAW sync
// is active.
// ─────────────────────────────────────────────────────────────────────────────
class TransportBarComponent : public juce::Component,
                              private juce::Timer
{
public:
    explicit TransportBarComponent(Sp3ctraAudioProcessor& proc);
    ~TransportBarComponent() override;

    /** Rebind every control to sampler engine @p i's sequencer bank. */
    void setSamplerIndex(int i);

    void paint(juce::Graphics& g) override;
    void resized() override;

    // Bar heights (single source of truth for requiredHeight + page layout).
    static constexpr int kOneRowH = 44;
    static constexpr int kTwoRowH = 76;

    /** Width below which the bar wraps onto two rows (Steps / Loop / DAW Sync
     *  move to the second row instead of being clipped past the right edge). */
    static int singleRowMinWidth() noexcept;
    /** Height the bar needs at the given width — one row or two. The host page
     *  must reserve this before laying the bar out. */
    static int requiredHeight(int width) noexcept;

private:
    void timerCallback() override;
    void updateTransportButtons();   ///< Refreshes play/hold/stop highlight state.
    void requestTransport(int mode); ///< Writes this engine's SeqTransport param.
    void rebindAttachments();        ///< (Re)creates attachments for samplerIndex_.

    Sp3ctraAudioProcessor& processor;
    int samplerIndex_ = 0;   // engine whose sequencer bank the bar edits

    // ── Sequencer play / hold / stop icons ───────────────────────────────────
    IconTextButton     seqPlayBtn;   // ▶ drawn as path
    IconTextButton     seqHoldBtn;   // ⏸ drawn as path (two vertical bars)
    IconTextButton     seqStopBtn;   // ■ drawn as path

    // ── BPM ───────────────────────────────────────────────────────────────────
    juce::Label        bpmLabel         { {}, "BPM" };
    juce::Slider       bpmSlider;

    // ── Steps ─────────────────────────────────────────────────────────────────
    juce::Label        stepsLabel       { {}, "Steps" };
    juce::Slider       stepsSlider;

    // ── Loop / DAW sync ───────────────────────────────────────────────────────
    juce::ToggleButton loopToggle       { "Loop" };
    juce::ToggleButton dawSyncToggle    { "DAW Sync" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bpmAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> stepsAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> dawSyncAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportBarComponent)
};
