/**
 * @file ScoreTransportBinding.h
 * @brief P7 — binds a generator page's transport widgets to ONE instance bank.
 *
 * The four SCORE-family generator pages (SCORE / TIMBRE / MIDI SCORE / VOICE)
 * are single components that VIEW the selected instance. Since P7 each instance
 * owns its own play / speed / loop / reverse bank (scoreXportParam, keyed by the
 * player-pool slot), so switching the selected module must re-point the page's
 * APVTS attachments AND its MIDI-Learn targets at the new bank — exactly what
 * the pooled-insert editors already do on setSlot().
 *
 * One object per page: call rebind() from the constructor and again from
 * setScoreSlot(). playParamId() gives the id the page's own Play button and its
 * transport mirror must drive (Play stays a hand-driven command — it has no
 * attachment because the button is a toggle over the engine state).
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "../midi/MidiLearnAttachment.h"
#include "../ui/ModuleParamManifest.h"

class ScoreTransportBinding
{
public:
    // Declaring the deleted copy ctor below suppresses the implicit default one.
    ScoreTransportBinding() = default;

    /** @param type   the page's score-family module type
     *  @param slot   the SELECTED instance's player-pool slot (0..7)
     *  @param mm     MIDI mapping engine (right-click Learn targets)
     *  Attachments are destroyed before being rebuilt, so the widgets never
     *  see two live attachments at once. */
    void rebind(juce::AudioProcessorValueTreeState& apvts,
                MidiMappingEngine& mm,
                ModuleType type, int slot,
                juce::Button& playButton,
                juce::Button& loopButton,
                juce::Button& reverseButton,
                juce::Slider& speedSlider)
    {
        type_ = type;
        slot_ = juce::jlimit(0, 7, slot);

        loopAttach_.reset();
        reverseAttach_.reset();
        speedAttach_.reset();
        learnAtts_.clear();

        loopAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            apvts, paramId("Loop"), loopButton);
        reverseAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            apvts, paramId("Reverse"), reverseButton);
        speedAttach_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            apvts, paramId("Speed"), speedSlider);

        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, playButton,    paramId("Playing")));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, loopButton,    paramId("Loop")));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, reverseButton, paramId("Reverse")));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, speedSlider,   paramId("Speed")));
    }

    /** Id of the bound instance's PLAY command (the DAW-visible transport). */
    juce::String playParamId() const { return paramId("Playing"); }

    int slot() const noexcept { return slot_; }

private:
    juce::String paramId(const char* suffix) const
    { return scoreXportParam(type_, slot_, suffix); }

    ModuleType type_ { ModuleType::Score };
    int        slot_ { 0 };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttach_, reverseAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach_;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreTransportBinding)
};
