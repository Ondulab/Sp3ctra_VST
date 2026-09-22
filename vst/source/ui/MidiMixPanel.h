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
#include "../Sp3ctraDialog.h"
#include <functional>
#include <memory>
#include <vector>

/**
 * @brief Right-band MIDI MIX strip — the MASTER of every MIDI TAP probe.
 *
 * ONE button: REC captures every ENABLED probe into a faithful (black MIDI)
 * .mid — dense mode is forced for the take by the processor, the write is
 * never quantized, and the tempo is a fixed internal 120 (SMF timing is
 * absolute, the value is pure display convention — so there is nothing to
 * configure). The retired BPM / GRID / per-probe ARM controls are gone; their
 * params survive only for session compatibility. Each probe keeps what
 * "counts as a note" on its own zone-3 page.
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
    static constexpr int kMasterH = 2 * 24 + 1 * 4;   // REC / OUT
    static constexpr int kRowH    = 24;
    static constexpr int kRowGap  = 4;
    static constexpr int kPad     = 6;
    static constexpr int kLabelW  = 46;               // painted master-row labels
    /** Probe TILE: header row (chain label + OUT + MPE) over a control row
     *  (level + channel) — survives narrow bands where a single row could
     *  not fit five controls. */
    static constexpr int kTileH   = 54;
    static constexpr int kTileGap = 6;

    /** Height for the CURRENT tile count — the editor uses it to split zone 4. */
    int preferredHeight() const noexcept
    {
        if (voices_.empty()) return 0;
        return kHeaderH + kMasterH + kPad
             + (int) voices_.size() * (kTileH + kTileGap) + kPad;
    }

    bool hasProbes() const noexcept { return ! voices_.empty(); }

    /** Fired after the collapse state changed (editor relayouts + persists). */
    std::function<void(bool)> onCollapseToggled;
    /** Fired when a row is clicked — the editor selects that probe's block. */
    std::function<void(int)>  onProbeSelected;

    explicit MidiMixPanel(Sp3ctraAudioProcessor& p) : processor_(p)
    {
        chevron_.collapsed = &collapsed_;
        chevron_.onClick = [this] { setCollapsed(! collapsed_, true); };
        chevron_.setTooltip("Collapse / expand MIDI MIX");
        addAndMakeVisible(chevron_);

        addAndMakeVisible(recBtn_);
        recBtn_.onClick = [this] { toggleRecording(); };
        recBtn_.setColour(juce::TextButton::buttonOnColourId,
                          juce::Colour(0xffff3b30).withAlpha(0.85f));
        recBtn_.setTooltip("Record every enabled probe to a .mid take "
                           "(faithful black-MIDI capture, one shared timeline). "
                           "Press again to stop and write the file.");

        addAndMakeVisible(destCombo_);
        destCombo_.onChange = [this] { applyDestination(); };
        rebuildDestinations();

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

    /** Rebuild the rows from processor.activeMidiTapSlotChains(). Cheap no-op
     *  when the list is unchanged (same contract as VideoMixerComponent). */
    void refreshActiveSlots()
    {
        auto slots = processor_.activeMidiTapSlotChains();
        if (slots == activeSlots_)
        {
            // Same topology, but a chain RENAME can change the row labels —
            // refresh them (and the sinks' outward identity) in place.
            bool changed = false;
            for (auto& v : voices_)
            {
                auto label = processor_.midiTapLabel(v->slot);
                if (label != v->label) { v->label = std::move(label); changed = true; }
            }
            if (changed)
            {
                processor_.refreshMidiTapDisplayNames();
                repaint();
            }
            return;
        }
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
        if (collapsed_)
        {
            // Folded band keeps a live readout: the running take while
            // recording, else the probe(s) whose notes are flowing right now
            // (sampled + held by timerCallback so a dense stream won't
            // flicker at the timer rate).
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            if (processor_.isMidiCapturing())
            {
                const double s = processor_.midiCaptureElapsed();
                g.setColour(juce::Colour(0xffff3b30));
                g.drawText(juce::String::formatted("%02d:%04.1f", (int) (s / 60.0),
                                                   s - 60.0 * (double) (int) (s / 60.0))
                               + juce::String::fromUTF8(" \xC2\xB7 ")
                               + juce::String(processor_.midiCaptureNoteCount()),
                           readoutArea_, juce::Justification::centredRight, false);
            }
            else if (hotLabel_.isNotEmpty()
                     && juce::Time::getMillisecondCounterHiRes() - hotMs_ < kHotHoldMs)
            {
                g.setColour(kAccent.withAlpha(0.85f));
                g.drawText(hotLabel_, readoutArea_,
                           juce::Justification::centredRight, false);
            }
            return;
        }

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
            const int ready = enabledCount();
            g.drawText(ready == 0 ? juce::String("no probe enabled")
                                  : juce::String(ready) + (ready > 1 ? " probes ready"
                                                                     : " probe ready"),
                       readoutArea_, juce::Justification::centredLeft, false);
        }

        if (! destLabelArea_.isEmpty())
        {
            g.setColour(juce::Colour(0xff9aa6ba));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText("OUT", destLabelArea_, juce::Justification::centredLeft, false);
        }

        if (voices_.empty()) return;

        auto strip = stripArea_;
        for (auto& v : voices_)
        {
            auto tile = strip.removeFromTop(kTileH);
            strip.removeFromTop(kTileGap);

            g.setColour(juce::Colour(0xff14141c));
            g.fillRoundedRectangle(tile.toFloat(), 4.0f);

            // Activity: ● notes flowing / ◐ enabled-idle / ○ disabled — the
            // same three states as the rack LED, read from the RT instance.
            auto* st = midi_tap_instance(v->slot);
            const bool on  = st != nullptr && st->config.enabled != 0;
            const bool hot = on && v->tickMoved(st != nullptr
                                                ? midi_tap_active_ticks(st) : 0u);
            g.setColour(! on ? kAccent.withAlpha(0.25f)
                             : (hot ? kAccent : kAccent.withAlpha(0.6f)));
            g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
            g.drawText(v->label,
                       tile.reduced(kPad, 0).removeFromTop(kTileH / 2),
                       juce::Justification::centredLeft, false);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        // Chevron at the panel's TRUE right edge — aligned with the VIDEO MIX
        // header buttons and the MIDI MAP chevron whatever the zone width.
        const int cbtn = kHeaderH - 6;
        chevron_.setBounds(getWidth() - cbtn - 6, (kHeaderH - cbtn) / 2, cbtn, cbtn);

        // Rows stop stretching with a very wide zone — every stored rect
        // (master rows, readout, strip) derives from this capped width.
        r.setWidth(juce::jmin(r.getWidth(), Sp3ctraTheme::kMaxContentW));
        r.removeFromTop(kHeaderH);

        const bool showFull = ! collapsed_;
        recBtn_     .setVisible(true);   // transport stays reachable when folded
        destCombo_  .setVisible(showFull);
        busBtn_     .setVisible(showFull);
        for (auto& v : voices_)
        {
            v->out  .setVisible(showFull);
            v->mpe  .setVisible(showFull);
            v->level.setVisible(showFull);
            v->chan .setVisible(showFull);
        }

        if (collapsed_)
        {
            // REC docks beside the chevron — it must not cover the "MIDI MIX"
            // title on the left; the space between them is the live readout.
            recBtn_.setBounds(chevron_.getX() - 4 - 44, 2, 44, 20);
            readoutArea_ = { 70, 0,
                             juce::jmax(0, recBtn_.getX() - 6 - 70), kHeaderH };
            stripArea_ = destLabelArea_ = {};
            return;
        }

        auto master = r.removeFromTop(kMasterH).reduced(kPad, 2);

        // Row 1 — transport. 44 px so "REC" actually fits (26 px rendered "...").
        auto row1 = master.removeFromTop(24);
        recBtn_.setBounds(row1.removeFromLeft(44).reduced(0, 1));
        row1.removeFromLeft(6);
        readoutArea_ = row1;

        // Row 2 — real-time destination + the plugin-bus toggle.
        master.removeFromTop(4);
        auto row2 = master.removeFromTop(24);
        destLabelArea_ = row2.removeFromLeft(kLabelW);
        busBtn_.setBounds(row2.removeFromRight(44).reduced(0, 1));
        row2.removeFromRight(6);
        destCombo_.setBounds(row2);

        r.removeFromTop(kPad);
        stripArea_ = r.removeFromTop(
            juce::jmax(0, (int) voices_.size() * (kTileH + kTileGap)));

        auto strip = stripArea_;
        for (auto& v : voices_)
        {
            auto tile = strip.removeFromTop(kTileH).reduced(kPad, 3);
            strip.removeFromTop(kTileGap);

            // Header: painted chain label left, OUT + MPE right.
            auto top = tile.removeFromTop(tile.getHeight() / 2);
            v->mpe.setBounds(top.removeFromRight(40).reduced(0, 1));
            top.removeFromRight(4);
            v->out.setBounds(top.removeFromRight(40).reduced(0, 1));

            // Controls: level takes the width, channel on the right.
            tile.removeFromTop(2);
            v->chan .setBounds(tile.removeFromRight(64).reduced(0, 1));
            tile.removeFromRight(4);
            v->level.setBounds(tile.reduced(0, 1));
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (collapsed_ || ! stripArea_.contains(e.getPosition())) return;
        const int idx = (e.getPosition().y - stripArea_.getY())
                      / (kTileH + kTileGap);
        if (idx >= 0 && idx < (int) voices_.size() && onProbeSelected)
            onProbeSelected(voices_[(size_t) idx]->slot);
    }

private:
    static const juce::Colour kAccent;

    //── Header chevron — ▾ expanded / ▸ collapsed (mirrors MidiMapPanel) ─────
    struct Chevron : juce::Button
    {
        Chevron() : juce::Button("collapse") {}
        bool* collapsed = nullptr;
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            const auto b = getLocalBounds().toFloat().reduced(4.0f);
            g.setColour(kAccent.withAlpha(over ? 0.95f : 0.6f));
            juce::Path p;
            if (collapsed != nullptr && *collapsed)
                p.addTriangle(b.getX(), b.getY(), b.getX(), b.getBottom(),
                              b.getRight(), b.getCentreY());          // ▸
            else
                p.addTriangle(b.getX(), b.getY(), b.getRight(), b.getY(),
                              b.getCentreX(), b.getBottom());         // ▾
            g.fillPath(p);
        }
    };

    struct Voice
    {
        int slot  { -1 };
        int chain { -1 };
        juce::String label;   // "CHAIN 3" (+ a/b when a chain hosts 2 probes)
        juce::TextButton   out;   // live-output mute (historical "arm" param)
        juce::TextButton   mpe;   // live-output MPE mode
        Sp3ctraBarSlider   level;
        juce::ComboBox     chan;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   outAtt, mpeAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   levelAtt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> chanAtt;
        std::unique_ptr<MidiLearnAttachment> outLearn, mpeLearn, levelLearn, chanLearn;

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
        for (const auto& [slot, chain] : activeSlots_)
        {
            auto v = std::make_unique<Voice>();
            v->slot  = slot;
            v->chain = chain;

            v->out.setButtonText("OUT");
            v->out.setClickingTogglesState(true);
            v->out.setColour(juce::TextButton::buttonOnColourId,
                             kAccent.withAlpha(0.85f));
            v->out.setTooltip("Live MIDI output of this probe (port + BUS).\n"
                              "Off mutes the stream; REC is never affected.");
            addAndMakeVisible(v->out);

            v->mpe.setButtonText("MPE");
            v->mpe.setClickingTogglesState(true);
            v->mpe.setColour(juce::TextButton::buttonOnColourId,
                             kAccent.withAlpha(0.85f));
            v->mpe.setTooltip("Stream the port as MPE: one channel per note, "
                              "crest bends as per-note pitch (\xc2\xb1"
                              "2 st) and the level envelope as pressure/CC11.\n"
                              "Enable MPE on the receiving synth. Files are "
                              "unaffected.");
            addAndMakeVisible(v->mpe);

            v->level.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            v->level.setRange(0.0, 1.0, 0.01);
            v->level.setTooltip("Live-output velocity scale (monitoring gain). "
                                "Takes stay faithful.");
            addAndMakeVisible(v->level);

            for (int c = 1; c <= 16; ++c) v->chan.addItem("Ch " + juce::String(c), c);
            addAndMakeVisible(v->chan);

            using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
            using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
            using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
            v->outAtt   = std::make_unique<BA>(apvts, mtParam(slot, "arm"),     v->out);
            v->mpeAtt   = std::make_unique<BA>(apvts, mtParam(slot, "portMpe"), v->mpe);
            v->levelAtt = std::make_unique<SA>(apvts, mtParam(slot, "level"),   v->level);
            v->chanAtt  = std::make_unique<CA>(apvts, mtParam(slot, "channel"), v->chan);
            v->outLearn   = std::make_unique<MidiLearnAttachment>(
                                processor_.getMidiMap(), v->out,   mtParam(slot, "arm"));
            v->mpeLearn   = std::make_unique<MidiLearnAttachment>(
                                processor_.getMidiMap(), v->mpe,   mtParam(slot, "portMpe"));
            v->levelLearn = std::make_unique<MidiLearnAttachment>(
                                processor_.getMidiMap(), v->level, mtParam(slot, "level"));
            v->chanLearn  = std::make_unique<MidiLearnAttachment>(
                                processor_.getMidiMap(), v->chan,  mtParam(slot, "channel"));
            voices_.push_back(std::move(v));
        }

        // Row identity = the HOST CHAIN (a probe is "the MIDI of chain 3",
        // not "pool slot 2"); a chain hosting several probes gets a/b/c
        // suffixes. One source of truth with the take files and the virtual
        // ports — and the sinks learn their name right away.
        for (auto& v : voices_)
            v->label = processor_.midiTapLabel(v->slot);
        processor_.refreshMidiTapDisplayNames();
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
            Sp3ctraDialog::showWarning(this, "MIDI output", err);
    }

    void toggleRecording()
    {
        if (processor_.isMidiCapturing())
        {
            processor_.stopMidiCapture();
            recBtn_.setToggleState(false, juce::dontSendNotification);
            // Silent on success (the readout already told the story) — only a
            // failure earns a dialog.
            const auto err = processor_.midiTapLastError();
            if (err.isNotEmpty())
                Sp3ctraDialog::showWarning(this, "MIDI recording", err);
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
            Sp3ctraDialog::showWarning(
                this, "MIDI recording",
                err.isNotEmpty() ? err : juce::String("Could not start recording."));
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
        else
        {
            // Folded-band readout: sample the probes' activity HERE (paint
            // stays pure) and hold the label kHotHoldMs past the last event
            // so a dense stream doesn't flicker at the timer rate.
            juce::String hot;
            for (auto& v : voices_)
            {
                auto* st = midi_tap_instance(v->slot);
                if (st != nullptr && st->config.enabled != 0
                    && v->tickMoved(midi_tap_active_ticks(st)))
                    hot += (hot.isEmpty() ? juce::String()
                                          : juce::String::fromUTF8(" \xC2\xB7 "))
                         + v->label;
            }
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (hot.isNotEmpty()) { hotLabel_ = hot; hotMs_ = now; }
            const bool lit = processor_.isMidiCapturing()
                          || (hotLabel_.isNotEmpty() && now - hotMs_ < kHotHoldMs);
            if (lit || wasLit_) repaint(readoutArea_);
            wasLit_ = lit;
        }
        recBtn_.setToggleState(processor_.isMidiCapturing(), juce::dontSendNotification);
        // Channel only addresses the classic notes stream (and BUS): while
        // MPE streams, channels are allocated per note — grey it out.
        for (auto& v : voices_)
            v->chan.setEnabled(! v->mpe.getToggleState());
    }

    Sp3ctraAudioProcessor& processor_;
    std::vector<std::unique_ptr<Voice>> voices_;
    std::vector<std::pair<int, int>> activeSlots_;   // {slot, chain} pairs

    /** Probes that would actually write a take (module enabled). */
    int enabledCount() const
    {
        int n = 0;
        auto& apvts = processor_.getAPVTS();
        for (const auto& v : voices_)
        {
            auto* e = apvts.getRawParameterValue(mtParam(v->slot, "enabled"));
            if (e != nullptr && e->load() >= 0.5f)
                ++n;
        }
        return n;
    }

    Chevron          chevron_;
    juce::TextButton recBtn_ { "REC" };
    juce::ComboBox   destCombo_;
    juce::TextButton busBtn_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> busAtt_;

    juce::Rectangle<int> stripArea_, readoutArea_, destLabelArea_;
    bool collapsed_ { false };

    // Folded-band activity readout (see timerCallback).
    static constexpr double kHotHoldMs = 1200.0;
    juce::String hotLabel_;
    double       hotMs_  { -1.0e12 };
    bool         wasLit_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiMixPanel)
};

inline const juce::Colour MidiMixPanel::kAccent { moduleColour(ModuleType::MidiTap) };
