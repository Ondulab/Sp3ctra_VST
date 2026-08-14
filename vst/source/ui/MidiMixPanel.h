#pragma once

#include "ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "Sp3ctraBarSlider.h"
#include "../processing/midi_tap.h"
#include "../licensing/ActivationDialog.h"   // LicenseGate::blockIfDemo
#include "../session/MachinePrefs.h"         // MIDI OUT destination is machine-scoped
#include <functional>
#include <memory>
#include <vector>

/**
 * @brief Right-band MIDI MIX strip — the MASTER of every MIDI TAP probe.
 *
 * Owns what the probes must AGREE on: the REC transport with its common t0
 * (so N takes land on one DAW timeline), the tempo, and the real-time
 * destination. Each probe keeps what "counts as a note" on its own zone-3 page.
 *
 * The section only EXISTS while at least one probe is patched: hasProbes() is
 * derived from processor.activeMidiTapSlots() in refreshActiveSlots(), and the
 * editor gives it 0 px and hides it otherwise. Row anatomy mirrors
 * VideoMixerComponent (labels painted, not juce::Labels) and the chrome mirrors
 * AudioMixPanel.
 */
class MidiMixPanel : public juce::Component,
                     private juce::Timer
{
public:
    static constexpr int kHeaderH = 24;
    static constexpr int kMasterH = 4 * 24 + 3 * 4;   // REC / BPM / GRID / OUT
    static constexpr int kRowH    = 24;
    static constexpr int kRowGap  = 4;
    static constexpr int kPad     = 6;
    static constexpr int kLabelW  = 46;               // painted master-row labels

    /** Height for the CURRENT row count — the editor uses it to split zone 4. */
    int preferredHeight() const noexcept
    {
        if (voices_.empty()) return 0;
        return kHeaderH + kMasterH + kPad
             + (int) voices_.size() * (kRowH + kRowGap) + kPad;
    }

    bool hasProbes() const noexcept { return ! voices_.empty(); }

    /** Fired after the collapse state changed (editor relayouts + persists). */
    std::function<void(bool)> onCollapseToggled;
    /** Fired when a row is clicked — the editor selects that probe's block. */
    std::function<void(int)>  onProbeSelected;

    explicit MidiMixPanel(Sp3ctraAudioProcessor& p) : processor_(p)
    {
        addAndMakeVisible(recBtn_);
        recBtn_.onClick = [this] { toggleRecording(); };

            // No " BPM" suffix: the painted row label carries the unit instead.
        tempoSlider_.setDoubleClickReturnValue(true, 120.0);   // cycle centre
        addAndMakeVisible(tempoSlider_);
        tempoAtt_ = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor_.getAPVTS(), "midiTempo", tempoSlider_);
        tempoLearn_ = std::make_unique<MidiLearnAttachment>(
            processor_.getMidiMap(), tempoSlider_, "midiTempo");

        addAndMakeVisible(destCombo_);
        destCombo_.onChange = [this] { applyDestination(); };
        rebuildDestinations();

        // Write-time grid. FILE ONLY — the port and bus sinks stay unquantized.
        for (const char* g : { "Off", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4" })
            gridCombo_.addItem(g, gridCombo_.getNumItems() + 1);
        gridCombo_.setTooltip("Rhythmic grid applied when the .mid is written.\n"
                              "MuseScore 4 has no MIDI import panel, so an "
                              "unquantized file gets whatever the reader invents.");
        addAndMakeVisible(gridCombo_);
        gridAtt_ = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            processor_.getAPVTS(), "midiQuantize", gridCombo_);

        // A TextButton, not a ToggleButton: a bare tick box next to a combo read
        // as decoration — the label has to be ON the control.
        addAndMakeVisible(busBtn_);
        busBtn_.setButtonText("BUS");
        busBtn_.setClickingTogglesState(true);
        busBtn_.setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.85f));
        busBtn_.setTooltip("Also send the extracted notes to the plugin's MIDI "
                           "output bus (towards the host).\n"
                           "Off by default: in a DAW a track routed back to this "
                           "instance would feed PITCH/MASK and self-oscillate.");
        busAtt_ = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor_.getAPVTS(), "midiBusEnable", busBtn_);

        refreshActiveSlots();
        startTimerHz(10);
    }

    ~MidiMixPanel() override { stopTimer(); }

    /** Rebuild the rows from processor.activeMidiTapSlots(). Cheap no-op when
     *  the slot list is unchanged (same contract as VideoMixerComponent). */
    void refreshActiveSlots()
    {
        auto slots = processor_.activeMidiTapSlots();
        if (slots == activeSlots_) return;
        activeSlots_ = slots;
        rebuildStrip();
    }

    void setCollapsed(bool shouldCollapse, bool notify)
    {
        if (collapsed_ == shouldCollapse) { resized(); return; }
        collapsed_ = shouldCollapse;
        resized();
        if (notify && onCollapseToggled) onCollapseToggled(collapsed_);
    }
    bool isCollapsed() const noexcept { return collapsed_; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0c0c10));

        g.setColour(kAccent);
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("MIDI MIX", 8, 0, getWidth() - 16, kHeaderH,
                   juce::Justification::centredLeft, false);
        if (collapsed_) return;

        // Master rows: painted labels + the take readout.
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        if (processor_.isMidiCapturing())
        {
            const double s = processor_.midiCaptureElapsed();
            g.setColour(juce::Colour(0xffff3b30));
            g.drawText(juce::String::formatted("%02d:%04.1f", (int) (s / 60.0),
                                               s - 60.0 * (double) (int) (s / 60.0)),
                       readoutArea_, juce::Justification::centredLeft, false);
            g.setColour(kAccent.withAlpha(0.8f));
            g.drawText(juce::String(processor_.midiCaptureNoteCount()) + " notes",
                       readoutArea_, juce::Justification::centredRight, false);
        }
        else
        {
            g.setColour(juce::Colour(0xff9aa6ba));
            const int armed = armedCount();
            g.drawText(armed == 0 ? juce::String("no probe armed")
                                  : juce::String(armed) + (armed > 1 ? " probes armed"
                                                                     : " probe armed"),
                       readoutArea_, juce::Justification::centredLeft, false);
        }

        auto drawRowLabel = [&](const juce::String& t, juce::Rectangle<int> box)
        {
            if (box.isEmpty()) return;
            g.setColour(juce::Colour(0xff9aa6ba));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText(t, box, juce::Justification::centredLeft, false);
        };
        drawRowLabel("BPM",  tempoLabelArea_);
        drawRowLabel("GRID", gridLabelArea_);
        drawRowLabel("OUT",  destLabelArea_);

        if (voices_.empty()) return;

        g.setColour(juce::Colour(0xff14141c));
        g.fillRoundedRectangle(stripArea_.toFloat(), 4.0f);

        auto strip = stripArea_.reduced(kPad, kPad / 2);
        for (auto& v : voices_)
        {
            auto row = strip.removeFromTop(kRowH);
            strip.removeFromTop(kRowGap);
            auto labelBox = row.removeFromLeft(kLabelW).reduced(2, 0);

            // Activity: ● notes flowing / ◐ enabled-idle / ○ disabled — the
            // same three states as the rack LED, read from the RT instance.
            auto* st = midi_tap_instance(v->slot);
            const bool on  = st != nullptr && st->config.enabled != 0;
            const bool hot = on && v->tickMoved(st != nullptr
                                                ? midi_tap_active_ticks(st) : 0u);
            g.setColour(! on ? kAccent.withAlpha(0.25f)
                             : (hot ? kAccent : kAccent.withAlpha(0.6f)));
            g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
            g.drawText("MIDI " + juce::String(v->slot + 1), labelBox,
                       juce::Justification::centredLeft, false);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        // Rows stop stretching with a very wide zone — every stored rect
        // (master rows, readout, strip) derives from this capped width.
        r.setWidth(juce::jmin(r.getWidth(), Sp3ctraTheme::kMaxContentW));
        auto header = r.removeFromTop(kHeaderH);
        juce::ignoreUnused(header);

        const bool showFull = ! collapsed_;
        recBtn_     .setVisible(true);   // transport stays reachable when folded
        tempoSlider_.setVisible(showFull);
        gridCombo_  .setVisible(showFull);
        destCombo_  .setVisible(showFull);
        busBtn_     .setVisible(showFull);
        for (auto& v : voices_)
        {
            v->arm  .setVisible(showFull);
            v->level.setVisible(showFull);
            v->chan .setVisible(showFull);
        }

        if (collapsed_)
        {
            recBtn_.setBounds(4, 2, 44, 20);
            stripArea_ = readoutArea_ = {};
            tempoLabelArea_ = gridLabelArea_ = destLabelArea_ = {};
            return;
        }

        auto master = r.removeFromTop(kMasterH).reduced(kPad, 2);

        // Row 1 — transport. 44 px so "REC" actually fits (26 px rendered "...").
        auto row1 = master.removeFromTop(24);
        recBtn_.setBounds(row1.removeFromLeft(44).reduced(0, 1));
        row1.removeFromLeft(6);
        readoutArea_ = row1;

        // Row 2 — tempo, with a painted unit label instead of a truncated suffix.
        master.removeFromTop(4);
        auto row2 = master.removeFromTop(24);
        tempoLabelArea_ = row2.removeFromLeft(kLabelW);
        tempoSlider_.setBounds(row2);

        // Row 3 — write-time grid (file only).
        master.removeFromTop(4);
        auto row3 = master.removeFromTop(24);
        gridLabelArea_ = row3.removeFromLeft(kLabelW);
        gridCombo_.setBounds(row3);

        // Row 4 — real-time destination + the plugin-bus toggle.
        master.removeFromTop(4);
        auto row4 = master.removeFromTop(24);
        destLabelArea_ = row4.removeFromLeft(kLabelW);
        busBtn_.setBounds(row4.removeFromRight(44).reduced(0, 1));
        row4.removeFromRight(6);
        destCombo_.setBounds(row4);

        r.removeFromTop(kPad);
        stripArea_ = r.removeFromTop(juce::jmax(0, (int) voices_.size() * (kRowH + kRowGap)));

        auto strip = stripArea_.reduced(kPad, kPad / 2);
        for (auto& v : voices_)
        {
            auto row = strip.removeFromTop(kRowH);
            strip.removeFromTop(kRowGap);
            row.removeFromLeft(kLabelW);                  // painted "MIDI n"
            // 54 px truncated "Ch 16" to "..." once the dropdown arrow was
            // accounted for; the level slider has room to spare.
            v->chan .setBounds(row.removeFromRight(66).reduced(0, 1));
            row.removeFromRight(4);
            v->arm  .setBounds(row.removeFromLeft(40).reduced(0, 2));
            row.removeFromLeft(4);
            v->level.setBounds(row.reduced(0, 2));
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (collapsed_ || ! stripArea_.contains(e.getPosition())) return;
        const int idx = (e.getPosition().y - stripArea_.getY() - kPad / 2)
                      / (kRowH + kRowGap);
        if (idx >= 0 && idx < (int) voices_.size() && onProbeSelected)
            onProbeSelected(voices_[(size_t) idx]->slot);
    }

private:
    static const juce::Colour kAccent;

    struct Voice
    {
        int slot { -1 };
        juce::TextButton   arm;
        Sp3ctraBarSlider   level;
        juce::ComboBox     chan;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   armAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   levelAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> chanAtt;
        std::unique_ptr<MidiLearnAttachment> armLearn, levelLearn, chanLearn;

        uint32_t lastTicks { 0 };
        bool     seeded    { false };
        /** True when the probe emitted since the previous UI refresh. */
        bool tickMoved(uint32_t t)
        {
            if (! seeded) { lastTicks = t; seeded = true; return false; }
            const bool moved = (t != lastTicks);
            lastTicks = t;
            return moved;
        }
    };

    void rebuildStrip()
    {
        voices_.clear();
        auto& apvts = processor_.getAPVTS();
        for (int slot : activeSlots_)
        {
            auto v = std::make_unique<Voice>();
            v->slot = slot;

            v->arm.setButtonText("ARM");
            v->arm.setClickingTogglesState(true);
            v->arm.setColour(juce::TextButton::buttonOnColourId,
                             juce::Colour(0xffff3b30).withAlpha(0.85f));
            v->arm.setTooltip("Armed: this probe's notes reach the master REC "
                              "and the real-time destination.");
            addAndMakeVisible(v->arm);

            v->level.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            v->level.setRange(0.0, 1.0, 0.01);
            addAndMakeVisible(v->level);

            for (int c = 1; c <= 16; ++c) v->chan.addItem("Ch " + juce::String(c), c);
            addAndMakeVisible(v->chan);

            using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
            using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
            using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
            v->armAtt   = std::make_unique<BA>(apvts, mtParam(slot, "arm"),     v->arm);
            v->levelAtt = std::make_unique<SA>(apvts, mtParam(slot, "level"),   v->level);
            v->chanAtt  = std::make_unique<CA>(apvts, mtParam(slot, "channel"), v->chan);
            v->armLearn   = std::make_unique<MidiLearnAttachment>(
                                processor_.getMidiMap(), v->arm,   mtParam(slot, "arm"));
            v->levelLearn = std::make_unique<MidiLearnAttachment>(
                                processor_.getMidiMap(), v->level, mtParam(slot, "level"));
            v->chanLearn  = std::make_unique<MidiLearnAttachment>(
                                processor_.getMidiMap(), v->chan,  mtParam(slot, "channel"));
            voices_.push_back(std::move(v));
        }
        resized();
        repaint();
    }

    void rebuildDestinations()
    {
        const juce::String current = processor_.midiTapDestination();
        destCombo_.clear(juce::dontSendNotification);
        destCombo_.addItem("No port", 1);
        destCombo_.addItem("Virtual port", 2);
        int id = 3;
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
        {
            // Never offer our OWN virtual port as a destination: selecting it
            // would close the feedback loop the channel default guards against.
            if (d.name.startsWith("Sp3ctra ")) continue;
            destCombo_.addItem(d.name, id++);
        }
        int sel = 1;
        if (current == "Virtual") sel = 2;
        else if (current.isNotEmpty())
            for (int i = 0; i < destCombo_.getNumItems(); ++i)
                if (destCombo_.getItemText(i) == current) sel = destCombo_.getItemId(i);
        destCombo_.setSelectedId(sel, juce::dontSendNotification);
    }

    void applyDestination()
    {
        const int id = destCombo_.getSelectedId();
        const juce::String name = (id == 1) ? juce::String()
                                : (id == 2) ? juce::String("Virtual")
                                            : destCombo_.getText();
        processor_.setMidiTapDestination(name);
        // Machine-scoped: the port belongs to this computer, not the session.
        // (PluginEditor's restore still reads the legacy in-state property as
        // a fallback for sessions saved before the scoping.)
        MachinePrefs::file().setValue("midiTapDest", name);
        const auto err = processor_.midiTapLastError();
        if (err.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "MIDI output", err, "OK");
    }

    void toggleRecording()
    {
        if (processor_.isMidiCapturing())
        {
            processor_.stopMidiCapture();
            recBtn_.setToggleState(false, juce::dontSendNotification);
            const auto err = processor_.midiTapLastError();
            if (err.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon, "MIDI recording", err, "OK");
            repaint();
            return;
        }

        if (LicenseGate::blockIfDemo(this, "Record MIDI MIX")) return;

        const auto stamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
        auto* sessions = processor_.sessions();
        juce::File dir;
        if (sessions != nullptr && sessions->isStandalone() && ! sessions->isGlobal())
        {
            dir = sessions->exportsDir();
            dir.createDirectory();
        }
        else
        {
            const juce::File fallback =
                juce::File::getSpecialLocation(juce::File::userMusicDirectory);
            dir = (sessions != nullptr)
                ? sessions->startDirFor(PathKeys::midiCapture, fallback, true)
                : fallback;
        }

        juce::String err;
        if (! processor_.startMidiCapture(dir, "Sp3ctra_" + stamp, err))
        {
            recBtn_.setToggleState(false, juce::dontSendNotification);
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "MIDI recording",
                err.isNotEmpty() ? err : juce::String("Could not start recording."),
                "OK");
            return;
        }
        if (sessions != nullptr)
            sessions->rememberDirFor(PathKeys::midiCapture, dir.getChildFile("x.mid"));
        recBtn_.setToggleState(true, juce::dontSendNotification);
        repaint();
    }

    void timerCallback() override
    {
        if (! collapsed_) repaint(stripArea_.getUnion(readoutArea_));
        recBtn_.setToggleState(processor_.isMidiCapturing(), juce::dontSendNotification);
    }

    Sp3ctraAudioProcessor& processor_;
    std::vector<std::unique_ptr<Voice>> voices_;
    std::vector<int> activeSlots_;

    /** Probes that would actually write a take (enabled AND armed). */
    int armedCount() const
    {
        int n = 0;
        auto& apvts = processor_.getAPVTS();
        for (const auto& v : voices_)
        {
            auto* a = apvts.getRawParameterValue(mtParam(v->slot, "arm"));
            auto* e = apvts.getRawParameterValue(mtParam(v->slot, "enabled"));
            if (a != nullptr && e != nullptr && a->load() >= 0.5f && e->load() >= 0.5f)
                ++n;
        }
        return n;
    }

    juce::TextButton recBtn_ { "REC" };
    Sp3ctraBarSlider tempoSlider_;
    juce::ComboBox   destCombo_;
    juce::ComboBox   gridCombo_;
    juce::TextButton busBtn_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tempoAtt_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   busAtt_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> gridAtt_;
    std::unique_ptr<MidiLearnAttachment> tempoLearn_;

    juce::Rectangle<int> stripArea_, readoutArea_;
    juce::Rectangle<int> tempoLabelArea_, gridLabelArea_, destLabelArea_;
    bool collapsed_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiMixPanel)
};

inline const juce::Colour MidiMixPanel::kAccent { moduleColour(ModuleType::MidiTap) };
