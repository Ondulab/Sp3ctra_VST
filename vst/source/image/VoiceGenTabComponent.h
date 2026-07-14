/**
 * @file VoiceGenTabComponent.h
 * @brief PLAY page for the VOICE block — type a text, synthesize it offline
 *        with a Piper neural voice (language auto-detected or picked), then
 *        generate / export / play its printable vocal spectrum.
 *
 * Exact functional clone of the SCORE page (ScoreGenTabComponent) where the
 * source WAV comes from the embedded TTS instead of a file picker:
 *   TTS block (voice + text + Rate/Expression/Silence)
 *   → synthesized WAV (cached in Application Support/Sp3ctra/voice_renders)
 *   → same top waveform strip with page-window picker + source audition
 *   → same Writing Speed / Page / DPI / Multi-res generation controls
 *   → GENERATE (worker thread) → page preview + IMAGE EQ + PNG/JPEG export
 *   → same shared score-player transport (scoreLoop/scoreReverse/scoreSpeed/
 *     scorePlaying params, LuxSampler::loadScoreFramesFromImage + uiPlayScore).
 *
 * GENERATE re-runs the TTS only when the text / voice / TTS options changed;
 * otherwise it re-encodes the SAME take (the waveform window stays meaningful
 * — VITS synthesis is stochastic, a new take would change the audio under the
 * selection). Page state persists as JSON in apvts.state ("voiceGenState");
 * the EQ curve rides in "voiceEqCurve" (same pattern as scoreEqCurve).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../IconPaths.h"
#include "ScoreGenRenderer.h"
#include "ScoreEqComponent.h"
#include "WaveformSelectorComponent.h"
#include "../tts/PiperTts.h"
#include "../tts/VoiceGenJob.h"
#include "../tts/LanguageDetector.h"

class VoiceGenTabComponent : public juce::Component,
                             private juce::Timer
{
public:
    static constexpr uint32_t kAccentARGB = 0xffd06a9e;   // rose (VOICE identity)
    static constexpr int      kPreferredH = 780;

    explicit VoiceGenTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        score_settings_defaults(&settings_);
        settings_.writingSpeed = 2.5;          // match the SCORE page default
        restoreState();                        // may override externalVoicesDir
        voices = PiperTts::listVoices(externalVoicesDir);

        // ── TTS block: voice picker row ─────────────────────────────────────
        rebuildVoiceCombo();
        voiceCombo.onChange = [this]
        {
            const int id = voiceCombo.getSelectedId();
            if (id == 1)
                autoMode = true;
            else if (id >= 2 && id - 2 < voices.size())
            {
                autoMode = false;
                const auto& v = voices.getReference(id - 2);
                selectedVoiceId = v.id;
                if (v.lang.isNotEmpty())
                    langPref.set(v.lang, v.id);   // explicit pick = AUTO preference
            }
            ttsDirty = true;
            markDirty();
        };
        addAndMakeVisible(voiceCombo);

        rescanButton.setButtonText("Rescan");
        rescanButton.setTooltip("Rescan the built-in and external voice bundles");
        rescanButton.onClick = [this]
        {
            voices = PiperTts::listVoices(externalVoicesDir);
            rebuildVoiceCombo();
            synthStatus.setText(juce::String(voices.size()) + " voice(s) available",
                                juce::dontSendNotification);
        };
        addAndMakeVisible(rescanButton);

        folderButton.setButtonText("Voices...");
        folderButton.setTooltip("Choose the EXTERNAL voices folder — extra voices "
                                "besides the built-in ones (one extracted "
                                "vits-piper-* bundle per sub-folder; see "
                                "scripts/install_piper_voices.sh)");
        folderButton.onClick = [this]
        {
            externalVoicesDir.createDirectory();
            fileChooser = std::make_unique<juce::FileChooser>(
                "Select the external voices folder", externalVoicesDir);
            fileChooser->launchAsync(
                juce::FileBrowserComponent::openMode
                    | juce::FileBrowserComponent::canSelectDirectories,
                [safe = juce::Component::SafePointer<VoiceGenTabComponent>(this)]
                (const juce::FileChooser& fc)
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;
                    const auto dir = fc.getResult();
                    if (! dir.isDirectory()) return;
                    self->externalVoicesDir = dir;
                    self->voices = PiperTts::listVoices(dir);
                    self->rebuildVoiceCombo();
                    self->synthStatus.setText(
                        juce::String(self->voices.size()) + " voice(s) available ("
                            + dir.getFileName() + ")",
                        juce::dontSendNotification);
                    self->markDirty();
                });
        };
        addAndMakeVisible(folderButton);

        // ── TTS block: text ─────────────────────────────────────────────────
        textEditor.setMultiLine(true, true);
        textEditor.setReturnKeyStartsNewLine(true);
        textEditor.setFont(juce::FontOptions(15.0f));
        textEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff10131a));
        textEditor.setColour(juce::TextEditor::textColourId,       juce::Colour(0xffd8dee8));
        textEditor.setColour(juce::TextEditor::outlineColourId,    juce::Colour(0xff33373f));
        textEditor.setColour(juce::TextEditor::focusedOutlineColourId,
                             juce::Colour(kAccentARGB).withAlpha(0.6f));
        textEditor.setTextToShowWhenEmpty("Type the text to speak...",
                                          juce::Colour(0xff55606f));
        textEditor.setText(text, juce::dontSendNotification);
        textEditor.onTextChange = [this]
        {
            text = textEditor.getText();
            ttsDirty = true;
            markDirty();
        };
        addAndMakeVisible(textEditor);

        // ── TTS block: synthesis options ────────────────────────────────────
        auto ttsKnob = [this](juce::Slider& s, juce::Label& l, const char* name,
                              double lo, double hi, double def, const char* suffix)
        {
            l.setText(name, juce::dontSendNotification);
            l.setJustificationType(juce::Justification::centred);
            l.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            l.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
            addAndMakeVisible(l);
            initKnob(s, lo, hi, 0.01, def, suffix);
        };
        ttsKnob(rateSlider,    rateLabel,    "Rate",       0.5,  2.0, rate,    "x");
        rateSlider.setSkewFactorFromMidPoint(1.0);
        rateSlider.setTooltip("Speech rate (1x = the voice's natural pace)");
        ttsKnob(exprSlider,    exprLabel,    "Expression", 0.0,  1.5, expr,    nullptr);
        exprSlider.setTooltip("VITS noise scale - 0 = flat/robotic, higher = livelier");
        ttsKnob(silenceSlider, silenceLabel, "Silence",    0.25, 3.0, silence, "x");
        silenceSlider.setTooltip("Scales the silence between sentences");
        rateSlider.onValueChange    = [this] { rate    = rateSlider.getValue();    ttsDirty = true; markDirty(); };
        exprSlider.onValueChange    = [this] { expr    = exprSlider.getValue();    ttsDirty = true; markDirty(); };
        silenceSlider.onValueChange = [this] { silence = silenceSlider.getValue(); ttsDirty = true; markDirty(); };

        // Last-synthesis status ("fr → siwis — 3.2s, 22050 Hz") — the VOICE
        // equivalent of SCORE's loaded-file label.
        synthStatus.setText("No take synthesized yet", juce::dontSendNotification);
        synthStatus.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        synthStatus.setColour(juce::Label::textColourId, juce::Colour(0xffb8c0d0));
        addAndMakeVisible(synthStatus);

        // ── Writing Speed (essential — maps audio duration to page width) ────
        initLabel(wsLabel, "Writing Speed (cm/s)");
        initSlider(wsSlider, 0.5, 10.0, 0.1, settings_.writingSpeed);
        wsSlider.onValueChange = [this]
        {
            settings_.writingSpeed = wsSlider.getValue();
            updateExportWindow();   // window width depends on writing speed
            markDirty();
        };

        // ── Generate + progress ────────────────────────────────────────────
        generateButton.setButtonText("GENERATE");
        generateButton.onClick = [this] { startGenerate(); };
        addAndMakeVisible(generateButton);

        progressBar.setPercentageDisplay(true);
        addChildComponent(progressBar);

        // ── Export ─────────────────────────────────────────────────────────
        exportPngButton.setButtonText("Export PNG");
        exportPngButton.onClick = [this] { chooseExport(true); };
        exportPngButton.setEnabled(false);
        addAndMakeVisible(exportPngButton);

        exportJpgButton.setButtonText("Export JPEG");
        exportJpgButton.onClick = [this] { chooseExport(false); };
        exportJpgButton.setEnabled(false);
        addAndMakeVisible(exportJpgButton);

        // ── Playback transport (shared SCORE player channel) ────────────────
        playStopButton.setEnabled(false);
        playStopButton.setTooltip("Play / stop the generated vocal spectrum");
        playStopButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playStopButton);

        loopBtn.setEnabled(false);
        loopBtn.setTooltip("Loop playback");
        addAndMakeVisible(loopBtn);
        loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "scoreLoop", loopBtn);

        reverseBtn.setEnabled(false);
        reverseBtn.setTooltip("Reverse (play the take backward)");
        addAndMakeVisible(reverseBtn);
        reverseAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "scoreReverse", reverseBtn);

        initLabel(speedLabel, "Speed");
        initKnob(speedSlider, 0.1, 6.0, 0.01, 1.0, "x");
        speedSlider.setSkewFactorFromMidPoint(1.0);
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

        setTransportEnabled(false);

        playHint.setText("Set LuxStral source = Sampler to hear the voice.",
                         juce::dontSendNotification);
        playHint.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        playHint.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        addAndMakeVisible(playHint);

        // ── Format options (same as SCORE; VOICE takes are mono → no Stereo) ─
        initLabel(pageLabel, "Page");
        pageCombo.addItem("A4 Portrait", 1);
        pageCombo.addItem("A3 Landscape", 2);
        pageCombo.setSelectedId(settings_.pageFormat == 1 ? 2 : 1,
                                juce::dontSendNotification);
        pageCombo.onChange = [this]
        {
            settings_.pageFormat = (pageCombo.getSelectedId() == 2) ? 1 : 0;
            updateExportWindow();   // A4/A3 changes the page-window length
            markDirty();
        };
        addAndMakeVisible(pageCombo);

        initLabel(dpiLabel, "DPI");
        for (int d : { 200, 300, 400, 600, 800 })
            dpiCombo.addItem(juce::String(d), d);
        dpiCombo.setSelectedId((int) settings_.printerDpi, juce::dontSendNotification);
        dpiCombo.onChange = [this]
        {
            settings_.printerDpi = (double) juce::jmax(72, dpiCombo.getSelectedId());
            markDirty();
        };
        addAndMakeVisible(dpiCombo);

        multiResToggle.setButtonText("Multi-res transients");
        multiResToggle.setToggleState(settings_.enableMultiRes != 0,
                                      juce::dontSendNotification);
        multiResToggle.onClick = [this]
        {
            settings_.enableMultiRes = multiResToggle.getToggleState() ? 1 : 0;
            markDirty();
        };
        addAndMakeVisible(multiResToggle);

        // ── Waveform region picker (which part of the take to extract) ───────
        waveform.onStartChange = [this](double startSec)
        {
            settings_.startTimeSec = startSec;
            markDirty();
        };
        addAndMakeVisible(waveform);

        // ── Audition button: play/pause the SELECTED region of the take ─────
        previewButton.setEnabled(false);
        previewButton.onClick = [this] { togglePreview(); };
        addAndMakeVisible(previewButton);
        refreshPreviewButton();

        // ── Image EQ (edits the generated image, never the take) ────────────
        eqEditor.onChange = [this] { eqDirty = true; };
        addAndMakeVisible(eqEditor);
        {
            const juce::String eq = processor.getAPVTS().state
                .getProperty("voiceEqCurve", "").toString();
            if (eq.isNotEmpty())
                eqEditor.decodeState(eq);
        }

        // ── Restore the cached take (WAV survives sessions; frames don't) ────
        {
            const juce::File wav = cacheWavFile();
            if (wav.existsAsFile())
            {
                const auto info = scoregen::probeWav(wav);
                if (info.ok)
                {
                    ttsDirty = false;   // same take replayable without re-synthesis
                    waveform.setFile(wav);
                    waveform.setStartSeconds(settings_.startTimeSec);
                    updateExportWindow();
                    previewButton.setEnabled(true);
                    setSynthStatus(info.durationSec, info.sampleRate);
                    setTransportEnabled(true);   // PLAY re-encodes then plays
                }
            }
        }

        // ── Log ────────────────────────────────────────────────────────────
        logLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        logLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        logLabel.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(logLabel);

        if (! PiperTts::isEngineAvailable())
            logLabel.setText("TTS engine not available in this build "
                             "(SP3CTRA_ENABLE_TTS=OFF)", juce::dontSendNotification);
        else if (voices.isEmpty())
            logLabel.setText("No voices installed - run scripts/install_piper_voices.sh "
                             "then Rescan", juce::dontSendNotification);

        // Worker callbacks marshal back to the message thread.
        job.onProgress = [safe = juce::Component::SafePointer<VoiceGenTabComponent>(this)]
                         (float pr)
        {
            juce::MessageManager::callAsync([safe, pr]
            {
                if (auto* self = safe.getComponent())
                    self->progress = (double) pr;
            });
        };
        job.onDone = [safe = juce::Component::SafePointer<VoiceGenTabComponent>(this)]
                     (VoiceGenJob::Result r)
        {
            juce::MessageManager::callAsync([safe, r = std::move(r)]() mutable
            {
                if (auto* self = safe.getComponent())
                    self->onJobFinished(std::move(r));
            });
        };

        startTimerHz(10);
    }

    ~VoiceGenTabComponent() override
    {
        stopTimer();
        if (stateDirty)
            persistState();
        // The page is a VIEW — closing it must not cut the shared channel.
        processor.stopScorePreview();
        job.stopThread(4000);
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

            // Reading head — only while OUR frames sit in the shared player.
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
            juce::String hint;
            if (! PiperTts::isEngineAvailable())
                hint = "TTS engine not available (build with SP3CTRA_ENABLE_TTS=ON)";
            else if (voices.isEmpty())
                hint = "No voices installed - run scripts/install_piper_voices.sh";
            else if (busy)
                hint = "Generating...";
            else
                hint = "Type a text and press GENERATE";
            g.drawText(hint, previewArea, juce::Justification::centred);
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
    // Manual play-head scrub on the preview (same behaviour as SCORE).
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

    void resized() override
    {
        const int pad = 8;
        const int ch  = Sp3ctraTheme::kControlH;
        const int gap = 6;

        // ── Audition button + waveform region picker across the top ─────────
        const int waveH = 74;
        const int pbW   = 40;
        previewButton.setBounds(pad, pad, pbW, waveH);
        waveform.setBounds(pad + pbW + 4, pad, getWidth() - 2 * pad - pbW - 4, waveH);
        const int contentTop = pad + waveH + gap;

        // ── Control column (left) ───────────────────────────────────────────
        const int colW = juce::jmin(330, getWidth() - 2 * pad);
        int y = contentTop;

        // TTS block: voice row → text → knobs → status.
        {
            const int btnW = 58;
            voiceCombo.setBounds(pad, y, colW - 2 * (btnW + gap), ch);
            rescanButton.setBounds(pad + colW - 2 * btnW - gap, y, btnW, ch);
            folderButton.setBounds(pad + colW - btnW, y, btnW, ch);
            y += ch + gap;
        }
        textEditor.setBounds(pad, y, colW, 56);
        y += 56 + gap;
        {
            const int knobW    = (colW - 2 * gap) / 3;
            const int knobDrwH = 42, knobValH = 14, lblH = 12;
            int x = pad;
            auto place = [&](juce::Label& l, juce::Slider& s)
            {
                l.setBounds(x, y, knobW, lblH);
                s.setBounds(x, y + lblH, knobW, knobDrwH + knobValH);
                x += knobW + gap;
            };
            place(rateLabel,    rateSlider);
            place(exprLabel,    exprSlider);
            place(silenceLabel, silenceSlider);
            y += lblH + knobDrwH + knobValH + gap;
        }
        synthStatus.setBounds(pad, y, colW, ch);
        y += ch + gap + 8;

        // SCORE-identical column from here on.
        {
            const int lblW = 150;
            wsLabel.setBounds(pad, y, lblW, ch);
            wsSlider.setBounds(pad + lblW + gap, y, colW - lblW - gap, ch);
            y += ch + gap + 4;
        }
        {
            const int half = (colW - gap) / 2;
            pageLabel.setBounds(pad, y, 40, ch);
            pageCombo.setBounds(pad + 40 + gap, y, half - 40 - gap, ch);
            dpiLabel.setBounds(pad + half + gap, y, 34, ch);
            dpiCombo.setBounds(pad + half + gap + 34 + gap, y, half - 34 - gap, ch);
            y += ch + gap + 4;
        }
        multiResToggle.setBounds(pad, y, colW, ch);
        y += ch + gap + 4;

        generateButton.setBounds(pad, y, colW, ch + 4); y += ch + 8;
        progressBar.setBounds(pad, y, colW, ch);        y += ch + gap;
        exportPngButton.setBounds(pad, y, (colW - gap) / 2, ch);
        exportJpgButton.setBounds(pad + (colW - gap) / 2 + gap, y, (colW - gap) / 2, ch);
        y += ch + gap + 6;

        // ── Playback transport (identical bar to SCORE) ─────────────────────
        {
            const int knobW    = 56;
            const int knobDrwH = 42;
            const int knobValH = 14;
            const int blockH   = knobDrwH + knobValH;
            const int btn      = 40;
            const int icon     = 34;

            int x = pad;
            playStopButton.setBounds(x, y + (blockH - btn) / 2, btn, btn);   x += btn + gap;
            loopBtn.setBounds   (x, y + (blockH - icon) / 2, icon, icon);    x += icon + 4;
            reverseBtn.setBounds(x, y + (blockH - icon) / 2, icon, icon);    x += icon + gap;

            const int knobX = pad + colW - knobW;
            speedSlider.setBounds(knobX, y, knobW, blockH);
            speedLabel.setBounds(x, y + (knobDrwH - ch) / 2,
                                 juce::jmax(0, knobX - gap - x), ch);
            y += blockH + gap;
        }
        playHint.setBounds(pad, y, colW, ch); y += ch + gap + 4;
        const int colBottom = y;

        // ── Image EQ strip across the bottom (full width) ───────────────────
        const int eqTop = juce::jmax(colBottom + gap,
                                     getHeight() - pad - ScoreEqComponent::kPreferredH);
        const int eqH   = juce::jmax(0, getHeight() - pad - eqTop);
        eqEditor.setBounds(pad, eqTop, getWidth() - 2 * pad, eqH);
        const int contentBottom = eqTop - gap;

        // ── Log fills the gap above the EQ strip ────────────────────────────
        const int logH = contentBottom - colBottom;
        logLabel.setVisible(logH >= 14);
        if (logH >= 14)
            logLabel.setBounds(pad, colBottom, colW, logH);

        // Preview occupies the area to the right of the control column.
        const int previewX = pad + colW + 10;
        previewArea = juce::Rectangle<int>(previewX, contentTop,
                                           juce::jmax(80, getWidth() - previewX - pad),
                                           juce::jmax(80, contentBottom - contentTop));
    }

private:
    //==========================================================================
    /** Square play/stop transport button (same visual language as SCORE's). */
    class TransportPlayButton : public juce::Button
    {
    public:
        TransportPlayButton() : juce::Button("voicePlayStop") {}

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
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportPlayButton)
    };

    /** Compact loop/inverse pictogram toggle — same glyph as the SCORE page,
     *  only the accent differs. */
    class VoiceIconToggle : public juce::Button
    {
    public:
        enum class Glyph { Loop, Inverse };

        explicit VoiceIconToggle(Glyph g) : juce::Button("voiceLoopToggle"), glyph(g)
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
         *  shared shape with the SCORE / TIMBRE / MIDI SCORE toggles. */
        static void drawLoopGlyph(juce::Graphics& g, juce::Rectangle<float> r,
                                  juce::Colour col, bool reversed)
        {
            const float h  = r.getHeight();
            const float th = juce::jmax(2.0f, h * 0.12f);

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
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceIconToggle)
    };

    //==========================================================================
    void initLabel(juce::Label& lbl, const juce::String& textIn)
    {
        lbl.setText(textIn, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    void initSlider(juce::Slider& s, double lo, double hi, double step, double val)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, Sp3ctraTheme::kControlH);
        s.setRange(lo, hi, step);
        s.setValue(val, juce::dontSendNotification);
        addAndMakeVisible(s);
    }

    void initKnob(juce::Slider& s, double lo, double hi, double step, double val,
                  const char* suffix = nullptr)
    {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 14);
        s.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxTextColourId,       juce::Colour(0xffa0c4e8));
        s.setRange(lo, hi, step);
        s.setValue(val, juce::dontSendNotification);
        if (suffix != nullptr) s.setTextValueSuffix(suffix);
        addAndMakeVisible(s);
    }

    void markDirty()
    {
        stateDirty = true;
        lastEditMs = juce::Time::getMillisecondCounter();
    }

    static juce::File cacheWavFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("Application Support/Sp3ctra/voice_renders/voice_last.wav");
    }

    bool modelHasVoice() const
    {
        for (const auto& ch : processor.getChainModel().chains)
            for (const auto& m : ch.modules)
                if (m.type == ModuleType::Voice)
                    return true;
        return false;
    }

    void rebuildVoiceCombo()
    {
        voiceCombo.clear(juce::dontSendNotification);
        voiceCombo.addItem("AUTO (detect language)", 1);
        for (int i = 0; i < voices.size(); ++i)
            voiceCombo.addItem(voices.getReference(i).displayName(), 2 + i);

        int select = 1;
        if (! autoMode)
            for (int i = 0; i < voices.size(); ++i)
                if (voices.getReference(i).id == selectedVoiceId)
                    { select = 2 + i; break; }
        voiceCombo.setSelectedId(select, juce::dontSendNotification);
        autoMode = (select == 1);
    }

    void setSynthStatus(double durationSec, int sampleRate)
    {
        const juce::String arrow = juce::String::fromUTF8(" \xe2\x86\x92 ");
        juce::String s;
        if (lastLang.isNotEmpty() && lastVoiceName.isNotEmpty())
            s = lastLang + arrow + lastVoiceName + "  ";
        s << "(" << juce::String(durationSec, 2) << "s, "
          << juce::String(sampleRate) << " Hz, 1 ch)";
        synthStatus.setText(s, juce::dontSendNotification);
    }

    /** AUTO mode: detected language → preferred (last picked) voice for it,
     *  else best installed match (medium > high > low; en → US then GB). */
    bool resolveAutoVoice(const juce::String& forText, PiperVoiceInfo& out,
                          juce::String& outLang)
    {
        juce::StringArray langs;
        for (const auto& v : voices)
            if (v.lang.isNotEmpty())
                langs.addIfNotAlreadyThere(v.lang);

        outLang = LanguageDetector::detect(forText, langs);

        auto findById = [this](const juce::String& id) -> const PiperVoiceInfo*
        {
            for (const auto& v : voices)
                if (v.id == id) return &v;
            return nullptr;
        };

        if (outLang.isEmpty())
        {
            if (const auto* v = findById(selectedVoiceId)) { out = *v; outLang = v->lang; return true; }
            if (! voices.isEmpty()) { out = voices.getReference(0); outLang = out.lang; return true; }
            return false;
        }

        if (const auto* pref = langPref.getVarPointer(juce::Identifier(outLang)))
            if (const auto* v = findById(pref->toString()))
                { out = *v; return true; }

        const PiperVoiceInfo* best = nullptr;
        auto qualityRank = [](const juce::String& q)
        { return q == "medium" ? 0 : q == "high" ? 1 : 2; };
        auto regionRank = [&](const juce::String& r)
        { return (outLang == "en") ? (r == "US" ? 0 : r == "GB" ? 1 : 2) : 0; };
        for (const auto& v : voices)
        {
            if (v.lang != outLang) continue;
            if (best == nullptr
                || qualityRank(v.quality) < qualityRank(best->quality)
                || (qualityRank(v.quality) == qualityRank(best->quality)
                    && regionRank(v.region) < regionRank(best->region)))
                best = &v;
        }
        if (best != nullptr) { out = *best; return true; }

        if (const auto* v = findById(selectedVoiceId)) { out = *v; return true; }
        if (! voices.isEmpty()) { out = voices.getReference(0); return true; }
        return false;
    }

    /** Resize the export window (seconds-per-page) and sync the start offset. */
    void updateExportWindow()
    {
        waveform.setWindowSeconds(scoregen::pageWindowSeconds(settings_));
        settings_.startTimeSec = waveform.getStartSeconds();
    }

    //==========================================================================
    void startGenerate(bool autoPlayWhenDone = false)
    {
        if (busy) return;

        const juce::String t = textEditor.getText().trim();
        const bool needSynth = ttsDirty || ! cacheWavFile().existsAsFile();

        VoiceGenJob::Request req;
        if (needSynth)
        {
            if (! PiperTts::isEngineAvailable())
            {
                logLabel.setText("TTS engine not available in this build "
                                 "(SP3CTRA_ENABLE_TTS=OFF)", juce::dontSendNotification);
                return;
            }
            if (t.isEmpty())
            {
                logLabel.setText("Type some text first", juce::dontSendNotification);
                return;
            }
            if (voices.isEmpty())
            {
                logLabel.setText("No voices installed - run scripts/install_piper_voices.sh "
                                 "then Rescan", juce::dontSendNotification);
                return;
            }

            PiperVoiceInfo v;
            juce::String lang;
            if (autoMode)
            {
                if (! resolveAutoVoice(t, v, lang))
                    return;
            }
            else
            {
                bool found = false;
                for (const auto& vi : voices)
                    if (vi.id == selectedVoiceId) { v = vi; found = true; break; }
                if (! found) v = voices.getReference(0);
                lang = v.lang;
            }
            lastLang      = lang;
            lastVoiceName = v.name.isNotEmpty() ? v.name : v.id;

            req.text  = t;
            req.voice = v;
            req.opts.lengthScale          = (float) (1.0 / juce::jlimit(0.25, 4.0, rate));
            req.opts.noiseScale           = (float) expr;
            req.opts.sentenceSilenceScale = (float) silence;
            settings_.startTimeSec = 0.0;      // new take → window back to the start
        }
        else
        {
            req.renderOnly = true;             // same take, new page settings/window
        }

        ScoreSettings s = settings_;
        double lo = 0.0, hi = 0.0;
        processor.getScoreFrequencyRange(lo, hi);
        s.minFreq = lo;
        s.maxFreq = hi;
        s.enableStereoMode = 0;                // TTS takes are mono by construction
        genMinFreq = lo;
        genMaxFreq = hi;
        genDpi     = s.printerDpi;
        req.score   = s;
        req.wavFile = cacheWavFile();
        pendingAutoPlay = autoPlayWhenDone;

        busy = true;
        progress = 0.0;
        generateButton.setEnabled(false);
        exportPngButton.setEnabled(false);
        exportJpgButton.setEnabled(false);
        if (auto* fs = processor.getLuxSampler())
            if (framesAreOurs)
                fs->uiStopScore();
        processor.stopScorePreview();
        refreshPreviewButton();
        setTransportEnabled(false);
        refreshPlayButton();
        progressBar.setVisible(true);
        previewImage = juce::Image();
        logLabel.setText(needSynth ? "Synthesizing..." : "Generating...",
                         juce::dontSendNotification);
        repaint();
        job.start(std::move(req));
    }

    void onJobFinished(VoiceGenJob::Result r)
    {
        busy = false;
        progressBar.setVisible(false);
        generateButton.setEnabled(true);

        if (! r.ok())
        {
            pendingAutoPlay = false;
            baseImage      = juce::Image();
            generatedImage = juce::Image();
            previewImage   = juce::Image();
            scrubHead      = -1;
            framesAreOurs  = false;
            const juce::String why = r.error.isNotEmpty() ? r.error : r.render.log;
            logLabel.setText("Failed: " + why, juce::dontSendNotification);
            if (auto* fs = processor.getLuxSampler())
                if (framesAreOurs)
                    fs->uiStopScore();
            setTransportEnabled(false);
            refreshPlayButton();
            repaint();
            return;
        }

        // Fresh synthesis → refresh the waveform strip + take status.
        if (! r.voiceId.isEmpty() || ttsDirty)
        {
            ttsDirty = false;
            waveform.setFile(r.wavFile);
            waveform.setStartSeconds(settings_.startTimeSec);
            updateExportWindow();
            previewButton.setEnabled(true);
            previewFile = juce::File(); previewStart = -1.0; previewLen = -1.0;
            const auto info = scoregen::probeWav(r.wavFile);
            if (info.ok)
                setSynthStatus(info.durationSec, info.sampleRate);
        }

        baseImage     = r.render.image;
        spectroBand   = r.render.spectroBand;
        genDynRangeDB = settings_.dynamicRangeDB;

        // Keep the EQ curve across regenerations; rebuild its band grid only
        // when the frequency span actually changes.
        if (genMinFreq != lastEqMinFreq || genMaxFreq != lastEqMaxFreq)
        {
            eqEditor.setRange(genMinFreq, genMaxFreq);
            lastEqMinFreq = genMinFreq;
            lastEqMaxFreq = genMaxFreq;
        }

        applyEqToImageAndReload();   // builds generatedImage (+EQ), preview, loads frames

        exportPngButton.setEnabled(true);
        exportJpgButton.setEnabled(true);
        logLabel.setText(r.render.log + "\n" + previewStats, juce::dontSendNotification);
        scrubHead = -1;
        setTransportEnabled(true);
        refreshPlayButton();

        if (pendingAutoPlay)
        {
            pendingAutoPlay = false;
            if (auto* p = processor.getAPVTS().getParameter("scorePlaying"))
                if (p->getValue() < 0.5f)
                    p->setValueNotifyingHost(1.0f);
        }

        persistState();
        repaint();
    }

    // ── Image EQ: shape the GENERATED image (never the synthesized take) ─────
    void applyEqToImage()
    {
        if (! baseImage.isValid()) { generatedImage = juce::Image(); return; }
        if (eqEditor.isFlat() || genMinFreq <= 0.0 || genMaxFreq <= genMinFreq)
        {
            generatedImage = baseImage;
            return;
        }

        generatedImage = baseImage.createCopy();

        juce::Rectangle<int> band = spectroBand.getIntersection(generatedImage.getBounds());
        if (band.isEmpty()) band = generatedImage.getBounds();

        const double bandBottom = (double) band.getBottom();
        const double bandH      = (double) juce::jmax(1, band.getHeight());
        const double range      = juce::jmax(1.0, genDynRangeDB);
        const double ratio      = genMaxFreq / genMinFreq;

        juce::Image::BitmapData bmp(generatedImage, juce::Image::BitmapData::readWrite);
        for (int yy = band.getY(); yy < band.getBottom(); ++yy)
        {
            const double pos  = juce::jlimit(0.0, 1.0, (bandBottom - (yy + 0.5)) / bandH);
            const double freq = genMinFreq * std::pow(ratio, pos);
            const double gdb  = eqEditor.gainDbAtFreq(freq);
            if (std::abs(gdb) < 0.01) continue;
            const double dShift = gdb / range;

            juce::uint8 lut[256];
            for (int v = 0; v < 256; ++v)
            {
                if (v >= 255 && dShift > 0.0) { lut[v] = 255; continue; }
                const double dk = juce::jlimit(0.0, 1.0, (1.0 - v / 255.0) + dShift);
                lut[v] = (juce::uint8) juce::jlimit(0, 255, (int) std::lround((1.0 - dk) * 255.0));
            }
            juce::uint8* line = bmp.getLinePointer(yy);
            for (int xx = band.getX(); xx < band.getRight(); ++xx)
            {
                juce::uint8* px = line + xx * bmp.pixelStride;
                const juce::uint8 nv = lut[px[0]];   // greyscale (mono take)
                px[0] = px[1] = px[2] = nv;
            }
        }
    }

    void applyEqToImageAndReload()
    {
        applyEqToImage();
        buildPreview();
        if (auto* fs = processor.getLuxSampler())
        {
            const bool wasPlaying = fs->isScorePlaying() && framesAreOurs;
            const int savedHead = wasPlaying ? fs->getScorePlayHead() : 0;
            fs->loadScoreFramesFromImage(generatedImage, spectroBand,
                                         genMinFreq, genMaxFreq, false);
            framesAreOurs    = true;
            loadedFrameCount = fs->getScoreFrameCount();
            if (wasPlaying)
            {
                fs->setScoreResumeHead(savedHead);
                fs->uiPlayScore();
            }
        }
        repaint();
    }

    void buildPreview()
    {
        if (! generatedImage.isValid())
        {
            previewImage = generatedImage;
            return;
        }
        // Preview only the spectrogram band (what is actually played).
        juce::Rectangle<int> band =
            (spectroBand.getWidth() > 0 && spectroBand.getHeight() > 0)
                ? spectroBand.getIntersection(generatedImage.getBounds())
                : generatedImage.getBounds();
        if (band.isEmpty())
            band = generatedImage.getBounds();

        constexpr int kPreviewMaxPx = 1800;
        const double s = juce::jmin(1.0,
            (double) kPreviewMaxPx / juce::jmax(band.getWidth(), band.getHeight()));
        const int pw = juce::jmax(1, (int) (band.getWidth()  * s));
        const int ph = juce::jmax(1, (int) (band.getHeight() * s));

        juce::Image tmp = generatedImage.getClippedImage(band)
                              .rescaled(pw, ph, juce::Graphics::highResamplingQuality);

        // Display-only white-point lift (same rationale as the SCORE page: the
        // dB floor reads grey once cropped to the band; playback/export untouched).
        juce::Image::BitmapData bd(tmp, juce::Image::BitmapData::readWrite);
        juce::int64 sum = 0;
        int mn = 255, mx = 0;
        const int total = pw * ph;
        for (int yy = 0; yy < ph; ++yy)
            for (int xx = 0; xx < pw; ++xx)
            {
                const int v = bd.getPixelPointer(xx, yy)[0];
                sum += v; mn = juce::jmin(mn, v); mx = juce::jmax(mx, v);
            }
        const double meanN = total > 0 ? (double) sum / total / 255.0 : 1.0;
        const double wp = juce::jlimit(0.45, 0.98, meanN);
        for (int yy = 0; yy < ph; ++yy)
            for (int xx = 0; xx < pw; ++xx)
            {
                double n = bd.getPixelPointer(xx, yy)[0] / 255.0;
                n = juce::jlimit(0.0, 1.0, n / wp);
                const auto v = (juce::uint8) (n * 255.0 + 0.5);
                bd.setPixelColour(xx, yy, juce::Colour(v, v, v));
            }
        previewStats = "Band grey: min=" + juce::String(mn)
                     + " mean=" + juce::String((int) (meanN * 255.0))
                     + " max=" + juce::String(mx)
                     + "  (preview white-point=" + juce::String(wp, 2) + ")";
        previewImage = tmp;
    }

    void chooseExport(bool asPng)
    {
        if (! generatedImage.isValid())
            return;
        const juce::String ext = asPng ? "png" : "jpg";
        // Suggested name from the first words of the text ("voice" fallback).
        juce::String slug = text.trim().replaceCharacters(" \t\n\r", "----")
                                .retainCharacters("abcdefghijklmnopqrstuvwxyz"
                                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")
                                .substring(0, 32).trimCharactersAtEnd("-");
        if (slug.isEmpty()) slug = "voice";
        const juce::File suggested = startDir().getChildFile(slug + "_score." + ext);
        fileChooser = std::make_unique<juce::FileChooser>(
            "Export Vocal Score Image", suggested, "*." + ext);
        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = juce::Component::SafePointer<VoiceGenTabComponent>(this), asPng, ext]
            (const juce::FileChooser& fc)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                auto dest = fc.getResult();
                if (dest.getFullPathName().isEmpty()) return;
                dest = dest.withFileExtension(ext);
                const bool ok = scoregen::exportImage(self->generatedImage, dest, asPng,
                                                      self->genDpi);
                self->logLabel.setText(ok ? ("Exported: " + dest.getFileName())
                                          : "Export failed",
                                       juce::dontSendNotification);
            });
    }

    //==========================================================================
    // Source-audio preview — play/pause the SELECTED region of the take.
    static juce::String playGlyph()  { return juce::String(juce::CharPointer_UTF8("\xe2\x96\xb6")); } // ▶
    static juce::String pauseGlyph() { return juce::String(juce::CharPointer_UTF8("\xe2\x8f\xb8")); } // ⏸

    void togglePreview()
    {
        const juce::File wav = cacheWavFile();
        if (processor.isScorePreviewPlaying())
        {
            processor.pauseScorePreview();
        }
        else if (wav.existsAsFile())
        {
            const double len = scoregen::pageWindowSeconds(settings_);
            const bool sameRegion = (wav == previewFile)
                                  && std::abs(settings_.startTimeSec - previewStart) < 1e-6
                                  && std::abs(len - previewLen) < 1e-6;
            if (! (sameRegion && processor.resumeScorePreview()))
            {
                processor.startScorePreview(wav, settings_.startTimeSec, len);
                previewFile  = wav;
                previewStart = settings_.startTimeSec;
                previewLen   = len;
            }
        }
        refreshPreviewButton();
    }

    void refreshPreviewButton()
    {
        previewButton.setButtonText(processor.isScorePreviewPlaying() ? pauseGlyph() : playGlyph());
    }

    //==========================================================================
    void togglePlay()
    {
        auto* fs = processor.getLuxSampler();
        if (fs == nullptr || busy) return;

        const bool play = ! (fs->isScorePlaying() && framesAreOurs);

        if (play && ! framesAreOurs)
        {
            if (generatedImage.isValid())
            {
                // Another module took the shared channel — reclaim it.
                applyEqToImageAndReload();
            }
            else if (cacheWavFile().existsAsFile())
            {
                // Restored session: re-encode the cached take, then auto-play.
                startGenerate(/*autoPlayWhenDone*/ true);
                return;
            }
            else
            {
                logLabel.setText("GENERATE first", juce::dontSendNotification);
                return;
            }
        }
        if (! play)
            scrubHead = -1;

        // Route through the scorePlaying param so the DAW lane stays truthful.
        if (auto* p = processor.getAPVTS().getParameter("scorePlaying"))
        {
            const float norm = play ? 1.0f : 0.0f;
            if (! juce::approximatelyEqual(p->getValue(), norm))
                p->setValueNotifyingHost(norm);
            else if (play) fs->uiPlayScore();
            else           fs->uiStopScore();
        }
        refreshPlayButton();
        repaint(previewArea);
    }

    void refreshPlayButton()
    {
        auto* fs = processor.getLuxSampler();
        const bool playing = (fs != nullptr) && fs->isScorePlaying() && framesAreOurs;
        playStopButton.setPlaying(playing);
    }

    void setTransportEnabled(bool on)
    {
        playStopButton.setEnabled(on);
        loopBtn.setEnabled(on);
        reverseBtn.setEnabled(on);
        speedSlider.setEnabled(on);

        const juce::Colour lblCol = on ? juce::Colour(0xffd6dbe4) : juce::Colour(0xff555a62);
        speedLabel.setColour(juce::Label::textColourId, lblCol);
        speedSlider.setColour(juce::Slider::textBoxTextColourId,
                              on ? juce::Colour(0xffa0c4e8) : juce::Colour(0xff555a62));
        speedLabel.repaint();
        speedSlider.repaint();
    }

    void timerCallback() override
    {
        // Reapply the EQ once the user releases a node (deferred, like SCORE).
        if (eqDirty && ! eqEditor.isDragging())
        {
            eqDirty = false;
            processor.getAPVTS().state
                .setProperty("voiceEqCurve", eqEditor.encodeState(), nullptr);
            if (baseImage.isValid())
                applyEqToImageAndReload();
        }

        // Debounced page-state persistence (typing-friendly).
        if (stateDirty
            && juce::Time::getMillisecondCounter() - lastEditMs > 800)
            persistState();

        auto* fs = processor.getLuxSampler();
        if (fs != nullptr && framesAreOurs)
        {
            // Ownership check: SCORE/TIMBRE/MIDI SCORE (or a restore) reloaded
            // the shared channel → our head display no longer applies.
            if (fs->getScoreFrameCount() != loadedFrameCount)
            {
                framesAreOurs = false;
                scrubHead     = -1;
                repaint(previewArea);
            }
            else if (fs->isScorePlaying())
                repaint(previewArea);
        }
        refreshPlayButton();

        // Source-audio preview: drive the waveform playhead + button state.
        if (processor.isScorePreviewPlaying())
            waveform.setPlayhead(previewStart + processor.getScorePreviewPositionSec());
        else
            waveform.setPlayhead(-1.0);
        refreshPreviewButton();

        // Voice-model RAM housekeeping: module left the chain model → release
        // the resident engine (~200 MB) once idle.
        if (++idleTicks >= 50)   // every ~5 s at 10 Hz
        {
            idleTicks = 0;
            if (! busy && job.engineLoaded() && ! modelHasVoice())
                job.unloadEngine();
        }
    }

    juce::File startDir() const
    {
        const auto out = processor.getSamplerOutputDir();
        if (out.isNotEmpty() && juce::File(out).isDirectory())
            return juce::File(out);
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    }

    //==========================================================================
    // Persistence — one JSON blob in apvts.state (like midiScoreGenState);
    // the EQ curve rides separately in "voiceEqCurve" (scoreEqCurve pattern).
    void persistState()
    {
        stateDirty = false;
        auto* root = new juce::DynamicObject();
        root->setProperty("text",  text);
        root->setProperty("voice", autoMode ? juce::String("auto") : selectedVoiceId);
        root->setProperty("rate",  rate);
        root->setProperty("expr",  expr);
        root->setProperty("sil",   silence);
        root->setProperty("ws",    settings_.writingSpeed);
        root->setProperty("page",  settings_.pageFormat);
        root->setProperty("dpi",   settings_.printerDpi);
        root->setProperty("mres",  settings_.enableMultiRes);
        root->setProperty("start", settings_.startTimeSec);
        root->setProperty("lang",  lastLang);
        root->setProperty("vname", lastVoiceName);
        root->setProperty("extdir", externalVoicesDir.getFullPathName());
        auto* prefs = new juce::DynamicObject();
        for (int i = 0; i < langPref.size(); ++i)
            prefs->setProperty(langPref.getName(i), langPref.getValueAt(i));
        root->setProperty("pref", juce::var(prefs));
        processor.getAPVTS().state.setProperty(
            "voiceGenState", juce::JSON::toString(juce::var(root), true), nullptr);
    }

    void restoreState()
    {
        const juce::String blob = processor.getAPVTS().state
            .getProperty("voiceGenState", "").toString();
        if (blob.isEmpty()) return;
        const juce::var root = juce::JSON::parse(blob);
        auto* o = root.getDynamicObject();
        if (o == nullptr) return;

        text = o->getProperty("text").toString();
        const juce::String v = o->getProperty("voice").toString();
        autoMode        = (v.isEmpty() || v == "auto");
        selectedVoiceId = autoMode ? juce::String() : v;
        if (o->hasProperty("rate"))  rate    = (double) o->getProperty("rate");
        if (o->hasProperty("expr"))  expr    = (double) o->getProperty("expr");
        if (o->hasProperty("sil"))   silence = (double) o->getProperty("sil");
        if (o->hasProperty("ws"))    settings_.writingSpeed   = (double) o->getProperty("ws");
        if (o->hasProperty("page"))  settings_.pageFormat     = (int)    o->getProperty("page");
        if (o->hasProperty("dpi"))   settings_.printerDpi     = (double) o->getProperty("dpi");
        if (o->hasProperty("mres"))  settings_.enableMultiRes = (int)    o->getProperty("mres");
        if (o->hasProperty("start")) settings_.startTimeSec   = (double) o->getProperty("start");
        lastLang      = o->getProperty("lang").toString();
        lastVoiceName = o->getProperty("vname").toString();
        const juce::String extdir = o->getProperty("extdir").toString();
        if (extdir.isNotEmpty() && juce::File(extdir).isDirectory())
            externalVoicesDir = juce::File(extdir);
        if (auto* prefs = o->getProperty("pref").getDynamicObject())
            langPref = prefs->getProperties();
    }

    //==========================================================================
    Sp3ctraAudioProcessor& processor;

    // TTS block.
    juce::Array<PiperVoiceInfo> voices;
    juce::File       externalVoicesDir { PiperTts::voicesDirectory() };
    juce::ComboBox   voiceCombo;
    juce::TextButton rescanButton, folderButton;
    juce::TextEditor textEditor;
    juce::Label      rateLabel, exprLabel, silenceLabel, synthStatus;
    juce::Slider     rateSlider, exprSlider, silenceSlider;
    juce::String     text;
    bool             autoMode = true;
    juce::String     selectedVoiceId;
    double           rate = 1.0, expr = 0.667, silence = 1.0;
    juce::NamedValueSet langPref;      // lang → last explicitly picked voice
    juce::String     lastLang, lastVoiceName;
    bool             ttsDirty = true;  // text/voice/options changed → re-synthesize

    // Generation controls (SCORE-identical).
    juce::TextButton generateButton, exportPngButton, exportJpgButton;
    juce::Label      logLabel;
    juce::Label      wsLabel;
    juce::Slider     wsSlider;
    juce::Label      pageLabel, dpiLabel;
    juce::ComboBox   pageCombo, dpiCombo;
    juce::ToggleButton multiResToggle;
    ScoreSettings    settings_ {};     // VOICE's own page settings (persisted in the blob)

    // Playback transport (shared SCORE player channel).
    TransportPlayButton playStopButton;
    VoiceIconToggle     loopBtn    { VoiceIconToggle::Glyph::Loop };
    VoiceIconToggle     reverseBtn { VoiceIconToggle::Glyph::Inverse };
    juce::Slider        speedSlider;
    juce::Label         speedLabel, playHint;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttach, reverseAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach;
    int  scrubHead { -1 };
    bool scrubbing { false };
    bool scrubAuditioning { false };
    juce::Rectangle<float> previewImgArea;

    // Waveform strip + source audition + image EQ.
    ScoreEqComponent eqEditor { juce::Colour(kAccentARGB) };
    WaveformSelectorComponent waveform { juce::Colour(kAccentARGB) };
    juce::TextButton previewButton;
    juce::File previewFile;
    double     previewStart { -1.0 };
    double     previewLen   { -1.0 };

    double progress { 0.0 };
    juce::ProgressBar progressBar { progress };

    juce::Rectangle<int> previewArea;
    juce::Rectangle<int> spectroBand;
    double genMinFreq { 0.0 };
    double genMaxFreq { 0.0 };
    juce::String previewStats;
    juce::Image generatedImage;   // full resolution (export source, EQ baked in)
    juce::Image baseImage;        // raw render before EQ
    juce::Image previewImage;     // downscaled band crop for painting
    bool   eqDirty { false };
    double genDynRangeDB { 50.0 };
    double genDpi { 400.0 };
    double lastEqMinFreq { 0.0 }, lastEqMaxFreq { 0.0 };
    bool busy { false };
    bool pendingAutoPlay { false };
    bool framesAreOurs { false };
    int  loadedFrameCount { 0 };
    bool stateDirty { false };
    juce::uint32 lastEditMs { 0 };
    int idleTicks { 0 };

    std::unique_ptr<juce::FileChooser> fileChooser;
    VoiceGenJob job;

    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceGenTabComponent)
};
