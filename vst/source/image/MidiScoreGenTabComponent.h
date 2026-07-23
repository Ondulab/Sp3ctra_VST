/**
 * @file MidiScoreGenTabComponent.h
 * @brief PLAY page for the MIDI SCORE block — turn a standard MIDI file into
 *        a printable / playable graphical score, one timbre per voice.
 *
 * Port of the offline tool Sp3ctra-Midi-to-Score-Gen, upgraded with the
 * TIMBRE partial model: LOAD a .mid file, its notes are grouped into up to
 * six voices (tracks or channels) and each voice gets a timbre preset
 * (Sine reproduces the original tool's bare fundamental bars; Square,
 * Brass, Bell… print the full partial stack — see MidiScoreGenRenderer).
 *
 * Sibling of ScoreGenTabComponent / TimbreGenTabComponent: same band
 * geometry, same greyscale/dB conventions, same DPI-stamped export
 * (paginated on A4 for long pieces), and the same audition path — the
 * rendered strip is loaded into the shared SCORE player channel
 * (LuxSampler::loadScoreFramesFromImage + uiPlayScore), so the transport
 * params (scoreLoop / scoreReverse / scoreSpeed / scorePlaying) drive it
 * exactly like a generated score. All state persists as JSON in
 * apvts.state ("midiScoreGenState") — offline tool, not host-automatable.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <vector>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../IconPaths.h"
#include "MidiScoreGenRenderer.h"

class MidiScoreGenTabComponent : public juce::Component,
                                 private juce::Timer
{
public:
    static constexpr uint32_t kAccentARGB = 0xffc9a13e;   // bronze (MIDI SCORE identity)
    static constexpr int      kPreferredH = 745;          // +4 vibrato rows

    explicit MidiScoreGenTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        // Every voice defaults to Sine — the original midi_to_sp3ctra behaviour
        // (bare fundamental bars); timbres are an opt-in per voice.
        for (auto& v : voices)
        {
            timbregen::applyPreset(v, 0);
            v.enabled = true;
        }
        restoreState();   // persisted page (and its MIDI file) when present

        // ── MIDI file row ────────────────────────────────────────────────────
        loadButton.setButtonText("Load MIDI...");
        loadButton.onClick = [this] { chooseMidiFile(); };
        addAndMakeVisible(loadButton);

        fileLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        fileLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb8c0d0));
        addAndMakeVisible(fileLabel);
        refreshFileLabel();

        // ── Voice tabs ───────────────────────────────────────────────────────
        for (int i = 0; i < midiscoregen::kMaxVoices; ++i)
        {
            auto tab = std::make_unique<VoiceTab>(i);
            tab->onClick = [this, i] { selectVoice(i); };
            tab->textProvider = [this](int idx)
            {
                const auto& q = voices[(size_t) idx];
                const juce::String name = (idx < data.numVoices)
                    ? data.voiceNames[(size_t) idx] : juce::String("-");
                const juce::String preset = (q.preset >= 0)
                    ? timbregen::presetName(q.preset) : "Custom";
                return name + "\n" + preset;
            };
            tab->enabledProvider = [this](int idx)
            {
                return voices[(size_t) idx].enabled && idx < visibleVoiceCount();
            };
            tab->selectedProvider = [this](int idx) { return idx == selectedVoice; };
            // Visibility follows the loaded file (see updateVoiceTabs) — a
            // single-channel MIDI shows ONE tab, not five dead ones.
            addChildComponent(tab.get());
            voiceTabs[(size_t) i] = std::move(tab);
        }

        // ── Per-voice timbre parameters (same model as TIMBRE) ───────────────
        initLabel(presetLabel, "Timbre");
        for (int i = 0; i < timbregen::numPresets(); ++i)
            presetCombo.addItem(timbregen::presetName(i), i + 1);
        presetCombo.addItem("Custom", timbregen::numPresets() + 1);
        presetCombo.onChange = [this]
        {
            const int id = presetCombo.getSelectedId();
            if (id <= 0 || id > timbregen::numPresets())
                return;   // "Custom" is a display state, not a template
            timbregen::applyPreset(cur(), id - 1);
            refreshVoiceControls();
            voiceTabs[(size_t) selectedVoice]->repaint();
            markDirty();
        };
        addAndMakeVisible(presetCombo);

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
        decaySlider.onValueChange = [this]
        {
            cur().decaySec = decaySlider.getValue();
            // HF damping only multiplies the DECAY rate of upper partials —
            // it has nothing to act on while the notes sustain.
            hfDampSlider.setEnabled(cur().decaySec > 0.0);
            becomeCustom();
            markDirty();
        };

        initLabel(hfDampLabel, "HF damping");
        initSlider(hfDampSlider, 0.0, 1.0, 0.01, 0.5);
        hfDampSlider.setTooltip("Upper partials decay faster. Needs Decay > 0 "
                                "and a multi-partial timbre.");
        timbral(hfDampSlider, [](timbregen::TimbreSlotParams& q, double v) { q.hfDamp = v; });

        initLabel(vibDepthLabel, "Vibrato (cents)");
        initSlider(vibDepthSlider, 0.0, 100.0, 1.0, 0.0);
        vibDepthSlider.setTooltip("Pitch wave of the whole partial stack, "
                                  "peak depth in cents. 0 = off.");
        vibDepthSlider.onValueChange = [this]
        {
            cur().vibCents = vibDepthSlider.getValue();
            // Rate/onset/life shape the wave — nothing to shape at depth 0.
            const bool vib = cur().vibCents > 0.0;
            vibRateSlider .setEnabled(vib);
            vibOnsetSlider.setEnabled(vib);
            vibLifeSlider .setEnabled(vib);
            becomeCustom();
            markDirty();
        };

        initLabel(vibRateLabel, "Vib rate (Hz)");
        initSlider(vibRateSlider, 0.5, 10.0, 0.1, 5.5);
        timbral(vibRateSlider, [](timbregen::TimbreSlotParams& q, double v) { q.vibRateHz = v; });

        initLabel(vibOnsetLabel, "Vib onset (s)");
        initSlider(vibOnsetSlider, 0.0, 2.0, 0.05, 0.4);
        vibOnsetSlider.setTooltip("Time for the vibrato to develop after the "
                                  "note starts (delay + smooth rise).");
        timbral(vibOnsetSlider, [](timbregen::TimbreSlotParams& q, double v) { q.vibOnsetSec = v; });

        initLabel(vibLifeLabel, "Vib life");
        initSlider(vibLifeSlider, 0.0, 1.0, 0.01, 0.5);
        vibLifeSlider.setTooltip("Humanisation: the depth waves, the rate drifts, "
                                 "and every note gets its own phase / rate / depth "
                                 "defects. 0 = mechanical sine, 1 = loose.");
        timbral(vibLifeSlider, [](timbregen::TimbreSlotParams& q, double v) { q.vibLife = v; });

        initLabel(levelLabel, "Level (dB)");
        initSlider(levelSlider, -24.0, 6.0, 0.1, 0.0);
        levelSlider.setTooltip("Ink / playback gain of this voice "
                               "(0 dB = full black at maximum velocity).");
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

        initLabel(velLabel, "Velocity range (dB)");
        initSlider(velSlider, 0.0, 50.0, 0.5, pageSettings.velocityRangeDb);
        velSlider.setTooltip("Ink depth of the velocity: 127 prints at full level, "
                             "velocity 1 prints this many dB below.");
        velSlider.onValueChange = [this]
        {
            pageSettings.velocityRangeDb = velSlider.getValue();
            markDirty();
        };

        labelsToggle.setButtonText("Title + footer (top margin)");
        labelsToggle.setTooltip("Print the file name, page number and the reproduction "
                                "footer at the very top of each page. Off = clean page.");
        labelsToggle.setToggleState(pageSettings.showLabels, juce::dontSendNotification);
        labelsToggle.onClick = [this]
        {
            pageSettings.showLabels = labelsToggle.getToggleState();
            stateDirty = true;   // export-only — preview/playback unchanged
        };
        addAndMakeVisible(labelsToggle);

        initLabel(dpiLabel, "DPI");
        for (int d : { 200, 300, 400, 600, 800 })
            dpiCombo.addItem(juce::String(d), d);
        dpiCombo.setSelectedId((int) pageSettings.printerDpi, juce::dontSendNotification);
        dpiCombo.onChange = [this]
        {
            pageSettings.printerDpi = (double) juce::jmax(72, dpiCombo.getSelectedId());
            stateDirty = true;      // export resolution only — preview unchanged
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
        playStopButton.setTooltip("Play / stop the piece through the score player");
        playStopButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playStopButton);

        loopBtn.setTooltip("Loop playback");
        addAndMakeVisible(loopBtn);
        loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "scoreLoop", loopBtn);

        reverseBtn.setTooltip("Reverse (play the piece backward)");
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

        playHint.setText("PLAY loads the piece into the score player "
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
        if (data.ok)
            logLabel.setText(data.log, juce::dontSendNotification);

        refreshVoiceControls();
        updateVoiceTabs();
        regenPreview();
        startTimerHz(20);
    }

    ~MidiScoreGenTabComponent() override
    {
        stopTimer();
        if (stateDirty)
            persistState();
        // Like SCORE/TIMBRE: the page is a VIEW — closing it must not cut audio.
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

            // Reading head — only meaningful while OUR frames sit in the player.
            auto* fs = boundChannel();
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
            g.drawText(data.ok ? "No notes in this MIDI file"
                               : "Load a MIDI file to generate its score",
                       previewArea, juce::Justification::centred);
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
    // Preview interactions: click = scrub (when our frames are loaded — same
    // behaviour as the SCORE and TIMBRE pages).
    void mouseDown(const juce::MouseEvent& e) override
    {
        scrubbing = framesAreOurs && previewImage.isValid()
                 && previewArea.contains(e.getPosition());
        if (! scrubbing) return;
        scrubTo(e);
        if (auto* fs = boundChannel())
            if (! fs->isScorePlaying())
                scrubAuditioning = fs->uiBeginScoreScrub();
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (scrubbing) scrubTo(e); }
    void mouseUp  (const juce::MouseEvent&)   override
    {
        scrubbing = false;
        if (scrubAuditioning)
        {
            if (auto* fs = boundChannel()) fs->uiEndScoreScrub();
            scrubAuditioning = false;
        }
    }

    void resized() override
    {
        const int pad = 8;
        const int ch  = Sp3ctraTheme::kControlH;
        const int gap = 6;

        // ── File row across the top ──────────────────────────────────────────
        int y = pad;
        loadButton.setBounds(pad, y, 110, ch);
        fileLabel.setBounds(pad + 110 + gap, y, getWidth() - pad * 2 - 110 - gap, ch);
        y += ch + gap;

        // ── Voice tabs (only the file's voices are visible — left-aligned,
        //     constant tab size so 1 voice doesn't stretch across the page) ───
        const int tabH = 36;
        {
            const int w = (getWidth() - 2 * pad - (midiscoregen::kMaxVoices - 1) * 4)
                        / midiscoregen::kMaxVoices;
            int x = pad;
            for (auto& t : voiceTabs)
            {
                if (! t->isVisible())
                    continue;
                t->setBounds(x, y, w, tabH);
                x += w + 4;
            }
        }
        const int contentTop = y + tabH + gap + 2;

        // ── Control column (left) ────────────────────────────────────────────
        const int colW = juce::jmin(340, getWidth() - 2 * pad);
        const int lblW = 130;
        y = contentTop;

        auto row = [&](juce::Label& l, juce::Component& c)
        {
            l.setBounds(pad, y, lblW, ch);
            c.setBounds(pad + lblW + gap, y, colW - lblW - gap, ch);
            y += ch + 3;
        };

        presetLabel.setBounds(pad, y, 60, ch);
        presetCombo.setBounds(pad + 60 + gap, y, colW - 60 - gap, ch);
        y += ch + gap;

        row(partialsLabel, partialsSlider);
        row(slopeLabel,    slopeSlider);
        row(oddLabel,      oddSlider);
        row(inharmLabel,   inharmSlider);
        row(combLabel,     combSlider);
        row(combPosLabel,  combPosSlider);
        row(attackLabel,   attackSlider);
        row(decayLabel,    decaySlider);
        row(hfDampLabel,   hfDampSlider);
        row(vibDepthLabel, vibDepthSlider);
        row(vibRateLabel,  vibRateSlider);
        row(vibOnsetLabel, vibOnsetSlider);
        row(vibLifeLabel,  vibLifeSlider);
        row(levelLabel,    levelSlider);
        y += 6;

        row(wsLabel,   wsSlider);
        row(lineLabel, lineSlider);
        row(velLabel,  velSlider);

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
    class MidiScorePlayButton : public juce::Button
    {
    public:
        MidiScorePlayButton() : juce::Button("midiScorePlayStop") {}

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
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiScorePlayButton)
    };

    /** Compact loop/inverse pictogram toggle — same glyph as the SCORE page
     *  (ScoreIconToggle) and the TIMBRE page, only the accent differs. */
    class MidiScoreIconToggle : public juce::Button
    {
    public:
        enum class Glyph { Loop, Inverse };

        explicit MidiScoreIconToggle(Glyph g) : juce::Button("midiScoreLoopToggle"), glyph(g)
        {
            setClickingTogglesState(true);
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const bool on = getToggleState() && isEnabled();
            const juce::Colour accent(kAccentARGB);

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
        /** Stadium (racetrack) loop, open at the top, arrow capping the gap —
         *  shared shape with ScoreIconToggle / TimbreIconToggle. */
        static void drawLoopGlyph(juce::Graphics& g, juce::Rectangle<float> r,
                                  juce::Colour col, bool reversed)
        {
            const float h  = r.getHeight();
            const float th = juce::jmax(2.0f, h * 0.12f);   // stroke thickness

            const float ringH  = h * 0.64f;
            const float L = r.getX() + th * 0.6f;
            const float R = r.getRight() - th * 0.6f;
            const float T = r.getCentreY() - ringH * 0.5f;
            const float B = r.getCentreY() + ringH * 0.5f;
            const float radius = (B - T) * 0.5f;
            const float midY   = (T + B) * 0.5f;
            const float topLx  = L + radius;
            const float topRx  = R - radius;
            const float gx0    = juce::jmap(0.34f, topLx, topRx);
            const float gx1    = juce::jmap(0.66f, topLx, topRx);

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
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiScoreIconToggle)
    };

    /** Top tab for one voice: track/channel name + timbre preset, dimmed when
     *  the voice is disabled or absent from the loaded file. */
    class VoiceTab : public juce::Button
    {
    public:
        explicit VoiceTab(int idx) : juce::Button("midiScoreVoice"), index(idx) {}

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
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceTab)
    };

    //==========================================================================
    timbregen::TimbreSlotParams& cur() { return voices[(size_t) selectedVoice]; }

    /** Tabs worth showing: the loaded file's voices (≥1 so the timbre editor
     *  always has a target — voice 1 — before any file is loaded). */
    int visibleVoiceCount() const
    {
        return data.ok ? juce::jmax(1, data.numVoices) : 1;
    }

    /** Re-syncs tab visibility/selection with the loaded file: hides the tabs
     *  beyond the file's voice count and pulls the selection back in range. */
    void updateVoiceTabs()
    {
        const int n = visibleVoiceCount();
        if (selectedVoice >= n)
        {
            selectedVoice = 0;
            refreshVoiceControls();
        }
        for (int i = 0; i < midiscoregen::kMaxVoices; ++i)
            voiceTabs[(size_t) i]->setVisible(i < n);
        resized();
        for (auto& t : voiceTabs)
            t->repaint();
    }

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

    void selectVoice(int i)
    {
        if (i == selectedVoice) return;
        selectedVoice = juce::jlimit(0, midiscoregen::kMaxVoices - 1, i);
        refreshVoiceControls();
        for (auto& t : voiceTabs) t->repaint();
    }

    /** Pushes the SELECTED voice's params into the widgets (no notifications). */
    void refreshVoiceControls()
    {
        const auto& q = cur();
        presetCombo.setSelectedId(q.preset >= 0 ? q.preset + 1
                                                : timbregen::numPresets() + 1,
                                  juce::dontSendNotification);
        partialsSlider.setValue(q.numPartials,  juce::dontSendNotification);
        slopeSlider  .setValue(q.slopeDbPerOct, juce::dontSendNotification);
        oddSlider    .setValue(q.oddBias,       juce::dontSendNotification);
        inharmSlider .setValue(q.inharmonicity, juce::dontSendNotification);
        combSlider   .setValue(q.combDepth,     juce::dontSendNotification);
        combPosSlider.setValue(q.combPos,       juce::dontSendNotification);
        attackSlider .setValue(q.attackMs,      juce::dontSendNotification);
        decaySlider  .setValue(q.decaySec,      juce::dontSendNotification);
        hfDampSlider .setValue(q.hfDamp,        juce::dontSendNotification);
        vibDepthSlider.setValue(q.vibCents,     juce::dontSendNotification);
        vibRateSlider .setValue(q.vibRateHz,    juce::dontSendNotification);
        vibOnsetSlider.setValue(q.vibOnsetSec,  juce::dontSendNotification);
        vibLifeSlider .setValue(q.vibLife,      juce::dontSendNotification);
        levelSlider  .setValue(q.levelDb,       juce::dontSendNotification);

        // Bell presets fix their partial set — grey the harmonic-series fields.
        const bool harmonic = ! q.bellMode;
        partialsSlider.setEnabled(harmonic);
        slopeSlider.setEnabled(harmonic);
        oddSlider.setEnabled(harmonic);
        inharmSlider.setEnabled(harmonic);
        combSlider.setEnabled(harmonic);
        combPosSlider.setEnabled(harmonic);
        // HF damping is a decay-rate multiplier — inert on sustained notes.
        hfDampSlider.setEnabled(q.decaySec > 0.0);
        // Vibrato works in both harmonic and bell modes; its shape controls
        // only matter once there is a depth to shape.
        const bool vib = q.vibCents > 0.0;
        vibRateSlider .setEnabled(vib);
        vibOnsetSlider.setEnabled(vib);
        vibLifeSlider .setEnabled(vib);
    }

    /** A timbral tweak turns the voice into a hand-tuned "Custom" patch. */
    void becomeCustom()
    {
        if (cur().preset != timbregen::kPresetCustom)
        {
            cur().preset = timbregen::kPresetCustom;
            presetCombo.setSelectedId(timbregen::numPresets() + 1,
                                      juce::dontSendNotification);
            voiceTabs[(size_t) selectedVoice]->repaint();
        }
    }

    void markDirty()
    {
        previewDirty = true;
        playDirty    = true;
        stateDirty   = true;
        lastEditMs   = juce::Time::getMillisecondCounter();
    }

    //==========================================================================
    midiscoregen::MidiScoreSettings settingsWithTuning() const
    {
        midiscoregen::MidiScoreSettings s = pageSettings;
        double lo = 0.0, hi = 0.0;
        processor.getScoreFrequencyRange(lo, hi);   // follows the musical tuning
        if (lo > 0.0 && hi > lo) { s.minFreq = lo; s.maxFreq = hi; }
        return s;
    }

    //==========================================================================
    void chooseMidiFile()
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Load MIDI File",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.mid;*.midi");
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles,
            [safe = juce::Component::SafePointer<MidiScoreGenTabComponent>(this)]
            (const juce::FileChooser& fc)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                const auto f = fc.getResult();
                if (f.getFullPathName().isEmpty()) return;
                self->loadMidiFile(f);
            });
    }

    void loadMidiFile(const juce::File& f)
    {
        data = midiscoregen::parseMidiFile(f);
        logLabel.setText(data.ok ? data.log : ("Failed: " + data.error),
                         juce::dontSendNotification);
        refreshFileLabel();
        updateVoiceTabs();
        markDirty();
        regenPreview();
        persistState();
    }

    void refreshFileLabel()
    {
        if (! data.ok)
        {
            fileLabel.setText("No MIDI file loaded", juce::dontSendNotification);
            return;
        }
        const auto s = settingsWithTuning();
        const int pages = midiscoregen::pageCount(data, s);
        fileLabel.setText(juce::File(data.sourcePath).getFileName()
                          + juce::String::fromUTF8("  —  ") + juce::String((int) data.notes.size()) + " notes, "
                          + juce::String(data.numVoices) + " voice(s), "
                          + juce::String(data.durationSec, 1) + " s, "
                          + juce::String(pages) + " page(s) A4",
                          juce::dontSendNotification);
    }

    //==========================================================================
    void regenPreview()
    {
        previewDirty = false;
        if (! data.ok || data.notes.empty())
        {
            previewImage = juce::Image();
            repaint(previewArea);
            return;
        }
        // Overview strip: whole piece, width-capped, reduced frequency axis.
        constexpr double kPreviewDpiY   = 150.0;
        constexpr double kPreviewMaxPxW = 3600.0;
        const double dur = juce::jmax(0.1, data.durationSec);
        const double pxPerSec = juce::jmin(400.0, kPreviewMaxPxW / dur);
        const auto r = midiscoregen::renderStrip(data, voices, settingsWithTuning(),
                                                 0.0, dur, pxPerSec, kPreviewDpiY);
        previewImage = (r.ok && r.image.isValid()) ? r.image : juce::Image();
        if (! r.ok)
            logLabel.setText(r.log, juce::dontSendNotification);
        repaint(previewArea);
    }

    //==========================================================================
    // Playback: render the whole piece as one strip at the physical time scale
    // (DPI × writing speed), capped in frame count for very long pieces, and
    // load it into the shared score player. The player injects 1000 columns/s
    // at speed 1x, so "real tempo" ≈ (px/s ÷ 1000) on the Speed knob — logged.
    static constexpr int kMaxPlayFrames = 30000;   // ≈ 310 MB of frames

    bool reloadPlayFrames()
    {
        auto* fs = boundChannel();
        if (fs == nullptr || ! data.ok || data.notes.empty())
            return false;

        const auto s = settingsWithTuning();
        const double dur = juce::jmax(0.05, data.durationSec);
        double pxPerSec = (s.printerDpi / 2.54) * s.writingSpeed;
        bool reduced = false;
        if (dur * pxPerSec > (double) kMaxPlayFrames)
        {
            pxPerSec = (double) kMaxPlayFrames / dur;   // long piece: coarser time grid
            reduced  = true;
        }

        const auto r = midiscoregen::renderStrip(data, voices, s, 0.0, dur,
                                                 pxPerSec, 400.0);   // full CIS height
        if (! (r.ok && r.image.isValid()))
        {
            logLabel.setText("Failed: " + r.log, juce::dontSendNotification);
            return false;
        }

        fs->loadScoreFramesFromImage(r.image, r.spectroBand, s.minFreq, s.maxFreq, false);
        framesAreOurs    = true;
        loadedFrameCount = fs->getScoreFrameCount();
        scrubHead        = -1;
        playDirty        = false;

        juce::String msg = juce::String(loadedFrameCount) + juce::String::fromUTF8(" frames loaded — ")
                         + "real tempo at Speed " + juce::String(pxPerSec / 1000.0, 2) + "x";
        if (reduced)
            msg += " (long piece: time grid reduced to "
                 + juce::String(pxPerSec, 0) + " px/s)";
        logLabel.setText(msg, juce::dontSendNotification);
        return true;
    }

    //==========================================================================
    // Live audition: a param tweak while OUR piece is playing re-renders the
    // strip on a background thread (renderStrip is pure/thread-safe) and
    // hot-swaps the player's frames at the same musical position — no
    // STOP/PLAY round-trip to hear the change. Debounced above the preview's
    // 200 ms so a slider drag settles before the full-height render fires.
    static constexpr juce::uint32 kLiveReloadDebounceMs = 400;

    void maybeStartLiveReload()
    {
        if (! playDirty || liveRenderBusy_ || ! framesAreOurs
            || ! data.ok || data.notes.empty())
            return;
        if (juce::Time::getMillisecondCounter() - lastEditMs < kLiveReloadDebounceMs)
            return;
        auto* fs = boundChannel();
        if (fs == nullptr || ! fs->isScorePlaying()
            || fs->getScoreFrameCount() != loadedFrameCount)
            return;   // stopped or channel reclaimed → next PLAY reloads

        const auto s = settingsWithTuning();
        const double dur = juce::jmax(0.05, data.durationSec);
        double pxPerSec = (s.printerDpi / 2.54) * s.writingSpeed;
        if (dur * pxPerSec > (double) kMaxPlayFrames)
            pxPerSec = (double) kMaxPlayFrames / dur;

        playDirty       = false;   // edits landing during the render re-arm it
        liveRenderBusy_ = true;

        juce::Thread::launch(
            [safe = juce::Component::SafePointer<MidiScoreGenTabComponent>(this),
             dataCopy = data, voicesCopy = voices, s, dur, pxPerSec]
            {
                auto r = midiscoregen::renderStrip(dataCopy, voicesCopy, s,
                                                   0.0, dur, pxPerSec, 400.0);
                juce::MessageManager::callAsync(
                    [safe, r = std::move(r), lo = s.minFreq, hi = s.maxFreq]
                    {
                        if (auto* self = safe.getComponent())
                            self->applyLiveReload(r, lo, hi);
                    });
            });
    }

    void applyLiveReload(const scoregen::RenderResult& r,
                         double minFreq, double maxFreq)
    {
        liveRenderBusy_ = false;
        auto* fs = boundChannel();
        // The transport stopped or the channel changed hands (other page /
        // instance rebind) during the render — don't hot-swap a player that
        // is no longer ours; re-arm so the next PLAY reloads instead.
        if (fs == nullptr || ! framesAreOurs || ! fs->isScorePlaying()
            || fs->getScoreFrameCount() != loadedFrameCount)
        {
            playDirty = true;
            return;
        }
        if (! (r.ok && r.image.isValid()))
        {
            logLabel.setText("Failed: " + r.log, juce::dontSendNotification);
            return;
        }

        // Same recipe as the SCORE page's EQ hot-reload: capture the head
        // BEFORE the load (it stops playback and zeroes it) and resume at the
        // same musical position — as a fraction, because the frame count
        // moves with the writing speed.
        const int oldCount  = juce::jmax(1, fs->getScoreFrameCount());
        const int savedHead = fs->getScorePlayHead();
        fs->loadScoreFramesFromImage(r.image, r.spectroBand, minFreq, maxFreq, false);
        const int newCount = fs->getScoreFrameCount();
        if (newCount <= 0)
        {
            framesAreOurs = false;
            return;
        }
        const int head = juce::jlimit(0, newCount - 1,
            juce::roundToInt((double) savedHead * newCount / oldCount));
        fs->setScoreResumeHead(head);
        fs->uiPlayScore();
        loadedFrameCount = newCount;
        logLabel.setText(juce::String::fromUTF8("Live update — ")
                             + juce::String(newCount) + " frames",
                         juce::dontSendNotification);
        repaint(previewArea);
    }

    void togglePlay()
    {
        auto* fs = boundChannel();
        if (fs == nullptr) return;

        const bool play = ! (fs->isScorePlaying() && framesAreOurs);

        if (play)
        {
            // (Re)load OUR piece into the shared score player — replaces whatever
            // SCORE/TIMBRE generated last; regenerating there reclaims the channel.
            if ((! framesAreOurs || playDirty) && ! reloadPlayFrames())
                return;
            scrubHead = -1;
        }
        else
            scrubHead = -1;

        // P5-M4: drive THIS instance's own player slot directly. The
        // scorePlaying param now belongs to the SCORE module's transport —
        // routing through it here would start the WRONG player (per-instance
        // automatable transports are M5).
        if (play) fs->uiPlayScore();
        else      fs->uiStopScore();
        repaint(previewArea);
    }

    void scrubTo(const juce::MouseEvent& e)
    {
        auto* fs = boundChannel();
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
            refreshFileLabel();   // page count follows the writing speed
            persistState();
        }

        // Longer debounce than the preview: full-height re-render + hot-swap
        // of the playing frames, only while OUR piece is audible.
        maybeStartLiveReload();

        auto* fs = boundChannel();
        if (fs != nullptr && framesAreOurs)
        {
            // Ownership check: the SCORE/TIMBRE page (or a session restore)
            // reloaded the shared channel → our head display no longer applies.
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
    // Export: one A4 page per pageSeconds() window; multi-page pieces save
    // numbered siblings ("name_p01.png", "name_p02.png"…).
    void chooseExport(bool asPng)
    {
        if (! data.ok || data.notes.empty())
        {
            logLabel.setText("Load a MIDI file first", juce::dontSendNotification);
            return;
        }
        const auto s = settingsWithTuning();
        const int nPages = midiscoregen::pageCount(data, s);
        if (nPages <= 0)
        {
            logLabel.setText("Nothing to export", juce::dontSendNotification);
            return;
        }
        if (nPages > 60)
        {
            logLabel.setText("Too many pages (" + juce::String(nPages)
                             + juce::String::fromUTF8(") — raise the writing speed"),
                             juce::dontSendNotification);
            return;
        }

        const juce::String ext = asPng ? "png" : "jpg";
        const juce::String base = juce::File(data.sourcePath)
                                      .getFileNameWithoutExtension() + "_score";
        const juce::File suggested = startDir().getChildFile(base + "." + ext);
        fileChooser = std::make_unique<juce::FileChooser>(
            "Export MIDI Score", suggested, "*." + ext);
        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = juce::Component::SafePointer<MidiScoreGenTabComponent>(this),
             asPng, ext, nPages]
            (const juce::FileChooser& fc)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                auto dest = fc.getResult();
                if (dest.getFullPathName().isEmpty()) return;
                dest = dest.withFileExtension(ext);
                self->exportPages(dest, asPng, ext, nPages);
            });
    }

    void exportPages(const juce::File& dest, bool asPng,
                     const juce::String& ext, int nPages)
    {
        const auto s = settingsWithTuning();
        int written = 0;
        for (int k = 0; k < nPages; ++k)
        {
            const auto r = midiscoregen::renderPage(data, voices, s, k);
            if (! (r.ok && r.image.isValid()))
            {
                logLabel.setText("Failed: " + r.log, juce::dontSendNotification);
                return;
            }
            const juce::File out = (nPages == 1)
                ? dest
                : dest.getSiblingFile(dest.getFileNameWithoutExtension()
                        + "_p" + juce::String(k + 1).paddedLeft('0', 2) + "." + ext);
            if (! scoregen::exportImage(r.image, out, asPng, s.printerDpi))
            {
                logLabel.setText("Export failed: " + out.getFileName(),
                                 juce::dontSendNotification);
                return;
            }
            ++written;
        }
        logLabel.setText("Exported " + juce::String(written) + " page(s): "
                         + dest.getFileName(), juce::dontSendNotification);
    }

    juce::File startDir() const
    {
        const auto out = processor.getSamplerOutputDir();
        if (out.isNotEmpty() && juce::File(out).isDirectory())
            return juce::File(out);
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    }

    //==========================================================================
    // Persistence — one JSON blob in apvts.state (like timbreGenState).
    void persistState()
    {
        stateDirty = false;
        juce::Array<juce::var> arr;
        for (const auto& q : voices)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("en",    q.enabled);
            o->setProperty("preset",q.preset);
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
            o->setProperty("vibC",  q.vibCents);
            o->setProperty("vibR",  q.vibRateHz);
            o->setProperty("vibO",  q.vibOnsetSec);
            o->setProperty("vibL",  q.vibLife);
            arr.add(juce::var(o));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty("voices", arr);
        root->setProperty("file",   data.ok ? data.sourcePath : juce::String());
        root->setProperty("ws",     pageSettings.writingSpeed);
        root->setProperty("line",   pageSettings.lineWidthMM);
        root->setProperty("vel",    pageSettings.velocityRangeDb);
        root->setProperty("dpi",    pageSettings.printerDpi);
        root->setProperty("labels", pageSettings.showLabels);
        processor.getAPVTS().state.setProperty(
            "midiScoreGenState", juce::JSON::toString(juce::var(root), true), nullptr);
    }

    void restoreState()
    {
        const juce::String blob = processor.getAPVTS().state
            .getProperty("midiScoreGenState", "").toString();
        if (blob.isEmpty()) return;
        const juce::var root = juce::JSON::parse(blob);
        auto* o = root.getDynamicObject();
        if (o == nullptr) return;

        if (o->hasProperty("ws"))     pageSettings.writingSpeed    = (double) o->getProperty("ws");
        if (o->hasProperty("line"))   pageSettings.lineWidthMM     = (double) o->getProperty("line");
        if (o->hasProperty("vel"))    pageSettings.velocityRangeDb = (double) o->getProperty("vel");
        if (o->hasProperty("dpi"))    pageSettings.printerDpi      = (double) o->getProperty("dpi");
        if (o->hasProperty("labels")) pageSettings.showLabels      = (bool)   o->getProperty("labels");

        if (const auto* arr = o->getProperty("voices").getArray())
        {
            for (int i = 0; i < juce::jmin((int) arr->size(), midiscoregen::kMaxVoices); ++i)
            {
                auto* so = (*arr)[i].getDynamicObject();
                if (so == nullptr) continue;
                auto& q = voices[(size_t) i];
                auto get = [&](const char* k, double d)
                { return so->hasProperty(k) ? (double) so->getProperty(k) : d; };
                // "en" is ignored: the per-voice Active toggle is gone, every
                // voice always prints/plays (a state saved before its removal
                // must not leave a voice muted with no UI to unmute it).
                q.preset        = (int) get("preset", q.preset);
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
                q.vibCents      = get("vibC",  q.vibCents);
                q.vibRateHz     = get("vibR",  q.vibRateHz);
                q.vibOnsetSec   = get("vibO",  q.vibOnsetSec);
                q.vibLife       = get("vibL",  q.vibLife);
            }
        }

        // Reload the persisted MIDI file (best effort — it may have moved).
        const juce::String path = o->getProperty("file").toString();
        if (path.isNotEmpty())
        {
            const juce::File f(path);
            if (f.existsAsFile())
                data = midiscoregen::parseMidiFile(f);
        }
    }

    //==========================================================================
public:
    /** P5-M5 — bind this page to the SELECTED instance's player slot
     *  (-1 = first placed instance of this page's type). Ends any running
     *  scrub on the previously bound channel; the new instance's frames are
     *  (re)loaded on demand by the next PLAY/GENERATE. */
    void setScoreSlot(int slot)
    {
        if (slot == boundScoreSlot_)
            return;
        if (scrubAuditioning)
            if (auto* fs = boundChannel())
                fs->uiEndScoreScrub();
        scrubAuditioning = false;
        framesAreOurs    = false;
        loadedFrameCount = 0;
        scrubHead        = -1;
        scrubbing        = false;
        boundScoreSlot_  = slot;
        repaint();
    }

private:
    /** The bound score channel: the selected instance's slot while it is
     *  still in the rack, else the first placed instance of this type. */
    ScoreChannel* boundChannel() const
    {
        if (boundScoreSlot_ >= 0
            && processor.scorePlayerSlotInUse(boundScoreSlot_))
            return processor.getScoreChannelForSlot(boundScoreSlot_);
        return processor.getScoreChannel(ModuleType::MidiScore);
    }
    int boundScoreSlot_ = -1;

    Sp3ctraAudioProcessor& processor;

    midiscoregen::MidiScoreData data;                 // parsed file (ok=false when none)
    std::array<timbregen::TimbreSlotParams, midiscoregen::kMaxVoices> voices;
    midiscoregen::MidiScoreSettings pageSettings;
    int selectedVoice = 0;

    std::array<std::unique_ptr<VoiceTab>, midiscoregen::kMaxVoices> voiceTabs;

    juce::Label    presetLabel, partialsLabel, slopeLabel, oddLabel,
                   inharmLabel, combLabel, combPosLabel, attackLabel, decayLabel,
                   hfDampLabel, vibDepthLabel, vibRateLabel, vibOnsetLabel,
                   vibLifeLabel, levelLabel, wsLabel, lineLabel, velLabel, dpiLabel,
                   speedLabel, playHint, logLabel, fileLabel;
    juce::ComboBox presetCombo, dpiCombo;
    juce::ToggleButton labelsToggle;
    MidiScoreIconToggle loopBtn    { MidiScoreIconToggle::Glyph::Loop };
    MidiScoreIconToggle reverseBtn { MidiScoreIconToggle::Glyph::Inverse };
    juce::Slider   partialsSlider, slopeSlider, oddSlider, inharmSlider,
                   combSlider, combPosSlider, attackSlider, decaySlider, hfDampSlider,
                   vibDepthSlider, vibRateSlider, vibOnsetSlider, vibLifeSlider,
                   levelSlider, wsSlider, lineSlider, velSlider, speedSlider;
    juce::TextButton loadButton, exportPngButton, exportJpgButton;
    MidiScorePlayButton playStopButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttach, reverseAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    juce::Rectangle<int>   previewArea;
    juce::Rectangle<float> previewImgArea;
    juce::Image previewImage;            // whole-piece overview strip (live)

    bool previewDirty = false, playDirty = true, stateDirty = false;
    bool liveRenderBusy_ = false;        // one background strip render at a time
    juce::uint32 lastEditMs = 0;

    bool framesAreOurs = false;          // our piece currently sits in the score player
    int  loadedFrameCount = 0;
    int  scrubHead = -1;
    bool scrubbing = false, scrubAuditioning = false;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiScoreGenTabComponent)
};
