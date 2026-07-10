/**
 * @file TimbreGenTabComponent.h
 * @brief PLAY page for the TIMBRE block — synthesise printable instrument
 *        spectra (bell, brass, e-guitar, square, triangle…) from parameters
 *        and export them as an A4 page holding 6 sounds side by side.
 *
 * Sibling of ScoreGenTabComponent: SCORE encodes a WAV, TIMBRE encodes a
 * PARAMETRIC MODEL (partial sets — see TimbreGenRenderer). Same band
 * geometry, same greyscale/dB conventions, same DPI-stamped export, and the
 * same audition path: the generated page is loaded into the shared SCORE
 * player channel (LuxSampler::loadScoreFramesFromImage + uiPlayScore), so
 * the loop/speed transport params (scoreLoop / scoreReverse / scoreSpeed /
 * scorePlaying) drive it exactly like a generated score.
 *
 * Rendering is cheap (pure drawing, no FFT): the preview regenerates live
 * (debounced) at a reduced DPI; the full-resolution page is built on demand
 * for EXPORT and PLAY. All state persists as JSON in apvts.state
 * ("timbreGenState") — offline tool, deliberately not host-automatable.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <vector>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../IconPaths.h"
#include "TimbreGenRenderer.h"

class TimbreGenTabComponent : public juce::Component,
                              private juce::Timer
{
public:
    static constexpr uint32_t kAccentARGB = 0xffd97b52;   // terracotta (TIMBRE identity)
    static constexpr int      kPreferredH = 640;

    explicit TimbreGenTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        // ── Default page: six classic timbres, ready to print ───────────────
        static const int kDefaultPresets[timbregen::kNumSlots] = { 1, 2, 3, 9, 6, 7 };
        for (int i = 0; i < timbregen::kNumSlots; ++i)
        {
            timbregen::applyPreset(slots[(size_t) i], kDefaultPresets[i]);
            slots[(size_t) i].enabled  = true;
            slots[(size_t) i].midiNote = 57;   // A3
        }
        restoreState();   // overwrite with the persisted page when present

        // ── Slot tabs ────────────────────────────────────────────────────────
        for (int i = 0; i < timbregen::kNumSlots; ++i)
        {
            auto tab = std::make_unique<SlotTab>(i);
            tab->onClick = [this, i] { selectSlot(i); };
            tab->textProvider = [this](int idx)
            {
                const auto& q = slots[(size_t) idx];
                const juce::String name = (q.preset >= 0) ? timbregen::presetName(q.preset)
                                                          : "Custom";
                const juce::String note = timbregen::midiNoteLabel(q.midiNote)
                                              .upToFirstOccurrenceOf(" (", false, false);
                return juce::String(idx + 1) + " " + name + "\n" + note;
            };
            tab->enabledProvider  = [this](int idx) { return slots[(size_t) idx].enabled; };
            tab->selectedProvider = [this](int idx) { return idx == selectedSlot; };
            addAndMakeVisible(tab.get());
            slotTabs[(size_t) i] = std::move(tab);
        }

        // ── Per-slot parameters ──────────────────────────────────────────────
        initLabel(presetLabel, "Preset");
        for (int i = 0; i < timbregen::numPresets(); ++i)
            presetCombo.addItem(timbregen::presetName(i), i + 1);
        presetCombo.addItem("Custom", timbregen::numPresets() + 1);
        presetCombo.onChange = [this]
        {
            const int id = presetCombo.getSelectedId();
            if (id <= 0 || id > timbregen::numPresets())
                return;   // "Custom" is a display state, not a template
            timbregen::applyPreset(cur(), id - 1);
            refreshSlotControls();
            markDirty();
        };
        addAndMakeVisible(presetCombo);

        activeToggle.setButtonText("Active");
        activeToggle.onClick = [this]
        {
            cur().enabled = activeToggle.getToggleState();
            slotTabs[(size_t) selectedSlot]->repaint();
            markDirty();
        };
        addAndMakeVisible(activeToggle);

        initLabel(noteLabel, "Note");
        initSlider(noteSlider, 24, 108, 1, 57);
        noteSlider.textFromValueFunction = [](double v)
        { return timbregen::midiNoteLabel((int) v); };
        noteSlider.valueFromTextFunction = nullptr;
        noteSlider.onValueChange = [this]
        {
            cur().midiNote = (int) noteSlider.getValue();
            slotTabs[(size_t) selectedSlot]->repaint();
            markDirty();   // note is not a timbral field — preset stays
        };

        auto timbral = [this](juce::Slider& s, auto setter)
        {
            s.onValueChange = [this, &s, setter]
            {
                setter(cur(), s.getValue());
                becomeCustom();
                markDirty();
            };
        };

        initLabel(partialsLabel, "Partials");
        initSlider(partialsSlider, 1, 64, 1, 24);
        timbral(partialsSlider, [](timbregen::TimbreSlotParams& q, double v) { q.numPartials = (int) v; });

        initLabel(slopeLabel, "Slope (dB/oct)");
        initSlider(slopeSlider, -24.0, 6.0, 0.1, -6.0);
        timbral(slopeSlider, [](timbregen::TimbreSlotParams& q, double v) { q.slopeDbPerOct = v; });

        initLabel(oddLabel, "Odd bias");
        initSlider(oddSlider, 0.0, 1.0, 0.01, 0.0);
        timbral(oddSlider, [](timbregen::TimbreSlotParams& q, double v) { q.oddBias = v; });

        initLabel(inharmLabel, "Inharmonicity");
        initSlider(inharmSlider, 0.0, 0.02, 0.0001, 0.0);
        inharmSlider.setSkewFactor(0.4);
        timbral(inharmSlider, [](timbregen::TimbreSlotParams& q, double v) { q.inharmonicity = v; });

        initLabel(combLabel, "Pluck comb");
        initSlider(combSlider, 0.0, 1.0, 0.01, 0.0);
        timbral(combSlider, [](timbregen::TimbreSlotParams& q, double v) { q.combDepth = v; });

        initLabel(combPosLabel, "Pluck position");
        initSlider(combPosSlider, 0.05, 0.5, 0.005, 0.28);
        timbral(combPosSlider, [](timbregen::TimbreSlotParams& q, double v) { q.combPos = v; });

        initLabel(attackLabel, "Attack (ms)");
        initSlider(attackSlider, 0.0, 300.0, 1.0, 4.0);
        attackSlider.setSkewFactor(0.5);
        timbral(attackSlider, [](timbregen::TimbreSlotParams& q, double v) { q.attackMs = v; });

        initLabel(decayLabel, "Decay (s)");
        initSlider(decaySlider, 0.0, 8.0, 0.05, 0.0);
        decaySlider.textFromValueFunction = [](double v)
        { return v <= 0.0 ? juce::String("sustain") : juce::String(v, 2); };
        timbral(decaySlider, [](timbregen::TimbreSlotParams& q, double v) { q.decaySec = v; });

        initLabel(hfDampLabel, "HF damping");
        initSlider(hfDampSlider, 0.0, 1.0, 0.01, 0.5);
        timbral(hfDampSlider, [](timbregen::TimbreSlotParams& q, double v) { q.hfDamp = v; });

        initLabel(levelLabel, "Level (dB)");
        initSlider(levelSlider, -24.0, 6.0, 0.1, 0.0);
        levelSlider.onValueChange = [this]
        {
            cur().levelDb = levelSlider.getValue();   // gain — preset stays
            markDirty();
        };

        // ── Page-level settings ──────────────────────────────────────────────
        initLabel(wsLabel, "Writing Speed (cm/s)");
        initSlider(wsSlider, 0.5, 10.0, 0.1, pageSettings.writingSpeed);
        wsSlider.onValueChange = [this]
        {
            pageSettings.writingSpeed = wsSlider.getValue();
            markDirty();
        };

        initLabel(lineLabel, "Line width (mm)");
        initSlider(lineSlider, 0.10, 0.80, 0.01, pageSettings.lineWidthMM);
        lineSlider.onValueChange = [this]
        {
            pageSettings.lineWidthMM = lineSlider.getValue();
            markDirty();
        };

        labelsToggle.setButtonText("Titles + footer (top margin)");
        labelsToggle.setTooltip("Print the slot titles and the reproduction footer "
                                "at the very top of the page. Off = clean page "
                                "(band + cut marks only).");
        labelsToggle.setToggleState(pageSettings.showLabels, juce::dontSendNotification);
        labelsToggle.onClick = [this]
        {
            pageSettings.showLabels = labelsToggle.getToggleState();
            markDirty();
        };
        addAndMakeVisible(labelsToggle);

        initLabel(dpiLabel, "DPI");
        for (int d : { 200, 300, 400, 600, 800 })
            dpiCombo.addItem(juce::String(d), d);
        dpiCombo.setSelectedId((int) pageSettings.printerDpi, juce::dontSendNotification);
        dpiCombo.onChange = [this]
        {
            pageSettings.printerDpi = (double) juce::jmax(72, dpiCombo.getSelectedId());
            fullDirty  = true;      // export/play resolution only — preview unchanged
            stateDirty = true;
        };
        addAndMakeVisible(dpiCombo);

        // ── Export ───────────────────────────────────────────────────────────
        exportPngButton.setButtonText("Export PNG");
        exportPngButton.onClick = [this] { chooseExport(true); };
        addAndMakeVisible(exportPngButton);

        exportJpgButton.setButtonText("Export JPEG");
        exportJpgButton.onClick = [this] { chooseExport(false); };
        addAndMakeVisible(exportJpgButton);

        // ── Audition transport (shared SCORE player channel) ────────────────
        playStopButton.setTooltip("Play / stop the timbre page through the score player");
        playStopButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playStopButton);

        loopBtn.setTooltip("Loop playback");
        addAndMakeVisible(loopBtn);
        loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "scoreLoop", loopBtn);

        reverseBtn.setTooltip("Reverse (play the timbre page backward)");
        addAndMakeVisible(reverseBtn);
        reverseAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "scoreReverse", reverseBtn);

        initLabel(speedLabel, "Speed");
        speedSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        speedSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 14);
        speedSlider.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
        speedSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        speedSlider.setColour(juce::Slider::textBoxTextColourId,       juce::Colour(0xffa0c4e8));
        speedSlider.setRange(0.1, 6.0, 0.01);
        speedSlider.setTextValueSuffix("x");
        speedSlider.setSkewFactorFromMidPoint(1.0);
        addAndMakeVisible(speedSlider);
        speedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getAPVTS(), "scoreSpeed", speedSlider);

        // Right-click MIDI Learn on the shared SCORE transport params.
        {
            auto& mm = processor.getMidiMap();
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, playStopButton, "scorePlaying"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, loopBtn,        "scoreLoop"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, reverseBtn,     "scoreReverse"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, speedSlider,    "scoreSpeed"));
        }

        playHint.setText("PLAY loads the page into the score player "
                         "(set LuxStral source = Sampler to hear it).",
                         juce::dontSendNotification);
        playHint.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        playHint.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        addAndMakeVisible(playHint);

        // ── Log ──────────────────────────────────────────────────────────────
        logLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        logLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        logLabel.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(logLabel);

        refreshSlotControls();
        regenPreview();          // synchronous — rendering is cheap
        startTimerHz(20);
    }

    ~TimbreGenTabComponent() override
    {
        stopTimer();
        if (stateDirty)
            persistState();
        // Like SCORE: the page is a VIEW — closing the editor must not cut audio.
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent(kAccentARGB);

        g.setColour(juce::Colour(0xff10131a));
        g.fillRect(previewArea);
        g.setColour(accent.withAlpha(0.35f));
        g.drawRect(previewArea, 1);

        if (previewImage.isValid())
        {
            const auto imgArea = previewImageBounds();
            previewImgArea = imgArea;
            g.setOpacity(1.0f);
            g.drawImage(previewImage, imgArea);

            // Selected-slot highlight (fractions are DPI-independent).
            {
                double fx0 = 0.0, fx1 = 1.0;
                slotRangeFrac(selectedSlot, fx0, fx1);
                juce::Rectangle<float> r(imgArea.getX() + (float) fx0 * imgArea.getWidth(),
                                         imgArea.getY(),
                                         (float) (fx1 - fx0) * imgArea.getWidth(),
                                         imgArea.getHeight());
                g.setColour(accent.withAlpha(0.7f));
                g.drawRect(r, 1.5f);
            }

            // Reading head — only meaningful while OUR frames sit in the player.
            auto* fs = processor.getLuxSampler();
            if (fs != nullptr && framesAreOurs)
            {
                const bool playing = fs->isScorePlaying();
                int headFrame = -1;
                if (playing)             headFrame = fs->getScorePlayHead();
                else if (scrubHead >= 0) headFrame = scrubHead;
                if (headFrame >= 0)
                {
                    const int n = juce::jmax(1, fs->getScoreFrameCount());
                    const float frac = juce::jlimit(0.f, 1.f, (float) headFrame / (float) n);
                    const float lx = imgArea.getX() + frac * imgArea.getWidth();
                    g.setColour(accent.withAlpha(playing ? 0.9f : 0.6f));
                    g.fillRect(lx - 0.75f, imgArea.getY(), 1.5f, imgArea.getHeight());
                }
            }
        }
        else
        {
            g.setColour(juce::Colour(0xff55606f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
            g.drawText("Enable at least one sound slot", previewArea,
                       juce::Justification::centred);
        }
    }

    juce::Rectangle<float> previewImageBounds() const
    {
        if (! previewImage.isValid() || previewArea.isEmpty())
            return {};
        const juce::Rectangle<float> dest(
            (float) previewArea.getX() + 2, (float) previewArea.getY() + 2,
            (float) previewArea.getWidth() - 4, (float) previewArea.getHeight() - 4);
        const juce::RectanglePlacement place(juce::RectanglePlacement::centred);
        return place.appliedTo(
            juce::Rectangle<float>(0.f, 0.f,
                (float) previewImage.getWidth(), (float) previewImage.getHeight()),
            dest);
    }

    //==========================================================================
    // Preview interactions: click = scrub (when our frames are loaded, like the
    // SCORE page); double-click = select the sound slot under the cursor.
    void mouseDown(const juce::MouseEvent& e) override
    {
        scrubbing = framesAreOurs && previewImage.isValid()
                 && previewArea.contains(e.getPosition());
        if (! scrubbing) return;
        scrubTo(e);
        if (auto* fs = processor.getLuxSampler())
            if (! fs->isScorePlaying())
                scrubAuditioning = fs->uiBeginScoreScrub();
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (scrubbing) scrubTo(e); }
    void mouseUp  (const juce::MouseEvent&)   override
    {
        scrubbing = false;
        if (scrubAuditioning)
        {
            if (auto* fs = processor.getLuxSampler()) fs->uiEndScoreScrub();
            scrubAuditioning = false;
        }
    }
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        const auto area = previewImgArea.isEmpty() ? previewImageBounds() : previewImgArea;
        if (area.getWidth() <= 0.f || ! area.contains(e.position)) return;
        const double fx = juce::jlimit(0.0, 1.0,
            ((double) e.position.x - area.getX()) / area.getWidth());
        for (int i = 0; i < timbregen::kNumSlots; ++i)
        {
            double f0 = 0.0, f1 = 1.0;
            slotRangeFrac(i, f0, f1);
            if (fx >= f0 && fx <= f1) { selectSlot(i); break; }
        }
    }

    void resized() override
    {
        const int pad = 8;
        const int ch  = Sp3ctraTheme::kControlH;
        const int gap = 6;

        // ── Slot tabs across the top ─────────────────────────────────────────
        const int tabH = 36;
        {
            const int w = (getWidth() - 2 * pad - (timbregen::kNumSlots - 1) * 4)
                        / timbregen::kNumSlots;
            int x = pad;
            for (auto& t : slotTabs)
            {
                t->setBounds(x, pad, w, tabH);
                x += w + 4;
            }
        }
        const int contentTop = pad + tabH + gap + 2;

        // ── Control column (left) ────────────────────────────────────────────
        const int colW = juce::jmin(340, getWidth() - 2 * pad);
        const int lblW = 130;
        int y = contentTop;

        auto row = [&](juce::Label& l, juce::Component& c)
        {
            l.setBounds(pad, y, lblW, ch);
            c.setBounds(pad + lblW + gap, y, colW - lblW - gap, ch);
            y += ch + 3;
        };

        presetLabel.setBounds(pad, y, 60, ch);
        presetCombo.setBounds(pad + 60 + gap, y, colW - 60 - gap - 70, ch);
        activeToggle.setBounds(pad + colW - 66, y, 66, ch);
        y += ch + gap;

        row(noteLabel,     noteSlider);
        row(partialsLabel, partialsSlider);
        row(slopeLabel,    slopeSlider);
        row(oddLabel,      oddSlider);
        row(inharmLabel,   inharmSlider);
        row(combLabel,     combSlider);
        row(combPosLabel,  combPosSlider);
        row(attackLabel,   attackSlider);
        row(decayLabel,    decaySlider);
        row(hfDampLabel,   hfDampSlider);
        row(levelLabel,    levelSlider);
        y += 6;

        row(wsLabel,   wsSlider);
        row(lineLabel, lineSlider);

        labelsToggle.setBounds(pad, y, colW, ch);
        y += ch + 3;

        dpiLabel.setBounds(pad, y, 34, ch);
        dpiCombo.setBounds(pad + 34 + gap, y, 90, ch);
        exportPngButton.setBounds(pad + 34 + gap + 90 + gap, y,
                                  (colW - 34 - 2 * gap - 90 - gap) / 2, ch);
        exportJpgButton.setBounds(exportPngButton.getRight() + gap, y,
                                  colW - (exportPngButton.getRight() + gap - pad), ch);
        y += ch + gap + 4;

        // ── Transport bar ────────────────────────────────────────────────────
        {
            const int knobW = 56, knobDrwH = 42, knobValH = 14;
            const int blockH = knobDrwH + knobValH;
            const int btn = 40;
            const int icon = 34;   // loop / inverse pictograms (matches SCORE)

            int x = pad;
            playStopButton.setBounds(x, y + (blockH - btn)  / 2, btn,  btn);  x += btn + gap;
            loopBtn.setBounds      (x, y + (blockH - icon) / 2, icon, icon);  x += icon + 4;
            reverseBtn.setBounds   (x, y + (blockH - icon) / 2, icon, icon);  x += icon + gap;

            const int knobX = pad + colW - knobW;
            speedSlider.setBounds(knobX, y, knobW, blockH);
            speedLabel.setBounds(x, y + (knobDrwH - ch) / 2,
                                 juce::jmax(0, knobX - gap - x), ch);
            y += blockH + gap;
        }
        playHint.setBounds(pad, y, colW, ch); y += ch + 2;

        // Log fills whatever is left under the column.
        logLabel.setBounds(pad, y, colW, juce::jmax(0, getHeight() - pad - y));

        // ── Preview (right of the column) ────────────────────────────────────
        const int previewX = pad + colW + 10;
        previewArea = juce::Rectangle<int>(previewX, contentTop,
                                           juce::jmax(80, getWidth() - previewX - pad),
                                           juce::jmax(80, getHeight() - pad - contentTop));
    }

private:
    //==========================================================================
    /** Square play/stop transport button (same visual language as SCORE's). */
    class TimbrePlayButton : public juce::Button
    {
    public:
        TimbrePlayButton() : juce::Button("timbrePlayStop") {}

        void setPlaying(bool p)
        {
            if (p == playing) return;
            playing = p;
            repaint();
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const juce::Colour bg(0xff222230);
            g.setColour(down ? bg.brighter(0.30f) : over ? bg.brighter(0.12f) : bg);
            g.fillRoundedRectangle(b, 3.f);
            g.setColour(juce::Colour(0xff33373f));
            g.drawRoundedRectangle(b, 3.f, 1.f);

            const auto inner = b.reduced(b.getHeight() * 0.30f);
            if (! isEnabled())
                Icons::fillPath(g, Icons::play(), inner, juce::Colour(0xff555a62));
            else if (playing)
                Icons::fillPath(g, Icons::stop(), inner, juce::Colour(kAccentARGB));
            else
                Icons::fillPath(g, Icons::play(), inner, juce::Colour(0xff66cc88));
        }

    private:
        bool playing = false;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimbrePlayButton)
    };

    /** Compact pictogram toggle for the loop controls — same visual language as
     *  the SCORE page (ScoreIconToggle) and the SAMPLER loop-mode pictograms
     *  (LoopModeButton), so the transport reads identically across pages; only
     *  the accent differs (terracotta = TIMBRE identity).
     *   • Loop    → a "racetrack" loop with a left-pointing arrow (repeat).
     *   • Inverse → the same loop, mirrored (right-pointing arrow) = play backward.
     *  JUCE toggle — APVTS-bound (scoreLoop / scoreReverse), shared with SCORE. */
    class TimbreIconToggle : public juce::Button
    {
    public:
        enum class Glyph { Loop, Inverse };

        explicit TimbreIconToggle(Glyph g) : juce::Button("timbreLoopToggle"), glyph(g)
        {
            setClickingTogglesState(true);
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const bool on = getToggleState() && isEnabled();
            const juce::Colour accent(kAccentARGB); // terracotta (TIMBRE identity)

            const juce::Colour bg = on ? accent.withAlpha(0.22f) : juce::Colour(0xff222230);
            g.setColour(down ? bg.brighter(0.30f) : over ? bg.brighter(0.12f) : bg);
            g.fillRoundedRectangle(b, 3.f);
            g.setColour(on ? accent.withAlpha(0.9f) : juce::Colour(0xff33373f));
            g.drawRoundedRectangle(b, 3.f, 1.f);

            const auto inner = b.reduced(b.getHeight() * 0.22f);
            const juce::Colour fg = on ? accent
                                       : juce::Colour(isEnabled() ? 0xff9aa6ba : 0xff555a62);
            drawLoopGlyph(g, inner, fg, glyph == Glyph::Inverse);
        }

    private:
        /** Stadium (racetrack) loop, open at the top, with an arrow capping the
         *  gap — reads as "repeat". The RING is centred (the arrow head overshoots
         *  its top, so it is EXCLUDED from the centring measurement). @p reversed
         *  mirrors it horizontally (arrow points right) = play backward.
         *  Shared shape with ScoreIconToggle / LoopModeButton. */
        static void drawLoopGlyph(juce::Graphics& g, juce::Rectangle<float> r,
                                  juce::Colour col, bool reversed)
        {
            const float h  = r.getHeight();
            const float th = juce::jmax(2.0f, h * 0.12f);   // stroke thickness

            // Ring built symmetric about r's centre → centred by construction.
            const float ringH  = h * 0.64f;
            const float L = r.getX() + th * 0.6f;
            const float R = r.getRight() - th * 0.6f;
            const float T = r.getCentreY() - ringH * 0.5f;
            const float B = r.getCentreY() + ringH * 0.5f;
            const float radius = (B - T) * 0.5f;
            const float midY   = (T + B) * 0.5f;
            const float topLx  = L + radius;                // top straight: left end
            const float topRx  = R - radius;                // top straight: right end
            const float gx0    = juce::jmap(0.34f, topLx, topRx); // gap (arrow) start
            const float gx1    = juce::jmap(0.66f, topLx, topRx); // gap (arrow) end

            juce::Path loop;
            loop.startNewSubPath(gx1, T);
            loop.lineTo(topRx, T);
            loop.addCentredArc(topRx, midY, radius, radius, 0.0f,
                               0.0f, juce::MathConstants<float>::pi, false);
            loop.lineTo(topLx, B);
            loop.addCentredArc(topLx, midY, radius, radius, 0.0f,
                               juce::MathConstants<float>::pi,
                               juce::MathConstants<float>::twoPi, false);
            loop.lineTo(gx0, T);

            const float aH     = radius * 0.85f;
            const float aTipX  = gx0 - th * 0.25f;
            const float aBackX = gx1 + th * 0.25f;
            juce::Path arrow;
            arrow.addTriangle(aTipX, T, aBackX, T - aH, aBackX, T + aH);

            const auto ringBounds = loop.getBounds().expanded(th * 0.5f);
            const auto offset = r.getCentre() - ringBounds.getCentre();
            const auto move = juce::AffineTransform::translation(offset.x, offset.y);
            loop.applyTransform(move);
            arrow.applyTransform(move);

            if (reversed)
            {
                const auto flip = juce::AffineTransform::scale(-1.0f, 1.0f)
                                      .translated(r.getCentreX() * 2.0f, 0.0f);
                loop.applyTransform(flip);
                arrow.applyTransform(flip);
            }

            g.setColour(col);
            g.strokePath(loop, juce::PathStrokeType(th, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
            g.fillPath(arrow);
        }

        Glyph glyph;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimbreIconToggle)
    };

    /** Top tab for one sound slot: index + preset + note, dimmed when the slot
     *  is disabled, accent frame when selected. */
    class SlotTab : public juce::Button
    {
    public:
        explicit SlotTab(int idx) : juce::Button("timbreSlot"), index(idx) {}

        std::function<juce::String(int)> textProvider;   // set by the page
        std::function<bool(int)>         enabledProvider;
        std::function<bool(int)>         selectedProvider;

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const bool sel = selectedProvider && selectedProvider(index);
            const bool on  = enabledProvider  && enabledProvider(index);
            const juce::Colour accent(kAccentARGB);

            juce::Colour bg = sel ? accent.withAlpha(0.20f) : juce::Colour(0xff1a1d26);
            g.setColour(down ? bg.brighter(0.25f) : over ? bg.brighter(0.10f) : bg);
            g.fillRoundedRectangle(b, 3.f);
            g.setColour(sel ? accent.withAlpha(0.9f) : juce::Colour(0xff33373f));
            g.drawRoundedRectangle(b, 3.f, sel ? 1.4f : 1.f);

            g.setColour(on ? (sel ? accent : juce::Colour(0xffb8c0d0))
                           : juce::Colour(0xff555a62));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            g.drawFittedText(textProvider ? textProvider(index) : juce::String(index + 1),
                             getLocalBounds().reduced(4, 2),
                             juce::Justification::centred, 2);
        }

    private:
        int index;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotTab)
    };

    //==========================================================================
    timbregen::TimbreSlotParams& cur() { return slots[(size_t) selectedSlot]; }

    void initLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    void initSlider(juce::Slider& s, double lo, double hi, double step, double val)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, Sp3ctraTheme::kControlH);
        s.setRange(lo, hi, step);
        s.setValue(val, juce::dontSendNotification);
        addAndMakeVisible(s);
    }

    void selectSlot(int i)
    {
        if (i == selectedSlot) return;
        selectedSlot = juce::jlimit(0, timbregen::kNumSlots - 1, i);
        refreshSlotControls();
        for (auto& t : slotTabs) t->repaint();
        repaint(previewArea);
    }

    /** Pushes the SELECTED slot's params into the widgets (no notifications). */
    void refreshSlotControls()
    {
        const auto& q = cur();
        presetCombo.setSelectedId(q.preset >= 0 ? q.preset + 1
                                                : timbregen::numPresets() + 1,
                                  juce::dontSendNotification);
        activeToggle .setToggleState(q.enabled, juce::dontSendNotification);
        noteSlider   .setValue(q.midiNote,      juce::dontSendNotification);
        partialsSlider.setValue(q.numPartials,  juce::dontSendNotification);
        slopeSlider  .setValue(q.slopeDbPerOct, juce::dontSendNotification);
        oddSlider    .setValue(q.oddBias,       juce::dontSendNotification);
        inharmSlider .setValue(q.inharmonicity, juce::dontSendNotification);
        combSlider   .setValue(q.combDepth,     juce::dontSendNotification);
        combPosSlider.setValue(q.combPos,       juce::dontSendNotification);
        attackSlider .setValue(q.attackMs,      juce::dontSendNotification);
        decaySlider  .setValue(q.decaySec,      juce::dontSendNotification);
        hfDampSlider .setValue(q.hfDamp,        juce::dontSendNotification);
        levelSlider  .setValue(q.levelDb,       juce::dontSendNotification);

        // Bell presets fix their partial set — grey the harmonic-series fields.
        const bool harmonic = ! q.bellMode;
        partialsSlider.setEnabled(harmonic);
        slopeSlider.setEnabled(harmonic);
        oddSlider.setEnabled(harmonic);
        inharmSlider.setEnabled(harmonic);
        combSlider.setEnabled(harmonic);
        combPosSlider.setEnabled(harmonic);
    }

    /** A timbral tweak turns the slot into a hand-tuned "Custom" patch. */
    void becomeCustom()
    {
        if (cur().preset != timbregen::kPresetCustom)
        {
            cur().preset = timbregen::kPresetCustom;
            presetCombo.setSelectedId(timbregen::numPresets() + 1,
                                      juce::dontSendNotification);
            slotTabs[(size_t) selectedSlot]->repaint();
        }
    }

    void markDirty()
    {
        previewDirty = true;
        fullDirty    = true;
        stateDirty   = true;
        lastEditMs   = juce::Time::getMillisecondCounter();
    }

    //==========================================================================
    timbregen::TimbrePageSettings settingsForDpi(double dpi) const
    {
        timbregen::TimbrePageSettings s = pageSettings;
        s.printerDpi = dpi;
        double lo = 0.0, hi = 0.0;
        processor.getScoreFrequencyRange(lo, hi);   // follows the musical tuning
        if (lo > 0.0 && hi > lo) { s.minFreq = lo; s.maxFreq = hi; }
        return s;
    }

    /** X extent of slot i inside the band, as fractions (DPI-independent). */
    static void slotRangeFrac(int i, double& fx0, double& fx1)
    {
        const double labelMarginMM = 150.0 * 25.4 / 400.0;
        const double bandW = SCORE_A4_WIDTH_MM - labelMarginMM;
        const double gap   = 4.0;   // pageSettings.slotGapMM default (display only)
        const double slotW = (bandW - (timbregen::kNumSlots - 1) * gap)
                           / (double) timbregen::kNumSlots;
        fx0 = (i * (slotW + gap)) / bandW;
        fx1 = fx0 + slotW / bandW;
    }

    void regenPreview()
    {
        previewDirty = false;
        constexpr double kPreviewDpi = 150.0;
        const auto r = timbregen::renderTimbrePage(slots, settingsForDpi(kPreviewDpi));
        if (r.ok && r.image.isValid())
        {
            const auto band = r.spectroBand.getIntersection(r.image.getBounds());
            previewImage = band.isEmpty() ? r.image : r.image.getClippedImage(band);
            logLabel.setText(r.log, juce::dontSendNotification);
        }
        else
        {
            previewImage = juce::Image();
            logLabel.setText(r.log, juce::dontSendNotification);
        }
        repaint(previewArea);
    }

    /** Full-resolution page at the export DPI, cached until the next edit. */
    bool ensureFullImage()
    {
        if (fullImage.isValid() && ! fullDirty)
            return true;
        const auto r = timbregen::renderTimbrePage(slots,
                                                   settingsForDpi(pageSettings.printerDpi));
        if (! (r.ok && r.image.isValid()))
        {
            logLabel.setText("Failed: " + r.log, juce::dontSendNotification);
            return false;
        }
        fullImage  = r.image;
        fullBand   = r.spectroBand;
        fullMinFreq = settingsForDpi(pageSettings.printerDpi).minFreq;
        fullMaxFreq = settingsForDpi(pageSettings.printerDpi).maxFreq;
        fullDirty  = false;
        return true;
    }

    //==========================================================================
    void chooseExport(bool asPng)
    {
        if (! ensureFullImage())
            return;
        const juce::String ext = asPng ? "png" : "jpg";
        const juce::File suggested = startDir().getChildFile("timbres_A4." + ext);
        fileChooser = std::make_unique<juce::FileChooser>(
            "Export Timbre Page", suggested, "*." + ext);
        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = juce::Component::SafePointer<TimbreGenTabComponent>(this), asPng, ext]
            (const juce::FileChooser& fc)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                auto dest = fc.getResult();
                if (dest.getFullPathName().isEmpty()) return;
                dest = dest.withFileExtension(ext);
                const bool ok = scoregen::exportImage(self->fullImage, dest, asPng,
                                                      self->pageSettings.printerDpi);
                self->logLabel.setText(ok ? ("Exported: " + dest.getFileName())
                                          : "Export failed",
                                       juce::dontSendNotification);
            });
    }

    juce::File startDir() const
    {
        const auto out = processor.getSamplerOutputDir();
        if (out.isNotEmpty() && juce::File(out).isDirectory())
            return juce::File(out);
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    }

    //==========================================================================
    void togglePlay()
    {
        auto* fs = processor.getLuxSampler();
        if (fs == nullptr) return;

        const bool play = ! (fs->isScorePlaying() && framesAreOurs);

        if (play)
        {
            if (! ensureFullImage())
                return;
            // (Re)load OUR page into the shared score player — replaces whatever
            // SCORE generated last; regenerating there reclaims the channel.
            fs->loadScoreFramesFromImage(fullImage, fullBand,
                                         fullMinFreq, fullMaxFreq, false);
            framesAreOurs    = true;
            loadedFrameCount = fs->getScoreFrameCount();
            scrubHead        = -1;
        }
        else
            scrubHead = -1;

        // Route through the scorePlaying param so the DAW lane stays truthful
        // (same path as the SCORE page — shared transport, shared channel).
        if (auto* p = processor.getAPVTS().getParameter("scorePlaying"))
        {
            const float norm = play ? 1.0f : 0.0f;
            if (! juce::approximatelyEqual(p->getValue(), norm))
                p->setValueNotifyingHost(norm);
            else if (play) fs->uiPlayScore();
            else           fs->uiStopScore();
        }
        repaint(previewArea);
    }

    void scrubTo(const juce::MouseEvent& e)
    {
        auto* fs = processor.getLuxSampler();
        if (fs == nullptr || ! framesAreOurs) return;
        const int n = fs->getScoreFrameCount();
        if (n <= 0) return;
        const auto area = previewImgArea.isEmpty() ? previewImageBounds() : previewImgArea;
        if (area.getWidth() <= 0.f) return;
        const float fx = juce::jlimit(0.f, 1.f,
            ((float) e.position.x - area.getX()) / area.getWidth());
        scrubHead = juce::jlimit(0, n - 1, (int) (fx * (float) n));
        fs->uiSeekScore(scrubHead);
        repaint(previewArea);
    }

    void timerCallback() override
    {
        // Debounced live regeneration (drag-friendly).
        if (previewDirty
            && juce::Time::getMillisecondCounter() - lastEditMs > 200)
        {
            regenPreview();
            persistState();
        }

        auto* fs = processor.getLuxSampler();
        if (fs != nullptr && framesAreOurs)
        {
            // Ownership check: the SCORE page (or a session restore) reloaded the
            // shared channel → our head/scrub display no longer applies.
            if (fs->getScoreFrameCount() != loadedFrameCount)
            {
                framesAreOurs = false;
                scrubHead     = -1;
                repaint(previewArea);
            }
            else
            {
                playStopButton.setPlaying(fs->isScorePlaying());
                if (fs->isScorePlaying())
                    repaint(previewArea);
            }
        }
        else
            playStopButton.setPlaying(false);
    }

    //==========================================================================
    // Persistence — one JSON blob in apvts.state (like scoreEqCurve).
    void persistState()
    {
        stateDirty = false;
        juce::Array<juce::var> arr;
        for (const auto& q : slots)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("en",    q.enabled);
            o->setProperty("preset",q.preset);
            o->setProperty("note",  q.midiNote);
            o->setProperty("part",  q.numPartials);
            o->setProperty("slope", q.slopeDbPerOct);
            o->setProperty("odd",   q.oddBias);
            o->setProperty("inh",   q.inharmonicity);
            o->setProperty("comb",  q.combDepth);
            o->setProperty("cpos",  q.combPos);
            o->setProperty("atk",   q.attackMs);
            o->setProperty("dec",   q.decaySec);
            o->setProperty("hf",    q.hfDamp);
            o->setProperty("lvl",   q.levelDb);
            o->setProperty("bell",  q.bellMode);
            o->setProperty("bellT", q.bellTable);
            arr.add(juce::var(o));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("slots", arr);
        root->setProperty("ws",     pageSettings.writingSpeed);
        root->setProperty("line",   pageSettings.lineWidthMM);
        root->setProperty("dpi",    pageSettings.printerDpi);
        root->setProperty("labels", pageSettings.showLabels);
        processor.getAPVTS().state.setProperty(
            "timbreGenState", juce::JSON::toString(juce::var(root), true), nullptr);
    }

    void restoreState()
    {
        const juce::String blob = processor.getAPVTS().state
            .getProperty("timbreGenState", "").toString();
        if (blob.isEmpty()) return;
        const juce::var root = juce::JSON::parse(blob);
        auto* o = root.getDynamicObject();
        if (o == nullptr) return;

        if (o->hasProperty("ws"))     pageSettings.writingSpeed = (double) o->getProperty("ws");
        if (o->hasProperty("line"))   pageSettings.lineWidthMM  = (double) o->getProperty("line");
        if (o->hasProperty("dpi"))    pageSettings.printerDpi   = (double) o->getProperty("dpi");
        if (o->hasProperty("labels")) pageSettings.showLabels   = (bool)   o->getProperty("labels");

        const auto* arr = o->getProperty("slots").getArray();
        if (arr == nullptr) return;
        for (int i = 0; i < juce::jmin((int) arr->size(), timbregen::kNumSlots); ++i)
        {
            auto* so = (*arr)[i].getDynamicObject();
            if (so == nullptr) continue;
            auto& q = slots[(size_t) i];
            auto get = [&](const char* k, double d)
            { return so->hasProperty(k) ? (double) so->getProperty(k) : d; };
            q.enabled       = (bool) so->getProperty("en");
            q.preset        = (int) get("preset", q.preset);
            q.midiNote      = juce::jlimit(0, 127, (int) get("note", q.midiNote));
            q.numPartials   = juce::jlimit(1, 64, (int) get("part", q.numPartials));
            q.slopeDbPerOct = get("slope", q.slopeDbPerOct);
            q.oddBias       = get("odd",   q.oddBias);
            q.inharmonicity = get("inh",   q.inharmonicity);
            q.combDepth     = get("comb",  q.combDepth);
            q.combPos       = get("cpos",  q.combPos);
            q.attackMs      = get("atk",   q.attackMs);
            q.decaySec      = get("dec",   q.decaySec);
            q.hfDamp        = get("hf",    q.hfDamp);
            q.levelDb       = get("lvl",   q.levelDb);
            q.bellMode      = (bool) so->getProperty("bell");
            q.bellTable     = (int) get("bellT", q.bellTable);
        }
    }

    //==========================================================================
    Sp3ctraAudioProcessor& processor;

    std::array<timbregen::TimbreSlotParams, timbregen::kNumSlots> slots;
    timbregen::TimbrePageSettings pageSettings;
    int selectedSlot = 0;

    std::array<std::unique_ptr<SlotTab>, timbregen::kNumSlots> slotTabs;

    juce::Label    presetLabel, noteLabel, partialsLabel, slopeLabel, oddLabel,
                   inharmLabel, combLabel, combPosLabel, attackLabel, decayLabel,
                   hfDampLabel, levelLabel, wsLabel, lineLabel, dpiLabel,
                   speedLabel, playHint, logLabel;
    juce::ComboBox presetCombo, dpiCombo;
    juce::ToggleButton activeToggle, labelsToggle;
    TimbreIconToggle   loopBtn    { TimbreIconToggle::Glyph::Loop };
    TimbreIconToggle   reverseBtn { TimbreIconToggle::Glyph::Inverse };
    juce::Slider   noteSlider, partialsSlider, slopeSlider, oddSlider, inharmSlider,
                   combSlider, combPosSlider, attackSlider, decaySlider, hfDampSlider,
                   levelSlider, wsSlider, lineSlider, speedSlider;
    juce::TextButton exportPngButton, exportJpgButton;
    TimbrePlayButton playStopButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttach, reverseAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    juce::Rectangle<int>   previewArea;
    juce::Rectangle<float> previewImgArea;
    juce::Image previewImage;            // band crop @150 DPI (live)
    juce::Image fullImage;               // full page @export DPI (on demand)
    juce::Rectangle<int> fullBand;
    double fullMinFreq = 0.0, fullMaxFreq = 0.0;

    bool previewDirty = false, fullDirty = true, stateDirty = false;
    juce::uint32 lastEditMs = 0;

    bool framesAreOurs = false;          // our page currently sits in the score player
    int  loadedFrameCount = 0;
    int  scrubHead = -1;
    bool scrubbing = false, scrubAuditioning = false;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimbreGenTabComponent)
};
