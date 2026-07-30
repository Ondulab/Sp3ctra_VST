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
 * (PNG/JPEG, A4/A3/FULL sheet and DPI on the SETUP face — pieces longer
 * than one page frame their export zone in a picker window; ALL PAGES keeps
 * the numbered paginated export), and the same audition path — the
 * rendered strip is loaded into the shared SCORE player channel
 * (LuxSampler::loadScoreFramesFromImage + uiPlayScore), so the transport
 * params (scoreLoop / scoreReverse / scoreSpeed / scorePlaying) drive it
 * exactly like a generated score. All state persists as JSON in
 * apvts.state ("midiScoreGenState") — offline tool, not host-automatable.
 *
 * PLAYBACK SHAPING (SAMPLER-style, playback-only — exports stay clean):
 * a crop window (green start / orange end bars dragged on the preview, or
 * the chips under it), edge fades (LIN/EXP/LOG/S curve + power, same
 * handles and chips as the SAMPLER slot editor) and a SCORE-style IMAGE EQ
 * shape what the score player receives: PLAY renders only the crop window,
 * then the fades darken→silence the edges and the EQ shifts each band
 * row's ink in dB, before the frames are loaded. The preview keeps showing
 * the raw piece with the shaping drawn as an overlay.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../IconPaths.h"
#include "../licensing/ActivationDialog.h"
#include "../luxsampler/FadeCurve.h"       // fade curve shapes (LIN/EXP/LOG/S)
#include "../sampler/SamplerValueBox.h"    // crop / fade param chips under the preview
#include "MidiScoreGenRenderer.h"
#include "ScoreEqComponent.h"
#include "EqCurve.h"                       // shared Catmull-Rom EQ evaluator

class MidiScoreGenTabComponent : public juce::Component,
                                 private juce::Timer,
                                 private juce::ScrollBar::Listener
{
public:
    static constexpr uint32_t kAccentARGB = 0xffc9a13e;   // bronze (MIDI SCORE identity)
    static constexpr int      kPreferredH = 920;          // + crop/fade chips + IMAGE EQ

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
            tab->soloProvider = [this](int idx)
            {
                return voiceSolo[(size_t) idx] && idx < visibleVoiceCount();
            };
            tab->audibleProvider = [this](int idx)
            {
                return idx < visibleVoiceCount() && voiceAudible(idx);
            };
            tab->selectedProvider = [this](int idx) { return idx == selectedVoice; };
            tab->onToggle = [this](int idx)
            {
                auto& q = voices[(size_t) idx];
                q.enabled = ! q.enabled;
                voiceTabs[(size_t) idx]->repaint();
                markDirty();   // preview + live playback re-render, state saved
            };
            tab->onSolo = [this](int idx)
            {
                voiceSolo[(size_t) idx] = ! voiceSolo[(size_t) idx];
                for (auto& t : voiceTabs)
                    t->repaint();   // solo dims every OTHER tab too
                markDirty();
            };
            tab->setTooltip("Dot = mute/unmute. S = solo: only soloed voices "
                            "play/print (solo wins over mute).");
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

        // ── Export — format / sheet size / DPI live on the SETUP face; long
        //    pieces open the zone picker so the exported window is framed.
        exportButton.setButtonText("Export image");
        exportButton.setTooltip("Export the score as an image (PNG/JPEG, sheet "
                                "size and DPI in SETUP). When the piece is "
                                "longer than one page, a window opens to frame "
                                "the exported zone.");
        exportButton.onClick = [this] { exportNow(); };
        addAndMakeVisible(exportButton);

        // ── Preview zoom scrollbars (visible only while zoomed in) ──────────
        for (auto* sb : { &previewHScroll, &previewVScroll })
        {
            sb->setAutoHide(false);
            sb->setRangeLimits(0.0, 1.0, juce::dontSendNotification);
            sb->addListener(this);
            addChildComponent(sb);
        }

        // ── Audition transport (shared SCORE player channel) ────────────────
        playStopButton.setTooltip("Play / stop the piece through the score player");
        playStopButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playStopButton);

        pauseButton.setTooltip("Freeze the current instant: the held column "
                               "keeps sounding; click/drag the preview to move "
                               "it. Works while playing or from stop.");
        pauseButton.onClick = [this] { togglePause(); };
        addAndMakeVisible(pauseButton);

        loopBtn.setTooltip("Loop playback");
        addAndMakeVisible(loopBtn);
        loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "midiScoreLoop", loopBtn);

        reverseBtn.setTooltip("Reverse (play the piece backward)");
        addAndMakeVisible(reverseBtn);
        reverseAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "midiScoreReverse", reverseBtn);

        initLabel(speedLabel, "Speed");
        speedSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        speedSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 22);
        speedSlider.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
        speedSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        speedSlider.setColour(juce::Slider::textBoxTextColourId,       juce::Colour(0xffa0c4e8));
        speedSlider.setRange(0.1, 6.0, 0.01);
        speedSlider.setTextValueSuffix("x");
        speedSlider.setSkewFactorFromMidPoint(1.0);
        addAndMakeVisible(speedSlider);
        speedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getAPVTS(), "midiScoreSpeed", speedSlider);

        // Right-click MIDI Learn — MIDI SCORE's own transport (play/loop/reverse/speed).
        {
            auto& mm = processor.getMidiMap();
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, playStopButton, "midiScorePlaying"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, loopBtn,        "midiScoreLoop"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, reverseBtn,     "midiScoreReverse"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, speedSlider,    "midiScoreSpeed"));
        }

        playHint.setText("PLAY loads the piece into the score player "
                         "(set LuxStral source = Sampler to hear it).",
                         juce::dontSendNotification);
        playHint.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        playHint.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        // Render at true glyph proportions: wrap across the 2-line box rather
        // than horizontally squishing to fit (JUCE Labels compress to 0.7 by
        // default, which reads as a "cramped" font once the text is longer).
        playHint.setMinimumHorizontalScale(1.0f);
        addAndMakeVisible(playHint);

        // ── Playback shaping: crop / fade chips (SAMPLER-style) + IMAGE EQ ──
        {
            const auto pct = [](float n)
            { return juce::String(juce::roundToInt(n * 100.0f)) + "%"; };
            const auto pow2 = [](float n)
            { return juce::String(shapePowerRange().convertFrom0to1(n), 2); };
            const auto curve = [](float n)
            {
                static const char* kNames[] = { "LIN", "EXP", "LOG", "S" };
                return juce::String(kNames[juce::jlimit(0, kNumFadeCurveTypes - 1,
                                                        (int) std::lround(n * 3.0f))]);
            };

            auto setup = [this](SamplerValueBox& b,
                                std::function<float()> rd,
                                std::function<void(float)> ap,
                                std::function<juce::String(float)> fmt)
            {
                b.readNorm  = std::move(rd);
                b.applyNorm = [this, ap = std::move(ap)](float n)
                {
                    ap(n);
                    markShapeDirty();
                };
                b.format = std::move(fmt);
                addAndMakeVisible(b);
            };
            setup(cropStartBox_,
                  [this] { return cropStart_; },
                  [this](float n) { cropStart_ = juce::jlimit(0.0f, cropEnd_ - 0.01f, n); },
                  pct);
            setup(cropEndBox_,
                  [this] { return cropEnd_; },
                  [this](float n) { cropEnd_ = juce::jlimit(cropStart_ + 0.01f, 1.0f, n); },
                  pct);
            setup(fadeInLenBox_,
                  [this] { return fadeInLen_; },
                  [this](float n) { fadeInLen_ = juce::jlimit(0.0f, 1.0f, n); },
                  pct);
            setup(fadeInTypeBox_,
                  [this] { return (float) (int) fadeInType_ / 3.0f; },
                  [this](float n) { fadeInType_ = (FadeCurveType)
                        juce::jlimit(0, kNumFadeCurveTypes - 1, (int) std::lround(n * 3.0f)); },
                  curve);
            setup(fadeInPowBox_,
                  [this] { return shapePowerRange().convertTo0to1(fadeInPow_); },
                  [this](float n) { fadeInPow_ = shapePowerRange().convertFrom0to1(n); },
                  pow2);
            setup(fadeOutLenBox_,
                  [this] { return fadeOutLen_; },
                  [this](float n) { fadeOutLen_ = juce::jlimit(0.0f, 1.0f, n); },
                  pct);
            setup(fadeOutTypeBox_,
                  [this] { return (float) (int) fadeOutType_ / 3.0f; },
                  [this](float n) { fadeOutType_ = (FadeCurveType)
                        juce::jlimit(0, kNumFadeCurveTypes - 1, (int) std::lround(n * 3.0f)); },
                  curve);
            setup(fadeOutPowBox_,
                  [this] { return shapePowerRange().convertTo0to1(fadeOutPow_); },
                  [this](float n) { fadeOutPow_ = shapePowerRange().convertFrom0to1(n); },
                  pow2);
            fadeInTypeBox_ .setChoices({ "LIN", "EXP", "LOG", "S" });
            fadeOutTypeBox_.setChoices({ "LIN", "EXP", "LOG", "S" });

            // IMAGE EQ — shapes the PLAYED frames (never the export). The band
            // grid follows the instrument's tuning; the saved curve was decoded
            // by restoreState() above, syncEqRange() re-grids it if needed.
            eqEditor.onChange = [this] { markShapeDirty(); };
            addAndMakeVisible(eqEditor);
            syncEqRange();
        }

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
            // Zoomed, the image rect overflows the frame on every side — clip
            // so the strip (and its overlays) never bleed over the controls.
            g.saveState();
            g.reduceClipRegion(previewArea.reduced(1));
            g.setOpacity(1.0f);
            g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            g.drawImage(previewImage, imgArea);

            // Hi-res tile of the visible window (background-rendered at the
            // screen density once the zoom settles) — overlaid on the coarse
            // base strip, byte-same renderer so the two match seamlessly.
            if (hiResTile_.isValid() && previewZoom_ > 1.001
                && tileT1_ > tileT0_ && data.durationSec > 0.0)
            {
                const double dur = juce::jmax(0.05, (double) data.durationSec);
                const float x0 = imgArea.getX()
                               + (float) (tileT0_ / dur) * imgArea.getWidth();
                const float x1 = imgArea.getX()
                               + (float) (tileT1_ / dur) * imgArea.getWidth();
                g.drawImage(hiResTile_,
                            juce::Rectangle<float>(x0, imgArea.getY(),
                                                   x1 - x0, imgArea.getHeight()));
            }

            // Reading head — only meaningful while OUR frames sit in the player.
            auto* fs = boundChannel();
            if (fs != nullptr && framesAreOurs)
            {
                const bool playing = fs->isScorePlaying();
                const bool paused  = pauseMode != PauseMode::none;
                int headFrame = -1;
                if (playing || paused)   headFrame = fs->getScorePlayHead();
                else if (scrubHead >= 0) headFrame = scrubHead;
                if (headFrame >= 0)
                {
                    // The loaded frames cover the CROP window only — map the
                    // head back onto the whole-piece preview strip.
                    const int n = juce::jmax(1, fs->getScoreFrameCount());
                    const float frac = juce::jlimit(0.f, 1.f, (float) headFrame / (float) n);
                    const float pieceFrac = cropStart_ + frac * (cropEnd_ - cropStart_);
                    const float lx = imgArea.getX() + pieceFrac * imgArea.getWidth();
                    g.setColour(accent.withAlpha(playing || paused ? 0.9f : 0.6f));
                    g.fillRect(lx - 0.75f, imgArea.getY(), 1.5f, imgArea.getHeight());
                }
            }

            drawShapeOverlay(g);
            drawPanOverlay(g);
            g.restoreState();

            if (previewZoom_ > 1.001)
            {
                g.setColour(juce::Colour(0xcc10131a));
                g.fillRoundedRectangle((float) previewArea.getX() + 4.f,
                                       (float) previewArea.getY() + 4.f, 46.f, 15.f, 3.f);
                g.setColour(accent.withAlpha(0.85f));
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
                g.drawText(juce::String(previewZoom_, 1) + "x",
                           previewArea.getX() + 4, previewArea.getY() + 4, 46, 15,
                           juce::Justification::centred);
            }
        }
        else
        {
            bool anyVoiceOn = false;
            for (int i = 0; i < midiscoregen::kMaxVoices; ++i)
                anyVoiceOn = anyVoiceOn || voiceAudible(i);
            g.setColour(juce::Colour(0xff55606f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
            g.drawText(! data.ok    ? "Load a MIDI file to generate its score"
                     : ! anyVoiceOn ? "Enable at least one voice"
                                    : "No notes in this MIDI file",
                       previewArea, juce::Justification::centred);
        }
    }

    juce::Rectangle<float> previewDestArea() const
    {
        return juce::Rectangle<float>(
            (float) previewArea.getX() + 2, (float) previewArea.getY() + 2,
            (float) previewArea.getWidth() - 4, (float) previewArea.getHeight() - 4);
    }

    /** The image rect at zoom 1 — whole strip fitted in the frame (the
     *  historical display). Zoom scales THIS rect, so 1x always re-fits. */
    juce::Rectangle<float> fittedPreviewRect() const
    {
        const juce::RectanglePlacement place(juce::RectanglePlacement::centred);
        return place.appliedTo(
            juce::Rectangle<float>(0.f, 0.f,
                (float) previewImage.getWidth(), (float) previewImage.getHeight()),
            previewDestArea());
    }

    juce::Rectangle<float> previewImageBounds() const
    {
        if (! previewImage.isValid() || previewArea.isEmpty())
            return {};
        const auto fit = fittedPreviewRect();
        if (previewZoom_ <= 1.001)
            return fit;

        // Zoomed: scale the fit rect and position it so previewCx_/Cy_ (image
        // fraction at the frame centre) holds; centred when it still fits,
        // clamped edge-to-edge (no letterbox gap) when it overflows.
        const auto  dest = previewDestArea();
        const float w = fit.getWidth()  * (float) previewZoom_;
        const float h = fit.getHeight() * (float) previewZoom_;
        float x = dest.getCentreX() - (float) previewCx_ * w;
        float y = dest.getCentreY() - (float) previewCy_ * h;
        x = (w <= dest.getWidth())  ? dest.getCentreX() - w * 0.5f
                                    : juce::jlimit(dest.getRight()  - w, dest.getX(), x);
        y = (h <= dest.getHeight()) ? dest.getCentreY() - h * 0.5f
                                    : juce::jlimit(dest.getBottom() - h, dest.getY(), y);
        return { x, y, w, h };
    }

    //==========================================================================
    // Preview zoom (mouse wheel / pinch) + scrollbars. Zoom 1 = whole piece
    // fitted (the historical view); previewCx_/Cy_ = image fraction shown at
    // the frame centre while zoomed.
    static constexpr double kMaxPreviewZoom = 16.0;

    void zoomPreviewAt(juce::Point<float> pos, double factor)
    {
        if (! previewImage.isValid())
            return;
        const double target = juce::jlimit(1.0, kMaxPreviewZoom,
                                           previewZoom_ * factor);
        if (juce::approximatelyEqual(target, previewZoom_))
            return;

        // Image fractions under the cursor BEFORE the zoom — kept under the
        // cursor after, so the wheel dives into what it points at.
        const auto before = previewImageBounds();
        const double fx = before.getWidth()  > 0.f
            ? juce::jlimit(0.0, 1.0, (double) ((pos.x - before.getX()) / before.getWidth()))
            : 0.5;
        const double fy = before.getHeight() > 0.f
            ? juce::jlimit(0.0, 1.0, (double) ((pos.y - before.getY()) / before.getHeight()))
            : 0.5;

        previewZoom_ = target;
        const auto  dest = previewDestArea();
        const auto  fit  = fittedPreviewRect();
        const double w = fit.getWidth()  * previewZoom_;
        const double h = fit.getHeight() * previewZoom_;
        if (w > 0.0) previewCx_ = fx + (dest.getCentreX() - pos.x) / w;
        if (h > 0.0) previewCy_ = fy + (dest.getCentreY() - pos.y) / h;
        clampPreviewView();
        updatePreviewScrollbars();
        lastViewChangeMs_ = juce::Time::getMillisecondCounter();
        repaint(previewArea);
    }

    void clampPreviewView()
    {
        const auto dest = previewDestArea();
        const auto fit  = previewImage.isValid() ? fittedPreviewRect()
                                                 : juce::Rectangle<float>();
        const double w = fit.getWidth()  * previewZoom_;
        const double h = fit.getHeight() * previewZoom_;
        const double visW = w > 0.0 ? juce::jmin(1.0, dest.getWidth()  / w) : 1.0;
        const double visH = h > 0.0 ? juce::jmin(1.0, dest.getHeight() / h) : 1.0;
        previewCx_ = visW >= 1.0 ? 0.5
                                 : juce::jlimit(visW * 0.5, 1.0 - visW * 0.5, previewCx_);
        previewCy_ = visH >= 1.0 ? 0.5
                                 : juce::jlimit(visH * 0.5, 1.0 - visH * 0.5, previewCy_);
    }

    void updatePreviewScrollbars()
    {
        const auto dest = previewDestArea();
        const bool zoomed = previewZoom_ > 1.001 && previewImage.isValid();
        const auto fit = previewImage.isValid() ? fittedPreviewRect()
                                                : juce::Rectangle<float>();
        const double w = fit.getWidth()  * previewZoom_;
        const double h = fit.getHeight() * previewZoom_;
        const double visW = w > 0.0 ? juce::jmin(1.0, dest.getWidth()  / w) : 1.0;
        const double visH = h > 0.0 ? juce::jmin(1.0, dest.getHeight() / h) : 1.0;

        const bool showH = zoomed && visW < 1.0;
        const bool showV = zoomed && visH < 1.0;
        previewHScroll.setVisible(showH);
        previewVScroll.setVisible(showV);
        if (showH)
            previewHScroll.setCurrentRange(previewCx_ - visW * 0.5, visW,
                                           juce::dontSendNotification);
        if (showV)
            previewVScroll.setCurrentRange(previewCy_ - visH * 0.5, visH,
                                           juce::dontSendNotification);
    }

    void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override
    {
        if (bar == &previewHScroll)
            previewCx_ = newRangeStart + bar->getCurrentRangeSize() * 0.5;
        else if (bar == &previewVScroll)
            previewCy_ = newRangeStart + bar->getCurrentRangeSize() * 0.5;
        else
            return;
        lastViewChangeMs_ = juce::Time::getMillisecondCounter();
        repaint(previewArea);
    }

    void mouseWheelMove(const juce::MouseEvent& e,
                        const juce::MouseWheelDetails& wheel) override
    {
        if (previewImage.isValid() && previewArea.contains(e.getPosition()))
        {
            if (wheel.deltaY != 0.f)
                zoomPreviewAt(e.position, std::exp((double) wheel.deltaY * 2.2));
            return;   // consumed — never scrolls the page viewport underneath
        }
        juce::Component::mouseWheelMove(e, wheel);
    }

    void mouseMagnify(const juce::MouseEvent& e, float scaleFactor) override
    {
        if (previewImage.isValid() && previewArea.contains(e.getPosition()))
            zoomPreviewAt(e.position, (double) scaleFactor);
    }

    //==========================================================================
    // Hi-res zoom tile. The base preview is width-capped (≈3600 px for the
    // whole piece), so zooming it is pure upscale blur. Once the view settles,
    // the VISIBLE time window (+35% margin each side, so small pans stay
    // covered) is re-rendered on a background thread at the screen's physical
    // density and overlaid in paint(). Cost stays ~viewport-sized whatever the
    // zoom, because only the visible slice is rendered.
    struct TileSpec
    {
        double vis0 = 0.0, vis1 = 0.0;   ///< visible window (s) — coverage test
        double t0 = 0.0, t1 = 0.0;       ///< rendered window (s), margin included
        double pxPerSec = 0.0, dpiY = 0.0;
        bool   valid = false;
    };

    TileSpec desiredTileSpec() const
    {
        TileSpec ts;
        if (previewZoom_ <= 1.001 || ! previewImage.isValid()
            || ! data.ok || data.notes.empty())
            return ts;
        const auto imgArea = previewImageBounds();
        const auto vis = imgArea.getIntersection(previewArea.toFloat());
        if (vis.isEmpty() || imgArea.getWidth() <= 0.f || imgArea.getHeight() <= 0.f)
            return ts;

        const double dur = juce::jmax(0.05, (double) data.durationSec);
        const double f0  = (vis.getX()     - imgArea.getX()) / imgArea.getWidth();
        const double f1  = (vis.getRight() - imgArea.getX()) / imgArea.getWidth();
        const double margin = 0.35 * (f1 - f0);
        ts.vis0 = juce::jlimit(0.0, dur, f0 * dur);
        ts.vis1 = juce::jlimit(0.0, dur, f1 * dur);
        ts.t0   = juce::jlimit(0.0, dur, (f0 - margin) * dur);
        ts.t1   = juce::jlimit(0.0, dur, (f1 + margin) * dur);
        if (ts.t1 - ts.t0 < 1.0e-3)
            return ts;

        double scale = 2.0;   // physical px per logical px (Retina default)
        if (auto* d = juce::Desktop::getInstance().getDisplays()
                          .getDisplayForRect(getScreenBounds()))
            scale = d->scale;

        // Match the DRAWN density: whole piece spans imgArea.width on screen.
        ts.pxPerSec = juce::jmax(1.0, (double) imgArea.getWidth() * scale / dur);
        ts.dpiY = juce::jlimit(60.0, 1200.0,
            (double) imgArea.getHeight() * scale * 25.4
                / juce::jmax(1.0, pageSettings.spectroHeightMM));

        // Pixel budget — bounds the render/memory whatever the geometry.
        const double w = (ts.t1 - ts.t0) * ts.pxPerSec;
        const double h = pageSettings.spectroHeightMM / 25.4 * ts.dpiY;
        constexpr double kBudgetPx = 24.0e6;
        if (w * h > kBudgetPx)
        {
            const double k = std::sqrt(kBudgetPx / (w * h));
            ts.pxPerSec *= k;
            ts.dpiY = juce::jmax(60.0, ts.dpiY * k);
        }
        ts.valid = true;
        return ts;
    }

    static constexpr juce::uint32 kTileDebounceMs = 180;

    void maybeStartTileRender()
    {
        if (previewZoom_ <= 1.001)
        {
            if (hiResTile_.isValid() && ! tileRenderBusy_)
            {
                hiResTile_ = juce::Image();   // dezoomed: drop the tile
                tileT0_ = tileT1_ = 0.0;
            }
            return;
        }
        if (tileRenderBusy_
            || juce::Time::getMillisecondCounter() - lastViewChangeMs_ < kTileDebounceMs)
            return;
        const auto ts = desiredTileSpec();
        if (! ts.valid)
            return;
        // Still sharp and still covering the visible window? Keep it.
        if (hiResTile_.isValid()
            && ts.vis0 >= tileT0_ - 1.0e-6 && ts.vis1 <= tileT1_ + 1.0e-6
            && tilePxPerSec_ > 0.0
            && std::abs(ts.pxPerSec / tilePxPerSec_ - 1.0) < 0.25)
            return;

        tileRenderBusy_ = true;
        juce::Thread::launch(
            [safe = juce::Component::SafePointer<MidiScoreGenTabComponent>(this),
             dataCopy = data, voicesCopy = effectiveVoices(),
             s = settingsWithTuning(), ts, epoch = tileEpoch_]
            {
                auto r = midiscoregen::renderStrip(dataCopy, voicesCopy, s,
                                                   ts.t0, ts.t1,
                                                   ts.pxPerSec, ts.dpiY);
                juce::MessageManager::callAsync(
                    [safe, r = std::move(r), ts, epoch]() mutable
                    {
                        if (auto* self = safe.getComponent())
                            self->applyTileRender(r, ts, epoch);
                    });
            });
    }

    void applyTileRender(const scoregen::RenderResult& r, const TileSpec& ts, int epoch)
    {
        tileRenderBusy_ = false;
        // Content changed while this tile rendered — never display old bytes.
        if (epoch != tileEpoch_)
            return;
        if (! (r.ok && r.image.isValid()))
            return;                      // keep the base strip alone
        if (previewZoom_ <= 1.001)
        {
            hiResTile_ = juce::Image();  // dezoomed while rendering
            return;
        }
        hiResTile_   = r.image;
        tileT0_      = ts.t0;
        tileT1_      = ts.t1;
        tilePxPerSec_ = ts.pxPerSec;
        repaint(previewArea);
    }

    //==========================================================================
    // Pan automation line over the preview — X = time (fraction of the piece),
    // Y = pan: TOP = LEFT, BOTTOM = RIGHT, centre = mono/grey ink. Click the
    // line to add a handle, drag handles to shape the pan over time,
    // right-click (or double-click) a handle to remove it. The exact same
    // curve tints the render (midiscoregen::panAt on both sides).
    juce::Rectangle<float> panArea() const
    {
        return previewImgArea.isEmpty() ? previewImageBounds() : previewImgArea;
    }
    float  fracToX(double f) const
    {
        const auto a = panArea();
        return a.getX() + (float) f * a.getWidth();
    }
    double xToFrac(float x) const
    {
        const auto a = panArea();
        return a.getWidth() <= 0.f ? 0.0
             : juce::jlimit(0.0, 1.0, (double) ((x - a.getX()) / a.getWidth()));
    }
    float  panToY(double pan) const
    {
        const auto a = panArea();
        return a.getY() + (float) ((pan + 1.0) * 0.5) * a.getHeight();
    }
    double yToPan(float y) const
    {
        const auto a = panArea();
        return a.getHeight() <= 0.f ? 0.0
             : juce::jlimit(-1.0, 1.0,
                   (double) ((y - a.getY()) / a.getHeight()) * 2.0 - 1.0);
    }

    /** Colour of a pan value, matching the ink coding: left = red,
     *  right = blue, centre = neutral grey. The 0.6 exponent makes the
     *  colour commit early — a slight pan already reads on screen. */
    static juce::Colour panColour(double p)
    {
        const juce::Colour centre(0xff6a6f78), left(0xffe83c3c), right(0xff3c6ce8);
        const auto amt = (float) std::pow(std::abs(p), 0.6);
        return p < 0.0 ? centre.interpolatedWith(left,  amt)
                       : centre.interpolatedWith(right, amt);
    }

    /** One pan curve as a polyline, coloured by its local pan value (red
     *  left … blue right). Short steps so the S-curves draw smooth. */
    void drawPanCurve(juce::Graphics& g, const std::vector<midiscoregen::PanPoint>& pts,
                      float thickness, float alpha, bool halo)
    {
        const auto a = panArea();
        const float step = 3.0f;
        for (int pass = halo ? 0 : 1; pass < 2; ++pass)
        {
            float  x0 = a.getX();
            double p0 = midiscoregen::panAt(pts, xToFrac(x0));
            while (x0 < a.getRight() - 0.5f)
            {
                const float  x1 = juce::jmin(x0 + step, a.getRight());
                const double p1 = midiscoregen::panAt(pts, xToFrac(x1));
                g.setColour(pass == 0
                    ? juce::Colours::white.withAlpha(0.65f * alpha)
                    : panColour((p0 + p1) * 0.5).withAlpha(alpha));
                g.drawLine(x0, panToY(p0), x1, panToY(p1),
                           pass == 0 ? thickness + 2.3f : thickness);
                x0 = x1;
                p0 = p1;
            }
        }
    }

    void drawPanOverlay(juce::Graphics& g)
    {
        const auto a = panArea();
        if (a.isEmpty())
            return;

        // Faint centre guide (pan 0 reference).
        g.setColour(juce::Colour(0x22000000));
        g.fillRect(a.getX(), panToY(0.0) - 0.5f, a.getWidth(), 1.0f);

        // Ghost curves of the OTHER voices (context only — no handles), then
        // the SELECTED voice's curve full strength with its handles.
        const int nVis = visibleVoiceCount();
        for (int v = 0; v < nVis; ++v)
        {
            if (v == selectedVoice || ! voiceAudible(v)
                || pageSettings.panPoints[(size_t) v].empty())
                continue;
            drawPanCurve(g, pageSettings.panPoints[(size_t) v], 1.6f, 0.30f, false);
        }

        const auto& pts = pageSettings.panPoints[(size_t) selectedVoice];
        drawPanCurve(g, pts, 3.2f, 1.0f, true);

        // Handles: white disc, coloured core, soft dark rim for contrast.
        for (const auto& q : pts)
        {
            const float hx = fracToX(q.pos), hy = panToY(q.pan);
            g.setColour(juce::Colours::white);
            g.fillEllipse(hx - 7.0f, hy - 7.0f, 14.0f, 14.0f);
            g.setColour(panColour(q.pan));
            g.fillEllipse(hx - 5.0f, hy - 5.0f, 10.0f, 10.0f);
            g.setColour(juce::Colour(0x50000000));
            g.drawEllipse(hx - 7.0f, hy - 7.0f, 14.0f, 14.0f, 1.0f);
        }
    }

    /** Pan-line editing on mouse-down, on the SELECTED voice's curve; true =
     *  the event was consumed (no scrub). Handles win over the line; a single
     *  click on the line inserts a new breakpoint and starts dragging it. */
    bool handlePanMouseDown(const juce::MouseEvent& e)
    {
        if (! previewImage.isValid())
            return false;
        // Zoomed, the image rect (= pan area) overflows the preview frame:
        // only presses INSIDE the frame may edit the line.
        if (! previewArea.contains(e.getPosition()))
            return false;
        const auto a = panArea();
        if (a.isEmpty() || ! a.expanded(6.0f).contains(e.position))
            return false;
        auto& pts = pageSettings.panPoints[(size_t) selectedVoice];

        for (size_t i = 0; i < pts.size(); ++i)
        {
            const juce::Point<float> c(fracToX(pts[i].pos), panToY(pts[i].pan));
            if (c.getDistanceFrom(e.position) <= 10.0f)
            {
                if (e.mods.isRightButtonDown() || e.getNumberOfClicks() >= 2)
                {
                    pts.erase(pts.begin() + (std::ptrdiff_t) i);
                    dragPanIdx = -1;
                    markDirty();
                    repaint(previewArea);
                }
                else
                    dragPanIdx = (int) i;
                return true;
            }
        }

        // Single clicks only: the second press of a double-click on the line
        // must not re-add what the first one just created.
        if (e.getNumberOfClicks() == 1 && ! e.mods.isRightButtonDown())
        {
            const double f = xToFrac(e.position.x);
            const double curvePan = midiscoregen::panAt(pts, f);
            if (std::abs(e.position.y - panToY(curvePan)) <= 8.0f)
            {
                size_t k = 0;
                while (k < pts.size() && pts[k].pos < f)
                    ++k;
                pts.insert(pts.begin() + (std::ptrdiff_t) k, { f, curvePan });
                dragPanIdx = (int) k;
                markDirty();
                repaint(previewArea);
                return true;
            }
        }
        return false;
    }

    void dragPanHandle(const juce::MouseEvent& e)
    {
        auto& pts = pageSettings.panPoints[(size_t) selectedVoice];
        if (dragPanIdx < 0 || dragPanIdx >= (int) pts.size())
            return;
        // Keep the points ordered: a handle stops at its neighbours.
        double lo = dragPanIdx > 0 ? pts[(size_t) dragPanIdx - 1].pos + 1.0e-3 : 0.0;
        double hi = dragPanIdx + 1 < (int) pts.size()
                        ? pts[(size_t) dragPanIdx + 1].pos - 1.0e-3 : 1.0;
        if (hi < lo)
            hi = lo;
        pts[(size_t) dragPanIdx].pos = juce::jlimit(lo, hi, xToFrac(e.position.x));
        pts[(size_t) dragPanIdx].pan = yToPan(e.position.y);
        markDirty();
        repaint(previewArea);
    }

    //==========================================================================
    // Crop / fade overlay on the preview — the SAMPLER slot editor's visual
    // language: green start / orange end bars (drag anywhere on the bar),
    // full-height fade ramps with an END handle (length) and a MID handle
    // (shape — drag bends the curve, right-click picks LIN/EXP/LOG/S,
    // double-click resets to LIN). All playback-only: the preview strip
    // underneath stays the raw piece.
    static constexpr float kShapeHandleR = 4.5f;   // node radius (EQ-sized)
    static constexpr int   kShapeGrabR   = 10;     // fade handle grab radius
    static constexpr int   kShapeSnap    = 8;      // crop bar snap (px)

    /** Skewed 0.1..10 power range (1.0 at centre) — mirrors the SAMPLER chips. */
    static const juce::NormalisableRange<float>& shapePowerRange()
    {
        static const juce::NormalisableRange<float> r = []
        { juce::NormalisableRange<float> rr(0.1f, 10.0f); rr.setSkewForCentre(1.0f); return rr; }();
        return r;
    }

    float cropSpan() const noexcept { return juce::jmax(1.0e-4f, cropEnd_ - cropStart_); }

    /** Peak-end (length) handle of a fade — in preview coordinates. */
    juce::Point<float> shapeFadeEndPoint(bool in) const
    {
        const auto a = panArea();
        if (a.isEmpty() || ! previewImage.isValid()) return { -1.0f, -1.0f };
        const float x = in
            ? fracToX(cropStart_ + fadeInLen_  * cropSpan())
            : fracToX(cropEnd_   - fadeOutLen_ * cropSpan());
        return { x, a.getY() + kShapeHandleR + 2.0f };
    }

    /** MID (shape) handle ON the fade curve — hidden for near-zero fades. */
    juce::Point<float> shapeFadeMidPoint(bool in) const
    {
        const auto a = panArea();
        if (a.isEmpty() || ! previewImage.isValid()) return { -1.0f, -1.0f };
        float x0, x1; FadeCurveType type; float power;
        if (in)
        {
            x0 = fracToX(cropStart_);
            x1 = fracToX(cropStart_ + fadeInLen_ * cropSpan());
            type = fadeInType_; power = fadeInPow_;
        }
        else
        {
            x0 = fracToX(cropEnd_ - fadeOutLen_ * cropSpan());
            x1 = fracToX(cropEnd_);
            type = fadeOutType_; power = fadeOutPow_;
        }
        if (x1 - x0 <= 8.0f)
            return { -1.0f, -1.0f };
        const float peak = a.getY() + kShapeHandleR + 2.0f;
        const float bot  = a.getBottom() - 1.0f;
        const float mg   = applyFadeCurve(0.5f, type, power);
        return { (x0 + x1) * 0.5f, bot - mg * juce::jmax(1.0f, bot - peak) };
    }

    void drawShapeOverlay(juce::Graphics& g)
    {
        const auto a = panArea();
        if (a.isEmpty())
            return;

        const float sx = fracToX(cropStart_);
        const float ex = fracToX(cropEnd_);
        const float top = a.getY(), H = a.getHeight();

        // Dim outside [start, end] — that part never reaches the player.
        g.setColour(juce::Colour(0x88080810));
        if (sx > a.getX())    g.fillRect(a.getX(), top, sx - a.getX(), H);
        if (ex < a.getRight()) g.fillRect(ex, top, a.getRight() - ex, H);

        // Start / End bars with a dark edge for contrast on the white score.
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillRect(sx - 1.0f, top, 4.0f, H);
        g.setColour(juce::Colour(0xff33ff99));
        g.fillRect(sx, top, 2.0f, H);
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillRect(ex - 1.0f, top, 4.0f, H);
        g.setColour(juce::Colour(0xffff6633));
        g.fillRect(ex, top, 2.0f, H);

        // Fade ramps + handles (port of SlotSpectralEditorComponent::drawFades).
        const float hy   = a.getY() + kShapeHandleR + 2.0f;   // curve peak (gain 1)
        const float sBot = a.getBottom() - 1.0f;              // curve foot (gain 0)
        const float fH   = juce::jmax(1.0f, sBot - hy);

        const auto drawOne = [&](float x0, float x1, FadeCurveType type, float power,
                                 juce::Colour col, bool rising, bool active, bool hover,
                                 bool midActive, bool midHover)
        {
            const int steps = juce::jmax(2, (int) std::abs(x1 - x0));
            if (x1 > x0 + 0.5f)
            {
                juce::Path curve, fill;
                for (int s = 0; s <= steps; ++s)
                {
                    const float t    = (float) s / (float) steps;
                    const float p    = rising ? t : (1.0f - t);
                    const float gain = applyFadeCurve(juce::jlimit(0.0f, 1.0f, p), type, power);
                    const float x    = x0 + t * (x1 - x0);
                    const float y    = sBot - gain * fH;
                    if (s == 0) { curve.startNewSubPath(x, y); fill.startNewSubPath(x, sBot); fill.lineTo(x, y); }
                    else        { curve.lineTo(x, y);          fill.lineTo(x, y); }
                }
                fill.lineTo(x1, sBot);
                fill.closeSubPath();
                g.setColour(col.withAlpha(0.18f));
                g.fillPath(fill);
                g.setColour(col.withAlpha(0.9f));
                g.strokePath(curve, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved));
            }

            const float hx = rising ? x1 : x0;
            const float r  = (active || hover) ? kShapeHandleR + 1.0f : kShapeHandleR;
            if (active || hover)
            {
                g.setColour(col.withAlpha(0.25f));
                g.fillEllipse(hx - r - 2.5f, hy - r - 2.5f, 2 * (r + 2.5f), 2 * (r + 2.5f));
            }
            g.setColour(active ? col.brighter(0.3f) : juce::Colour(0xff20202a));
            g.fillEllipse(hx - r, hy - r, 2 * r, 2 * r);
            g.setColour(active ? juce::Colours::white : col.withAlpha(0.9f));
            g.drawEllipse(hx - r, hy - r, 2 * r, 2 * r, 1.4f);

            if (x1 > x0 + 8.0f)
            {
                const float mx = (x0 + x1) * 0.5f;
                const float mg = applyFadeCurve(0.5f, type, power);
                const float my = sBot - mg * fH;
                const float mr = (midActive || midHover) ? kShapeHandleR + 1.5f
                                                         : kShapeHandleR + 0.5f;
                if (midActive || midHover)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.25f));
                    g.fillEllipse(mx - mr - 2.5f, my - mr - 2.5f,
                                  2 * (mr + 2.5f), 2 * (mr + 2.5f));
                }
                g.setColour(juce::Colour(0xff20202a));
                g.fillEllipse(mx - mr, my - mr, 2 * mr, 2 * mr);
                g.setColour(midActive ? col.brighter(0.5f)
                                      : juce::Colours::white.withAlpha(0.9f));
                g.drawEllipse(mx - mr, my - mr, 2 * mr, 2 * mr, 1.6f);
            }
        };

        drawOne(sx, fracToX(cropStart_ + fadeInLen_ * cropSpan()),
                fadeInType_, fadeInPow_, juce::Colour(0xff44ee88), /*rising=*/true,
                shapeDrag_ == ShapeDrag::FadeIn,       shapeHover_ == 1,
                shapeDrag_ == ShapeDrag::FadeInShape,  shapeHover_ == 3);
        drawOne(fracToX(cropEnd_ - fadeOutLen_ * cropSpan()), ex,
                fadeOutType_, fadeOutPow_, juce::Colour(0xffff6633), /*rising=*/false,
                shapeDrag_ == ShapeDrag::FadeOut,      shapeHover_ == 2,
                shapeDrag_ == ShapeDrag::FadeOutShape, shapeHover_ == 4);
    }

    void showShapeFadeMenu(bool in)
    {
        const auto cur = in ? fadeInType_ : fadeOutType_;
        juce::PopupMenu m;
        m.addSectionHeader(in ? "Fade in curve" : "Fade out curve");
        static const char* kNames[] = { "LIN", "EXP", "LOG", "S" };
        for (int i = 0; i < kNumFadeCurveTypes; ++i)
            m.addItem(i + 1, kNames[i], true, static_cast<int>(cur) == i);

        juce::Component::SafePointer<MidiScoreGenTabComponent> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this)
                                                  .withMousePosition(),
            [safe, in](int result)
            {
                if (safe == nullptr || result <= 0) return;
                const auto t = static_cast<FadeCurveType>(result - 1);
                if (in) safe->fadeInType_  = t;
                else    safe->fadeOutType_ = t;
                safe->markShapeDirty();
            });
    }

    /** Crop/fade editing on mouse-down; true = consumed (no scrub). Fade
     *  handles win, then the crop bars; a plain click elsewhere stays a scrub. */
    bool handleShapeMouseDown(const juce::MouseEvent& e)
    {
        if (! previewImage.isValid() || ! previewArea.contains(e.getPosition()))
            return false;
        const auto a = panArea();
        if (a.isEmpty())
            return false;

        const auto isNear = [&](juce::Point<float> p)
        { return p.x >= 0.0f && e.position.getDistanceFrom(p) <= (float) kShapeGrabR; };
        const bool endIn  = isNear(shapeFadeEndPoint(true));
        const bool endOut = isNear(shapeFadeEndPoint(false));
        const bool midIn  = isNear(shapeFadeMidPoint(true));
        const bool midOut = isNear(shapeFadeMidPoint(false));

        // Right-click a fade handle → curve type menu (LIN/EXP/LOG/S).
        if (e.mods.isRightButtonDown())
        {
            if (endIn || midIn)        { showShapeFadeMenu(true);  return true; }
            if (endOut || midOut)      { showShapeFadeMenu(false); return true; }
            return false;
        }

        // Double-click the MID handle → back to a straight (LIN) fade.
        if ((midIn || midOut) && e.getNumberOfClicks() >= 2)
        {
            if (midIn) { fadeInType_  = FadeCurveType::LINEAR; fadeInPow_  = 1.0f; }
            else       { fadeOutType_ = FadeCurveType::LINEAR; fadeOutPow_ = 1.0f; }
            markShapeDirty();
            return true;
        }

        if      (endIn)  shapeDrag_ = ShapeDrag::FadeIn;
        else if (endOut) shapeDrag_ = ShapeDrag::FadeOut;
        else if (midIn)  shapeDrag_ = ShapeDrag::FadeInShape;
        else if (midOut) shapeDrag_ = ShapeDrag::FadeOutShape;
        else
        {
            const float sx = fracToX(cropStart_), ex = fracToX(cropEnd_);
            if      (std::abs(e.position.x - sx) <= (float) kShapeSnap) shapeDrag_ = ShapeDrag::Start;
            else if (std::abs(e.position.x - ex) <= (float) kShapeSnap) shapeDrag_ = ShapeDrag::End;
            else return false;   // free clicks keep scrubbing the play head
        }
        dragShapeHandle(e);
        return true;
    }

    void dragShapeHandle(const juce::MouseEvent& e)
    {
        if (shapeDrag_ == ShapeDrag::None)
            return;
        const float f = (float) xToFrac(e.position.x);
        switch (shapeDrag_)
        {
            case ShapeDrag::Start:
                cropStart_ = juce::jlimit(0.0f, cropEnd_ - 0.01f, f);
                break;
            case ShapeDrag::End:
                cropEnd_ = juce::jlimit(cropStart_ + 0.01f, 1.0f, f);
                break;
            case ShapeDrag::FadeIn:
                fadeInLen_ = juce::jlimit(0.0f, 1.0f, (f - cropStart_) / cropSpan());
                break;
            case ShapeDrag::FadeOut:
                fadeOutLen_ = juce::jlimit(0.0f, 1.0f, (cropEnd_ - f) / cropSpan());
                break;
            case ShapeDrag::FadeInShape:
            case ShapeDrag::FadeOutShape:
            {
                // Bend the curve so its mid-point passes through the mouse:
                // below the diagonal → EXP, above → LOG, close to it → LIN;
                // an S curve keeps its type and takes the power instead
                // (same mapping as the SAMPLER slot editor).
                const bool  in   = (shapeDrag_ == ShapeDrag::FadeInShape);
                const auto  a    = panArea();
                const float peak = a.getY() + kShapeHandleR + 2.0f;
                const float bot  = a.getBottom() - 1.0f;
                const float gv   = juce::jlimit(0.02f, 0.98f,
                    (bot - e.position.y) / juce::jmax(1.0f, bot - peak));
                const float lg   = std::log(gv) / std::log(0.5f);

                const auto cur = in ? fadeInType_ : fadeOutType_;
                FadeCurveType type;
                float         power;
                if (cur == FadeCurveType::SCURVE)
                { type = FadeCurveType::SCURVE;      power = juce::jlimit(0.1f, 10.0f, lg); }
                else if (std::abs(gv - 0.5f) < 0.015f)
                { type = FadeCurveType::LINEAR;      power = 1.0f; }
                else if (gv < 0.5f)
                { type = FadeCurveType::EXPONENTIAL; power = juce::jlimit(0.1f, 10.0f, lg - 1.0f); }
                else
                { type = FadeCurveType::LOGARITHMIC; power = juce::jlimit(0.1f, 10.0f, 1.0f / lg - 1.0f); }

                if (in) { fadeInType_  = type; fadeInPow_  = power; }
                else    { fadeOutType_ = type; fadeOutPow_ = power; }
                break;
            }
            default: break;
        }
        markShapeDirty();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int newHover = 0;
        juce::MouseCursor cursor = juce::MouseCursor::NormalCursor;
        if (previewImage.isValid() && previewArea.contains(e.getPosition()))
        {
            const auto isNear = [&](juce::Point<float> p)
            { return p.x >= 0.0f && e.position.getDistanceFrom(p) <= (float) kShapeGrabR; };
            if      (isNear(shapeFadeEndPoint(true)))  newHover = 1;
            else if (isNear(shapeFadeEndPoint(false))) newHover = 2;
            else if (isNear(shapeFadeMidPoint(true)))  newHover = 3;
            else if (isNear(shapeFadeMidPoint(false))) newHover = 4;

            if (newHover != 0)
                cursor = juce::MouseCursor::PointingHandCursor;
            else
            {
                const float sx = fracToX(cropStart_), ex = fracToX(cropEnd_);
                if (std::abs(e.position.x - sx) <= (float) kShapeSnap
                    || std::abs(e.position.x - ex) <= (float) kShapeSnap)
                    cursor = juce::MouseCursor::LeftRightResizeCursor;
            }
        }
        setMouseCursor(cursor);
        if (newHover != shapeHover_)
        {
            shapeHover_ = newHover;
            repaint(previewArea);
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (shapeHover_ != 0)
        {
            shapeHover_ = 0;
            repaint(previewArea);
        }
    }

    void refreshShapeChips()
    {
        for (auto* b : { &cropStartBox_, &cropEndBox_,
                         &fadeInLenBox_,  &fadeInTypeBox_,  &fadeInPowBox_,
                         &fadeOutLenBox_, &fadeOutTypeBox_, &fadeOutPowBox_ })
            b->repaint();
    }

    /** A crop/fade/EQ edit: playback + saved state change, the preview strip
     *  itself doesn't (the shaping is an overlay) — no strip re-render. */
    void markShapeDirty()
    {
        playDirty  = true;
        stateDirty = true;
        lastEditMs = juce::Time::getMillisecondCounter();
        refreshShapeChips();
        repaint(previewArea);
    }

    /** Re-grid the EQ nodes onto the instrument's current tuning span.
     *  No-op (curve kept) while the span is unchanged. */
    void syncEqRange()
    {
        const auto s = settingsWithTuning();
        if (s.minFreq > 0.0 && s.maxFreq > s.minFreq)
            eqEditor.setRange(s.minFreq, s.maxFreq);
    }

    //==========================================================================
    // Preview interactions: the pan line/handles get first pick, then the
    // crop/fade handles, everything else is a scrub click (when our frames
    // are loaded — same behaviour as the SCORE and TIMBRE pages).
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (handlePanMouseDown(e))
            return;
        if (handleShapeMouseDown(e))
            return;
        scrubbing = framesAreOurs && previewImage.isValid()
                 && previewArea.contains(e.getPosition());
        if (! scrubbing) return;
        scrubTo(e);
        // Pause mode already sustains a session (held or frozen transport):
        // the seek above moved its column — no transient audition to start,
        // and mouseUp must NOT end the held session.
        if (pauseMode == PauseMode::none)
            if (auto* fs = boundChannel())
                if (! fs->isScorePlaying())
                    scrubAuditioning = fs->uiBeginScoreScrub();
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragPanIdx >= 0) { dragPanHandle(e); return; }
        if (shapeDrag_ != ShapeDrag::None) { dragShapeHandle(e); return; }
        if (scrubbing) scrubTo(e);
    }
    void mouseUp  (const juce::MouseEvent& e)   override
    {
        dragPanIdx = -1;
        scrubbing  = false;
        if (shapeDrag_ != ShapeDrag::None)
        {
            shapeDrag_ = ShapeDrag::None;
            mouseMove(e);   // refresh hover state under the released cursor
        }
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

        // Format / sheet size / DPI live on the SETUP face — one button here.
        exportButton.setBounds(pad, y, colW, ch);
        y += ch + gap + 4;

        // ── Transport bar ────────────────────────────────────────────────────
        {
            const int knobW = 56, knobDrwH = 42, knobValH = 14;
            const int blockH = knobDrwH + knobValH;
            const int btn = 40;
            const int icon = 34;   // loop / inverse pictograms (matches SCORE)

            int x = pad;
            playStopButton.setBounds(x, y + (blockH - btn)  / 2, btn,  btn);  x += btn + gap;
            pauseButton.setBounds  (x, y + (blockH - btn)  / 2, btn,  btn);  x += btn + gap;
            loopBtn.setBounds      (x, y + (blockH - icon) / 2, icon, icon);  x += icon + 4;
            reverseBtn.setBounds   (x, y + (blockH - icon) / 2, icon, icon);  x += icon + gap;

            const int knobX = pad + colW - knobW;
            speedSlider.setBounds(knobX, y, knobW, blockH);
            speedLabel.setBounds(x, y + (knobDrwH - ch) / 2,
                                 juce::jmax(0, knobX - gap - x), ch);
            y += blockH + gap;
        }
        // Two rows so the routing tip wraps at true proportions (no squish).
        playHint.setBounds(pad, y, colW, ch * 2 - 6); y += ch * 2 - 6 + 2;

        // Log fills whatever is left under the column.
        logLabel.setBounds(pad, y, colW, juce::jmax(0, getHeight() - pad - y));

        // ── Preview (right of the column) + shaping strip under it ──────────
        const int previewX = pad + colW + 10;
        const int chipH = 16, chipGap = 2;
        const int eqH   = ScoreEqComponent::kPreferredH;
        const int shapeH = chipH + 4 + eqH;
        previewArea = juce::Rectangle<int>(previewX, contentTop,
                                           juce::jmax(80, getWidth() - previewX - pad),
                                           juce::jmax(80, getHeight() - pad - contentTop
                                                          - shapeH - gap));

        // Chips row — [start][end]  [in][type][pow]  [out][type][pow].
        {
            const int stripW = previewArea.getWidth();
            const int typeW  = 40;
            const int valW   = juce::jmax(30, (stripW - 2 * typeW - 7 * chipGap) / 6);
            int x = previewX;
            const int cy = previewArea.getBottom() + gap;
            auto place = [&](SamplerValueBox& b, int w)
            {
                b.setBounds(x, cy, w, chipH);
                x += w + chipGap;
            };
            place(cropStartBox_,   valW);
            place(cropEndBox_,     valW);
            place(fadeInLenBox_,   valW);
            place(fadeInTypeBox_,  typeW);
            place(fadeInPowBox_,   valW);
            place(fadeOutLenBox_,  valW);
            place(fadeOutTypeBox_, typeW);
            // Last chip absorbs the integer-division remainder.
            place(fadeOutPowBox_,  juce::jmax(30, previewX + stripW - x));

            eqEditor.setBounds(previewX, cy + chipH + 4, stripW, eqH);
        }

        layoutPreviewScrollbars();
        clampPreviewView();
        updatePreviewScrollbars();
    }

    /** Anchors the zoom scrollbars to the VISIBLE part of the preview frame.
     *  The page is often taller than the zone-3 viewport (small windows), so
     *  a bar glued to the page's far bottom edge would sit below the fold —
     *  exactly where nobody can see it. Re-run from the timer too: scrolling
     *  the outer viewport moves the visible window without notifying us. */
    void layoutPreviewScrollbars()
    {
        const int sb = 10;
        juce::Rectangle<int> vis = previewArea;
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
        {
            const auto seen = previewArea.getIntersection(
                getLocalArea(vp, vp->getLocalBounds()));
            if (! seen.isEmpty())
                vis = seen;
        }
        previewHScroll.setBounds(vis.getX() + 1, vis.getBottom() - sb - 1,
                                 vis.getWidth() - sb - 2, sb);
        previewVScroll.setBounds(previewArea.getRight() - sb - 1, vis.getY() + 1,
                                 sb, vis.getHeight() - sb - 2);
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

    /** Square pause toggle: freeze the transport on the CURRENT column — the
     *  player keeps re-injecting that instant every tick (sustained sound)
     *  and the head can be dragged in the preview while frozen. */
    class MidiScorePauseButton : public juce::Button
    {
    public:
        MidiScorePauseButton() : juce::Button("midiScorePause") {}

        void setPaused(bool p)
        {
            if (p == paused) return;
            paused = p;
            repaint();
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const bool on = paused && isEnabled();
            const juce::Colour accent(kAccentARGB);

            const juce::Colour bg = on ? accent.withAlpha(0.22f)
                                       : juce::Colour(0xff222230);
            g.setColour(down ? bg.brighter(0.30f) : over ? bg.brighter(0.12f) : bg);
            g.fillRoundedRectangle(b, 3.f);
            g.setColour(on ? accent.withAlpha(0.9f) : juce::Colour(0xff33373f));
            g.drawRoundedRectangle(b, 3.f, 1.f);

            const auto inner = b.reduced(b.getHeight() * 0.32f);
            const juce::Colour fg = on ? accent
                                       : juce::Colour(isEnabled() ? 0xff9aa6ba
                                                                  : 0xff555a62);
            const float bw = inner.getWidth() * 0.30f;
            g.setColour(fg);
            g.fillRoundedRectangle(inner.getX(), inner.getY(),
                                   bw, inner.getHeight(), 1.5f);
            g.fillRoundedRectangle(inner.getRight() - bw, inner.getY(),
                                   bw, inner.getHeight(), 1.5f);
        }

    private:
        bool paused = false;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiScorePauseButton)
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

    /** The PLAY-page export button: a plain themed TextButton until an export
     *  job runs, then a bronze progress bar fills its face — real fraction
     *  during the note render, full bar + travelling sheen while the file is
     *  encoded/written (that phase has no byte-level granularity). The page's
     *  20 Hz timer feeds setJobState, which also drives the sheen animation. */
    class ExportImageButton : public juce::TextButton
    {
    public:
        void setJobState(bool active, float frac, bool writing)
        {
            const bool repaintNeeded = active || active != active_;
            active_  = active;
            frac_    = frac;
            writing_ = writing;
            if (repaintNeeded)
                repaint();
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            juce::TextButton::paintButton(g, over, down);
            if (! active_)
                return;

            const auto b = getLocalBounds().toFloat().reduced(1.5f);
            const juce::Colour accent(kAccentARGB);

            g.setColour(accent.withAlpha(0.30f));
            g.fillRoundedRectangle(
                b.withWidth(b.getWidth() * juce::jlimit(0.f, 1.f, frac_)), 3.f);

            if (writing_)
            {
                const float t = (float) (juce::Time::getMillisecondCounter() % 1200u)
                              / 1200.f;
                const float bandW = b.getWidth() * 0.18f;
                const float x = b.getX() + t * (b.getWidth() + bandW) - bandW;
                juce::ColourGradient sheen(accent.withAlpha(0.f), x, 0.f,
                                           accent.withAlpha(0.f), x + bandW, 0.f,
                                           false);
                sheen.addColour(0.5, accent.withAlpha(0.35f));
                g.setGradientFill(sheen);
                g.fillRoundedRectangle(b, 3.f);
            }

            g.setColour(accent.withAlpha(0.9f));
            g.drawRoundedRectangle(b, 3.f, 1.2f);
        }

    private:
        bool  active_  = false, writing_ = false;
        float frac_    = 0.f;
    };

    /** Export zone picker for pieces longer than one page: the whole piece as
     *  one strip, a page-wide frame dragged over it, EXPORT ZONE writes that
     *  window as a single sheet (ALL PAGES keeps the classic numbered export).
     *  Parented to the top-level component and self-deleting, like
     *  Sp3ctraDialog. */
    class ExportZoneDialog : public juce::Component
    {
    public:
        static constexpr int kPad     = 12;
        static constexpr int kChromeH = 100;   ///< header + button row + padding

        std::function<void(double)> onExportZone;   ///< arg = window start (s)
        std::function<void()>       onExportAll;

        ExportZoneDialog(juce::Image strip, double durationSec, double pageSec,
                         int nPages, const juce::String& sheetName,
                         juce::Colour accentColour)
            : strip_(strip),
              duration_(juce::jmax(0.05, durationSec)),
              pageSec_(juce::jlimit(0.05, duration_, pageSec)),
              sheet_(sheetName), accent_(accentColour)
        {
            exportBtn_.setButtonText("EXPORT ZONE");
            exportBtn_.onClick = [this]
            {
                if (onExportZone)
                    onExportZone(startFrac_ * duration_);
                dismiss();
            };
            addAndMakeVisible(exportBtn_);

            allBtn_.setButtonText("ALL PAGES (" + juce::String(nPages) + ")");
            allBtn_.setTooltip("Export every page as numbered files instead "
                               "of framing one zone.");
            allBtn_.onClick = [this]
            {
                if (onExportAll)
                    onExportAll();
                dismiss();
            };
            addAndMakeVisible(allBtn_);

            cancelBtn_.setButtonText("CANCEL");
            cancelBtn_.onClick = [this] { dismiss(); };
            addAndMakeVisible(cancelBtn_);

            setWantsKeyboardFocus(true);
        }

        void paint(juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();
            g.setColour(juce::Colour(0xf5171a22));
            g.fillRoundedRectangle(b, 5.f);
            g.setColour(accent_.withAlpha(0.5f));
            g.drawRoundedRectangle(b.reduced(0.5f), 5.f, 1.f);

            const auto sr = stripRect();

            g.setColour(accent_);
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
            g.drawText("EXPORT " + sheet_
                           + juce::String::fromUTF8(" — drag the frame over the zone to print"),
                       kPad, 8, getWidth() - 2 * kPad, 18,
                       juce::Justification::centredLeft);

            const double t0 = startFrac_ * duration_;
            g.setColour(juce::Colour(0xffb8c0d0));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
            g.drawText(juce::String(t0, 1) + " - "
                           + juce::String(t0 + pageSec_, 1) + " s  /  "
                           + juce::String(duration_, 1) + " s",
                       kPad, 8, getWidth() - 2 * kPad, 18,
                       juce::Justification::centredRight);

            if (! sr.isEmpty())
            {
                g.setColour(juce::Colours::white);
                g.fillRect(sr);
                g.drawImage(strip_, sr);

                // Frame = the exported window; everything outside is dimmed.
                const auto fr = frameRect();
                g.setColour(juce::Colour(0x99000000));
                g.fillRect(juce::Rectangle<float>(sr.getX(), sr.getY(),
                                                  fr.getX() - sr.getX(), sr.getHeight()));
                g.fillRect(juce::Rectangle<float>(fr.getRight(), sr.getY(),
                                                  sr.getRight() - fr.getRight(),
                                                  sr.getHeight()));
                g.setColour(accent_);
                g.drawRect(fr.expanded(1.0f), 2.0f);
            }
        }

        void resized() override
        {
            const int ch = Sp3ctraTheme::kControlH;
            const int by = getHeight() - kPad - ch;
            exportBtn_.setBounds(kPad, by, 130, ch);
            allBtn_  .setBounds(kPad + 130 + 8, by, 130, ch);
            cancelBtn_.setBounds(getWidth() - kPad - 90, by, 90, ch);
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            const auto sr = stripRect();
            if (sr.isEmpty() || ! sr.contains(e.position))
                return;
            const double f = (e.position.x - sr.getX()) / sr.getWidth();
            const double pf = pageFrac();
            // Grab the frame where it stands; clicking outside it re-centres
            // it on the click first (then the same drag moves it).
            if (f < startFrac_ || f > startFrac_ + pf)
                startFrac_ = juce::jlimit(0.0, 1.0 - pf, f - pf * 0.5);
            dragOffset_ = f - startFrac_;
            dragging_   = true;
            repaint();
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (! dragging_)
                return;
            const auto sr = stripRect();
            if (sr.isEmpty())
                return;
            const double f = (e.position.x - sr.getX()) / sr.getWidth();
            startFrac_ = juce::jlimit(0.0, 1.0 - pageFrac(), f - dragOffset_);
            repaint();
        }

        void mouseUp(const juce::MouseEvent&) override { dragging_ = false; }

        bool keyPressed(const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::escapeKey)
            {
                dismiss();
                return true;
            }
            if (k == juce::KeyPress::returnKey)
            {
                exportBtn_.triggerClick();
                return true;
            }
            return false;
        }

    private:
        juce::Rectangle<float> stripRect() const
        {
            // Header (30) above, button row (+ padding) below — the strip
            // gets everything in between.
            const float top = 30.f;
            const float bot = (float) (getHeight() - kPad
                                       - Sp3ctraTheme::kControlH - 8);
            const juce::Rectangle<float> area(
                (float) kPad, top, (float) (getWidth() - 2 * kPad),
                juce::jmax(0.f, bot - top));
            if (! strip_.isValid() || area.isEmpty())
                return {};
            return juce::RectanglePlacement(juce::RectanglePlacement::centred)
                .appliedTo(juce::Rectangle<float>(0.f, 0.f,
                               (float) strip_.getWidth(), (float) strip_.getHeight()),
                           area);
        }

        double pageFrac() const { return juce::jlimit(0.0, 1.0, pageSec_ / duration_); }

        juce::Rectangle<float> frameRect() const
        {
            const auto sr = stripRect();
            const float w = juce::jmax(4.0f, (float) pageFrac() * sr.getWidth());
            return { sr.getX() + (float) startFrac_ * sr.getWidth(), sr.getY(),
                     juce::jmin(w, sr.getRight() - (sr.getX() + (float) startFrac_ * sr.getWidth())),
                     sr.getHeight() };
        }

        void dismiss()
        {
            juce::MessageManager::callAsync(
                [sp = juce::Component::SafePointer<ExportZoneDialog>(this)]
                {
                    if (sp != nullptr)
                    {
                        if (auto* p = sp->getParentComponent())
                            p->removeChildComponent(sp.getComponent());
                        delete sp.getComponent();
                    }
                });
        }

        juce::Image  strip_;
        double       duration_, pageSec_;
        juce::String sheet_;
        juce::Colour accent_;
        double startFrac_ = 0.0, dragOffset_ = 0.0;
        bool   dragging_  = false;
        juce::TextButton exportBtn_, allBtn_, cancelBtn_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExportZoneDialog)
    };

    /** Top tab for one voice: track/channel name + timbre preset, dimmed when
     *  the voice is inaudible (muted, un-soloed while another voice solos, or
     *  absent from the loaded file). The left strip holds two mixer buttons
     *  that act WITHOUT selecting the voice: the LED (top) mutes/unmutes,
     *  the S (bottom) solos. */
    class VoiceTab : public juce::Button
    {
    public:
        explicit VoiceTab(int idx) : juce::Button("midiScoreVoice"), index(idx) {}

        std::function<juce::String(int)> textProvider;   // set by the page
        std::function<bool(int)>         enabledProvider;   // own mute state (LED)
        std::function<bool(int)>         soloProvider;      // own solo state (S)
        std::function<bool(int)>         audibleProvider;   // net result (text dim)
        std::function<bool(int)>         selectedProvider;
        std::function<void(int)>         onToggle;        // LED clicked
        std::function<void(int)>         onSolo;          // S clicked

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const bool sel  = selectedProvider && selectedProvider(index);
            const bool on   = enabledProvider  && enabledProvider(index);
            const bool solo = soloProvider     && soloProvider(index);
            const bool aud  = audibleProvider  ? audibleProvider(index) : on;
            const juce::Colour accent(kAccentARGB);
            const juce::Colour soloCol(0xff58c470);

            juce::Colour bg = sel ? accent.withAlpha(0.20f) : juce::Colour(0xff1a1d26);
            g.setColour(down ? bg.brighter(0.25f) : over ? bg.brighter(0.10f) : bg);
            g.fillRoundedRectangle(b, 3.f);
            g.setColour(sel ? accent.withAlpha(0.9f) : juce::Colour(0xff33373f));
            g.drawRoundedRectangle(b, 3.f, sel ? 1.4f : 1.f);

            // Mute LED (top of the strip): lit = the voice itself is on.
            const auto led = ledBounds();
            if (on)
            {
                g.setColour(accent.withAlpha(hoverZone == Zone::led ? 1.0f : 0.85f));
                g.fillEllipse(led);
            }
            else
            {
                g.setColour(juce::Colour(hoverZone == Zone::led ? 0xff8a8f98
                                                                : 0xff555a62));
                g.drawEllipse(led.reduced(0.5f), 1.2f);
            }

            // Solo letter (bottom of the strip).
            g.setColour(solo ? soloCol.withAlpha(hoverZone == Zone::solo ? 1.0f : 0.9f)
                             : juce::Colour(hoverZone == Zone::solo ? 0xff8a8f98
                                                                    : 0xff555a62));
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText("S", soloZone(), juce::Justification::centred);

            g.setColour(aud ? (sel ? accent : juce::Colour(0xffb8c0d0))
                            : juce::Colour(0xff555a62));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            g.drawFittedText(textProvider ? textProvider(index) : juce::String(index + 1),
                             getLocalBounds().reduced(kLedZoneW, 2),
                             juce::Justification::centred, 2);
        }

        // The strip swallows its presses so mute/solo never steal the
        // selection (Button never sees the down → no onClick on release).
        void mouseDown(const juce::MouseEvent& e) override
        {
            pressZone = zoneAt(e.getPosition());
            if (pressZone == Zone::none)
                juce::Button::mouseDown(e);
        }
        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (pressZone == Zone::none)
                juce::Button::mouseDrag(e);
        }
        void mouseUp(const juce::MouseEvent& e) override
        {
            if (pressZone != Zone::none)
            {
                const Zone released = zoneAt(e.getPosition());
                if (released == pressZone)
                {
                    if (pressZone == Zone::led  && onToggle) onToggle(index);
                    if (pressZone == Zone::solo && onSolo)   onSolo(index);
                }
                pressZone = Zone::none;
                return;
            }
            juce::Button::mouseUp(e);
        }
        void mouseMove(const juce::MouseEvent& e) override
        {
            setHoverZone(zoneAt(e.getPosition()));
            juce::Button::mouseMove(e);
        }
        void mouseExit(const juce::MouseEvent& e) override
        {
            setHoverZone(Zone::none);
            juce::Button::mouseExit(e);
        }

    private:
        static constexpr int kLedZoneW = 18;   // clickable strip on the left

        enum class Zone { none, led, solo };

        juce::Rectangle<int> stripZone() const
        {
            return getLocalBounds().removeFromLeft(kLedZoneW);
        }
        juce::Rectangle<int> ledZone() const
        {
            auto s = stripZone();
            return s.removeFromTop(getHeight() / 2);
        }
        juce::Rectangle<int> soloZone() const
        {
            auto s = stripZone();
            s.removeFromTop(getHeight() / 2);
            return s;
        }
        Zone zoneAt(juce::Point<int> p) const
        {
            if (ledZone().contains(p))  return Zone::led;
            if (soloZone().contains(p)) return Zone::solo;
            return Zone::none;
        }
        juce::Rectangle<float> ledBounds() const
        {
            const float d = 7.f;
            const auto  z = ledZone().toFloat();
            return { z.getCentreX() - d * 0.5f, z.getCentreY() - d * 0.5f + 1.f, d, d };
        }
        void setHoverZone(Zone z)
        {
            if (z == hoverZone) return;
            hoverZone = z;
            repaint();
        }

        int  index;
        Zone hoverZone = Zone::none;
        Zone pressZone = Zone::none;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceTab)
    };

    //==========================================================================
    timbregen::TimbreSlotParams& cur() { return voices[(size_t) selectedVoice]; }

    bool anyVoiceSoloed() const
    {
        for (bool s : voiceSolo)
            if (s) return true;
        return false;
    }

    /** Net audibility of a voice: while ANY voice solos, only the soloed set
     *  plays/prints (solo wins over mute — soloing is asking to HEAR it);
     *  with no solo, the per-voice enable LEDs rule. */
    bool voiceAudible(int idx) const
    {
        return anyVoiceSoloed() ? voiceSolo[(size_t) idx]
                                : voices[(size_t) idx].enabled;
    }

    /** The render-facing voice set — user timbres with solo/mute resolved
     *  into the enabled flags. Every render path (preview, playback, live
     *  reload, export) goes through this so what you hear is what prints. */
    std::array<timbregen::TimbreSlotParams, midiscoregen::kMaxVoices>
    effectiveVoices() const
    {
        auto out = voices;
        for (int i = 0; i < midiscoregen::kMaxVoices; ++i)
            out[(size_t) i].enabled = voiceAudible(i);
        return out;
    }

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
        repaint(previewArea);   // the pan overlay follows the selected voice
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

    /** Colour frames only pan if LuxStral's stereo engine runs — turn it on
     *  (its published APVTS switch) so the pan isn't silently mono; never
     *  touched when loading mono frames (same policy as SCORE's stereo). */
    void maybeEnableLuxstralStereo(bool stereoFrames)
    {
        if (! stereoFrames)
            return;
        if (auto* p = processor.getAPVTS().getParameter("luxstralStereoEnable"))
            if (p->getValue() < 0.5f)
                p->setValueNotifyingHost(1.0f);
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
        // Import: seed from the last MIDI directory used.
        fileChooser = std::make_unique<juce::FileChooser>(
            "Load MIDI File",
            processor.sessions()->startDirFor(
                PathKeys::midiImport,
                juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)),
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
                self->processor.sessions()->rememberDirFor(PathKeys::midiImport, f);
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
        const juce::String sheet = s.pageFormat == 2 ? "1 sheet (FULL)"
                                 : juce::String(pages) + " page(s) "
                                   + (s.pageFormat == 1 ? "A3" : "A4");
        fileLabel.setText(juce::File(data.sourcePath).getFileName()
                          + juce::String::fromUTF8("  —  ") + juce::String((int) data.notes.size()) + " notes, "
                          + juce::String(data.numVoices) + " voice(s), "
                          + juce::String(data.durationSec, 1) + " s, "
                          + sheet,
                          juce::dontSendNotification);
    }

    //==========================================================================
    // Preview renders on a background thread — the OLD strip stays on screen
    // (and under the pan overlay) until the new one lands, so edits never
    // freeze the UI. One render in flight; edits meanwhile re-arm the timer.
    void regenPreview()
    {
        if (! data.ok || data.notes.empty())
        {
            previewDirty = false;
            previewImage = juce::Image();
            repaint(previewArea);
            return;
        }
        if (previewRenderBusy_)
        {
            previewDirty = true;   // picked up again when the render lands
            return;
        }
        previewDirty       = false;
        previewRenderBusy_ = true;

        // Overview strip: whole piece, width-capped, reduced frequency axis.
        constexpr double kPreviewDpiY   = 150.0;
        constexpr double kPreviewMaxPxW = 3600.0;
        const double dur = juce::jmax(0.1, data.durationSec);
        const double pxPerSec = juce::jmin(400.0, kPreviewMaxPxW / dur);
        juce::Thread::launch(
            [safe = juce::Component::SafePointer<MidiScoreGenTabComponent>(this),
             dataCopy = data, voicesCopy = effectiveVoices(),
             s = settingsWithTuning(), dur, pxPerSec]
            {
                auto r = midiscoregen::renderStrip(dataCopy, voicesCopy, s,
                                                   0.0, dur, pxPerSec, kPreviewDpiY);
                juce::MessageManager::callAsync(
                    [safe, r = std::move(r)]
                    {
                        if (auto* self = safe.getComponent())
                            self->applyPreviewRender(r);
                    });
            });
    }

    void applyPreviewRender(const scoregen::RenderResult& r)
    {
        previewRenderBusy_ = false;
        previewImage = (r.ok && r.image.isValid()) ? r.image : juce::Image();
        if (! r.ok)
            logLabel.setText(r.log, juce::dontSendNotification);
        if (! previewImage.isValid())
            previewZoom_ = 1.0;      // nothing to zoom into anymore
        // The strip changed (edit / new file): the hi-res tile shows the OLD
        // content — drop it, the timer re-renders the visible window. The
        // epoch bump also voids any tile still in flight on the worker.
        hiResTile_ = juce::Image();
        tileT0_ = tileT1_ = 0.0;
        ++tileEpoch_;
        clampPreviewView();          // new image ⇒ new fit geometry
        updatePreviewScrollbars();
        repaint(previewArea);
    }

    //==========================================================================
    // Playback: render the CROP WINDOW as one strip at the physical time scale
    // (DPI × writing speed), capped in frame count for very long pieces, shape
    // it (edge fades + image EQ), and load it into the shared score player.
    // The player injects 1000 columns/s at speed 1x, so "real tempo" ≈
    // (px/s ÷ 1000) on the Speed knob — logged.
    static constexpr int kMaxPlayFrames = 30000;   // ≈ 310 MB of frames

    /** Message-thread copy of the playback shaping, safe to carry onto the
     *  live-reload worker thread (the EQ curve is snapshotted as plain data). */
    struct ShapeSnapshot
    {
        float cropStart = 0.0f, cropEnd = 1.0f;
        float fadeInLen = 0.0f, fadeOutLen = 0.0f;
        FadeCurveType fadeInType = FadeCurveType::LINEAR;
        FadeCurveType fadeOutType = FadeCurveType::LINEAR;
        float fadeInPow = 1.0f, fadeOutPow = 1.0f;
        double eqMinF = 0.0, eqMaxF = 0.0;
        std::vector<float> eqGains;

        bool eqActive() const noexcept
        {
            if (eqGains.size() < 2 || eqMinF <= 0.0 || eqMaxF <= eqMinF)
                return false;
            for (float g : eqGains)
                if (std::abs(g) > 0.01f) return true;
            return false;
        }
        bool fadesActive() const noexcept
        { return fadeInLen > 1.0e-3f || fadeOutLen > 1.0e-3f; }
        bool active() const noexcept { return eqActive() || fadesActive(); }
    };

    ShapeSnapshot shapeSnapshot() const
    {
        ShapeSnapshot sp;
        sp.cropStart   = cropStart_;
        sp.cropEnd     = cropEnd_;
        sp.fadeInLen   = fadeInLen_;
        sp.fadeOutLen  = fadeOutLen_;
        sp.fadeInType  = fadeInType_;
        sp.fadeOutType = fadeOutType_;
        sp.fadeInPow   = fadeInPow_;
        sp.fadeOutPow  = fadeOutPow_;
        sp.eqMinF      = eqEditor.getMinFreq();
        sp.eqMaxF      = eqEditor.getMaxFreq();
        sp.eqGains     = eqEditor.getGains();
        return sp;
    }

    /** Shapes a rendered PLAYBACK strip in place (never the preview / export):
     *  each band row's ink shifts by the EQ gain over the score's dB range
     *  (same convention as SCORE's applyEqToImage — a −cut lightens toward
     *  silence, a +boost darkens, pure-white silence never gains energy) and
     *  the crop-window edges fade with the SAMPLER curves (linear gain →
     *  dB → darkness shift, so a fade ends in actual silence). Static + pure:
     *  called from the message thread AND the live-reload worker. */
    static void applyPlaybackShaping(juce::Image& img,
                                     juce::Rectangle<int> band, bool stereo,
                                     const ShapeSnapshot& sp,
                                     const midiscoregen::MidiScoreSettings& s)
    {
        if (! img.isValid() || ! sp.active())
            return;
        band = band.getIntersection(img.getBounds());
        if (band.isEmpty())
            band = img.getBounds();

        const double range = juce::jmax(1.0, s.dynamicRangeDB);

        // Per-row EQ darkness shift (constant along a row).
        std::vector<float> rowShift((size_t) band.getHeight(), 0.0f);
        if (sp.eqActive() && s.minFreq > 0.0 && s.maxFreq > s.minFreq)
        {
            const double bandBottom = (double) band.getBottom();
            const double bandH      = (double) juce::jmax(1, band.getHeight());
            const double ratio      = s.maxFreq / s.minFreq;
            const int    n          = (int) sp.eqGains.size();
            for (int yy = band.getY(); yy < band.getBottom(); ++yy)
            {
                const double pos  = juce::jlimit(0.0, 1.0, (bandBottom - (yy + 0.5)) / bandH);
                const double freq = s.minFreq * std::pow(ratio, pos);
                const double x    = std::log(freq / sp.eqMinF)
                                  / std::log(sp.eqMaxF / sp.eqMinF) * (double) (n - 1);
                const float gdb   = eqCurveDbAt(sp.eqGains.data(), n,
                                                juce::jlimit(0.0f, (float) (n - 1), (float) x),
                                                ScoreEqComponent::kGainRange);
                rowShift[(size_t) (yy - band.getY())] = (float) (gdb / range);
            }
        }

        // Per-column fade darkness shift (constant down a column). The strip
        // IS the crop window, so the fades sit on its first/last fractions.
        std::vector<float> colShift((size_t) band.getWidth(), 0.0f);
        if (sp.fadesActive())
        {
            const double bandW = (double) juce::jmax(1, band.getWidth());
            for (int xx = 0; xx < band.getWidth(); ++xx)
            {
                const double u = (xx + 0.5) / bandW;   // 0..1 across the window
                float gain = 1.0f;
                if (sp.fadeInLen > 1.0e-3f && u < (double) sp.fadeInLen)
                    gain *= applyFadeCurve((float) (u / sp.fadeInLen),
                                           sp.fadeInType, sp.fadeInPow);
                if (sp.fadeOutLen > 1.0e-3f && u > 1.0 - (double) sp.fadeOutLen)
                    gain *= applyFadeCurve((float) ((1.0 - u) / sp.fadeOutLen),
                                           sp.fadeOutType, sp.fadeOutPow);
                if (gain >= 0.999f)
                    continue;
                const double db = (gain <= 1.0e-6f)
                    ? -range : juce::jmax(-range, 20.0 * std::log10((double) gain));
                colShift[(size_t) xx] = (float) (db / range);
            }
        }

        juce::Image::BitmapData bmp(img, juce::Image::BitmapData::readWrite);
        for (int yy = band.getY(); yy < band.getBottom(); ++yy)
        {
            const float rs = rowShift[(size_t) (yy - band.getY())];
            // Row LUT covers the EQ part; fade columns add their own shift.
            juce::uint8 lut[256];
            for (int v = 0; v < 256; ++v)
            {
                if (v >= 255 && rs > 0.0f) { lut[v] = 255; continue; }   // silence stays silent
                const float dk = juce::jlimit(0.0f, 1.0f, (1.0f - (float) v / 255.0f) + rs);
                lut[v] = (juce::uint8) juce::jlimit(0, 255,
                             (int) std::lround((1.0f - dk) * 255.0f));
            }
            juce::uint8* line = bmp.getLinePointer(yy);
            for (int xx = band.getX(); xx < band.getRight(); ++xx)
            {
                const float cs = colShift[(size_t) (xx - band.getX())];
                juce::uint8* p = line + xx * bmp.pixelStride;
                if (cs == 0.0f)
                {
                    if (stereo) { p[0] = lut[p[0]]; p[1] = lut[p[1]]; p[2] = lut[p[2]]; }
                    else        { p[0] = p[1] = p[2] = lut[p[0]]; }
                    continue;
                }
                const float shift = rs + cs;
                auto shape = [shift](juce::uint8 v) -> juce::uint8
                {
                    if (v >= 255 && shift > 0.0f) return 255;
                    const float dk = juce::jlimit(0.0f, 1.0f,
                                                  (1.0f - (float) v / 255.0f) + shift);
                    return (juce::uint8) juce::jlimit(0, 255,
                               (int) std::lround((1.0f - dk) * 255.0f));
                };
                if (stereo) { p[0] = shape(p[0]); p[1] = shape(p[1]); p[2] = shape(p[2]); }
                else        { p[0] = p[1] = p[2] = shape(p[0]); }
            }
        }
    }

    bool reloadPlayFrames()
    {
        auto* fs = boundChannel();
        if (fs == nullptr || ! data.ok || data.notes.empty())
            return false;

        syncEqRange();   // playback maps EQ rows onto the current tuning span
        const auto s = settingsWithTuning();
        const double dur = juce::jmax(0.05, data.durationSec);
        const double t0  = juce::jlimit(0.0, dur, (double) cropStart_ * dur);
        const double t1  = juce::jlimit(t0 + 0.05, dur, (double) cropEnd_ * dur);
        const double win = juce::jmax(0.05, t1 - t0);
        double pxPerSec = (s.printerDpi / 2.54) * s.writingSpeed;
        bool reduced = false;
        if (win * pxPerSec > (double) kMaxPlayFrames)
        {
            pxPerSec = (double) kMaxPlayFrames / win;   // long window: coarser time grid
            reduced  = true;
        }

        auto r = midiscoregen::renderStrip(data, effectiveVoices(), s, t0, t1,
                                           pxPerSec, 400.0);   // full CIS height
        if (! (r.ok && r.image.isValid()))
        {
            logLabel.setText("Failed: " + r.log, juce::dontSendNotification);
            return false;
        }
        applyPlaybackShaping(r.image, r.spectroBand, r.stereo, shapeSnapshot(), s);

        fs->loadScoreFramesFromImage(r.image, r.spectroBand, s.minFreq, s.maxFreq,
                                     r.stereo);
        maybeEnableLuxstralStereo(r.stereo);
        framesAreOurs    = true;
        loadedFrameCount = fs->getScoreFrameCount();
        scrubHead        = -1;
        playDirty        = false;

        juce::String msg = juce::String(loadedFrameCount) + juce::String::fromUTF8(" frames loaded — ")
                         + "real tempo at Speed " + juce::String(pxPerSec / 1000.0, 2) + "x";
        if (cropStart_ > 0.001f || cropEnd_ < 0.999f)
            msg += "  (crop " + juce::String(juce::roundToInt(cropStart_ * 100.0f)) + "-"
                 + juce::String(juce::roundToInt(cropEnd_ * 100.0f)) + "%)";
        if (reduced)
            msg += " (long piece: time grid reduced to "
                 + juce::String(pxPerSec, 0) + " px/s)";
        if (r.stereo)
            msg += juce::String::fromUTF8(" — pan active (L=red / R=blue)");
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
        // Live-follow whenever OUR content is audibly fed: playing, frozen
        // (pause of a running transport keeps isScorePlaying true) or held
        // from stop — the hot-swap below never disturbs the transport.
        if (fs == nullptr
            || ! (fs->isScorePlaying() || pauseMode == PauseMode::held)
            || fs->getScoreFrameCount() != loadedFrameCount)
            return;   // stopped or channel reclaimed → next PLAY reloads

        syncEqRange();
        const auto s = settingsWithTuning();
        const double dur = juce::jmax(0.05, data.durationSec);
        const double t0  = juce::jlimit(0.0, dur, (double) cropStart_ * dur);
        const double t1  = juce::jlimit(t0 + 0.05, dur, (double) cropEnd_ * dur);
        const double win = juce::jmax(0.05, t1 - t0);
        double pxPerSec = (s.printerDpi / 2.54) * s.writingSpeed;
        if (win * pxPerSec > (double) kMaxPlayFrames)
            pxPerSec = (double) kMaxPlayFrames / win;

        playDirty       = false;   // edits landing during the render re-arm it
        liveRenderBusy_ = true;

        // The WHOLE expensive path runs off-thread: strip render, playback
        // shaping (fades + EQ) AND the image→frames conversion. The old frames
        // keep playing untouched; the message thread only performs an O(1)
        // vector swap at the end.
        juce::Thread::launch(
            [safe = juce::Component::SafePointer<MidiScoreGenTabComponent>(this),
             dataCopy = data, voicesCopy = effectiveVoices(), s, t0, t1, pxPerSec,
             sp = shapeSnapshot()]
            {
                auto r = midiscoregen::renderStrip(dataCopy, voicesCopy, s,
                                                   t0, t1, pxPerSec, 400.0);
                std::vector<CapturedFrame> frames;
                if (r.ok && r.image.isValid())
                {
                    applyPlaybackShaping(r.image, r.spectroBand, r.stereo, sp, s);
                    frames = ScorePlayerService::buildFramesFromImage(
                        r.image, r.spectroBand, s.minFreq, s.maxFreq, r.stereo);
                }
                r.image = juce::Image();   // big bitmap not needed anymore
                juce::MessageManager::callAsync(
                    [safe, r = std::move(r), frames = std::move(frames)]() mutable
                    {
                        if (auto* self = safe.getComponent())
                            self->applyLiveReload(r, std::move(frames));
                    });
            });
    }

    void applyLiveReload(const scoregen::RenderResult& r,
                         std::vector<CapturedFrame>&& frames)
    {
        liveRenderBusy_ = false;
        auto* fs = boundChannel();
        // The channel changed hands (other page / instance rebind) during
        // the render — don't touch a player that is no longer ours; re-arm
        // so the next PLAY reloads instead.
        if (fs == nullptr || ! framesAreOurs
            || fs->getScoreFrameCount() != loadedFrameCount)
        {
            playDirty = true;
            return;
        }
        if (! r.ok || frames.empty())
        {
            logLabel.setText("Failed: " + r.log, juce::dontSendNotification);
            return;
        }

        // In-place swap: the transport (and any pause hold) never stops, the
        // head lands on the same musical position in the new time grid.
        const int newCount = (int) frames.size();
        fs->uiHotSwapScoreFrames(std::move(frames));
        maybeEnableLuxstralStereo(r.stereo);
        loadedFrameCount = newCount;
        logLabel.setText(juce::String::fromUTF8("Live update — ")
                             + juce::String(newCount) + " frames",
                         juce::dontSendNotification);
        repaint(previewArea);
    }

    /** PAUSE = keep delivering the same image instant. Two flavours sharing
     *  one button: freezing a RUNNING transport rides the player's scrub-hold
     *  (session alive, held column re-injected every tick); from STOP it is a
     *  sticky scrub session (the drag-audition without holding the mouse).
     *  In both, clicking/dragging the preview moves the held column live. */
    enum class PauseMode { none, playing, held };

    void togglePause()
    {
        auto* fs = boundChannel();
        if (fs == nullptr) return;

        if (pauseMode != PauseMode::none)   // release
        {
            if (pauseMode == PauseMode::playing) fs->uiSetScorePaused(false);
            else                                 fs->uiEndScoreScrub();
            pauseMode = PauseMode::none;
            pauseButton.setPaused(false);
            repaint(previewArea);
            return;
        }

        if (fs->isScorePlaying() && framesAreOurs)
        {
            fs->uiSetScorePaused(true);
            pauseMode = PauseMode::playing;
        }
        else
        {
            if ((! framesAreOurs || playDirty) && ! reloadPlayFrames())
                return;
            if (! fs->uiBeginScoreScrub())
                return;
            pauseMode = PauseMode::held;   // holds wherever the head sits
        }
        pauseButton.setPaused(true);
        repaint(previewArea);
    }

    void releasePauseState()
    {
        pauseMode = PauseMode::none;
        pauseButton.setPaused(false);
    }

    void togglePlay()
    {
        auto* fs = boundChannel();
        if (fs == nullptr) return;

        // PLAY and STOP both leave pause mode (play() / stop() clear the
        // player-side hold themselves; the held session just becomes ours).
        releasePauseState();

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

        // Route through MIDI SCORE's own play param so the DAW sees the
        // transport; the processor pushes speed/loop and starts/stops its slot.
        if (auto* p = processor.getAPVTS().getParameter("midiScorePlaying"))
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
        auto* fs = boundChannel();
        if (fs == nullptr || ! framesAreOurs) return;
        const int n = fs->getScoreFrameCount();
        if (n <= 0) return;
        const auto area = previewImgArea.isEmpty() ? previewImageBounds() : previewImgArea;
        if (area.getWidth() <= 0.f) return;
        // The loaded frames cover the CROP window only: clamp the click into
        // it and map the piece fraction back onto the frame axis.
        const float fx = juce::jlimit(cropStart_, cropEnd_,
            ((float) e.position.x - area.getX()) / area.getWidth());
        const float wf = (fx - cropStart_) / cropSpan();
        scrubHead = juce::jlimit(0, n - 1, (int) (wf * (float) n));
        fs->uiSeekScore(scrubHead);
        repaint(previewArea);
    }

    void timerCallback() override
    {
        // Zoomed: keep the scrollbars pinned to the VISIBLE slice of the
        // preview while the outer zone-3 viewport scrolls (no notification),
        // and sharpen the visible window once the view settles.
        if (previewZoom_ > 1.001)
            layoutPreviewScrollbars();
        maybeStartTileRender();

        // Running export: feed the button's progress bar (and animate the
        // sheen) from the job's atomics; the label doubles as a status line.
        if (exportJob_ != nullptr)
        {
            const float frac    = exportJob_->frac;
            const bool  writing = exportJob_->writing;
            exportButton.setJobState(true, frac, writing);
            exportButton.setButtonText(
                writing ? juce::String::fromUTF8("Writing file…")
                        : "Exporting " + juce::String((int) (frac * 100.f)) + "%");
        }

        // Debounced live regeneration (drag-friendly).
        if (previewDirty && ! previewRenderBusy_
            && juce::Time::getMillisecondCounter() - lastEditMs > 200)
        {
            regenPreview();
            refreshFileLabel();   // page count follows the writing speed
            persistState();
        }

        // Crop/fade/EQ edits change no preview strip (overlay only) — persist
        // them on their own debounce, once the gesture has settled.
        if (stateDirty && ! previewDirty
            && shapeDrag_ == ShapeDrag::None && ! eqEditor.isDragging()
            && juce::Time::getMillisecondCounter() - lastEditMs > 400)
            persistState();

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
                releasePauseState();   // whoever reclaimed the slot ended it
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

        // Mirror MIDI SCORE's play param on the real engine state (DAW lane
        // truthful when a one-shot ends / an internal reload stops playback).
        if (auto* p = processor.getAPVTS().getParameter("midiScorePlaying"))
        {
            const float norm = (fs != nullptr && fs->isScorePlaying() && framesAreOurs)
                                 ? 1.0f : 0.0f;
            if (! juce::approximatelyEqual(p->getValue(), norm))
                p->setValueNotifyingHost(norm);
        }
    }

    //==========================================================================
    // Export. Format (PNG/JPEG), sheet size (A4/A3/FULL) and DPI come from the
    // SETUP face. One page (or FULL) exports straight away; a piece longer
    // than one page opens the zone picker — a movable page-wide frame over the
    // whole strip — with the classic numbered every-page export as fallback.
    void exportNow()
    {
        if (LicenseGate::blockIfDemo(this, "Export image"))
            return;
        if (exportJob_ != nullptr)
            return;   // an export is already running — the button shows it
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
        if (nPages == 1)
        {
            exportSheetAt(0.0, {});
            return;
        }
        openExportZoneDialog(s, nPages);
    }

    // Exports run on a BACKGROUND thread (renderSheet/exportImage are pure) so
    // the UI stays live: the button fills with the real render fraction, then
    // shows a travelling sheen while the file is encoded/written. Progress is
    // shared through a shared_ptr so a page teardown mid-export is harmless.
    struct ExportProgress
    {
        std::atomic<float> frac    { 0.f };
        std::atomic<bool>  writing { false };
    };

    void startExportJob()
    {
        exportJob_ = std::make_shared<ExportProgress>();
        exportButton.setJobState(true, 0.f, false);
    }

    void finishExport(const juce::String& msg)
    {
        exportJob_.reset();
        exportButton.setJobState(false, 0.f, false);
        exportButton.setButtonText("Export image");
        logLabel.setText(msg, juce::dontSendNotification);
    }

    /** Renders + writes ONE sheet starting at t0Sec (the zone picker's frame,
     *  or 0 for single-page / FULL exports). Asynchronous. */
    void exportSheetAt(double t0Sec, const juce::String& pageTag)
    {
        if (exportJob_ != nullptr)
            return;   // one export at a time
        const auto s = settingsWithTuning();
        const juce::File out = uniqueExportFile(exportAsPng_ ? "png" : "jpg");
        startExportJob();
        juce::Thread::launch(
            [safe = juce::Component::SafePointer<MidiScoreGenTabComponent>(this),
             job = exportJob_, dataCopy = data, voicesCopy = effectiveVoices(),
             s, t0Sec, pageTag, asPng = exportAsPng_, out,
             session = processor.sessions()->sessionName()]
            {
                const auto r = midiscoregen::renderSheet(
                    dataCopy, voicesCopy, s, t0Sec, pageTag,
                    [job](double p) { job->frac = (float) p; });

                juce::String msg;
                if (! (r.ok && r.image.isValid()))
                    msg = "Failed: " + r.log;
                // libjpeg hard-caps both dimensions at 65500 px — a FULL
                // sheet can be wider. Only PNG can hold it.
                else if (! asPng && (r.pixelWidth > 65500 || r.pixelHeight > 65500))
                    msg = "Too large for JPEG (65500 px max per side) "
                        + juce::String::fromUTF8("— switch the format to PNG in SETUP");
                else
                {
                    job->frac    = 1.f;
                    job->writing = true;
                    msg = writeExportImage(r, out, asPng, s)
                        ? "Exported: " + out.getFileName()
                              + juce::String::fromUTF8(" → ") + session + "/exports"
                        : "Export failed: " + out.getFileName();
                }
                juce::MessageManager::callAsync([safe, msg]
                {
                    if (auto* self = safe.getComponent())
                        self->finishExport(msg);
                });
            });
    }

    /** The classic paginated export — every page as numbered siblings
     *  ("name_p01.png", "name_p02.png"…). Asynchronous; the bar advances
     *  page by page (render fraction folded in). */
    void exportAllPages()
    {
        if (exportJob_ != nullptr)
            return;
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
        const juce::String ext = exportAsPng_ ? "png" : "jpg";
        const juce::File dest = uniqueExportFile(ext);
        startExportJob();
        juce::Thread::launch(
            [safe = juce::Component::SafePointer<MidiScoreGenTabComponent>(this),
             job = exportJob_, dataCopy = data, voicesCopy = effectiveVoices(),
             s, asPng = exportAsPng_, ext, dest, nPages,
             session = processor.sessions()->sessionName()]
            {
                juce::String msg;
                int written = 0;
                for (int k = 0; k < nPages; ++k)
                {
                    const auto r = midiscoregen::renderPage(
                        dataCopy, voicesCopy, s, k,
                        [job, k, nPages](double p)
                        { job->frac = ((float) k + (float) p) / (float) nPages; });
                    if (! (r.ok && r.image.isValid()))
                    {
                        msg = "Failed: " + r.log;
                        break;
                    }
                    const juce::File out = (nPages == 1)
                        ? dest
                        : dest.getSiblingFile(dest.getFileNameWithoutExtension()
                                + "_p" + juce::String(k + 1).paddedLeft('0', 2)
                                + "." + ext);
                    job->writing = true;
                    const bool ok = writeExportImage(r, out, asPng, s);
                    job->writing = false;
                    if (! ok)
                    {
                        msg = "Export failed: " + out.getFileName();
                        break;
                    }
                    ++written;
                    job->frac = (float) (k + 1) / (float) nPages;
                }
                if (msg.isEmpty())
                    msg = "Exported " + juce::String(written) + " page(s): "
                        + dest.getFileName() + juce::String::fromUTF8(" → ")
                        + session + "/exports";
                juce::MessageManager::callAsync([safe, msg]
                {
                    if (auto* self = safe.getComponent())
                        self->finishExport(msg);
                });
            });
    }

    /** DPI-stamped write with the embedded Sp3ctraCal chunk so a re-load into
     *  the SAMPLER reproduces the EXACT midi-score calage. Static — called
     *  from the export thread, must not touch the component. */
    static bool writeExportImage(const scoregen::RenderResult& r,
                                 const juce::File& out, bool asPng,
                                 const midiscoregen::MidiScoreSettings& s)
    {
        scoregen::SpectroCalibration cal;
        cal.band   = r.spectroBand;
        cal.minHz  = s.minFreq;
        cal.maxHz  = s.maxFreq;
        cal.stereo = r.stereo;
        cal.valid  = r.spectroBand.getWidth() > 0
                  && r.spectroBand.getHeight() > 0
                  && s.maxFreq > s.minFreq && s.minFreq > 0.0;
        return scoregen::exportImage(r.image, out, asPng, s.printerDpi, &cal);
    }

    /** Unique name in exports/: collides with neither a previous single-sheet
     *  export ("base.png") nor a previous paginated one ("base_p01.png"). */
    juce::File uniqueExportFile(const juce::String& ext) const
    {
        const juce::String base = juce::File(data.sourcePath)
                                      .getFileNameWithoutExtension() + "_score";
        const juce::File dir = exportDir();
        juce::String name = base;
        for (int k = 2; dir.getChildFile(name + "." + ext).exists()
                     || dir.getChildFile(name + "_p01." + ext).exists(); ++k)
            name = base + "_" + juce::String(k);
        return dir.getChildFile(name + "." + ext);
    }

    void openExportZoneDialog(const midiscoregen::MidiScoreSettings& s, int nPages)
    {
        if (! previewImage.isValid())
        {
            logLabel.setText("Preview still rendering, retry in a moment",
                             juce::dontSendNotification);
            return;
        }
        juce::Component* host = getTopLevelComponent();
        if (host == nullptr)
            return;

        auto* dlg = new ExportZoneDialog(previewImage, data.durationSec,
                                         midiscoregen::pageSeconds(s), nPages,
                                         s.pageFormat == 1 ? "A3" : "A4",
                                         juce::Colour(kAccentARGB));
        juce::Component::SafePointer<MidiScoreGenTabComponent> safe(this);
        dlg->onExportZone = [safe](double t0)
        {
            if (auto* self = safe.getComponent())
                self->exportSheetAt(t0, {});
        };
        dlg->onExportAll = [safe]
        {
            if (auto* self = safe.getComponent())
                self->exportAllPages();
        };

        // Size: strip aspect at ~90% of the window width, capped in height.
        const int w = juce::jlimit(480, 940, host->getWidth() - 60);
        const int stripW = w - 2 * ExportZoneDialog::kPad;
        const int stripH = juce::jlimit(120, juce::jmax(140, host->getHeight() - 220),
                                        stripW * previewImage.getHeight()
                                            / juce::jmax(1, previewImage.getWidth()));
        dlg->setSize(w, stripH + ExportZoneDialog::kChromeH);
        host->addAndMakeVisible(dlg);
        dlg->setTopLeftPosition(juce::jmax(0, (host->getWidth()  - dlg->getWidth())  / 2),
                                juce::jmax(0, (host->getHeight() - dlg->getHeight()) / 2));
        dlg->toFront(true);
        dlg->grabKeyboardFocus();
    }

    juce::File exportDir() const
    {
        // Image exports never ask where to save: they land in the active
        // session's exports/ folder — the built-in Global session when hosted
        // in a DAW (no session UI) or before any session is named.
        const juce::File dir = processor.sessions()->exportsDir();
        dir.createDirectory();
        return dir;
    }

    //==========================================================================
    // Persistence — one JSON blob in apvts.state (like timbreGenState).
    void persistState()
    {
        stateDirty = false;
        juce::Array<juce::var> arr;
        for (int vi = 0; vi < midiscoregen::kMaxVoices; ++vi)
        {
            const auto& q = voices[(size_t) vi];
            auto* o = new juce::DynamicObject();
            juce::Array<juce::var> pan;
            for (const auto& pq : pageSettings.panPoints[(size_t) vi])
            {
                auto* po = new juce::DynamicObject();
                po->setProperty("p", pq.pos);
                po->setProperty("v", pq.pan);
                pan.add(juce::var(po));
            }
            o->setProperty("pan",   pan);
            o->setProperty("en",    q.enabled);
            o->setProperty("solo",  voiceSolo[(size_t) vi]);
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
        root->setProperty("fmt",    pageSettings.pageFormat);
        root->setProperty("png",    exportAsPng_);
        // Playback shaping — crop window, edge fades, IMAGE EQ curve.
        root->setProperty("cropS",  (double) cropStart_);
        root->setProperty("cropE",  (double) cropEnd_);
        root->setProperty("fiL",    (double) fadeInLen_);
        root->setProperty("fiT",    (int) fadeInType_);
        root->setProperty("fiP",    (double) fadeInPow_);
        root->setProperty("foL",    (double) fadeOutLen_);
        root->setProperty("foT",    (int) fadeOutType_);
        root->setProperty("foP",    (double) fadeOutPow_);
        root->setProperty("eq",     eqEditor.encodeState());
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
        if (o->hasProperty("fmt"))    pageSettings.pageFormat      =
            juce::jlimit(0, 2, (int) o->getProperty("fmt"));
        if (o->hasProperty("png"))    exportAsPng_                 = (bool)   o->getProperty("png");

        // Playback shaping — crop window, edge fades, IMAGE EQ curve.
        if (o->hasProperty("cropS")) cropStart_ =
            juce::jlimit(0.0f, 0.99f, (float) (double) o->getProperty("cropS"));
        if (o->hasProperty("cropE")) cropEnd_ =
            juce::jlimit(cropStart_ + 0.01f, 1.0f, (float) (double) o->getProperty("cropE"));
        if (o->hasProperty("fiL"))   fadeInLen_ =
            juce::jlimit(0.0f, 1.0f, (float) (double) o->getProperty("fiL"));
        if (o->hasProperty("fiT"))   fadeInType_ = (FadeCurveType)
            juce::jlimit(0, kNumFadeCurveTypes - 1, (int) o->getProperty("fiT"));
        if (o->hasProperty("fiP"))   fadeInPow_ =
            juce::jlimit(0.1f, 10.0f, (float) (double) o->getProperty("fiP"));
        if (o->hasProperty("foL"))   fadeOutLen_ =
            juce::jlimit(0.0f, 1.0f, (float) (double) o->getProperty("foL"));
        if (o->hasProperty("foT"))   fadeOutType_ = (FadeCurveType)
            juce::jlimit(0, kNumFadeCurveTypes - 1, (int) o->getProperty("foT"));
        if (o->hasProperty("foP"))   fadeOutPow_ =
            juce::jlimit(0.1f, 10.0f, (float) (double) o->getProperty("foP"));
        {
            const juce::String eq = o->getProperty("eq").toString();
            if (eq.isNotEmpty())
                eqEditor.decodeState(eq);
        }

        auto readPanArray = [](const juce::var& v,
                               std::vector<midiscoregen::PanPoint>& out)
        {
            const auto* parr = v.getArray();
            if (parr == nullptr)
                return false;
            out.clear();
            for (const auto& item : *parr)
                if (auto* po = item.getDynamicObject())
                    out.push_back(
                        { juce::jlimit(0.0, 1.0, (double) po->getProperty("p")),
                          juce::jlimit(-1.0, 1.0, (double) po->getProperty("v")) });
            std::sort(out.begin(), out.end(),
                      [](const midiscoregen::PanPoint& a,
                         const midiscoregen::PanPoint& b) { return a.pos < b.pos; });
            return true;
        };

        // Legacy single-curve state (root "pan", pre-per-voice): it panned
        // every voice — copy it to all of them; per-voice curves below win.
        {
            std::vector<midiscoregen::PanPoint> legacy;
            if (readPanArray(o->getProperty("pan"), legacy))
                for (auto& vp : pageSettings.panPoints)
                    vp = legacy;
        }

        if (const auto* arr = o->getProperty("voices").getArray())
        {
            for (int i = 0; i < juce::jmin((int) arr->size(), midiscoregen::kMaxVoices); ++i)
            {
                auto* so = (*arr)[i].getDynamicObject();
                if (so == nullptr) continue;
                auto& q = voices[(size_t) i];
                auto get = [&](const char* k, double d)
                { return so->hasProperty(k) ? (double) so->getProperty(k) : d; };
                readPanArray(so->getProperty("pan"),
                             pageSettings.panPoints[(size_t) i]);
                // Missing "en" (state saved while the toggle was absent) = on.
                q.enabled       = ! so->hasProperty("en")
                                  || (bool) so->getProperty("en");
                voiceSolo[(size_t) i] = (bool) so->getProperty("solo");
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
        if (auto* fs = boundChannel())
        {
            if (scrubAuditioning)
                fs->uiEndScoreScrub();
            // Don't leave the OLD instance frozen with no UI bound to it.
            if (pauseMode == PauseMode::playing) fs->uiSetScorePaused(false);
            if (pauseMode == PauseMode::held)    fs->uiEndScoreScrub();
        }
        releasePauseState();
        scrubAuditioning = false;
        framesAreOurs    = false;
        loadedFrameCount = 0;
        scrubHead        = -1;
        scrubbing        = false;
        boundScoreSlot_  = slot;
        repaint();
    }

    //==========================================================================
    // SETUP face access (MidiScoreSetupPanel) — export preferences. They are
    // export-only: neither the preview nor the loaded playback frames change,
    // exactly like the old on-page DPI combo.
    int  exportPageFormat() const { return pageSettings.pageFormat; }
    void setExportPageFormat(int f)
    {
        f = juce::jlimit(0, 2, f);
        if (f == pageSettings.pageFormat)
            return;
        pageSettings.pageFormat = f;
        refreshFileLabel();      // the page count follows the sheet size
        persistState();
    }

    bool exportFormatIsPng() const { return exportAsPng_; }
    void setExportFormatPng(bool png)
    {
        if (png == exportAsPng_)
            return;
        exportAsPng_ = png;
        persistState();
    }

    int  exportDpi() const { return (int) pageSettings.printerDpi; }
    void setExportDpi(int dpi)
    {
        dpi = juce::jmax(72, dpi);
        if ((double) dpi == pageSettings.printerDpi)
            return;
        pageSettings.printerDpi = (double) dpi;
        persistState();
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
    std::array<bool, midiscoregen::kMaxVoices> voiceSolo {};
    midiscoregen::MidiScoreSettings pageSettings;
    int selectedVoice = 0;

    std::array<std::unique_ptr<VoiceTab>, midiscoregen::kMaxVoices> voiceTabs;

    juce::Label    presetLabel, partialsLabel, slopeLabel, oddLabel,
                   inharmLabel, combLabel, combPosLabel, attackLabel, decayLabel,
                   hfDampLabel, vibDepthLabel, vibRateLabel, vibOnsetLabel,
                   vibLifeLabel, levelLabel, wsLabel, lineLabel, velLabel,
                   speedLabel, playHint, logLabel, fileLabel;
    juce::ComboBox presetCombo;
    juce::ToggleButton labelsToggle;
    MidiScoreIconToggle loopBtn    { MidiScoreIconToggle::Glyph::Loop };
    MidiScoreIconToggle reverseBtn { MidiScoreIconToggle::Glyph::Inverse };
    juce::Slider   partialsSlider, slopeSlider, oddSlider, inharmSlider,
                   combSlider, combPosSlider, attackSlider, decaySlider, hfDampSlider,
                   vibDepthSlider, vibRateSlider, vibOnsetSlider, vibLifeSlider,
                   levelSlider, wsSlider, lineSlider, velSlider, speedSlider;
    juce::TextButton loadButton;
    ExportImageButton exportButton;
    bool exportAsPng_ = true;            // SETUP face: PNG (true) / JPEG
    std::shared_ptr<ExportProgress> exportJob_;   // non-null while exporting
    MidiScorePlayButton playStopButton;
    MidiScorePauseButton pauseButton;
    PauseMode pauseMode = PauseMode::none;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttach, reverseAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    juce::Rectangle<int>   previewArea;
    juce::Rectangle<float> previewImgArea;
    juce::Image previewImage;            // whole-piece overview strip (live)

    // Preview zoom state — 1 = whole piece fitted; Cx/Cy = image fraction at
    // the frame centre while zoomed (wheel/pinch zoom, scrollbars navigate).
    double previewZoom_ = 1.0, previewCx_ = 0.5, previewCy_ = 0.5;
    juce::ScrollBar previewHScroll { false }, previewVScroll { true };

    // Hi-res tile of the visible window while zoomed (see desiredTileSpec).
    juce::Image  hiResTile_;
    double       tileT0_ = 0.0, tileT1_ = 0.0, tilePxPerSec_ = 0.0;
    bool         tileRenderBusy_  = false;
    int          tileEpoch_ = 0;    // bumped on content change → voids in-flight tiles
    juce::uint32 lastViewChangeMs_ = 0;

    bool previewDirty = false, playDirty = true, stateDirty = false;
    bool liveRenderBusy_    = false;     // one background play-strip render at a time
    bool previewRenderBusy_ = false;     // one background preview render at a time
    juce::uint32 lastEditMs = 0;

    bool framesAreOurs = false;          // our piece currently sits in the score player
    int  loadedFrameCount = 0;
    int  scrubHead = -1;
    bool scrubbing = false, scrubAuditioning = false;
    int  dragPanIdx = -1;                // pan handle being dragged (-1 = none)

    // ── Playback shaping (SAMPLER-style): crop window + edge fades + EQ ─────
    enum class ShapeDrag { None, Start, End, FadeIn, FadeOut,
                           FadeInShape, FadeOutShape };
    float cropStart_  = 0.0f, cropEnd_    = 1.0f;   // fraction of the piece
    float fadeInLen_  = 0.0f, fadeOutLen_ = 0.0f;   // fraction of the crop span
    FadeCurveType fadeInType_  = FadeCurveType::LINEAR;
    FadeCurveType fadeOutType_ = FadeCurveType::LINEAR;
    float fadeInPow_  = 1.0f, fadeOutPow_ = 1.0f;
    ShapeDrag shapeDrag_  = ShapeDrag::None;
    int       shapeHover_ = 0;   // 1=in end · 2=out end · 3=in mid · 4=out mid

    ScoreEqComponent eqEditor { juce::Colour(kAccentARGB) };
    SamplerValueBox cropStartBox_   { "start", juce::Colour(0xff33ff99), false };
    SamplerValueBox cropEndBox_     { "end",   juce::Colour(0xffff6633), false };
    SamplerValueBox fadeInLenBox_   { "in",    juce::Colour(0xff44ee88), false };
    SamplerValueBox fadeInTypeBox_  { {},      juce::Colour(0xff44ee88), true  };
    SamplerValueBox fadeInPowBox_   { "pow",   juce::Colour(0xff44ee88), false };
    SamplerValueBox fadeOutLenBox_  { "out",   juce::Colour(0xffff6633), false };
    SamplerValueBox fadeOutTypeBox_ { {},      juce::Colour(0xffff6633), true  };
    SamplerValueBox fadeOutPowBox_  { "pow",   juce::Colour(0xffff6633), false };

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiScoreGenTabComponent)
};
