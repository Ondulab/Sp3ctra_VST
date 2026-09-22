/**
 * @file TimbreParamsPanel.h
 * @brief The timbre editor every synthesised-score page shares (TIMBRE slots,
 *        MIDI SCORE voices): family/subfamily browser and a compact tabbed
 *        sound editor. Browsing never changes a patch until a timbre is picked.
 *
 * The panel edits whatever target() returns and reports through the
 * callbacks; the page keeps ownership of the params, of what is NOT timbral
 * (enabled, note) and of persistence. A timbral gesture turns the patch into
 * "Custom (<the template it left>)" here, once, for every page.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include "../UITheme.h"
#include "../ui/Sp3ctraBarSlider.h"
#include "TimbreGenRenderer.h"

class TimbreParamsPanel : public juce::Component
{
public:
    using Params = timbregen::TimbreSlotParams;

    TimbreParamsPanel()
    {
        // ── Preset picker ────────────────────────────────────────────────────
        initLabel(familyLabel_, "Family");
        initLabel(subfamilyLabel_, "Subfamily");
        initLabel(presetLabel_, "Timbre");
        for (int i = 0; i < timbregen::numPresets(); ++i)
            families_.addIfNotAlreadyThere(timbregen::presetFamily(i));
        families_.sort(true);
        for (int i = 0; i < families_.size(); ++i)
            familyCombo_.addItem(families_[i], i + 1);
        familyCombo_.setComponentID("timbre-family");
        subfamilyCombo_.setComponentID("timbre-subfamily");
        presetCombo_.setComponentID("timbre-preset");
        familyCombo_.setTooltip("Browse instrument families. Your sound changes only when you choose a timbre.");
        subfamilyCombo_.setTooltip("Subfamily or playing style within the selected family.");
        presetCombo_.setTextWhenNothingSelected("Choose a timbre...");
        familyCombo_.onChange = [this] { fillSubfamilies(); fillPresets(); };
        subfamilyCombo_.onChange = [this] { fillPresets(); };
        addAndMakeVisible(familyCombo_);
        addAndMakeVisible(subfamilyCombo_);
        presetCombo_.onChange = [this]
        {
            const int id = presetCombo_.getSelectedId();
            if (id <= 0 || id > timbregen::numPresets() || ! target)
                return;   // "Custom" is a display state, not a template
            timbregen::applyPreset(target(), id - 1);
            refresh();
            if (onPresetChange) onPresetChange();
        };
        addAndMakeVisible(presetCombo_);

        // ── Partial series: the harmonic model or an inharmonic table ────────
        initLabel(seriesLabel_, "Resonator");
        seriesCombo_.addItem("Harmonic series", 1);
        for (int i = 0; i < timbregen::numTables(); ++i)
            seriesCombo_.addItem(timbregen::tableName(i), i + 2);
        seriesCombo_.setTooltip("Harmonic: the series shaped by the bars below. "
                                "A table: fixed inharmonic partials (bells, bars, "
                                "membranes, plates).");
        seriesCombo_.onChange = [this]
        {
            if (! target) return;
            const int id = seriesCombo_.getSelectedId();
            if (id <= 0) return;
            auto& q = target();
            q.bellMode  = id > 1;
            q.bellTable = q.bellMode ? id - 2 : q.bellTable;
            becomeCustom();
            refresh();
            if (onTimbralChange) onTimbralChange();
        };
        addAndMakeVisible(seriesCombo_);

        initLabel(characterLabel_, "Character");
        for (int i = 0; i < (int) timbregen::Spectrum::Count; ++i)
            characterCombo_.addItem(timbregen::spectrumName(i), i + 1);
        characterCombo_.setComponentID("timbre-character");
        characterCombo_.setTooltip("Harmonic fingerprint and vocal formants. Neutral restores an uncoloured harmonic series.");
        characterCombo_.onChange = [this]
        {
            if (!target || characterCombo_.getSelectedId() <= 0) return;
            target().spectrum = characterCombo_.getSelectedId() - 1;
            becomeCustom();
            if (onTimbralChange) onTimbralChange();
        };
        addAndMakeVisible(characterCombo_);

        const char* sections[] = { "Tone", "Envelope", "Filter", "Motion", "Texture" };
        for (size_t i = 0; i < sectionButtons_.size(); ++i)
        {
            auto& b = sectionButtons_[i];
            b.setButtonText(sections[i]);
            b.setComponentID("timbre-section-" + juce::String((int)i));
            b.setTooltip(groupName((Group)((int)Group::Stack + (int)i)));
            b.onClick = [this, i]
            {
                section_ = (Group)((int)Group::Stack + (int)i);
                resized(); repaint();
            };
            addAndMakeVisible(b);
        }

        // ── Bars ─────────────────────────────────────────────────────────────
        // Group gaps (extra px before the row) mark the model's sections:
        // the stack, the envelope, the motion, the textures, the level.
        addRow("Partials", 0, 128, 1, 24, 0.0, {}, Group::Stack,
               [](const Params& q) { return (double) q.numPartials; },
               [](Params& q, double v) { q.numPartials = (int) v; },
               "Harmonics in the series. 0 = no partial stack at all "
               "(a noise-only sound).");
        addRow("Slope (dB/oct)", -36.0, 12.0, 0.1, -6.0, 0.0, {}, Group::None,
               [](const Params& q) { return q.slopeDbPerOct; },
               [](Params& q, double v) { q.slopeDbPerOct = v; });
        addRow("Odd bias", 0.0, 1.0, 0.01, 0.0, 0.0, {}, Group::None,
               [](const Params& q) { return q.oddBias; },
               [](Params& q, double v) { q.oddBias = v; });
        addRow("Inharmonicity", 0.0, 0.1, 0.0001, 0.0, 0.3, {}, Group::None,
               [](const Params& q) { return q.inharmonicity; },
               [](Params& q, double v) { q.inharmonicity = v; });
        addRow("Pluck comb", 0.0, 1.0, 0.01, 0.0, 0.0, {}, Group::None,
               [](const Params& q) { return q.combDepth; },
               [](Params& q, double v) { q.combDepth = v; });
        addRow("Pluck position", 0.02, 0.5, 0.005, 0.28, 0.0, {}, Group::None,
               [](const Params& q) { return q.combPos; },
               [](Params& q, double v) { q.combPos = v; });

        addRow("Attack (ms)", 0.0, 1000.0, 1.0, 4.0, 0.4, {}, Group::Envelope,
               [](const Params& q) { return q.attackMs; },
               [](Params& q, double v) { q.attackMs = v; });
        addRow("Bright ramp", -1.0, 1.0, 0.01, 0.0, 0.0, {}, Group::None,
               [](const Params& q) { return q.brightRamp; },
               [](Params& q, double v) { q.brightRamp = v; },
               "How the partials arrive: > 0 the highs bloom after the "
               "fundamental (brass, gong), < 0 they speak first (pluck, "
               "hammer). 0 = all together.");
        addRow("Decay (s)", 0.0, 20.0, 0.05, 0.0, 0.5,
               [](double v) { return v <= 0.0 ? juce::String("sustain") : juce::String(v, 2); },
               Group::None,
               [](const Params& q) { return q.decaySec; },
               [](Params& q, double v) { q.decaySec = v; });
        addRow("HF damping", 0.0, 1.0, 0.01, 0.5, 0.0, {}, Group::None,
               [](const Params& q) { return q.hfDamp; },
               [](Params& q, double v) { q.hfDamp = v; },
               "Upper partials decay faster. Needs Decay > 0 and a "
               "multi-partial timbre.");

        addRow("Cutoff (Hz)", 0.0, 20000.0, 1.0, 0.0, 0.3,
               [](double v) { return v <= 0.0 ? juce::String("open") : juce::String(v, 0); }, Group::Filter,
               [](const Params& q) { return q.cutoffHz; },
               [](Params& q, double v) { q.cutoffHz = v; },
               "Low-pass colour. Open disables filtering; a low cutoff keeps basses warm.");
        addRow("Filter sweep (oct)", -4.0, 6.0, 0.05, 0.0, 0.0, {}, Group::None,
               [](const Params& q) { return q.filterSweepOct; },
               [](Params& q, double v) { q.filterSweepOct = v; },
               "Initial brightness above the final cutoff. Positive closes after the attack; negative opens gradually.");
        addRow("Filter time (ms)", 5.0, 5000.0, 1.0, 180.0, 0.4, {}, Group::None,
               [](const Params& q) { return q.filterDecayMs; },
               [](Params& q, double v) { q.filterDecayMs = v; });
        addRow("Pitch attack (ct)", -1200.0, 2400.0, 1.0, 0.0, 0.0, {}, Group::Motion,
               [](const Params& q) { return q.pitchAttackCents; },
               [](Params& q, double v) { q.pitchAttackCents = v; },
               "Pitch at the onset, settling into the note: subtle for plucks, deep for kicks and 808 bass.");
        addRow("Pitch time (ms)", 5.0, 1000.0, 1.0, 35.0, 0.4, {}, Group::None,
               [](const Params& q) { return q.pitchSettleMs; },
               [](Params& q, double v) { q.pitchSettleMs = v; });

        addRow("Vibrato (cents)", 0.0, 200.0, 1.0, 0.0, 0.0, {}, Group::None,
               [](const Params& q) { return q.vibCents; },
               [](Params& q, double v) { q.vibCents = v; },
               "Pitch wave of the whole partial stack, peak depth in cents. "
               "0 = off.");
        addRow("Vib rate (Hz)", 0.1, 16.0, 0.1, 5.5, 0.0, {}, Group::None,
               [](const Params& q) { return q.vibRateHz; },
               [](Params& q, double v) { q.vibRateHz = v; });
        addRow("Vib onset (s)", 0.0, 5.0, 0.05, 0.4, 0.0, {}, Group::None,
               [](const Params& q) { return q.vibOnsetSec; },
               [](Params& q, double v) { q.vibOnsetSec = v; },
               "Time for the vibrato to develop after the note starts "
               "(delay + smooth rise).");
        addRow("Vib life", 0.0, 1.0, 0.01, 0.5, 0.0, {}, Group::None,
               [](const Params& q) { return q.vibLife; },
               [](Params& q, double v) { q.vibLife = v; },
               "Humanisation: the depth waves, the rate drifts, and every "
               "note gets its own phase / rate / depth defects. 0 = "
               "mechanical sine, 1 = loose.");
        addRow("Drift (cents)", 0.0, 30.0, 0.1, 0.0, 0.0, {}, Group::None,
               [](const Params& q) { return q.driftCents; },
               [](Params& q, double v) { q.driftCents = v; },
               "Slow wander of the whole stack, alive from the first "
               "column: a partial is never a ruler.");

        addRow("Unison (cents)", 0.0, 40.0, 0.1, 0.0, 0.0, {}, Group::Texture,
               [](const Params& q) { return q.unisonCents; },
               [](Params& q, double v) { q.unisonCents = v; },
               "Splits every partial into a detuned pair (total spread). "
               "The two oscillators the reader plays beat for real: piano "
               "strings, a section, a chorus.");
        addRow("Body (Hz)", 0.0, 4000.0, 1.0, 0.0, 0.35,
               [](double v) { return v <= 0.0 ? juce::String("off") : juce::String(v, 0); },
               Group::None,
               [](const Params& q) { return q.bodyHz; },
               [](Params& q, double v) { q.bodyHz = v; },
               "One body resonance in absolute Hz: the partials near it are "
               "lifted (or cut) on EVERY note, so the same timbre changes "
               "colour with the pitch as a real instrument does.");
        addRow("Body (dB)", -18.0, 18.0, 0.1, 0.0, 0.0, {}, Group::None,
               [](const Params& q) { return q.bodyDb; },
               [](Params& q, double v) { q.bodyDb = v; });
        addRow("Burst (dB)", timbregen::kOffDb, 0.0, 0.5, timbregen::kOffDb, 0.0,
               offText, Group::None,
               [](const Params& q) { return q.burstDb; },
               [](Params& q, double v) { q.burstDb = v; },
               "Onset burst: the pick, hammer, tongue or bow grip — a short "
               "broadband grain with a smooth onset. Total RMS level relative to one peak partial; "
               "very faint noise falls below the printable range.");
        addRow("Burst (ms)", 0.5, 60.0, 0.5, 8.0, 0.4, {}, Group::None,
               [](const Params& q) { return q.burstMs; },
               [](Params& q, double v) { q.burstMs = v; },
               "Its length at 1 kHz; shorter above, longer below, the way "
               "a click spreads on a constant-Q image.");
        addRow("Burst tilt", -12.0, 12.0, 0.1, 0.0, 0.0, {}, Group::None,
               [](const Params& q) { return q.burstTilt; },
               [](Params& q, double v) { q.burstTilt = v; },
               "Its colour, dB per octave above 1 kHz: + = a bright click, "
               "− = a dull thud.");
        addRow("Noise (dB)", timbregen::kOffDb, 0.0, 0.5, timbregen::kOffDb, 0.0,
               offText, Group::None,
               [](const Params& q) { return q.noiseDb; },
               [](Params& q, double v) { q.noiseDb = v; },
               "Sustained noise — breath, bow hair, wind — tied to the "
               "fundamental, formants, filter and note envelope. Total RMS level "
               "relative to one peak partial, across all frequency bands.");
        addRow("Noise tilt", -12.0, 12.0, 0.1, -3.0, 0.0, {}, Group::None,
               [](const Params& q) { return q.noiseTilt; },
               [](Params& q, double v) { q.noiseTilt = v; },
               "Its colour, dB per octave above the fundamental.");

        addRow("Level (dB)", -36.0, 12.0, 0.1, 0.0, 0.0, {}, Group::Level,
               [](const Params& q) { return q.levelDb; },
               [](Params& q, double v) { q.levelDb = v; },
               "Playback gain. Line width changes the spectral spread without raising the level.",
               /*isLevel*/ true);
    }

    /** The params the panel edits. Called on every gesture and refresh(). */
    std::function<Params&()> target;
    /** A timbral gesture: the params changed and the patch became Custom
     *  (preset / customBase already updated). */
    std::function<void()> onTimbralChange;
    /** The level bar moved (a gain — the preset stays). */
    std::function<void()> onLevelChange;
    /** A template was applied from the preset picker. */
    std::function<void()> onPresetChange;

    /** Pushes target() into the widgets (no notifications) and applies the
     *  enable rules: a table fixes its partial set, HF damping needs a decay,
     *  the vibrato shape needs a depth, a texture's shape needs its level. */
    void refresh()
    {
        if (! target) return;
        const Params& q = target();

        const int base = q.preset >= 0 ? q.preset : q.customBase;
        if (base >= 0 && base < timbregen::numPresets())
        {
            familyCombo_.setSelectedId(families_.indexOf(timbregen::presetFamily(base)) + 1,
                                      juce::dontSendNotification);
            fillSubfamilies(timbregen::presetSubfamily(base));
        }
        else
        {
            familyCombo_.setSelectedId(0, juce::dontSendNotification);
            familyCombo_.setText("Custom", juce::dontSendNotification);
            subfamilyCombo_.clear(juce::dontSendNotification);
        }
        fillPresets();
        characterCombo_.setSelectedId(q.spectrum + 1, juce::dontSendNotification);
        characterCombo_.setEnabled(!q.bellMode);

        seriesCombo_.setSelectedId(q.bellMode ? q.bellTable + 2 : 1,
                                   juce::dontSendNotification);

        for (auto& r : rows_)
        {
            r->bar.setValue(r->read(q), juce::dontSendNotification);
            r->bar.updateText();   // an unchanged value leaves the label stale
        }

        const bool harmonic = ! q.bellMode;
        const bool vib      = q.vibCents > 0.0;
        for (auto& r : rows_)
        {
            bool on = true;
            switch (r->rule)
            {
                case Rule::Harmonic: on = harmonic; break;
                case Rule::Decay:    on = q.decaySec > 0.0; break;
                case Rule::Vibrato:  on = vib; break;
                case Rule::Body:     on = q.bodyHz > 0.0; break;
                case Rule::Burst:    on = q.burstDb > timbregen::kOffDb; break;
                case Rule::Noise:    on = q.noiseDb > timbregen::kOffDb; break;
                case Rule::Filter:   on = q.cutoffHz > 0.0; break;
                case Rule::Pitch:    on = std::abs(q.pitchAttackCents) > 0.0; break;
                case Rule::Always:   break;
            }
            // LABEL AND BAR (a dim number alone reads as "small value", not
            // "inert control").
            r->bar.setEnabled(on);
            r->label.setEnabled(on);
        }
    }

    /** Every bar + the pickers' outline wear this colour (a page's module
     *  colour, or MIDI SCORE's per-voice identity). */
    void setAccent(juce::Colour c)
    {
        for (auto& r : rows_)
            r->bar.setAccent(c);
        for (auto& b : sectionButtons_)
        {
            b.setColour(juce::TextButton::buttonOnColourId, c.withAlpha(0.3f));
            b.setColour(juce::TextButton::textColourOnId, c);
        }
        familyCombo_.setColour(juce::ComboBox::outlineColourId, c.withAlpha(0.55f));
        subfamilyCombo_.setColour(juce::ComboBox::outlineColourId, c.withAlpha(0.55f));
        characterCombo_.setColour(juce::ComboBox::outlineColourId, c.withAlpha(0.55f));
        presetCombo_.setColour(juce::ComboBox::outlineColourId, c.withAlpha(0.55f));
        seriesCombo_.setColour(juce::ComboBox::outlineColourId, c.withAlpha(0.55f));
    }

    void setRowMetrics(int labelW, int rowH, int gap)
    {
        lblW_ = labelW; ch_ = rowH; gap_ = gap;
        resized();
    }

    int preferredHeight() const
    {
        std::array<int, 7> heights{};
        heights[(size_t)Group::Stack] = (ch_ + gap_) * 2; // resonator / character
        for (const auto& r : rows_) heights[(size_t)r->section] += ch_ + 3;
        int maxSection = 0;
        for (int i = (int)Group::Stack; i <= (int)Group::Texture; ++i)
            maxSection = juce::jmax(maxSection, heights[(size_t)i]);
        // Fixed height across tabs keeps transport/export from jumping.
        return (ch_ + gap_) * 4 + maxSection + gap_ + heights[(size_t)Group::Level];
    }

    void resized() override
    {
        const int w = getWidth();
        int y = 0;
        auto picker = [&](juce::Label& label, juce::ComboBox& combo)
        {
            const int labelW = juce::jmin(lblW_, 85);
            label.setBounds(0, y, labelW, ch_);
            combo.setBounds(labelW + gap_, y, juce::jmax(0, w - labelW - gap_), ch_);
            y += ch_ + gap_;
        };
        picker(familyLabel_, familyCombo_);
        picker(subfamilyLabel_, subfamilyCombo_);
        picker(presetLabel_, presetCombo_);
        for (size_t i = 0; i < sectionButtons_.size(); ++i)
        {
            const int x0 = (int)i * w / (int)sectionButtons_.size();
            const int x1 = ((int)i + 1) * w / (int)sectionButtons_.size();
            auto& b = sectionButtons_[i];
            b.setBounds(x0, y, juce::jmax(0, x1 - x0 - 2), ch_);
            b.setToggleState((int)section_ == (int)Group::Stack + (int)i, juce::dontSendNotification);
        }
        y += ch_ + gap_;
        const bool tone = section_ == Group::Stack;
        for (auto* c : { (juce::Component*)&seriesLabel_, (juce::Component*)&seriesCombo_,
                         (juce::Component*)&characterLabel_, (juce::Component*)&characterCombo_ })
            c->setVisible(tone);
        if (tone)
        {
            picker(seriesLabel_, seriesCombo_);
            picker(characterLabel_, characterCombo_);
        }
        for (auto& r : rows_)
        {
            const bool level = r->section == Group::Level;
            const bool show = r->section == section_ || level;
            r->label.setVisible(show); r->bar.setVisible(show);
            if (!show) continue;
            const int rowY = level ? preferredHeight() - ch_ - 3 : y;
            r->label.setBounds(0, rowY, lblW_, ch_);
            r->bar.setBounds(lblW_ + gap_, rowY, juce::jmax(0, w - lblW_ - gap_), ch_);
            if (!level) y += ch_ + 3;
        }
    }

private:
    enum class Group { None, Stack, Envelope, Filter, Motion, Texture, Level };
    enum class Rule  { Always, Harmonic, Decay, Vibrato, Body, Burst, Noise, Filter, Pitch };

    struct Row
    {
        juce::Label      label;
        Sp3ctraBarSlider bar;
        std::function<double(const Params&)>  read;
        std::function<void(Params&, double)>  write;
        Group section = Group::Stack;
        Rule  rule  = Rule::Always;
        bool  isLevel = false;
    };

    static juce::String offText(double v)
    {
        return v <= timbregen::kOffDb + 0.01 ? juce::String("off") : juce::String(v, 1);
    }

    void fillSubfamilies(const juce::String& preferred = {})
    {
        juce::StringArray names;
        for (int i = 0; i < timbregen::numPresets(); ++i)
            if (familyCombo_.getText() == timbregen::presetFamily(i))
                names.addIfNotAlreadyThere(timbregen::presetSubfamily(i));
        names.sort(true);
        subfamilyCombo_.clear(juce::dontSendNotification);
        for (int i = 0; i < names.size(); ++i) subfamilyCombo_.addItem(names[i], i + 1);
        subfamilyCombo_.setSelectedId(names.isEmpty() ? 0 : juce::jmax(1, names.indexOf(preferred) + 1),
                                      juce::dontSendNotification);
    }

    void fillPresets()
    {
        presetCombo_.clear(juce::dontSendNotification);
        for (int i = 0; i < timbregen::numPresets(); ++i)
            if (familyCombo_.getText() == timbregen::presetFamily(i)
                && subfamilyCombo_.getText() == timbregen::presetSubfamily(i))
                presetCombo_.addItem(timbregen::presetName(i), i + 1);
        if (!target) return;
        const auto& q = target();
        const int base = q.preset >= 0 ? q.preset : q.customBase;
        const bool here = base < 0 || (familyCombo_.getText() == timbregen::presetFamily(base)
                            && subfamilyCombo_.getText() == timbregen::presetSubfamily(base));
        presetCombo_.addItem(customName(q), customId());
        presetCombo_.setItemEnabled(customId(), false); // display state, not an instrument
        if (here)
        {
            if (q.preset >= 0) presetCombo_.setSelectedId(q.preset + 1, juce::dontSendNotification);
            else presetCombo_.setText(customName(q), juce::dontSendNotification);
        }
        presetCombo_.setTooltip(here && base >= 0
            ? "Suggested register: " + timbregen::midiNoteLabel(timbregen::presetSuggestedNote(base))
              + ". Choosing a timbre preserves the note and MIDI pitches."
            : "Choose an instrument to apply its sound. Browsing preserves the current patch.");
    }

    static int customId() { return timbregen::numPresets() + 1; }

    static juce::String customName(const Params& q)
    {
        return q.customBase >= 0
            ? "Custom (" + juce::String(timbregen::presetName(q.customBase)) + ")"
            : juce::String("Custom");
    }

    static const char* groupName(Group g)
    {
        switch (g)
        {
            case Group::Stack: return "SPECTRUM";
            case Group::Envelope: return "ENVELOPE";
            case Group::Filter: return "BRIGHTNESS";
            case Group::Motion: return "PITCH & MOVEMENT";
            case Group::Texture: return "BODY & TEXTURE";
            case Group::Level: return "OUTPUT";
            case Group::None: return "";
        }
        return "";
    }

    void initLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    void addRow(const juce::String& name, double lo, double hi, double step, double def,
                double skew, std::function<juce::String(double)> text, Group group,
                std::function<double(const Params&)> read,
                std::function<void(Params&, double)> write,
                const juce::String& tooltip = {}, bool isLevel = false)
    {
        auto r = std::make_unique<Row>();
        initLabel(r->label, name);
        // The text function must be in place BEFORE the first setValue: JUCE
        // only refreshes the read-out when the value CHANGES, so a bar that
        // opens already sitting on its default ("off", "sustain") would keep
        // the raw number for the rest of the session.
        if (text)
        {
            r->bar.textFromValueFunction = std::move(text);
            r->bar.valueFromTextFunction = nullptr;
        }
        r->bar.setRange(lo, hi, step);
        if (skew > 0.0) r->bar.setSkewFactor(skew);
        r->bar.setValue(def, juce::dontSendNotification);
        r->bar.updateText();
        if (tooltip.isNotEmpty())
            r->bar.setTooltip(tooltip);
        r->read  = std::move(read);
        r->write = std::move(write);
        if (group != Group::None) currentGroup_ = group;
        r->section = currentGroup_;
        r->isLevel = isLevel;
        r->rule = ruleFor(name);

        Row* raw = r.get();
        r->bar.onValueChange = [this, raw]
        {
            if (! target) return;
            raw->write(target(), raw->bar.getValue());
            if (raw->isLevel)
            {
                if (onLevelChange) onLevelChange();
                return;
            }
            becomeCustom();
            refreshRules();
            if (onTimbralChange) onTimbralChange();
        };
        addAndMakeVisible(r->bar);
        rows_.push_back(std::move(r));
    }

    /** Which enable rule a row obeys, by its name — the rules are few and
     *  the names are the panel's own. */
    static Rule ruleFor(const juce::String& name)
    {
        if (name == "Partials" || name.startsWith("Slope") || name == "Odd bias"
            || name == "Inharmonicity" || name.startsWith("Pluck"))
            return Rule::Harmonic;
        if (name.startsWith("Filter "))                return Rule::Filter;
        if (name == "Pitch time (ms)")                  return Rule::Pitch;
        if (name == "HF damping")                       return Rule::Decay;
        if (name.startsWith("Vib "))                    return Rule::Vibrato;
        if (name == "Body (dB)")                        return Rule::Body;
        if (name == "Burst (ms)" || name == "Burst tilt") return Rule::Burst;
        if (name == "Noise tilt")                       return Rule::Noise;
        return Rule::Always;
    }

    /** Re-applies the enable rules only (a gesture changed a gate value). */
    void refreshRules()
    {
        if (! target) return;
        const Params& q = target();
        for (auto& r : rows_)
        {
            bool on = true;
            switch (r->rule)
            {
                case Rule::Harmonic: on = ! q.bellMode; break;
                case Rule::Decay:    on = q.decaySec > 0.0; break;
                case Rule::Vibrato:  on = q.vibCents > 0.0; break;
                case Rule::Body:     on = q.bodyHz > 0.0; break;
                case Rule::Burst:    on = q.burstDb > timbregen::kOffDb; break;
                case Rule::Noise:    on = q.noiseDb > timbregen::kOffDb; break;
                case Rule::Filter:   on = q.cutoffHz > 0.0; break;
                case Rule::Pitch:    on = std::abs(q.pitchAttackCents) > 0.0; break;
                case Rule::Always:   break;
            }
            r->bar.setEnabled(on);
            r->label.setEnabled(on);
        }
    }

    /** A timbral tweak turns the patch into a hand-tuned "Custom" —
     *  remembering WHICH template it drifted from ("Custom (Piano)"). */
    void becomeCustom()
    {
        Params& q = target();
        if (q.preset != timbregen::kPresetCustom)
        {
            q.customBase = q.preset;
            q.preset = timbregen::kPresetCustom;
        }
        refresh(); // restore the edited sound's family after browsing elsewhere

    }

    juce::Label    familyLabel_, subfamilyLabel_, presetLabel_, seriesLabel_, characterLabel_;
    juce::ComboBox familyCombo_, subfamilyCombo_, presetCombo_, seriesCombo_, characterCombo_;
    juce::StringArray families_;
    std::array<juce::TextButton, 5> sectionButtons_;
    Group section_ = Group::Stack, currentGroup_ = Group::Stack;
    std::vector<std::unique_ptr<Row>> rows_;
    int lblW_ = 130, ch_ = Sp3ctraTheme::kControlH, gap_ = 6;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimbreParamsPanel)
};
