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
 * the EQ curve rides inside each instance's own blob (P7).
 */
#pragma once

#include "../ui/ModuleCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <vector>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "ScoreTransportBinding.h"
#include "../ui/Sp3ctraBarSlider.h"
#include "../IconPaths.h"
#include "../licensing/ActivationDialog.h"
#include "../session/MachinePrefs.h"   // TTS install prefs are machine-scoped
#include "ScoreGenRenderer.h"
#include "ScoreEqComponent.h"
#include "WaveformSelectorComponent.h"
#include "../tts/PiperTts.h"
#include "../tts/VoiceGenJob.h"

class VoiceGenTabComponent : public juce::Component,
                             private juce::Timer,
                             private juce::ScrollBar::Listener
{
public:
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Voice).getARGB();   ///< inherited module colour
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
        folderButton.setTooltip(juce::String::fromUTF8(
                                "Choose the EXTERNAL voices folder — extra voices "
                                "besides the built-in ones (one extracted "
                                "vits-piper-* bundle per sub-folder; see "
                                "scripts/install_piper_voices.sh)"));
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

        // ── Export — format (PNG/JPEG), sheet size and DPI live on the SETUP
        //    face; this single button writes the generated image. ───────────
        exportButton.setButtonText("Export image");
        exportButton.setTooltip("Export the vocal spectrum as an image "
                                "(PNG/JPEG, sheet size and DPI in SETUP).");
        exportButton.onClick = [this] { exportNow(); };
        exportButton.setEnabled(false);
        addAndMakeVisible(exportButton);

        // ── Preview zoom scrollbars (visible only while zoomed in) ─────────
        for (auto* sb : { &previewHScroll, &previewVScroll })
        {
            sb->setAutoHide(false);
            sb->setRangeLimits(0.0, 1.0, juce::dontSendNotification);
            sb->addListener(this);
            addChildComponent(sb);
        }

        // ── Playback transport (this instance's own score-player slot) ──────
        playStopButton.setEnabled(false);
        playStopButton.setTooltip("Play / stop the generated vocal spectrum");
        playStopButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playStopButton);

        loopBtn.setEnabled(false);
        loopBtn.setTooltip("Loop playback");
        addAndMakeVisible(loopBtn);

        reverseBtn.setEnabled(false);
        reverseBtn.setTooltip("Reverse (play the take backward)");
        addAndMakeVisible(reverseBtn);

        initLabel(speedLabel, "Speed");
        initKnob(speedSlider, 0.1, 6.0, 0.01, 1.0, "x");
        speedSlider.setSkewFactorFromMidPoint(1.0);

        // P7 — transport attachments + MIDI-Learn follow the SELECTED
        // instance: bindTransport() re-points them on every setScoreSlot().
        bindTransport();

        setTransportEnabled(false);

        // P8 — VOICE feeds like a media source: once generated and active,
        // the parked column sounds even with the transport stopped.
        playHint.setText("Generated + active: the parked column keeps sounding "
                         "(drag it). PLAY scans the text; the LED silences.",
                         juce::dontSendNotification);
        playHint.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        playHint.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        addAndMakeVisible(playHint);

        // Page format / DPI / PNG-vs-JPEG moved to the SETUP face
        // (VoiceSetupPanel) — it edits this page's settings_ through the
        // public accessors below (VOICE takes are mono → no Stereo option).
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
        // Selection sheet (page format 2): both edges are draggable — persist
        // the region length too, the sheet stretches to hold it.
        waveform.onRegionChange = [this](double startSec, double lenSec)
        {
            settings_.startTimeSec = startSec;
            settings_.selectionSec = lenSec;
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
        if (docs_[(size_t) docSlot_].eqState.isNotEmpty())
            eqEditor.decodeState(docs_[(size_t) docSlot_].eqState);
        else
        {
            // Pre-P7 sessions kept ONE curve in "voiceEqCurve" — adopt it.
            const juce::String eq = processor.getAPVTS().state
                .getProperty("voiceEqCurve", "").toString();
            if (eq.isNotEmpty())
                eqEditor.decodeState(eq);
        }

        // ── Restore the cached take (WAV survives sessions; frames don't) ────
        {
            const juce::File wav = cacheWavFile(docSlot_);
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
            // Zoomed, the image rect overflows the frame on every side — clip
            // so the band never bleeds over the surrounding controls.
            g.saveState();
            g.reduceClipRegion(previewArea.reduced(1));
            g.setOpacity(1.0f);
            g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            g.drawImage(previewImage, imgArea);

            // Hi-res tile of the visible window (cut from the FULL-resolution
            // generatedImage in the background once the zoom settles) —
            // overlaid on the ≤1800 px base thumbnail, white-point matched.
            if (hiResTile_.isValid() && previewZoom_ > 1.001
                && tileFx1_ > tileFx0_ && tileFy1_ > tileFy0_)
            {
                g.drawImage(hiResTile_, juce::Rectangle<float>(
                    imgArea.getX() + (float) tileFx0_ * imgArea.getWidth(),
                    imgArea.getY() + (float) tileFy0_ * imgArea.getHeight(),
                    (float) (tileFx1_ - tileFx0_) * imgArea.getWidth(),
                    (float) (tileFy1_ - tileFy0_) * imgArea.getHeight()));
            }

            // Reading head — only while OUR frames sit in the shared player.
            auto* fs = boundChannel();
            if (fs != nullptr && framesAreOurs)
            {
                const bool playing = fs->isScorePlaying();
                int headFrame = -1;
                if (playing)             headFrame = fs->getScorePlayHead();
                else if (scrubHead >= 0) headFrame = scrubHead;
                else                     headFrame = fs->getScorePlayHead();
                // ^ stopped: the PARKED column (P8 — it keeps sounding, like
                //   a loaded IMAGE's frozen line; drag to move the drone).
                if (headFrame >= 0)
                {
                    const int n = juce::jmax(1, fs->getScoreFrameCount());
                    const float frac = juce::jlimit(0.f, 1.f, (float) headFrame / (float) n);
                    const float lx = imgArea.getX() + frac * imgArea.getWidth();
                    g.setColour(accent.withAlpha(playing ? 0.9f : 0.6f));
                    g.fillRect(lx - 0.75f, imgArea.getY(), 1.5f, imgArea.getHeight());
                }
            }
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

    juce::Rectangle<float> previewDestArea() const
    {
        return juce::Rectangle<float>(
            (float) previewArea.getX() + 2, (float) previewArea.getY() + 2,
            (float) previewArea.getWidth() - 4, (float) previewArea.getHeight() - 4);
    }

    /** The image rect at zoom 1 — whole band fitted in the frame. */
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
    // Preview zoom (mouse wheel / pinch) + overlay scrollbars — same behaviour
    // as the SCORE / TIMBRE / MIDI SCORE pages.
    static constexpr double kMaxPreviewZoom = 16.0;

    void zoomPreviewAt(juce::Point<float> pos, double factor)
    {
        if (! previewImage.isValid())
            return;
        const double target = juce::jlimit(1.0, kMaxPreviewZoom,
                                           previewZoom_ * factor);
        if (juce::approximatelyEqual(target, previewZoom_))
            return;

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

    /** Anchors the zoom scrollbars to the VISIBLE slice of the preview frame
     *  (the page can be taller than the zone-3 viewport). Re-run from the
     *  timer: scrolling the outer viewport moves the window silently. */
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
    // Hi-res zoom tile — same design as the SCORE page: the FULL-resolution
    // generatedImage already exists (it is the export source), so the visible
    // band window (+25% margin) is cropped from it, rescaled to the screen's
    // physical density and white-point matched on a background thread.
    struct TileSpec
    {
        double fx0 = 0.0, fx1 = 0.0, fy0 = 0.0, fy1 = 0.0;  ///< band fractions
        double vx0 = 0.0, vx1 = 0.0, vy0 = 0.0, vy1 = 0.0;  ///< visible (coverage)
        int    w = 0, h = 0;                                 ///< tile pixels
        bool   valid = false;
    };

    TileSpec desiredTileSpec() const
    {
        TileSpec ts;
        if (previewZoom_ <= 1.001 || ! previewImage.isValid()
            || ! generatedImage.isValid())
            return ts;
        const auto imgArea = previewImageBounds();
        const auto vis = imgArea.getIntersection(previewArea.toFloat());
        if (vis.isEmpty() || imgArea.getWidth() <= 0.f || imgArea.getHeight() <= 0.f)
            return ts;

        auto frac = [](float a, float lo, float span)
        { return juce::jlimit(0.0, 1.0, (double) ((a - lo) / span)); };
        ts.vx0 = frac(vis.getX(),      imgArea.getX(), imgArea.getWidth());
        ts.vx1 = frac(vis.getRight(),  imgArea.getX(), imgArea.getWidth());
        ts.vy0 = frac(vis.getY(),      imgArea.getY(), imgArea.getHeight());
        ts.vy1 = frac(vis.getBottom(), imgArea.getY(), imgArea.getHeight());
        const double mx = 0.25 * (ts.vx1 - ts.vx0), my = 0.25 * (ts.vy1 - ts.vy0);
        ts.fx0 = juce::jlimit(0.0, 1.0, ts.vx0 - mx);
        ts.fx1 = juce::jlimit(0.0, 1.0, ts.vx1 + mx);
        ts.fy0 = juce::jlimit(0.0, 1.0, ts.vy0 - my);
        ts.fy1 = juce::jlimit(0.0, 1.0, ts.vy1 + my);
        if (ts.fx1 - ts.fx0 < 1.0e-4 || ts.fy1 - ts.fy0 < 1.0e-4)
            return ts;

        double scale = 2.0;   // physical px per logical px (Retina default)
        if (auto* d = juce::Desktop::getInstance().getDisplays()
                          .getDisplayForRect(getScreenBounds()))
            scale = d->scale;

        const juce::Rectangle<int> band = tileSourceBand();
        double w = (ts.fx1 - ts.fx0) * imgArea.getWidth()  * scale;
        double h = (ts.fy1 - ts.fy0) * imgArea.getHeight() * scale;
        w = juce::jmin(w, (ts.fx1 - ts.fx0) * band.getWidth());
        h = juce::jmin(h, (ts.fy1 - ts.fy0) * band.getHeight());
        constexpr double kBudgetPx = 24.0e6;
        if (w * h > kBudgetPx)
        {
            const double k = std::sqrt(kBudgetPx / (w * h));
            w *= k;
            h *= k;
        }
        ts.w = (int) std::lround(w);
        ts.h = (int) std::lround(h);
        ts.valid = ts.w >= 2 && ts.h >= 2;
        return ts;
    }

    /** The band rect the preview thumbnail was cut from (same fallback rules
     *  as buildPreview so tile fractions and thumbnail fractions line up). */
    juce::Rectangle<int> tileSourceBand() const
    {
        juce::Rectangle<int> band =
            (spectroBand.getWidth() > 0 && spectroBand.getHeight() > 0)
                ? spectroBand.getIntersection(generatedImage.getBounds())
                : generatedImage.getBounds();
        return band.isEmpty() ? generatedImage.getBounds() : band;
    }

    static constexpr juce::uint32 kTileDebounceMs = 180;

    void maybeStartTileRender()
    {
        if (previewZoom_ <= 1.001)
        {
            if (hiResTile_.isValid() && ! tileRenderBusy_)
            {
                hiResTile_ = juce::Image();
                tileFx0_ = tileFx1_ = tileFy0_ = tileFy1_ = 0.0;
            }
            return;
        }
        if (tileRenderBusy_
            || juce::Time::getMillisecondCounter() - lastViewChangeMs_ < kTileDebounceMs)
            return;
        const auto ts = desiredTileSpec();
        if (! ts.valid)
            return;
        const double density = ts.w / juce::jmax(1.0e-6, ts.fx1 - ts.fx0);
        if (hiResTile_.isValid()
            && ts.vx0 >= tileFx0_ - 1.0e-6 && ts.vx1 <= tileFx1_ + 1.0e-6
            && ts.vy0 >= tileFy0_ - 1.0e-6 && ts.vy1 <= tileFy1_ + 1.0e-6
            && tileDensity_ > 0.0
            && std::abs(density / tileDensity_ - 1.0) < 0.25)
            return;

        tileRenderBusy_ = true;
        const juce::Rectangle<int> band = tileSourceBand();
        juce::Thread::launch(
            [safe = juce::Component::SafePointer<VoiceGenTabComponent>(this),
             full = generatedImage, band, ts, wp = previewWp_,
             epoch = tileEpoch_]
            {
                const juce::Rectangle<int> crop(
                    band.getX() + (int) std::floor(ts.fx0 * band.getWidth()),
                    band.getY() + (int) std::floor(ts.fy0 * band.getHeight()),
                    juce::jmax(1, (int) std::ceil((ts.fx1 - ts.fx0) * band.getWidth())),
                    juce::jmax(1, (int) std::ceil((ts.fy1 - ts.fy0) * band.getHeight())));
                juce::Image tile = full.getClippedImage(crop.getIntersection(full.getBounds()))
                                       .rescaled(ts.w, ts.h,
                                                 juce::Graphics::highResamplingQuality);
                // Same display-only white-point lift as the thumbnail — the
                // overlay must not read darker than the base around it.
                if (wp < 0.999 && wp > 0.0)
                {
                    juce::uint8 lut[256];
                    for (int v = 0; v < 256; ++v)
                        lut[v] = (juce::uint8) juce::jlimit(0, 255,
                            (int) std::lround(juce::jmin(1.0, v / 255.0 / wp) * 255.0));
                    juce::Image::BitmapData bd(tile, juce::Image::BitmapData::readWrite);
                    for (int y = 0; y < tile.getHeight(); ++y)
                        for (int x = 0; x < tile.getWidth(); ++x)
                        {
                            auto* p = bd.getPixelPointer(x, y);
                            p[0] = p[1] = p[2] = lut[p[0]];
                        }
                }
                juce::MessageManager::callAsync(
                    [safe, tile = std::move(tile), ts, epoch]() mutable
                    {
                        if (auto* self = safe.getComponent())
                            self->applyTileRender(tile, ts, epoch);
                    });
            });
    }

    void applyTileRender(const juce::Image& tile, const TileSpec& ts, int epoch)
    {
        tileRenderBusy_ = false;
        // Content changed while this tile was being cut — never display it.
        if (epoch != tileEpoch_)
            return;
        if (! tile.isValid())
            return;
        if (previewZoom_ <= 1.001)
        {
            hiResTile_ = juce::Image();
            return;
        }
        hiResTile_ = tile;
        tileFx0_ = ts.fx0;  tileFx1_ = ts.fx1;
        tileFy0_ = ts.fy0;  tileFy1_ = ts.fy1;
        tileDensity_ = ts.w / juce::jmax(1.0e-6, ts.fx1 - ts.fx0);
        repaint(previewArea);
    }

    //==========================================================================
    // Manual play-head scrub on the preview (same behaviour as SCORE).
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
        // Page / DPI / image format live on the SETUP face (VoiceSetupPanel).
        multiResToggle.setBounds(pad, y, colW, ch);
        y += ch + gap + 4;

        generateButton.setBounds(pad, y, colW, ch + 4); y += ch + 8;
        progressBar.setBounds(pad, y, colW, ch);        y += ch + gap;
        exportButton.setBounds(pad, y, colW, ch);
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
        layoutPreviewScrollbars();
        clampPreviewView();
        updatePreviewScrollbars();
    }

private:
    //==========================================================================
    /** Export button that shows its running job: a plain themed TextButton
     *  until an export starts, then a rose bar + travelling sheen while the
     *  image is encoded/written on the background thread (sibling of the
     *  SCORE / TIMBRE / MIDI SCORE export buttons — same visual language). */
    class VoiceExportButton : public juce::TextButton
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

    void initSlider(Sp3ctraBarSlider& s, double lo, double hi, double step, double val)
    {
        s.setRange(lo, hi, step);
        s.setValue(val, juce::dontSendNotification);
        addAndMakeVisible(s);
    }

    void initKnob(juce::Slider& s, double lo, double hi, double step, double val,
                  const char* suffix = nullptr)
    {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 22);
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
        // AUTO language detection removed (was macOS-only NaturalLanguage):
        // resolve to the selected voice, else the first installed one.
        juce::ignoreUnused(forText);
        outLang = {};

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
    /** Page format 2 = Selection: the picker switches to FREE mode (both
     *  edges draggable) and shows the persisted selection length. */
    void updateExportWindow()
    {
        waveform.setFreeSelection(settings_.pageFormat == 2);
        waveform.setWindowSeconds(scoregen::pageWindowSeconds(settings_));
        settings_.startTimeSec = waveform.getStartSeconds();
    }

    //==========================================================================
    void startGenerate(bool autoPlayWhenDone = false)
    {
        if (busy) return;

        const juce::String t = textEditor.getText().trim();
        const bool needSynth = ttsDirty || ! cacheWavFile(docSlot_).existsAsFile();

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
        req.wavFile = cacheWavFile(docSlot_);
        pendingAutoPlay = autoPlayWhenDone;

        busy = true;
        progress = 0.0;
        generateButton.setEnabled(false);
        exportButton.setEnabled(false);
        if (auto* fs = boundChannel())
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
            if (auto* fs = boundChannel())
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

        exportButton.setEnabled(true);
        logLabel.setText(r.render.log + "\n" + previewStats, juce::dontSendNotification);
        scrubHead = -1;
        setTransportEnabled(true);
        refreshPlayButton();

        if (pendingAutoPlay)
        {
            pendingAutoPlay = false;
            if (auto* p = processor.getAPVTS().getParameter(xport_.playParamId()))
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
        if (auto* fs = boundChannel())
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
        // The content changed (generate / EQ): whatever the hi-res zoom tile
        // shows is stale — drop it, the timer re-cuts the visible window. The
        // epoch bump also voids any tile still in flight on the worker.
        hiResTile_ = juce::Image();
        tileFx0_ = tileFx1_ = tileFy0_ = tileFy1_ = 0.0;
        ++tileEpoch_;
        previewWp_ = 1.0;

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
        previewWp_ = wp;   // the hi-res zoom tile applies the SAME lift
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

    // Export runs on a BACKGROUND thread (encode + DPI stamp + write of the
    // already-generated image) so the UI stays live; the button shows a rose
    // bar with a travelling sheen meanwhile. Format (PNG/JPEG) comes from the
    // SETUP face. juce::Image is COW, so the captured reference stays valid
    // even if EQ/generate swaps the member.
    void exportNow()
    {
        if (LicenseGate::blockIfDemo(this, "Export image"))
            return;
        if (! generatedImage.isValid() || exportBusy_)
            return;
        const bool asPng = exportAsPng_;
        const juce::String ext = asPng ? "png" : "jpg";
        // Name from the first words of the text ("voice" fallback).
        juce::String slug = text.trim().replaceCharacters(" \t\n\r", "----")
                                .retainCharacters("abcdefghijklmnopqrstuvwxyz"
                                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-")
                                .substring(0, 32).trimCharactersAtEnd("-");
        if (slug.isEmpty()) slug = "voice";
        const juce::File dest = exportDir().getNonexistentChildFile(
            slug + "_score", "." + ext, false);
        // Embed the band + frequency range so a re-load into the SAMPLER
        // reproduces the EXACT voice calage (mono take → stereo = false).
        scoregen::SpectroCalibration cal;
        cal.band   = spectroBand;
        cal.minHz  = genMinFreq;
        cal.maxHz  = genMaxFreq;
        cal.stereo = false;
        cal.valid  = spectroBand.getWidth() > 0
                  && spectroBand.getHeight() > 0
                  && genMaxFreq > genMinFreq
                  && genMinFreq > 0.0;

        // libjpeg hard-caps both dimensions at 65500 px — a Selection sheet
        // can be wider. Only PNG can hold it.
        if (! asPng && (generatedImage.getWidth()  > 65500
                     || generatedImage.getHeight() > 65500))
        {
            logLabel.setText("Too large for JPEG (65500 px max per side) "
                             + juce::String::fromUTF8("— switch the format to PNG in SETUP"),
                             juce::dontSendNotification);
            return;
        }

        exportBusy_ = true;
        exportButton.setButtonText(juce::String::fromUTF8("Writing…"));
        exportButton.setJobState(true, 1.f, true);

        juce::Thread::launch(
            [safe = juce::Component::SafePointer<VoiceGenTabComponent>(this),
             img = generatedImage, dest, asPng, cal, dpi = genDpi,
             session = processor.sessions()->sessionName()]
            {
                const bool ok = scoregen::exportImage(img, dest, asPng, dpi, &cal);
                const juce::String msg = ok
                    ? "Exported: " + dest.getFileName()
                          + juce::String::fromUTF8(" → ") + session + "/exports"
                    : "Export failed: " + dest.getFileName();
                juce::MessageManager::callAsync([safe, msg]
                {
                    if (auto* self = safe.getComponent())
                        self->finishExport(msg);
                });
            });
    }

    void finishExport(const juce::String& msg)
    {
        exportBusy_ = false;
        exportButton.setJobState(false, 0.f, false);
        exportButton.setButtonText("Export image");
        logLabel.setText(msg, juce::dontSendNotification);
    }

    //==========================================================================
    // Source-audio preview — play/pause the SELECTED region of the take.
    static juce::String playGlyph()  { return juce::String(juce::CharPointer_UTF8("\xe2\x96\xb6")); } // ▶
    static juce::String pauseGlyph() { return juce::String(juce::CharPointer_UTF8("\xe2\x8f\xb8")); } // ⏸

    void togglePreview()
    {
        const juce::File wav = cacheWavFile(docSlot_);
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
        auto* fs = boundChannel();
        if (fs == nullptr || busy) return;

        const bool play = ! (fs->isScorePlaying() && framesAreOurs);

        if (play && ! framesAreOurs)
        {
            if (generatedImage.isValid())
            {
                // Another module took the shared channel — reclaim it.
                applyEqToImageAndReload();
            }
            else if (cacheWavFile(docSlot_).existsAsFile())
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

        // Route through VOICE's own play param so the DAW sees the transport;
        // the processor pushes speed/loop and starts/stops VOICE's slot. If the
        // param already matches (brief drift window), drive the engine directly.
        if (auto* p = processor.getAPVTS().getParameter(xport_.playParamId()))
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
        auto* fs = boundChannel();
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
        // Zoomed: keep the scrollbars pinned to the VISIBLE slice of the
        // preview while the outer zone-3 viewport scrolls (no notification),
        // and sharpen the visible window once the view settles.
        if (previewZoom_ > 1.001)
            layoutPreviewScrollbars();
        maybeStartTileRender();

        // Running export: animate the sheen on the button.
        if (exportBusy_)
            exportButton.setJobState(true, 1.f, true);

        // Reapply the EQ once the user releases a node (deferred, like SCORE).
        if (eqDirty && ! eqEditor.isDragging())
        {
            eqDirty = false;
            // P7 - the curve belongs to THIS instance's doc (persisted with it).
            docs_[(size_t) docSlot_].eqState = eqEditor.encodeState();
            markDirty();
            if (baseImage.isValid())
                applyEqToImageAndReload();
        }

        // Debounced page-state persistence (typing-friendly).
        if (stateDirty
            && juce::Time::getMillisecondCounter() - lastEditMs > 800)
            persistState();

        auto* fs = boundChannel();
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

        // Mirror VOICE's play param on the real engine state so the DAW lane
        // stays truthful when a one-shot ends or an internal reload stops it.
        // parameterChanged() ignores writes that already match → no retrigger.
        if (auto* p = processor.getAPVTS().getParameter(xport_.playParamId()))
        {
            const float norm = (fs != nullptr && fs->isScorePlaying() && framesAreOurs)
                                 ? 1.0f : 0.0f;
            if (! juce::approximatelyEqual(p->getValue(), norm))
                p->setValueNotifyingHost(norm);
        }

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
    //==========================================================================
    // P7 — one page, N instances. This component is a VIEW over a single VOICE
    // instance at a time: everything the instance owns lives in an InstanceDoc
    // keyed by the module's score-player pool slot. Selecting another VOICE
    // parks the live members into their doc and pulls the target's out, so two
    // VOICE modules in two chains keep entirely separate texts, voices, page
    // settings, takes and renders. One APVTS key + one cached WAV per doc.
    //==========================================================================
    static constexpr int kMaxDocs = 8;   // == ScorePlayerService::kMaxSlots

    struct InstanceDoc
    {
        /** ScoreSettings is a flat C struct with NO member initialisers: `{}`
         *  zeroes it, and a zeroed sheet renders BLANK (spectroHeightMM = 0 ⇒
         *  no band, fftSize = 0 ⇒ 64-sample windows). Every doc must therefore
         *  start from the page defaults — applyDocToMembers() assigns settings
         *  over settings_ unconditionally, so the doc IS the source of truth. */
        InstanceDoc()
        {
            score_settings_defaults(&settings);
            settings.writingSpeed = 2.5;       // match the SCORE page default
        }

        juce::String  text;
        bool          autoMode = true;
        juce::String  selectedVoiceId;
        double        rate = 1.0, expr = 0.667, silence = 1.0;
        juce::String  lastLang, lastVoiceName;
        ScoreSettings settings {};
        bool          exportAsPng = true;
        bool          ttsDirty    = true;
        juce::String  eqState;                 // encoded EQ spline
        // Renders (copy-on-write: an untouched instance costs nothing).
        juce::Image          generatedImage, baseImage, previewImage;
        juce::Rectangle<int> spectroBand;
        double genMinFreq = 0.0, genMaxFreq = 0.0;
        double genDynRangeDB = 50.0, genDpi = 400.0;
        double previewWp = 1.0;
        juce::String previewStats;
        bool  framesAreOurs   = false;
        int   loadedFrameCount = 0;
    };

    // Persistence — one JSON blob per instance in apvts.state (slot 0 keeps the
    // legacy "voiceGenState" key, so pre-P7 sessions restore into the first
    // instance); the EQ curve rides inside the blob.
    static juce::String stateKey(int slot)
    {
        return slot <= 0 ? juce::String("voiceGenState")
                         : "voiceGenState" + juce::String(juce::jlimit(1, 7, slot));
    }

    /** Parks the live members into their doc, then writes EVERY instance doc.
     *  externalVoicesDir / langPref are TTS install preferences, shared by all
     *  instances — they stay on the legacy slot-0 blob. */
    void persistState()
    {
        stateDirty = false;
        captureDoc();
        for (int i = 0; i < kMaxDocs; ++i)
            persistDoc(i, docs_[(size_t) i]);
    }

    void persistDoc(int slot, const InstanceDoc& d) const
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("text",  d.text);
        root->setProperty("voice", d.autoMode ? juce::String("auto") : d.selectedVoiceId);
        root->setProperty("rate",  d.rate);
        root->setProperty("expr",  d.expr);
        root->setProperty("sil",   d.silence);
        root->setProperty("ws",    d.settings.writingSpeed);
        root->setProperty("page",  d.settings.pageFormat);
        root->setProperty("dpi",   d.settings.printerDpi);
        root->setProperty("mres",  d.settings.enableMultiRes);
        root->setProperty("start", d.settings.startTimeSec);
        root->setProperty("sel",   d.settings.selectionSec);
        root->setProperty("png",   d.exportAsPng);
        root->setProperty("lang",  d.lastLang);
        root->setProperty("vname", d.lastVoiceName);
        root->setProperty("eq",    d.eqState);
        root->setProperty("extdir", externalVoicesDir.getFullPathName());
        auto* prefs = new juce::DynamicObject();
        for (int i = 0; i < langPref.size(); ++i)
            prefs->setProperty(langPref.getName(i), langPref.getValueAt(i));
        const juce::var prefVar(prefs);
        root->setProperty("pref", prefVar);
        processor.getAPVTS().state.setProperty(
            stateKey(slot), juce::JSON::toString(juce::var(root), true), nullptr);
        // TTS INSTALL prefs (voices folder + per-language voice picks) are
        // machine-scoped: mirror them into machine.settings so a foreign
        // session/DAW blob can never repoint this computer's voice install.
        // (The blob copies remain as a read fallback for old sessions.)
        MachinePrefs::file().setValue("voice.extDir",
                                      externalVoicesDir.getFullPathName());
        MachinePrefs::file().setValue("voice.langPref",
                                      juce::JSON::toString(prefVar, true));
    }

    /** Loads every instance doc, then makes the live one current. Data only:
     *  the constructor calls this before the widgets exist. */
    void restoreState()
    {
        for (int i = 0; i < kMaxDocs; ++i)
            restoreDoc(i, docs_[(size_t) i]);
        // Machine copy of the TTS install prefs wins over whatever the blobs
        // carried (restoreDoc applied those as the pre-scoping fallback).
        if (const auto extdir = MachinePrefs::file().getValue("voice.extDir");
            extdir.isNotEmpty() && juce::File(extdir).isDirectory())
            externalVoicesDir = juce::File(extdir);
        if (const auto prefJson = MachinePrefs::file().getValue("voice.langPref");
            prefJson.isNotEmpty())
            if (auto* o = juce::JSON::parse(prefJson).getDynamicObject())
                langPref = o->getProperties();
        applyDocToMembers();
    }

    void restoreDoc(int slot, InstanceDoc& d)
    {
        const juce::String blob = processor.getAPVTS().state
            .getProperty(stateKey(slot), "").toString();
        if (blob.isEmpty()) return;
        const juce::var root = juce::JSON::parse(blob);
        auto* o = root.getDynamicObject();
        if (o == nullptr) return;

        d.text = o->getProperty("text").toString();
        const juce::String v = o->getProperty("voice").toString();
        d.autoMode        = (v.isEmpty() || v == "auto");
        d.selectedVoiceId = d.autoMode ? juce::String() : v;
        if (o->hasProperty("rate"))  d.rate    = (double) o->getProperty("rate");
        if (o->hasProperty("expr"))  d.expr    = (double) o->getProperty("expr");
        if (o->hasProperty("sil"))   d.silence = (double) o->getProperty("sil");
        if (o->hasProperty("ws"))    d.settings.writingSpeed   = (double) o->getProperty("ws");
        if (o->hasProperty("page"))  d.settings.pageFormat     = (int)    o->getProperty("page");
        if (o->hasProperty("dpi"))   d.settings.printerDpi     = (double) o->getProperty("dpi");
        if (o->hasProperty("mres"))  d.settings.enableMultiRes = (int)    o->getProperty("mres");
        if (o->hasProperty("start")) d.settings.startTimeSec   = (double) o->getProperty("start");
        if (o->hasProperty("sel"))   d.settings.selectionSec   = (double) o->getProperty("sel");
        if (o->hasProperty("png"))   d.exportAsPng             = (bool)   o->getProperty("png");
        d.settings.pageFormat = juce::jlimit(0, 2, d.settings.pageFormat);
        d.lastLang      = o->getProperty("lang").toString();
        d.lastVoiceName = o->getProperty("vname").toString();
        d.eqState       = o->getProperty("eq").toString();
        // Shared TTS install prefs — only the slot-0 blob carries the truth.
        const juce::String extdir = o->getProperty("extdir").toString();
        if (extdir.isNotEmpty() && juce::File(extdir).isDirectory())
            externalVoicesDir = juce::File(extdir);
        if (auto* prefs = o->getProperty("pref").getDynamicObject())
            langPref = prefs->getProperties();
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
        boundScoreSlot_  = slot;
        bindTransport();   // P7 — the transport follows the selected instance
        viewDoc(slot >= 0 ? slot : transportSlot());   // …and so do the page's own settings
        repaint();
    }

    //==========================================================================
    // SETUP face access (VoiceSetupPanel) — export/generation preferences.
    // Page + DPI shape the NEXT GENERATE (and the region-picker window);
    // PNG/JPEG only picks the export container.
    int  exportPageFormat() const { return settings_.pageFormat; }
    void setExportPageFormat(int f)
    {
        f = juce::jlimit(0, 2, f);
        if (f == settings_.pageFormat)
            return;
        settings_.pageFormat = f;
        updateExportWindow();     // A4/A3/Selection changes the window length
        markDirty();
    }

    bool exportFormatIsPng() const { return exportAsPng_; }
    void setExportFormatPng(bool png)
    {
        if (png == exportAsPng_)
            return;
        exportAsPng_ = png;
        markDirty();
    }

    int  exportDpi() const { return (int) settings_.printerDpi; }
    void setExportDpi(int dpi)
    {
        dpi = juce::jmax(72, dpi);
        if ((double) dpi == settings_.printerDpi)
            return;
        settings_.printerDpi = (double) dpi;
        markDirty();
    }

private:
    //── P7 — per-instance page docs (see InstanceDoc above) ───────────────────
    /** Per-instance cached take. Slot 0 keeps the legacy path so an existing
     *  install finds its last render. */
    static juce::File cacheWavFile(int slot)
    {
        const juce::File dir = juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory)
                .getChildFile("Application Support/Sp3ctra/voice_renders");
        return slot <= 0 ? dir.getChildFile("voice_last.wav")
                         : dir.getChildFile("voice_last" + juce::String(slot) + ".wav");
    }

    /** Parks the live members into the doc the page is currently viewing. */
    void captureDoc()
    {
        auto& d = docs_[(size_t) docSlot_];
        d.text             = text;
        d.autoMode         = autoMode;
        d.selectedVoiceId  = selectedVoiceId;
        d.rate             = rate;
        d.expr             = expr;
        d.silence          = silence;
        d.lastLang         = lastLang;
        d.lastVoiceName    = lastVoiceName;
        d.settings         = settings_;
        d.exportAsPng      = exportAsPng_;
        d.ttsDirty         = ttsDirty;
        d.eqState          = eqEditor.encodeState();
        d.generatedImage   = generatedImage;
        d.baseImage        = baseImage;
        d.previewImage     = previewImage;
        d.spectroBand      = spectroBand;
        d.genMinFreq       = genMinFreq;
        d.genMaxFreq       = genMaxFreq;
        d.genDynRangeDB    = genDynRangeDB;
        d.genDpi           = genDpi;
        d.previewWp        = previewWp_;
        d.previewStats     = previewStats;
        d.framesAreOurs    = framesAreOurs;
        d.loadedFrameCount = loadedFrameCount;
    }

    /** Pulls the viewed doc back into the live members (data only — safe
     *  before the widgets exist, i.e. from the constructor). */
    void applyDocToMembers()
    {
        const auto& d = docs_[(size_t) docSlot_];
        text             = d.text;
        autoMode         = d.autoMode;
        selectedVoiceId  = d.selectedVoiceId;
        rate             = d.rate;
        expr             = d.expr;
        silence          = d.silence;
        lastLang         = d.lastLang;
        lastVoiceName    = d.lastVoiceName;
        settings_        = d.settings;
        exportAsPng_     = d.exportAsPng;
        ttsDirty         = d.ttsDirty;
        generatedImage   = d.generatedImage;
        baseImage        = d.baseImage;
        previewImage     = d.previewImage;
        spectroBand      = d.spectroBand;
        genMinFreq       = d.genMinFreq;
        genMaxFreq       = d.genMaxFreq;
        genDynRangeDB    = d.genDynRangeDB;
        genDpi           = d.genDpi;
        previewWp_       = d.previewWp;
        previewStats     = d.previewStats;
        framesAreOurs    = d.framesAreOurs;
        loadedFrameCount = d.loadedFrameCount;

        // View state that never belongs to an instance: void the zoom tile so
        // no pixel of the previous instance can leak into this one's preview.
        hiResTile_ = juce::Image();
        tileFx0_ = tileFx1_ = tileFy0_ = tileFy1_ = 0.0;
        tileDensity_ = 0.0;
        ++tileEpoch_;
        previewZoom_ = 1.0; previewCx_ = 0.5; previewCy_ = 0.5;
        eqDirty = false;
    }

    /** Pushes the live members onto the widgets. Constructor-unsafe — call it
     *  only once the page is built. */
    void refreshUiFromMembers()
    {
        if (docs_[(size_t) docSlot_].eqState.isNotEmpty())
            eqEditor.decodeState(docs_[(size_t) docSlot_].eqState);
        textEditor.setText(text, juce::dontSendNotification);
        rateSlider   .setValue(rate,    juce::dontSendNotification);
        exprSlider   .setValue(expr,    juce::dontSendNotification);
        silenceSlider.setValue(silence, juce::dontSendNotification);
        wsSlider     .setValue(settings_.writingSpeed, juce::dontSendNotification);
        multiResToggle.setToggleState(settings_.enableMultiRes != 0,
                                      juce::dontSendNotification);
        rebuildVoiceCombo();

        // The take is a FILE per instance: re-point the waveform strip at it.
        const juce::File wav = cacheWavFile(docSlot_);
        if (wav.existsAsFile())
        {
            const auto info = scoregen::probeWav(wav);
            if (info.ok)
            {
                waveform.setFile(wav);
                waveform.setStartSeconds(settings_.startTimeSec);
                updateExportWindow();
                previewButton.setEnabled(true);
                setSynthStatus(info.durationSec, info.sampleRate);
                setTransportEnabled(true);
            }
        }
        else
        {
            waveform.setFile(juce::File());
            previewButton.setEnabled(false);
            setTransportEnabled(false);
        }
        resized();
        repaint();
    }

    /** P7 — instance @p slot left the rack: its document dies with it, so the
     *  next module that lands on this pool slot (possibly in another chain)
     *  starts from the page defaults instead of inheriting a stranger's work.
     *  Wiping the LIVE members too when the page is viewing that slot. */
public:
    void forgetScoreSlot(int slot)
    {
        slot = juce::jlimit(0, kMaxDocs - 1, slot);
        docs_[(size_t) slot] = defaultDoc_;
        stateDirty = true;            // the wipe must reach the next save
        if (slot == docSlot_)
        {
            applyDocToMembers();
            refreshUiFromMembers();
            boundScoreSlot_ = -1;
        }
    }
private:

    /** Switches the page to instance @p slot (parking the current one). */
    void viewDoc(int slot)
    {
        slot = juce::jlimit(0, kMaxDocs - 1, slot);
        if (slot == docSlot_)
            return;
        if (processor.isScorePreviewPlaying())
            processor.stopScorePreview();   // the take being auditioned is leaving
        captureDoc();
        docSlot_ = slot;
        applyDocToMembers();
        refreshUiFromMembers();
    }

    std::array<InstanceDoc, kMaxDocs> docs_ {};
    InstanceDoc defaultDoc_ {};   ///< pristine page — what a freed slot reverts to
    int docSlot_ = 0;          ///< instance whose doc is live in the members

    /** The bound score channel: the selected instance's slot while it is
     *  still in the rack, else the first placed instance of this type. */
    ScoreChannel* boundChannel() const
    {
        if (boundScoreSlot_ >= 0
            && processor.scorePlayerSlotInUse(boundScoreSlot_))
            return processor.getScoreChannelForSlot(boundScoreSlot_);
        return processor.getScoreChannel(ModuleType::Voice);
    }
    /** Player-pool slot whose transport bank the widgets are bound to. Falls
     *  back to the first placed instance of this type (then slot 0) so the page
     *  always shows a valid bank, even with no module in the rack. */
    int transportSlot() const
    {
        if (boundScoreSlot_ >= 0 && processor.scorePlayerSlotInUse(boundScoreSlot_))
            return boundScoreSlot_;
        if (auto* sc = processor.getScoreChannel(ModuleType::Voice))
            return sc->slot();
        return 0;
    }
    void bindTransport()
    {
        xport_.rebind(processor.getAPVTS(), processor.getMidiMap(),
                      ModuleType::Voice, transportSlot(),
                      playStopButton, loopBtn, reverseBtn, speedSlider);
    }
    int boundScoreSlot_ = -1;

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

    // Generation controls (SCORE-identical; page format / DPI / image format
    // live on the SETUP face).
    juce::TextButton generateButton;
    VoiceExportButton exportButton;
    bool exportAsPng_ = true;          // SETUP face: PNG (true) / JPEG
    bool exportBusy_  = false;         // one export at a time
    juce::Label      logLabel;
    juce::Label      wsLabel;
    Sp3ctraBarSlider wsSlider;
    juce::ToggleButton multiResToggle;
    ScoreSettings    settings_ {};     // VOICE's own page settings (persisted in the blob)

    // Playback transport (this instance's own score-player slot).
    TransportPlayButton playStopButton;
    VoiceIconToggle     loopBtn    { VoiceIconToggle::Glyph::Loop };
    VoiceIconToggle     reverseBtn { VoiceIconToggle::Glyph::Inverse };
    juce::Slider        speedSlider;
    juce::Label         speedLabel, playHint;
    int  scrubHead { -1 };
    bool scrubbing { false };
    bool scrubAuditioning { false };
    juce::Rectangle<float> previewImgArea;

    // Preview zoom state — 1 = whole band fitted; Cx/Cy = image fraction at
    // the frame centre while zoomed (wheel/pinch zoom, scrollbars navigate).
    double previewZoom_ = 1.0, previewCx_ = 0.5, previewCy_ = 0.5;
    juce::ScrollBar previewHScroll { false }, previewVScroll { true };

    // Hi-res tile of the visible band window while zoomed (see desiredTileSpec).
    juce::Image  hiResTile_;
    double       tileFx0_ = 0.0, tileFx1_ = 0.0, tileFy0_ = 0.0, tileFy1_ = 0.0;
    double       tileDensity_ = 0.0;    // tile px per band fraction (renew test)
    double       previewWp_ = 1.0;      // thumbnail white-point (tile matches it)
    bool         tileRenderBusy_ = false;
    int          tileEpoch_ = 0;        // bumped on content change → voids in-flight tiles
    juce::uint32 lastViewChangeMs_ = 0;

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


    // Transport bank of the SELECTED instance (attachments + MIDI-Learn).
    // Declared LAST so it is destroyed before the widgets it binds.
    ScoreTransportBinding xport_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceGenTabComponent)
};
