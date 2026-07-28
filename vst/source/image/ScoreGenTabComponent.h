/**
 * @file ScoreGenTabComponent.h
 * @brief PLAY page for the SCORE block — generate a printable greyscale
 *        spectrogram ("graphical score") from a WAV file and export it as
 *        PNG/JPEG.
 *
 * Port of the legacy Sp3ctraGen app (audio→spectrogram) into the VST shell:
 *   Load WAV → tweak parameters → GENERATE (worker thread) → preview → EXPORT.
 *
 * Parameters live in a local ScoreSettings (offline export — never read by
 * processBlock, so deliberately NOT in the APVTS / not host-automatable).
 *
 * Beyond export, the generated image can be PLAYED back through the synthesis
 * pipeline: the SCORE block now sits in CHAIN 1 (between SAMPLER and LUXSTRAL)
 * and reuses the LuxSampler playback engine. The Play/Stop/Loop/Speed transport
 * below drives LuxSampler::uiPlayScore()/uiStopScore().
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <vector>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../IconPaths.h"
#include "ScoreGenThread.h"
#include "ScoreGenRenderer.h"
#include "ScoreEqComponent.h"
#include "WaveformSelectorComponent.h"

class ScoreGenTabComponent : public juce::Component,
                             private juce::Timer,
                             private juce::ScrollBar::Listener
{
public:
    static constexpr uint32_t kAccentARGB = 0xffe0a24a;   // amber (SCORE identity)

    explicit ScoreGenTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        const juce::Colour accent(kAccentARGB);

        // ── Load WAV ───────────────────────────────────────────────────────
        // Generation parameters now live on the SETUP face (ScoreSetupPanel) —
        // this PLAY page keeps only the load/generate/export/transport actions.
        loadButton.setButtonText("Load WAV...");
        loadButton.onClick = [this] { chooseWav(); };
        addAndMakeVisible(loadButton);

        fileLabel.setText("No file loaded", juce::dontSendNotification);
        fileLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        fileLabel.setColour(juce::Label::textColourId, juce::Colour(0xffb8c0d0));
        addAndMakeVisible(fileLabel);

        // ── Writing Speed (essential — maps audio duration to page width) ────
        initLabel(wsLabel, "Writing Speed (cm/s)");
        initSlider(wsSlider, 0.5, 10.0, 0.1, processor.getScoreSettings().writingSpeed);
        wsSlider.onValueChange = [this]
        {
            processor.getScoreSettings().writingSpeed = wsSlider.getValue();
            updateExportWindow();   // window width depends on writing speed
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
        exportButton.setTooltip("Export the generated score as an image "
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

        // ── Playback transport (CHAIN 1) ───────────────────────────────────
        playStopButton.setEnabled(false);
        playStopButton.setTooltip("Play / stop the generated score");
        playStopButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playStopButton);

        // Loop + Inverse as compact pictogram toggles (replace the old text Loop).
        // APVTS-bound (scoreLoop / scoreReverse) so the DAW can automate /
        // MIDI-map them; the processor derives the engine LoopMode from the
        // two params (reverse → INVERSE, else loop → LOOP, else NONE).
        loopBtn.setEnabled(false);
        loopBtn.setTooltip("Loop playback");
        addAndMakeVisible(loopBtn);
        loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "scoreLoop", loopBtn);

        reverseBtn.setEnabled(false);
        reverseBtn.setTooltip("Reverse (play the score backward)");
        addAndMakeVisible(reverseBtn);
        reverseAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "scoreReverse", reverseBtn);

        initLabel(speedLabel, "Speed");
        initKnob(speedSlider, 0.1, 6.0, 0.01, 1.0, "x");
        speedSlider.setSkewFactorFromMidPoint(1.0); // 1.0× sits at the knob's centre (log feel)
        // APVTS-bound (scoreSpeed) — the processor relays changes to the engine
        // live, whether they come from this knob, DAW automation or MIDI.
        speedAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.getAPVTS(), "scoreSpeed", speedSlider);

        // Right-click MIDI Learn on the transport (SCORE is a singleton).
        {
            auto& mm = processor.getMidiMap();
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, playStopButton, "scorePlaying"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, loopBtn,        "scoreLoop"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, reverseBtn,     "scoreReverse"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, speedSlider,    "scoreSpeed"));
        }

        // Disabled (greyed) until a score is generated — but frames generated
        // in a PREVIOUS life of this page survive in the engine (the page is
        // just a view), so re-enable the transport when a score is already
        // loaded. The preview image itself is rebuilt on the next GENERATE
        // only (v1 limitation).
        setTransportEnabled(false);
        if (auto* fs0 = boundChannel())
            if (fs0->scoreHasContent())
                setTransportEnabled(true);

        playHint.setText("Set LuxStral source = Sampler to hear the score.",
                         juce::dontSendNotification);
        playHint.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        playHint.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        addAndMakeVisible(playHint);

        // Page format / DPI / PNG-vs-JPEG moved to the SETUP face
        // (ScoreSetupPanel) — the timer below mirrors a SETUP-side page-format
        // change into the region-picker window length.
        shownPageFormat_ = processor.getScoreSettings().pageFormat;

        // ── Stereo mode: two spectrograms (left=red, right=blue) ────────────
        // Takes effect on the next GENERATE. Reuses LuxStral's colour-temperature
        // panning (red→left ear, blue→right ear) — no synth code is touched.
        stereoToggle.setButtonText("Stereo (L=red / R=blue)");
        stereoToggle.setToggleState(processor.getScoreSettings().enableStereoMode != 0,
                                    juce::dontSendNotification);
        stereoToggle.onClick = [this]
        {
            const bool on = stereoToggle.getToggleState();
            processor.getScoreSettings().enableStereoMode = on ? 1 : 0;
            // The colour image only pans if LuxStral's stereo engine is enabled.
            // Turn it on (its published APVTS switch — no LuxStral code touched) so
            // the result isn't silently mono. Leave it untouched when toggling off,
            // since the user may rely on it elsewhere (e.g. live CIS stereo).
            if (on)
                if (auto* p = processor.getAPVTS().getParameter("luxstralStereoEnable"))
                    p->setValueNotifyingHost(1.0f);
        };
        addAndMakeVisible(stereoToggle);

        // ── Multi-resolution analysis: shorter FFT windows for the upper ────
        // octaves (sharper transients in the highs, full harmonic resolution
        // kept in the lows). Encoder-only — playback is unchanged. Takes
        // effect on the next GENERATE.
        multiResToggle.setButtonText("Multi-res transients");
        multiResToggle.setToggleState(processor.getScoreSettings().enableMultiRes != 0,
                                      juce::dontSendNotification);
        multiResToggle.onClick = [this]
        {
            processor.getScoreSettings().enableMultiRes =
                multiResToggle.getToggleState() ? 1 : 0;
        };
        addAndMakeVisible(multiResToggle);

        // ── Waveform region picker (which part of the WAV to extract) ───────
        waveform.onStartChange = [this](double startSec)
        { processor.getScoreSettings().startTimeSec = startSec; };
        // Selection sheet (page format 2): both edges are draggable — persist
        // the region length too, the sheet stretches to hold it.
        waveform.onRegionChange = [this](double startSec, double lenSec)
        {
            auto& s = processor.getScoreSettings();
            s.startTimeSec = startSec;
            s.selectionSec = lenSec;
        };
        addAndMakeVisible(waveform);

        // ── Audition button: play/pause the SELECTED SOURCE region ──────────
        previewButton.setEnabled(false);
        previewButton.onClick = [this] { togglePreview(); };
        addAndMakeVisible(previewButton);
        refreshPreviewButton();

        // ── Image EQ (edits the generated image, never the source WAV) ──────
        eqEditor.onChange = [this] { eqDirty = true; };
        addAndMakeVisible(eqEditor);

        // ── Restore persisted SCORE page state ──────────────────────────────
        // The EQ curve rides in apvts.state (written on every edit below);
        // the WAV path + extraction start live in the processor and were
        // restored by setStateInformation before this page is built.
        {
            const juce::String eq = processor.getAPVTS().state
                .getProperty("scoreEqCurve", "").toString();
            if (eq.isNotEmpty())
                eqEditor.decodeState(eq);

            const juce::File wav(processor.getScoreWavPath());
            if (wav.existsAsFile())
                setLoadedFile(wav, /*restoreRegion*/ true);
        }

        startTimerHz(10);   // reflect auto-stop (LoopMode::NONE end) on the button

        // ── Log ────────────────────────────────────────────────────────────
        logLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        logLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        logLabel.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(logLabel);

        // Worker callbacks marshal back to the message thread.
        job.onProgress = [safe = juce::Component::SafePointer<ScoreGenTabComponent>(this)]
                         (float pr)
        {
            juce::MessageManager::callAsync([safe, pr]
            {
                if (auto* self = safe.getComponent())
                    self->progress = (double) pr;
            });
        };
        job.onDone = [safe = juce::Component::SafePointer<ScoreGenTabComponent>(this)]
                     (scoregen::RenderResult r)
        {
            juce::MessageManager::callAsync([safe, r = std::move(r)]() mutable
            {
                if (auto* self = safe.getComponent())
                    self->onRenderFinished(std::move(r));
            });
        };

        juce::ignoreUnused(accent);
    }

    ~ScoreGenTabComponent() override
    {
        stopTimer();
        // Do NOT stop the score here: this page is a VIEW, not the transport —
        // closing the plugin window must not cut the music (the SEQUENCER
        // already survives it). The scorePlaying param mirror moved to the
        // processor's timer, which also covers the one-shot natural end while
        // no page is open.
        processor.stopScorePreview();
        job.stopThread(3000);
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent(kAccentARGB);

        // Preview frame
        g.setColour(juce::Colour(0xff10131a));
        g.fillRect(previewArea);
        g.setColour(accent.withAlpha(0.35f));
        g.drawRect(previewArea, 1);

        if (previewImage.isValid())
        {
            const auto imgArea = previewImageBounds();
            previewImgArea = imgArea;   // cache for scrub hit-testing / mapping
            // Zoomed, the image rect overflows the frame on every side — clip
            // so the strip never bleeds over the surrounding controls.
            g.saveState();
            g.reduceClipRegion(previewArea.reduced(1));
            // drawImage() modulates by the current fill's alpha; the 0.35 set
            // for the frame border above would otherwise blit the image at 35%
            // opacity over the dark frame (→ grey floor). Force full opacity.
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

            // ── Reading head: vertical line at the played column (live) or, when
            //    stopped, at the manually-placed scrub position. ───────────────
            auto* fs = boundChannel();
            const bool playing = (fs != nullptr) && fs->isScorePlaying();
            int headFrame = -1;
            if (playing && fs != nullptr) headFrame = fs->getScorePlayHead();
            else if (scrubHead >= 0)      headFrame = scrubHead;
            if (headFrame >= 0 && fs != nullptr)
            {
                const int n = juce::jmax(1, fs->getScoreFrameCount());
                const float frac = juce::jlimit(0.f, 1.f, (float) headFrame / (float) n);
                const float lx = imgArea.getX() + frac * imgArea.getWidth();
                g.setColour(accent.withAlpha(playing ? 0.9f : 0.6f));
                g.fillRect(lx - 0.75f, imgArea.getY(), 1.5f, imgArea.getHeight());
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
            g.drawText(busy ? "Generating..." : "Load a WAV and press GENERATE",
                       previewArea, juce::Justification::centred);
        }
    }

    juce::Rectangle<float> previewDestArea() const
    {
        return juce::Rectangle<float>(
            (float) previewArea.getX() + 2, (float) previewArea.getY() + 2,
            (float) previewArea.getWidth() - 4, (float) previewArea.getHeight() - 4);
    }

    /** The image rect at zoom 1 — whole band fitted in the frame (keeps aspect
     *  ratio, may enlarge on window resizes). Zoom scales THIS rect. */
    juce::Rectangle<float> fittedPreviewRect() const
    {
        const juce::RectanglePlacement place(juce::RectanglePlacement::centred);
        return place.appliedTo(
            juce::Rectangle<float>(0.f, 0.f,
                (float) previewImage.getWidth(), (float) previewImage.getHeight()),
            previewDestArea());
    }

    /** Destination rectangle where the preview image is blitted inside
     *  previewArea — the zoom 1 fit, or the scaled/panned rect while zoomed
     *  (centred while it fits, clamped edge-to-edge once it overflows). */
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
    // as the MIDI SCORE page. Zoom 1 = whole band fitted; previewCx_/Cy_ =
    // image fraction shown at the frame centre while zoomed.
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

    /** Anchors the zoom scrollbars to the VISIBLE slice of the preview frame —
     *  the page can be taller than the zone-3 viewport, and a bar glued to the
     *  page's far bottom would sit below the fold. Re-run from the timer:
     *  scrolling the outer viewport moves the visible window silently. */
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
    // Hi-res zoom tile. The preview thumbnail is capped at 1800 px, so zooming
    // it is pure upscale blur — but here the FULL-resolution generatedImage
    // already exists (it is the export source). Once the view settles, the
    // visible band window (+25% margin) is cropped from it, rescaled to the
    // screen's physical density and white-point-matched on a background
    // thread, then overlaid in paint(). juce::Image is COW, so the captured
    // reference stays valid even if a re-generate swaps the member meanwhile.
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

        // Screen density, capped at the band's NATIVE resolution (rescaling
        // above it would waste memory for zero extra detail).
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
                hiResTile_ = juce::Image();   // dezoomed: drop the tile
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
        // Still covering the visible window at a comparable density? Keep it.
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
            [safe = juce::Component::SafePointer<ScoreGenTabComponent>(this),
             full = generatedImage, band, ts, stereo = generatedStereo,
             wp = previewWp_, epoch = tileEpoch_]
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
                if (! stereo && wp < 0.999 && wp > 0.0)
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
        // Content changed while this tile was being cut — it shows the OLD
        // image, never display it.
        if (epoch != tileEpoch_)
            return;
        if (! tile.isValid())
            return;
        if (previewZoom_ <= 1.001)
        {
            hiResTile_ = juce::Image();  // dezoomed while rendering
            return;
        }
        hiResTile_ = tile;
        tileFx0_ = ts.fx0;  tileFx1_ = ts.fx1;
        tileFy0_ = ts.fy0;  tileFy1_ = ts.fy1;
        tileDensity_ = ts.w / juce::jmax(1.0e-6, ts.fx1 - ts.fx0);
        repaint(previewArea);
    }

    //==========================================================================
    // Manual play-head scrub: click/drag on the preview image moves the score
    // reading head. While playing the engine snaps the live head; while stopped it
    // arms the start position (and shows the line) for the next PLAY.
    void mouseDown(const juce::MouseEvent& e) override
    {
        scrubbing = previewImage.isValid() && previewArea.contains(e.getPosition());
        if (! scrubbing) return;
        scrubTo(e);  // arm the play head at the clicked column first
        // When STOPPED, generate the flux live so the user hears the column under
        // the cursor (sustained tone that follows the drag). While PLAYING, the
        // scrubTo above already performed a live seek — leave the transport alone.
        if (auto* fs = boundChannel())
            if (! fs->isScorePlaying())
                scrubAuditioning = fs->uiBeginScoreScrub();
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (scrubbing) scrubTo(e); }
    void mouseUp  (const juce::MouseEvent&)      override
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
        if (fs == nullptr) return;
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

        loadButton.setBounds(pad, y, 110, ch);
        fileLabel.setBounds(pad + 110 + gap, y, colW - 110 - gap, ch);
        y += ch + gap + 8;

        {
            const int lblW = 150;
            wsLabel.setBounds(pad, y, lblW, ch);
            wsSlider.setBounds(pad + lblW + gap, y, colW - lblW - gap, ch);
            y += ch + gap + 4;
        }

        // Page / DPI / image format moved to the SETUP face (ScoreSetupPanel).
        {
            const int half = (colW - gap) / 2;
            stereoToggle.setBounds(pad, y, half, ch);
            multiResToggle.setBounds(pad + half + gap, y, half, ch);
            y += ch + gap + 4;
        }

        generateButton.setBounds(pad, y, colW, ch + 4); y += ch + 8;
        progressBar.setBounds(pad, y, colW, ch);        y += ch + gap;
        exportButton.setBounds(pad, y, colW, ch);
        y += ch + gap + 6;

        // ── Playback transport ─────────────────────────────────────────────
        // One compact bar: [▶/⏹] [loop][rev]  …  Speed (rotary). The Speed knob is
        // anchored to the right with its label hugging its left edge; the play +
        // loop pictograms are vertically centred against the taller knob block.
        {
            const int knobW    = 56;                 // speed knob widget (knob + value)
            const int knobDrwH = 42;                 // rotary draw square (above value)
            const int knobValH = 14;                 // value text-box (below knob)
            const int blockH   = knobDrwH + knobValH; // 56 — transport bar height
            const int btn      = 40;                 // play/stop square
            const int icon     = 34;                 // loop / inverse pictograms

            int x = pad;
            playStopButton.setBounds(x, y + (blockH - btn) / 2, btn, btn);   x += btn + gap;
            loopBtn.setBounds   (x, y + (blockH - icon) / 2, icon, icon);    x += icon + 4;
            reverseBtn.setBounds(x, y + (blockH - icon) / 2, icon, icon);    x += icon + gap;

            const int knobX  = pad + colW - knobW;
            speedSlider.setBounds(knobX, y, knobW, blockH);
            // Label vertically centred on the knob CIRCLE (value sits below it).
            speedLabel.setBounds(x, y + (knobDrwH - ch) / 2,
                                 juce::jmax(0, knobX - gap - x), ch);
            y += blockH + gap;
        }
        playHint.setBounds(pad, y, colW, ch); y += ch + gap + 4;
        const int colBottom = y;   // natural foot of the control column

        // ── Image EQ strip across the bottom (full width) ───────────────────
        // Pinned to the bottom at its preferred height, but it YIELDS upward no
        // further than the column foot: when the panel is condensed it shrinks in
        // place instead of sliding under the controls / log, so nothing overlaps.
        const int eqTop = juce::jmax(colBottom + gap,
                                     getHeight() - pad - ScoreEqComponent::kPreferredH);
        const int eqH   = juce::jmax(0, getHeight() - pad - eqTop);
        eqEditor.setBounds(pad, eqTop, getWidth() - 2 * pad, eqH);
        const int contentBottom = eqTop - gap;     // controls + preview live above

        // ── Log fills the gap above the EQ strip — hidden once squeezed out ──
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
     *  until an export starts, then an amber bar + travelling sheen while the
     *  image is encoded/written on the background thread (sibling of MIDI
     *  SCORE's ExportImageButton — same visual language, SCORE accent). */
    class ScoreExportButton : public juce::TextButton
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
    /** Square transport button for the score player. Draws a ▶ play triangle
     *  while stopped (green = "press to start") and a ⏹ stop square while playing
     *  (amber = "press to stop"). The playing state is pushed in via setPlaying()
     *  from refreshPlayButton(); the click action is wired through Button::onClick
     *  (togglePlay), so this is a momentary button — not a JUCE toggle. */
    class TransportPlayButton : public juce::Button
    {
    public:
        TransportPlayButton() : juce::Button("scorePlayStop") {}

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
                Icons::fillPath(g, Icons::stop(), inner, juce::Colour(0xffe0a24a)); // amber = stop
            else
                Icons::fillPath(g, Icons::play(), inner, juce::Colour(0xff66cc88)); // green = go
        }

    private:
        bool playing = false;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportPlayButton)
    };

    /** Compact pictogram toggle for the loop controls.
     *   • Loop    → a "racetrack" loop with a left-pointing arrow (repeat).
     *   • Inverse → the same loop, mirrored (right-pointing arrow) = play backward.
     *  Lights up amber when active. JUCE toggle — APVTS-bound (scoreLoop /
     *  scoreReverse); the processor maps the two params to the engine LoopMode. */
    class ScoreIconToggle : public juce::Button
    {
    public:
        enum class Glyph { Loop, Inverse };

        explicit ScoreIconToggle(Glyph g) : juce::Button("scoreLoopToggle"), glyph(g)
        {
            setClickingTogglesState(true);
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const bool on = getToggleState() && isEnabled();
            const juce::Colour accent(kAccentARGB); // amber (SCORE identity)

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
         *  gap — reads as "repeat". The RING is what must look centred; the arrow
         *  head overshoots the ring's top, so it is deliberately EXCLUDED from the
         *  centring measurement (its overshoot must not bias placement).
         *  @p reversed mirrors it horizontally (arrow points right) = play backward.
         *  Shared shape with the SAMPLER loop-mode pictograms (LoopModeButton). */
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

            // Left-pointing arrow head straddling the top gap (modest overshoot).
            const float aH    = radius * 0.85f;
            const float aTipX = gx0 - th * 0.25f;
            const float aBackX = gx1 + th * 0.25f;
            juce::Path arrow;
            arrow.addTriangle(aTipX, T, aBackX, T - aH, aBackX, T + aH);

            // Centre on the RING ONLY (stroke-expanded), so the arrow's overshoot
            // never shifts the glyph. The arrow rides along with the same offset.
            const auto ringBounds = loop.getBounds().expanded(th * 0.5f);
            const auto offset = r.getCentre() - ringBounds.getCentre();
            const auto move = juce::AffineTransform::translation(offset.x, offset.y);
            loop.applyTransform(move);
            arrow.applyTransform(move);

            // Inverse: mirror about r's vertical centre (x' = 2·cx − x).
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
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreIconToggle)
    };

    //==========================================================================
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
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, Sp3ctraTheme::kControlH);
        s.setRange(lo, hi, step);
        s.setValue(val, juce::dontSendNotification);
        addAndMakeVisible(s);
    }

    /** Rotary knob (accent-arc cadran from Sp3ctraLookAndFeel) with a value
     *  text-box below — matches the audio-panel knob style. */
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

    //==========================================================================
    void chooseWav()
    {
        // Import: seed from the last WAV directory used (never the session).
        const juce::File start = processor.sessions()->startDirFor(
            PathKeys::wavImport,
            loadedWav.existsAsFile()
                ? loadedWav.getParentDirectory()
                : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
        fileChooser = std::make_unique<juce::FileChooser>(
            "Select an audio file", start, "*.wav;*.aif;*.aiff;*.flac");
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe = juce::Component::SafePointer<ScoreGenTabComponent>(this)]
            (const juce::FileChooser& fc)
            {
                auto* self = safe.getComponent();
                if (self == nullptr) return;
                const auto f = fc.getResult();
                if (f.existsAsFile())
                {
                    self->processor.sessions()->rememberDirFor(PathKeys::wavImport, f);
                    self->setLoadedFile(f);
                }
            });
    }

    /** @p restoreRegion true when reloading the persisted WAV on construction:
     *  the extraction start stored in ScoreSettings is then kept instead of
     *  being reset to 0 like a user-picked new file. */
    void setLoadedFile(const juce::File& f, bool restoreRegion = false)
    {
        loadedWav = f;
        const auto info = scoregen::probeWav(f);
        if (info.ok)
        {
            fileLabel.setText(f.getFileName() + "  ("
                              + juce::String(info.durationSec, 2) + "s, "
                              + juce::String(info.sampleRate) + " Hz, "
                              + juce::String(info.numChannels) + " ch)",
                              juce::dontSendNotification);
            if (! restoreRegion)
                processor.getScoreSettings().startTimeSec = 0.0; // new file → from start
            waveform.setFile(f);
            if (restoreRegion)
                waveform.setStartSeconds(processor.getScoreSettings().startTimeSec);
            updateExportWindow();
            processor.stopScorePreview();      // drop any preview of the old file
            previewFile = juce::File(); previewStart = -1.0; previewLen = -1.0;
            previewButton.setEnabled(true);
            refreshPreviewButton();
            processor.setScoreWavPath(f.getFullPathName());   // persists in state
        }
        else
        {
            loadedWav = juce::File();
            fileLabel.setText("Unreadable: " + info.error, juce::dontSendNotification);
            waveform.setFile(juce::File());
            processor.stopScorePreview();
            previewButton.setEnabled(false);
            refreshPreviewButton();
            processor.setScoreWavPath({});
        }
    }

    /** Resize the export window (seconds-per-page) and sync the start offset.
     *  Page format 2 = Selection: the picker switches to FREE mode (both
     *  edges draggable) and shows the persisted selection length. */
    void updateExportWindow()
    {
        auto& s = processor.getScoreSettings();
        waveform.setFreeSelection(s.pageFormat == 2);
        waveform.setWindowSeconds(scoregen::pageWindowSeconds(s));
        // setWindowSeconds may have re-clamped the start (e.g. window grew).
        s.startTimeSec = waveform.getStartSeconds();
    }

    void startGenerate()
    {
        if (! loadedWav.existsAsFile())
        {
            fileLabel.setText("Load a WAV first", juce::dontSendNotification);
            return;
        }
        // Generation parameters come from the shared SETUP settings; the
        // frequency span follows the instrument's musical Tuning + Root Note +
        // Octaves (like LuxStral), overriding any stored min/max.
        ScoreSettings s = processor.getScoreSettings();
        double lo = 0.0, hi = 0.0;
        processor.getScoreFrequencyRange(lo, hi);
        s.minFreq = lo;
        s.maxFreq = hi;
        genMinFreq = lo;   // remembered so playback maps the band's LOG freq axis
        genMaxFreq = hi;   // 1:1 onto the synth's log oscillators (= CIS scan)
        genDpi     = s.printerDpi;   // DPI actually rendered → stamped into the export
        busy = true;
        progress = 0.0;
        generateButton.setEnabled(false);
        exportButton.setEnabled(false);
        if (auto* fs = boundChannel())
            fs->uiStopScore();
        processor.stopScorePreview();   // don't let a source preview run during render
        refreshPreviewButton();
        setTransportEnabled(false);
        refreshPlayButton();
        progressBar.setVisible(true);
        previewImage = juce::Image();
        logLabel.setText("Generating...", juce::dontSendNotification);
        repaint();
        job.start(loadedWav, s);
    }

    void onRenderFinished(scoregen::RenderResult r)
    {
        busy = false;
        progressBar.setVisible(false);
        generateButton.setEnabled(true);

        if (r.ok && r.image.isValid())
        {
            baseImage     = r.image;           // raw render (EQ is applied on top)
            spectroBand   = r.spectroBand;
            generatedStereo = r.stereo;        // authoritative: false if source was mono
            genDynRangeDB = processor.getScoreSettings().dynamicRangeDB;

            // Keep the EQ curve across regenerations; only rebuild its band grid
            // (which resets the curve) when the frequency span actually changes.
            if (genMinFreq != lastEqMinFreq || genMaxFreq != lastEqMaxFreq)
            {
                eqEditor.setRange(genMinFreq, genMaxFreq);
                lastEqMinFreq = genMinFreq;
                lastEqMaxFreq = genMaxFreq;
            }

            applyEqToImageAndReload();   // builds generatedImage (+EQ), preview, loads frames

            exportButton.setEnabled(true);
            logLabel.setText(r.log + "\n" + previewStats, juce::dontSendNotification);
            scrubHead = -1;             // fresh score → play head sits at the start
            setTransportEnabled(true);
            refreshPlayButton();
        }
        else
        {
            baseImage      = juce::Image();
            generatedImage = juce::Image();
            previewImage   = juce::Image();
            scrubHead      = -1;
            logLabel.setText("Failed: " + r.log, juce::dontSendNotification);
            if (auto* fs = boundChannel())
                fs->uiStopScore();
            setTransportEnabled(false);
            refreshPlayButton();
        }
        repaint();
    }

    // ── Image EQ: shape the GENERATED image (never the source WAV) ──────────
    // Each band row's darkness is shifted by gain(freq)/dynamicRange: a +dB boost
    // darkens (more energy), a −dB cut lightens (toward silence). Pure-white
    // silence is left untouched on boosts so the EQ never invents energy. The
    // result is the same image the reader/print path sees, so live ≡ export.
    void applyEqToImage()
    {
        if (! baseImage.isValid()) { generatedImage = juce::Image(); return; }
        if (eqEditor.isFlat() || genMinFreq <= 0.0 || genMaxFreq <= genMinFreq)
        {
            generatedImage = baseImage;   // share, no per-pixel work
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
            const double dShift = gdb / range;        // darkness shift

            // Per-row 256→256 LUT (shift constant along the row); grayscale so any
            // channel byte == the gray level and we rewrite all three.
            juce::uint8 lut[256];
            for (int v = 0; v < 256; ++v)
            {
                if (v >= 255 && dShift > 0.0) { lut[v] = 255; continue; } // silence stays silent
                const double dk = juce::jlimit(0.0, 1.0, (1.0 - v / 255.0) + dShift);
                lut[v] = (juce::uint8) juce::jlimit(0, 255, (int) std::lround((1.0 - dk) * 255.0));
            }
            juce::uint8* line = bmp.getLinePointer(yy);
            for (int xx = band.getX(); xx < band.getRight(); ++xx)
            {
                juce::uint8* p = line + xx * bmp.pixelStride;
                if (generatedStereo)
                {
                    // Colour composite: each channel carries an independent energy
                    // in the same inverted convention (255 = no energy), so the LUT
                    // applies per channel — preserves the red/blue stereo image.
                    p[0] = lut[p[0]];
                    p[1] = lut[p[1]];
                    p[2] = lut[p[2]];
                }
                else
                {
                    const juce::uint8 nv = lut[p[0]];
                    p[0] = p[1] = p[2] = nv;
                }
            }
        }
    }

    void applyEqToImageAndReload()
    {
        applyEqToImage();
        buildPreview();
        if (auto* fs = boundChannel())
        {
            const bool wasPlaying = fs->isScorePlaying();
            // Capture the live head BEFORE the reload — loadScoreFramesFromImage()
            // stops playback and zeroes it. The frame count is unchanged by an EQ
            // tweak (only pixel darkness changes), so the head stays valid.
            const int savedHead = wasPlaying ? fs->getScorePlayHead() : 0;
            fs->loadScoreFramesFromImage(generatedImage, spectroBand, genMinFreq, genMaxFreq,
                                         generatedStereo);
            if (wasPlaying)
            {
                fs->setScoreResumeHead(savedHead);   // resume where we were…
                fs->uiPlayScore();                   // …instead of snapping back to 0
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
        // Preview only the spectrogram band (what is actually played) — the
        // surrounding white page margins would just wash the thumbnail grey.
        juce::Rectangle<int> band =
            (spectroBand.getWidth() > 0 && spectroBand.getHeight() > 0)
                ? spectroBand.getIntersection(generatedImage.getBounds())
                : generatedImage.getBounds();
        if (band.isEmpty())
            band = generatedImage.getBounds();

        // Build a LARGE base preview, sized to the content (not the current window),
        // so paint() can scale it up to fill an enlarged window and still look crisp
        // — no regenerate needed. Capped on the longest side to bound the one-time
        // white-point pass below; never upscaled beyond the native band resolution.
        constexpr int kPreviewMaxPx = 1800;
        const double s = juce::jmin(1.0,
            (double) kPreviewMaxPx / juce::jmax(band.getWidth(), band.getHeight()));
        const int pw = juce::jmax(1, (int) (band.getWidth()  * s));
        const int ph = juce::jmax(1, (int) (band.getHeight() * s));

        // Faithful downscale of the band (same content as the export).
        juce::Image tmp = generatedImage.getClippedImage(band)
                              .rescaled(pw, ph, juce::Graphics::highResamplingQuality);

        // ── Display-only white-point lift ─────────────────────────────────────
        // The score uses a 60 dB LOG dynamic range (SCORE_USE_LOG_AMPLITUDE), so
        // the band legitimately carries a broad grey floor (faint energy). The
        // full-page export hides it behind big white margins; cropped to the band
        // it reads grey. We lift the typical floor toward white for the PREVIEW
        // ONLY — playback and export are untouched. The measured stats below make
        // the cause visible in the log.
        //
        // Stereo (colour) skips this greyscale lift — it writes Colour(v,v,v) and
        // would desaturate the red/blue image. The colour band is already legible.
        if (generatedStereo)
        {
            previewStats = "Stereo preview (left=red, right=blue)";
        }
        else
        {
            juce::Image::BitmapData bd(tmp, juce::Image::BitmapData::readWrite);
            juce::int64 sum = 0;
            int mn = 255, mx = 0;
            const int total = pw * ph;
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x)
                {
                    const int v = bd.getPixelPointer(x, y)[0];
                    sum += v; mn = juce::jmin(mn, v); mx = juce::jmax(mx, v);
                }
            const double meanN = total > 0 ? (double) sum / total / 255.0 : 1.0;
            const double wp = juce::jlimit(0.45, 0.98, meanN);   // white point
            previewWp_ = wp;   // the hi-res zoom tile applies the SAME lift
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x)
                {
                    double n = bd.getPixelPointer(x, y)[0] / 255.0;
                    n = juce::jlimit(0.0, 1.0, n / wp);           // floor → white
                    const auto v = (juce::uint8) (n * 255.0 + 0.5);
                    bd.setPixelColour(x, y, juce::Colour(v, v, v));
                }
            previewStats = "Band grey: min=" + juce::String(mn)
                         + " mean=" + juce::String((int) (meanN * 255.0))
                         + " max=" + juce::String(mx)
                         + "  (preview white-point=" + juce::String(wp, 2) + ")";
        }
        previewImage = tmp;
    }

    // Export runs on a BACKGROUND thread (encode + DPI stamp + write of the
    // already-generated image) so the UI stays live; the button shows an
    // amber bar with a travelling sheen meanwhile. Format (PNG/JPEG) comes
    // from the SETUP face ("scoreExportPng" state property). juce::Image is
    // COW, so the captured reference stays valid even if EQ/generate swaps
    // the member.
    void exportNow()
    {
        if (! generatedImage.isValid() || exportBusy_)
            return;
        const bool asPng = (bool) processor.getAPVTS().state
                               .getProperty("scoreExportPng", true);
        const juce::String ext = asPng ? "png" : "jpg";
        const juce::File dest = exportDir().getNonexistentChildFile(
            loadedWav.getFileNameWithoutExtension() + "_score", "." + ext, false);
        // Embed band + frequency range (+ stereo) so a re-load into the
        // SAMPLER reproduces the EXACT score calage.
        scoregen::SpectroCalibration cal;
        cal.band   = spectroBand;
        cal.minHz  = genMinFreq;
        cal.maxHz  = genMaxFreq;
        cal.stereo = generatedStereo;
        cal.valid  = spectroBand.getWidth() > 0
                  && spectroBand.getHeight() > 0
                  && genMaxFreq > genMinFreq
                  && genMinFreq > 0.0;

        // libjpeg hard-caps both dimensions at 65500 px — a FULL sheet can be
        // wider. Only PNG can hold it.
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
            [safe = juce::Component::SafePointer<ScoreGenTabComponent>(this),
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
    // Source-audio preview — play/pause the SELECTED region of the loaded WAV.
    static juce::String playGlyph()  { return juce::String(juce::CharPointer_UTF8("\xe2\x96\xb6")); } // ▶
    static juce::String pauseGlyph() { return juce::String(juce::CharPointer_UTF8("\xe2\x8f\xb8")); } // ⏸

    void togglePreview()
    {
        if (processor.isScorePreviewPlaying())
        {
            processor.pauseScorePreview();
        }
        else if (loadedWav.existsAsFile())
        {
            const auto& s    = processor.getScoreSettings();
            const double len = scoregen::pageWindowSeconds(s);
            const bool sameRegion = (loadedWav == previewFile)
                                  && std::abs(s.startTimeSec - previewStart) < 1e-6
                                  && std::abs(len - previewLen) < 1e-6;
            if (! (sameRegion && processor.resumeScorePreview()))
            {
                processor.startScorePreview(loadedWav, s.startTimeSec, len);
                previewFile  = loadedWav;
                previewStart = s.startTimeSec;
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
    // Loop-mode mapping now lives in the processor (scoreLoop/scoreReverse
    // params → LoopMode), so DAW automation and this page share one path.

    void togglePlay()
    {
        auto* fs = boundChannel();
        if (fs == nullptr) return;

        const bool play = ! fs->isScorePlaying();
        if (! play)
            scrubHead = -1;     // Stop returns to the start; clear the armed scrub

        // Route through the scorePlaying param so the DAW sees the transport;
        // the processor pushes speed/loop and starts/stops the engine. If the
        // param already matches (brief drift window), drive the engine directly.
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
        auto* fs = boundChannel();
        const bool playing = (fs != nullptr) && fs->isScorePlaying();
        playStopButton.setPlaying(playing);
    }

    /** Enable/disable the whole playback transport (play + loop/inverse + speed).
     *  Also greys the Speed label + value text so the control clearly reads as
     *  inactive until a score has been generated. */
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

        // Mirror a SETUP-side page-format change (A4/A3/FULL) into the
        // region-picker window length — the two faces share ScoreSettings
        // but only this page owns the waveform widget.
        if (shownPageFormat_ != processor.getScoreSettings().pageFormat)
        {
            shownPageFormat_ = processor.getScoreSettings().pageFormat;
            updateExportWindow();
        }

        // Reapply the EQ to the image once the user releases a node (deferred so
        // the heavy per-pixel pass + frame reload don't run on every drag tick).
        if (eqDirty && ! eqEditor.isDragging())
        {
            eqDirty = false;
            // Persist the curve — rides in apvts.state so it survives close.
            processor.getAPVTS().state
                .setProperty("scoreEqCurve", eqEditor.encodeState(), nullptr);
            if (baseImage.isValid())
                applyEqToImageAndReload();
        }

        // Keep the button in sync when LoopMode::NONE playback ends on its own.
        refreshPlayButton();
        // Animate the reading head while playing.
        auto* fs = boundChannel();
        // Mirror the scorePlaying param on the real engine state so the DAW
        // lane stays truthful when playback ends on its own (one-shot end) or
        // is stopped by internal reload flows. parameterChanged() ignores
        // writes that already match the engine state, so this cannot retrigger.
        if (auto* p = processor.getAPVTS().getParameter("scorePlaying"))
        {
            const float norm = (fs != nullptr && fs->isScorePlaying()) ? 1.0f : 0.0f;
            if (! juce::approximatelyEqual(p->getValue(), norm))
                p->setValueNotifyingHost(norm);
        }
        if (fs != nullptr && fs->isScorePlaying())
            repaint(previewArea);

        // Source-audio preview: drive the waveform playhead + button state.
        if (processor.isScorePreviewPlaying())
            waveform.setPlayhead(previewStart + processor.getScorePreviewPositionSec());
        else
            waveform.setPlayhead(-1.0);
        refreshPreviewButton();
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
        scrubHead        = -1;
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
        return processor.getScoreChannel(ModuleType::Score);
    }
    int boundScoreSlot_ = -1;

    Sp3ctraAudioProcessor& processor;

    juce::TextButton loadButton, generateButton;
    ScoreExportButton exportButton;
    bool exportBusy_ = false;          // one export at a time
    int  shownPageFormat_ = 0;         // mirrors SETUP edits into the region picker
    juce::Label      fileLabel, logLabel;

    // Writing Speed — essential generation control kept on the PLAY page.
    juce::Label  wsLabel;
    juce::Slider wsSlider;

    // Playback transport (CHAIN 1 — reuses the LuxSampler engine).
    TransportPlayButton playStopButton;
    ScoreIconToggle     loopBtn    { ScoreIconToggle::Glyph::Loop };
    ScoreIconToggle     reverseBtn { ScoreIconToggle::Glyph::Inverse };
    juce::Slider        speedSlider;
    juce::Label         speedLabel, playHint;
    // APVTS bindings (declared after the widgets → destroyed first).
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopAttach, reverseAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> speedAttach;
    int                 scrubHead { -1 }; // armed/displayed score head when stopped (-1 = none)
    bool                scrubbing { false };
    bool                scrubAuditioning { false }; // true while a stopped-score scrub plays audio
    juce::Rectangle<float> previewImgArea;  // where the preview image is blitted (for scrubbing)

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

    // Generation toggles + image EQ + waveform region picker (page format,
    // DPI and image format live on the SETUP face).
    juce::ToggleButton stereoToggle;          // generate L/R spectrograms (red=L, blue=R)
    juce::ToggleButton multiResToggle;        // multi-resolution STFT (encoder-only)
    ScoreEqComponent eqEditor { juce::Colour(kAccentARGB) };
    WaveformSelectorComponent waveform { juce::Colour(kAccentARGB) };
    juce::TextButton previewButton;          // audition the selected source region
    juce::File previewFile;                  // region last handed to the preview…
    double     previewStart { -1.0 };        // …so play resumes vs restarts correctly
    double     previewLen   { -1.0 };

    double progress { 0.0 };
    juce::ProgressBar progressBar { progress };

    juce::Rectangle<int> previewArea;
    juce::Rectangle<int> spectroBand;  // band region inside generatedImage (played part)
    double genMinFreq { 0.0 };         // freq bounds used at GENERATE (Hz)
    double genMaxFreq { 0.0 };
    bool   generatedStereo { false };  // last GENERATE used stereo (colour) mode
    juce::String previewStats;         // band grey diagnostics (shown in log)
    juce::Image generatedImage;   // full resolution (export source, EQ baked in)
    juce::Image baseImage;        // raw render before EQ (EQ re-applied from this)
    juce::Image previewImage;     // downscaled for painting (band crop)
    bool   eqDirty { false };     // EQ changed → reapply on next idle tick
    double genDynRangeDB { 50.0 };// dynamic range at GENERATE (EQ darkness math)
    double genDpi { 400.0 };      // printer DPI used at GENERATE → embedded on export
    double lastEqMinFreq { 0.0 }, lastEqMaxFreq { 0.0 };
    bool busy { false };

    juce::File loadedWav;
    std::unique_ptr<juce::FileChooser> fileChooser;

    ScoreGenJob job;

    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreGenTabComponent)
};
