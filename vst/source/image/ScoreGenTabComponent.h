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
#include <vector>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../IconPaths.h"
#include "ScoreGenThread.h"
#include "ScoreGenRenderer.h"
#include "ScoreEqComponent.h"
#include "WaveformSelectorComponent.h"

class ScoreGenTabComponent : public juce::Component,
                             private juce::Timer
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

        // ── Export ─────────────────────────────────────────────────────────
        exportPngButton.setButtonText("Export PNG");
        exportPngButton.onClick = [this] { chooseExport(true); };
        exportPngButton.setEnabled(false);
        addAndMakeVisible(exportPngButton);

        exportJpgButton.setButtonText("Export JPEG");
        exportJpgButton.onClick = [this] { chooseExport(false); };
        exportJpgButton.setEnabled(false);
        addAndMakeVisible(exportJpgButton);

        // ── Playback transport (CHAIN 1) ───────────────────────────────────
        playStopButton.setEnabled(false);
        playStopButton.setTooltip("Play / stop the generated score");
        playStopButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playStopButton);

        // Loop + Inverse as compact pictogram toggles (replace the old text Loop).
        loopBtn.setToggleState(true, juce::dontSendNotification);   // loop on by default
        loopBtn.setEnabled(false);
        loopBtn.setTooltip("Loop playback");
        loopBtn.onClick = [this] { applyTransportMode(); };
        addAndMakeVisible(loopBtn);

        reverseBtn.setToggleState(false, juce::dontSendNotification);
        reverseBtn.setEnabled(false);
        reverseBtn.setTooltip("Reverse (play the score backward)");
        reverseBtn.onClick = [this] { applyTransportMode(); };
        addAndMakeVisible(reverseBtn);

        initLabel(speedLabel, "Speed");
        initKnob(speedSlider, 0.1, 6.0, 0.01, 1.0, "x");
        speedSlider.setSkewFactorFromMidPoint(1.0); // 1.0× sits at the knob's centre (log feel)
        speedSlider.onValueChange = [this]
        {
            if (auto* fs = processor.getLuxSampler())
                fs->setScoreSpeed((float) speedSlider.getValue());
        };

        // Disabled (greyed) until a score is generated.
        setTransportEnabled(false);

        playHint.setText("Set LuxStral source = Sampler to hear the score.",
                         juce::dontSendNotification);
        playHint.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        playHint.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        addAndMakeVisible(playHint);

        // ── Format options (moved here from SETUP) ──────────────────────────
        initLabel(pageLabel, "Page");
        pageCombo.addItem("A4 Portrait", 1);
        pageCombo.addItem("A3 Landscape", 2);
        pageCombo.setSelectedId(processor.getScoreSettings().pageFormat == 1 ? 2 : 1,
                                juce::dontSendNotification);
        pageCombo.onChange = [this]
        {
            processor.getScoreSettings().pageFormat = (pageCombo.getSelectedId() == 2) ? 1 : 0;
            updateExportWindow();   // A4/A3 changes the page-window length
        };
        addAndMakeVisible(pageCombo);

        initLabel(dpiLabel, "DPI");
        for (int d : { 200, 300, 400, 600, 800 })
            dpiCombo.addItem(juce::String(d), d);
        dpiCombo.setSelectedId((int) processor.getScoreSettings().printerDpi,
                               juce::dontSendNotification);
        dpiCombo.onChange = [this]
        { processor.getScoreSettings().printerDpi = (double) juce::jmax(72, dpiCombo.getSelectedId()); };
        addAndMakeVisible(dpiCombo);

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

        // ── Waveform region picker (which part of the WAV to extract) ───────
        waveform.onStartChange = [this](double startSec)
        { processor.getScoreSettings().startTimeSec = startSec; };
        addAndMakeVisible(waveform);

        // ── Audition button: play/pause the SELECTED SOURCE region ──────────
        previewButton.setEnabled(false);
        previewButton.onClick = [this] { togglePreview(); };
        addAndMakeVisible(previewButton);
        refreshPreviewButton();

        // ── Image EQ (edits the generated image, never the source WAV) ──────
        eqEditor.onChange = [this] { eqDirty = true; };
        addAndMakeVisible(eqEditor);

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
        if (auto* fs = processor.getLuxSampler())
            fs->uiStopScore();
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
            // drawImage() modulates by the current fill's alpha; the 0.35 set
            // for the frame border above would otherwise blit the image at 35%
            // opacity over the dark frame (→ grey floor). Force full opacity.
            g.setOpacity(1.0f);
            g.drawImage(previewImage, imgArea);

            // ── Reading head: vertical line at the played column (live) or, when
            //    stopped, at the manually-placed scrub position. ───────────────
            auto* fs = processor.getLuxSampler();
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
        }
        else
        {
            g.setColour(juce::Colour(0xff55606f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
            g.drawText(busy ? "Generating..." : "Load a WAV and press GENERATE",
                       previewArea, juce::Justification::centred);
        }
    }

    /** Destination rectangle where the preview image is blitted inside previewArea.
     *  Keeps aspect ratio and scales UP to fill (so the image follows window
     *  resizes without needing a regenerate). */
    juce::Rectangle<float> previewImageBounds() const
    {
        if (! previewImage.isValid() || previewArea.isEmpty())
            return {};
        const juce::Rectangle<float> dest(
            (float) previewArea.getX() + 2, (float) previewArea.getY() + 2,
            (float) previewArea.getWidth() - 4, (float) previewArea.getHeight() - 4);
        const juce::RectanglePlacement place(juce::RectanglePlacement::centred); // may enlarge
        return place.appliedTo(
            juce::Rectangle<float>(0.f, 0.f,
                (float) previewImage.getWidth(), (float) previewImage.getHeight()),
            dest);
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
        if (auto* fs = processor.getLuxSampler())
            if (! fs->isScorePlaying())
                scrubAuditioning = fs->uiBeginScoreScrub();
    }
    void mouseDrag(const juce::MouseEvent& e) override { if (scrubbing) scrubTo(e); }
    void mouseUp  (const juce::MouseEvent&)      override
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

        // ── Format row: Page + DPI (moved here from SETUP) ──────────────────
        {
            const int half = (colW - gap) / 2;
            pageLabel.setBounds(pad, y, 40, ch);
            pageCombo.setBounds(pad + 40 + gap, y, half - 40 - gap, ch);
            dpiLabel.setBounds(pad + half + gap, y, 34, ch);
            dpiCombo.setBounds(pad + half + gap + 34 + gap, y, half - 34 - gap, ch);
            y += ch + gap + 4;
        }

        stereoToggle.setBounds(pad, y, colW, ch); y += ch + gap + 4;

        generateButton.setBounds(pad, y, colW, ch + 4); y += ch + 8;
        progressBar.setBounds(pad, y, colW, ch);        y += ch + gap;
        exportPngButton.setBounds(pad, y, (colW - gap) / 2, ch);
        exportJpgButton.setBounds(pad + (colW - gap) / 2 + gap, y, (colW - gap) / 2, ch);
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
    }

private:
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
     *  Lights up amber when active. JUCE toggle — read in applyTransportMode(). */
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
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 14);
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
        const juce::File start = startDir();
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
                    self->setLoadedFile(f);
            });
    }

    void setLoadedFile(const juce::File& f)
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
            processor.getScoreSettings().startTimeSec = 0.0;   // new file → from start
            waveform.setFile(f);
            updateExportWindow();
            processor.stopScorePreview();      // drop any preview of the old file
            previewFile = juce::File(); previewStart = -1.0; previewLen = -1.0;
            previewButton.setEnabled(true);
            refreshPreviewButton();
        }
        else
        {
            loadedWav = juce::File();
            fileLabel.setText("Unreadable: " + info.error, juce::dontSendNotification);
            waveform.setFile(juce::File());
            processor.stopScorePreview();
            previewButton.setEnabled(false);
            refreshPreviewButton();
        }
    }

    /** Resize the export window (seconds-per-page) and sync the start offset. */
    void updateExportWindow()
    {
        waveform.setWindowSeconds(scoregen::pageWindowSeconds(processor.getScoreSettings()));
        // setWindowSeconds may have re-clamped the start (e.g. window grew).
        processor.getScoreSettings().startTimeSec = waveform.getStartSeconds();
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
        exportPngButton.setEnabled(false);
        exportJpgButton.setEnabled(false);
        if (auto* fs = processor.getLuxSampler())
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

            exportPngButton.setEnabled(true);
            exportJpgButton.setEnabled(true);
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
            if (auto* fs = processor.getLuxSampler())
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
        if (auto* fs = processor.getLuxSampler())
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

    void chooseExport(bool asPng)
    {
        if (! generatedImage.isValid())
            return;
        const juce::String ext = asPng ? "png" : "jpg";
        const juce::File suggested = startDir()
            .getChildFile(loadedWav.getFileNameWithoutExtension() + "_score." + ext);
        fileChooser = std::make_unique<juce::FileChooser>(
            "Export Score Image", suggested, "*." + ext);
        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = juce::Component::SafePointer<ScoreGenTabComponent>(this), asPng, ext]
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
    /** Map the two loop pictograms to the engine's LoopMode. Reverse always loops
     *  (the engine has no one-shot reverse), so: reverse → INVERSE, else loop →
     *  LOOP, else NONE (play once forward). */
    LoopMode currentLoopMode() const
    {
        if (reverseBtn.getToggleState()) return LoopMode::INVERSE;
        if (loopBtn.getToggleState())    return LoopMode::LOOP;
        return LoopMode::NONE;
    }

    void applyTransportMode()
    {
        if (auto* fs = processor.getLuxSampler())
            fs->setScoreLoopMode(currentLoopMode());
    }

    void togglePlay()
    {
        auto* fs = processor.getLuxSampler();
        if (fs == nullptr) return;

        if (fs->isScorePlaying())
        {
            fs->uiStopScore();
            scrubHead = -1;     // Stop returns to the start; clear the armed scrub
        }
        else
        {
            // Push current transport settings before starting.
            fs->setScoreSpeed((float) speedSlider.getValue());
            fs->setScoreLoopMode(currentLoopMode());
            fs->uiPlayScore();
        }
        refreshPlayButton();
        repaint(previewArea);
    }

    void refreshPlayButton()
    {
        auto* fs = processor.getLuxSampler();
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
        // Reapply the EQ to the image once the user releases a node (deferred so
        // the heavy per-pixel pass + frame reload don't run on every drag tick).
        if (eqDirty && ! eqEditor.isDragging())
        {
            eqDirty = false;
            if (baseImage.isValid())
                applyEqToImageAndReload();
        }

        // Keep the button in sync when LoopMode::NONE playback ends on its own.
        refreshPlayButton();
        // Animate the reading head while playing.
        auto* fs = processor.getLuxSampler();
        if (fs != nullptr && fs->isScorePlaying())
            repaint(previewArea);

        // Source-audio preview: drive the waveform playhead + button state.
        if (processor.isScorePreviewPlaying())
            waveform.setPlayhead(previewStart + processor.getScorePreviewPositionSec());
        else
            waveform.setPlayhead(-1.0);
        refreshPreviewButton();
    }

    juce::File startDir() const
    {
        const auto out = processor.getSamplerOutputDir();
        if (out.isNotEmpty() && juce::File(out).isDirectory())
            return juce::File(out);
        if (loadedWav.existsAsFile())
            return loadedWav.getParentDirectory();
        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    }

    //==========================================================================
    Sp3ctraAudioProcessor& processor;

    juce::TextButton loadButton, generateButton, exportPngButton, exportJpgButton;
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
    int                 scrubHead { -1 }; // armed/displayed score head when stopped (-1 = none)
    bool                scrubbing { false };
    bool                scrubAuditioning { false }; // true while a stopped-score scrub plays audio
    juce::Rectangle<float> previewImgArea;  // where the preview image is blitted (for scrubbing)

    // Format options (moved from SETUP) + image EQ + waveform region picker.
    juce::Label      pageLabel, dpiLabel;
    juce::ComboBox   pageCombo, dpiCombo;
    juce::ToggleButton stereoToggle;          // generate L/R spectrograms (red=L, blue=R)
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreGenTabComponent)
};
