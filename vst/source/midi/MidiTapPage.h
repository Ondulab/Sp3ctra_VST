#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "MidiLearnAttachment.h"
#include "../ui/Sp3ctraBarSlider.h"
#include <memory>
#include <vector>

/**
 * @brief Contextual zone-3 PLAY panel for a selected MIDI TAP probe.
 *
 * Holds ONLY the "what counts as a note" controls — the timebase, the file and
 * the real-time destination belong to the MIDI MIX master strip (right band),
 * which is why this module has no SETUP face (blockHasSetup excludes it).
 *
 * Per-instance: setSlot(slot) rebinds every APVTS attachment to the bank
 * midiTap{slot}_* (see mtParam()). slot < 0 unbinds (blank controls).
 *
 * The layout is driven by a ROW TABLE rather than 20 hand-written members:
 * every row is {section, label, suffix, kind}, so adding a parameter is one
 * line here plus one in ModuleParamManifest.h's kMidiTap.
 */
class MidiTapPage : public juce::Component
{
public:
    /** Natural content height — DERIVED from the row table (one step per row,
     *  one extra step + gap per section header, mirroring paint/resized), so
     *  adding a row can never outgrow the zone-3 scroll range again (the
     *  hardcoded-700 bug: the OUTPUT section sat below the scrollable area). */
    static int preferredHeight()
    {
        static const int h = []
        {
            int rowCount = 0, sections = 0;
            const char* sec = nullptr;
            for (const auto& r : makeRows())
            {
                if (sec == nullptr || std::strcmp(sec, r.section) != 0)
                {
                    sec = r.section;
                    ++sections;
                }
                ++rowCount;
            }
            return kTop + sections * (kStep + kSecEx) + rowCount * kStep + kBottom;
        }();
        return h;
    }

    explicit MidiTapPage(Sp3ctraAudioProcessor& proc) : processor_(proc)
    {
        for (auto& r : rows_)
        {
            switch (r.kind)
            {
                case Kind::Slider:
                    r.slider = std::make_unique<Sp3ctraBarSlider>();
                    if (r.suffixText != nullptr)
                        r.slider->setTextValueSuffix(r.suffixText);
                    addAndMakeVisible(*r.slider);
                    break;

                case Kind::Combo:
                    r.combo = std::make_unique<juce::ComboBox>();
                    for (int i = 0; r.choices[i] != nullptr; ++i)
                        r.combo->addItem(r.choices[i], i + 1);
                    addAndMakeVisible(*r.combo);
                    break;

                case Kind::Toggle:
                    r.button = std::make_unique<juce::ToggleButton>();
                    addAndMakeVisible(*r.button);
                    break;
            }
        }
    }

    /** Bind every control to the MIDI TAP bank of `slot` (0..7), or unbind
     *  (slot < 0). Called by the editor when a MIDI TAP block is selected. */
    void setSlot(int slot)
    {
        slot_ = slot;
        rebind();
    }

    int slot() const noexcept { return slot_; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const Metrics M = metrics(getWidth());

        int y = kTop;
        const char* section = nullptr;
        for (const auto& r : rows_)
        {
            if (section == nullptr || std::strcmp(section, r.section) != 0)
            {
                section = r.section;
                g.setColour(juce::Colour(0xff66cc88u));
                g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
                g.drawText(section, kHP, y,
                           juce::jmin(180, getWidth() - 2 * kHP), kCH,
                           juce::Justification::centredLeft, true);
                y += kStep + kSecEx;
            }

            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.setColour(juce::Colour(Sp3ctraTheme::kColText));
            g.drawText(r.label, juce::Rectangle<int>(kHP, y, M.labelW, kCH),
                       juce::Justification::centredRight, true);
            y += kStep;
        }

        if (slot_ < 0)
        {
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.5f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText("Select a MIDI TAP block",
                       getLocalBounds().removeFromBottom(28),
                       juce::Justification::centred, true);
        }
    }

    void resized() override
    {
        const Metrics M = metrics(getWidth());

        int y = kTop;
        const char* section = nullptr;
        for (auto& r : rows_)
        {
            if (section == nullptr || std::strcmp(section, r.section) != 0)
            {
                section = r.section;
                y += kStep + kSecEx;
            }
            switch (r.kind)
            {
                case Kind::Slider:
                    r.slider->setBounds(M.ctrlX, y, M.ctrlW, kCH);
                    break;
                case Kind::Combo:
                    r.combo->setBounds(M.ctrlX, y, M.ctrlW, kCH);
                    break;
                case Kind::Toggle:
                    r.button->setBounds(M.ctrlX, y, M.ctrlW, kCH);
                    break;
            }
            y += kStep;
        }
    }

private:
    enum class Kind { Slider, Combo, Toggle };

    struct Row
    {
        const char* section;
        const char* label;
        const char* suffix;      ///< APVTS bank suffix (mtParam)
        Kind        kind;
        const char* suffixText;  ///< slider unit text, or nullptr
        const char* choices[5];  ///< combo items, nullptr-terminated

        std::unique_ptr<juce::Slider>       slider;
        std::unique_ptr<juce::ComboBox>     combo;
        std::unique_ptr<juce::ToggleButton> button;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   sAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> cAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   bAtt;
        std::unique_ptr<MidiLearnAttachment> learn;
    };

    static std::vector<Row> makeRows()
    {
        std::vector<Row> v;
        auto sl = [&v](const char* sec, const char* lbl, const char* sfx,
                       const char* unit = nullptr)
        { v.push_back(Row{ sec, lbl, sfx, Kind::Slider, unit, { nullptr }, {}, {}, {}, {}, {}, {}, {} }); };
        // (The Kind::Combo helper left with "backgroundMode" — the pole is
        // chain-owned now; the Combo machinery stays for the next choice row.)
        auto tg = [&v](const char* sec, const char* lbl, const char* sfx)
        { v.push_back(Row{ sec, lbl, sfx, Kind::Toggle, nullptr, { nullptr }, {}, {}, {}, {}, {}, {}, {} }); };

        // Three sections, split by DOMAIN (user request — the file/live divide
        // must be structural, not marginal notes):
        //   COMMON      — what counts as a note, and its velocity. Feeds the
        //                 takes AND the live stream identically.
        //   EXPORT .MID — the takes. They are ALWAYS black MIDI (dense);
        //                 these knobs set the recorded envelope's resolution.
        //                 (The live stream borrows them while Black MIDI/MPE
        //                 is on — dense is dense.)
        //   LIVE        — the real-time stream only: the Black MIDI toggle
        //                 and the classic transcription timing. The rest of
        //                 the live controls (OUT / MPE / Level / Channel)
        //                 live on the probe's MIDI MIX tile.
        // Brightness IS the signal (LuxStral's convention; the background
        // pole is chain-owned — rack header selector) — Mode/Source/
        // Relative/Smooth/PITCH remain retired.
        sl("COMMON", "Threshold",   "threshold");
        sl("COMMON", "Hysteresis",  "hysteresis");
        tg("COMMON", "Peak only",   "peakOnly");
        sl("COMMON", "Max poly",    "maxPoly");
        sl("COMMON", "Vel. span",   "velSpan");

        sl("EXPORT .MID", "Retrig",     "retrigMs",   " ms");
        sl("EXPORT .MID", "Vel. delta", "retrigDelta");

        tg("LIVE",   "Black MIDI",  "dense");
        sl("LIVE",   "Attack",      "attackMs",   " ms");
        sl("LIVE",   "Release",     "releaseMs",  " ms");
        sl("LIVE",   "Min length",  "minOnMs",    " ms");
        sl("LIVE",   "Max length",  "maxOnMs",    " ms");
        return v;
    }

    //── Geometry ──────────────────────────────────────────────────────────────
    static constexpr int kHP    = Sp3ctraTheme::kHPad;
    static constexpr int kLW    = Sp3ctraTheme::kLabelW;
    static constexpr int kGap   = Sp3ctraTheme::kGap;
    static constexpr int kCH    = Sp3ctraTheme::kControlH;
    static constexpr int kStep  = Sp3ctraTheme::kRowStep;
    static constexpr int kSecEx = 10;
    static constexpr int kTop   = 10;
    static constexpr int kBottom = 12;   // breathing room under the last row

    struct Metrics { int labelW, ctrlX, ctrlW; };

    Metrics metrics(int width) const
    {
        Metrics m;
        m.labelW = juce::jlimit(64, kLW, width / 3);
        m.ctrlX  = kHP + m.labelW + kGap;
        m.ctrlW  = juce::jmax(70, width - kHP - m.ctrlX);
        return m;
    }

    //── Per-slot attachment (re)binding ───────────────────────────────────────
    void rebind()
    {
        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;

        for (auto& r : rows_)
        {
            r.sAtt.reset(); r.cAtt.reset(); r.bAtt.reset(); r.learn.reset();
        }
        if (slot_ < 0) { repaint(); return; }

        auto& apvts = processor_.getAPVTS();
        for (auto& r : rows_)
        {
            const juce::String id = mtParam(slot_, r.suffix);
            switch (r.kind)
            {
                case Kind::Slider:
                    r.sAtt  = std::make_unique<SA>(apvts, id, *r.slider);
                    r.learn = std::make_unique<MidiLearnAttachment>(
                                  processor_.getMidiMap(), *r.slider, id);
                    break;
                case Kind::Combo:
                    r.cAtt  = std::make_unique<CA>(apvts, id, *r.combo);
                    r.learn = std::make_unique<MidiLearnAttachment>(
                                  processor_.getMidiMap(), *r.combo, id);
                    break;
                case Kind::Toggle:
                    r.bAtt  = std::make_unique<BA>(apvts, id, *r.button);
                    r.learn = std::make_unique<MidiLearnAttachment>(
                                  processor_.getMidiMap(), *r.button, id);
                    break;
            }
        }
        repaint();
    }

    Sp3ctraAudioProcessor& processor_;
    int                    slot_ { -1 };
    std::vector<Row>       rows_ { makeRows() };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiTapPage)
};
