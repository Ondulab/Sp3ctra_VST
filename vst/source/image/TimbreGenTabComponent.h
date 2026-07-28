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
#include <cmath>
#include <vector>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../IconPaths.h"
#include "../licensing/ActivationDialog.h"
#include "TimbreGenRenderer.h"

class TimbreGenTabComponent : public juce::Component,
                              private juce::Timer,
                              private juce::ScrollBar::Listener
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

        // ── Export — format (PNG/JPEG) and DPI live on the SETUP face ───────
        exportButton.setButtonText("Export image");
        exportButton.setTooltip("Export the A4 page as an image (PNG/JPEG and "
                                "DPI in SETUP).");
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
        playStopButton.setTooltip("Play / stop the timbre page through the score player");
        playStopButton.onClick = [this] { togglePlay(); };
        addAndMakeVisible(playStopButton);

        loopBtn.setTooltip("Loop playback");
        addAndMakeVisible(loopBtn);
        loopAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "timbreLoop", loopBtn);

        reverseBtn.setTooltip("Reverse (play the timbre page backward)");
        addAndMakeVisible(reverseBtn);
        reverseAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor.getAPVTS(), "timbreReverse", reverseBtn);

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
            processor.getAPVTS(), "timbreSpeed", speedSlider);

        // Right-click MIDI Learn — TIMBRE's own transport (play/loop/reverse/speed).
        {
            auto& mm = processor.getMidiMap();
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, playStopButton, "timbrePlaying"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, loopBtn,        "timbreLoop"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, reverseBtn,     "timbreReverse"));
            learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, speedSlider,    "timbreSpeed"));
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
            // Zoomed, the image rect overflows the frame on every side — clip
            // so the band never bleeds over the surrounding controls.
            g.saveState();
            g.reduceClipRegion(previewArea.reduced(1));
            g.setOpacity(1.0f);
            g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            g.drawImage(previewImage, imgArea);

            // Hi-res tile of the visible window (cut from the FULL-resolution
            // page in the background once the zoom settles) — overlaid on the
            // 150 DPI base band.
            if (hiResTile_.isValid() && previewZoom_ > 1.001
                && tileFx1_ > tileFx0_ && tileFy1_ > tileFy0_)
            {
                g.drawImage(hiResTile_, juce::Rectangle<float>(
                    imgArea.getX() + (float) tileFx0_ * imgArea.getWidth(),
                    imgArea.getY() + (float) tileFy0_ * imgArea.getHeight(),
                    (float) (tileFx1_ - tileFx0_) * imgArea.getWidth(),
                    (float) (tileFy1_ - tileFy0_) * imgArea.getHeight()));
            }

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
            g.drawText("Enable at least one sound slot", previewArea,
                       juce::Justification::centred);
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
    // as the SCORE / MIDI SCORE pages.
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
    // Hi-res zoom tile. The preview band is a 150 DPI render, so zooming it is
    // upscale blur — but the FULL-resolution page (export DPI) either sits in
    // the fullImage cache or can be re-rendered cheaply (pure drawing, no
    // FFT). Once the view settles, the visible band window (+25% margin) is
    // cropped from it on a background thread — which also refreshes the
    // fullImage cache when it had to re-render (PLAY/EXPORT reuse it).
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
        if (previewZoom_ <= 1.001 || ! previewImage.isValid())
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

        double w = (ts.fx1 - ts.fx0) * imgArea.getWidth()  * scale;
        double h = (ts.fy1 - ts.fy0) * imgArea.getHeight() * scale;
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
        const bool needRender = fullDirty || ! fullImage.isValid();
        juce::Thread::launch(
            [safe = juce::Component::SafePointer<TimbreGenTabComponent>(this),
             slotsCopy = slots, s = settingsForDpi(pageSettings.printerDpi),
             page = fullImage, band = fullBand, needRender, ts,
             epoch = tileEpoch_]() mutable
            {
                if (needRender)
                {
                    const auto r = timbregen::renderTimbrePage(slotsCopy, s);
                    if (! (r.ok && r.image.isValid()))
                    {
                        juce::MessageManager::callAsync([safe]
                        {
                            if (auto* self = safe.getComponent())
                                self->tileRenderBusy_ = false;
                        });
                        return;
                    }
                    page = r.image;
                    band = r.spectroBand;
                }
                juce::Rectangle<int> b =
                    (band.getWidth() > 0 && band.getHeight() > 0)
                        ? band.getIntersection(page.getBounds())
                        : page.getBounds();
                if (b.isEmpty())
                    b = page.getBounds();
                const juce::Rectangle<int> crop(
                    b.getX() + (int) std::floor(ts.fx0 * b.getWidth()),
                    b.getY() + (int) std::floor(ts.fy0 * b.getHeight()),
                    juce::jmax(1, (int) std::ceil((ts.fx1 - ts.fx0) * b.getWidth())),
                    juce::jmax(1, (int) std::ceil((ts.fy1 - ts.fy0) * b.getHeight())));
                const auto src = page.getClippedImage(crop.getIntersection(page.getBounds()));
                // Never upscale past the native resolution — no extra detail.
                juce::Image tile = src.rescaled(juce::jmin(ts.w, src.getWidth()),
                                                juce::jmin(ts.h, src.getHeight()),
                                                juce::Graphics::highResamplingQuality);
                juce::MessageManager::callAsync(
                    [safe, tile = std::move(tile), ts, epoch,
                     freshPage = needRender ? page : juce::Image(),
                     freshBand = band, lo = s.minFreq, hi = s.maxFreq]() mutable
                    {
                        if (auto* self = safe.getComponent())
                            self->applyTileRender(tile, ts, epoch,
                                                  freshPage, freshBand, lo, hi);
                    });
            });
    }

    void applyTileRender(const juce::Image& tile, const TileSpec& ts, int epoch,
                         const juce::Image& freshPage,
                         juce::Rectangle<int> freshBand, double lo, double hi)
    {
        tileRenderBusy_ = false;
        // Content changed while this tile was being cut — never show (or
        // cache) the old bytes.
        if (epoch != tileEpoch_)
            return;
        // Side product: the tile render also produced a fresh full page —
        // adopt it so the next PLAY/EXPORT reuses it instead of re-rendering.
        if (freshPage.isValid())
        {
            fullImage   = freshPage;
            fullBand    = freshBand;
            fullMinFreq = lo;
            fullMaxFreq = hi;
            fullDirty   = false;
        }
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
    // Preview interactions: click = scrub (when our frames are loaded, like the
    // SCORE page); double-click = select the sound slot under the cursor.
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
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        // Zoomed, the virtual image rect overflows the frame: only clicks
        // INSIDE the frame may select a slot.
        if (! previewArea.contains(e.getPosition())) return;
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

        // Switch (~38 px) + 8 px gap + "Active" at 11 px needs ~84 px to render untruncated.
        const int togW = 84;
        presetLabel.setBounds(pad, y, 60, ch);
        presetCombo.setBounds(pad + 60 + gap, y, colW - 60 - gap - togW - gap, ch);
        activeToggle.setBounds(pad + colW - togW, y, togW, ch);
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

        // Format / DPI live on the SETUP face — one button here.
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
        layoutPreviewScrollbars();
        clampPreviewView();
        updatePreviewScrollbars();
    }

private:
    //==========================================================================
    /** Export button that shows its running job: a plain themed TextButton
     *  until an export starts, then a terracotta bar + travelling sheen while
     *  the page is rendered/encoded/written on the background thread (sibling
     *  of the SCORE / MIDI SCORE export buttons — same visual language). */
    class TimbreExportButton : public juce::TextButton
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
        // The page changed: the hi-res zoom tile shows the OLD content — drop
        // it, the timer re-cuts the visible window. The epoch bump also voids
        // any tile still in flight on the worker.
        hiResTile_ = juce::Image();
        tileFx0_ = tileFx1_ = tileFy0_ = tileFy1_ = 0.0;
        ++tileEpoch_;
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
    // Export runs on a BACKGROUND thread (render-if-dirty + encode + DPI stamp
    // + write) so the UI stays live; the button shows a terracotta bar with a
    // travelling sheen meanwhile. Format (PNG/JPEG) comes from the SETUP face.
    void exportNow()
    {
        if (LicenseGate::blockIfDemo(this, "Export image"))
            return;
        if (exportBusy_)
            return;
        const bool asPng = exportAsPng_;
        const juce::String ext = asPng ? "png" : "jpg";
        const juce::File dest = exportDir().getNonexistentChildFile(
            "timbres_A4", "." + ext, false);

        exportBusy_ = true;
        exportButton.setButtonText(juce::String::fromUTF8("Writing…"));
        exportButton.setJobState(true, 1.f, true);

        juce::Thread::launch(
            [safe = juce::Component::SafePointer<TimbreGenTabComponent>(this),
             slotsCopy = slots, s = settingsForDpi(pageSettings.printerDpi),
             page = fullImage, needRender = (fullDirty || ! fullImage.isValid()),
             dest, asPng, dpi = pageSettings.printerDpi,
             session = processor.sessions()->sessionName()]() mutable
            {
                juce::String msg;
                if (needRender)
                {
                    const auto r = timbregen::renderTimbrePage(slotsCopy, s);
                    if (r.ok && r.image.isValid())
                        page = r.image;
                    else
                        msg = "Failed: " + r.log;
                }
                if (msg.isEmpty())
                    msg = scoregen::exportImage(page, dest, asPng, dpi)
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
    void togglePlay()
    {
        auto* fs = boundChannel();
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

        // Route through TIMBRE's own play param so the DAW sees the transport;
        // the processor pushes speed/loop and starts/stops its slot.
        if (auto* p = processor.getAPVTS().getParameter("timbrePlaying"))
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
        const float fx = juce::jlimit(0.f, 1.f,
            ((float) e.position.x - area.getX()) / area.getWidth());
        scrubHead = juce::jlimit(0, n - 1, (int) (fx * (float) n));
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

        // Running export: animate the sheen on the button.
        if (exportBusy_)
            exportButton.setJobState(true, 1.f, true);

        // Debounced live regeneration (drag-friendly).
        if (previewDirty
            && juce::Time::getMillisecondCounter() - lastEditMs > 200)
        {
            regenPreview();
            persistState();
        }

        auto* fs = boundChannel();
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

        // Mirror TIMBRE's play param on the real engine state (DAW lane truthful
        // when a one-shot ends / an internal reload stops playback).
        if (auto* p = processor.getAPVTS().getParameter("timbrePlaying"))
        {
            const float norm = (fs != nullptr && fs->isScorePlaying() && framesAreOurs)
                                 ? 1.0f : 0.0f;
            if (! juce::approximatelyEqual(p->getValue(), norm))
                p->setValueNotifyingHost(norm);
        }
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
        root->setProperty("png",    exportAsPng_);
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
        if (o->hasProperty("png"))    exportAsPng_              = (bool)   o->getProperty("png");

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

    //==========================================================================
    // SETUP face access (TimbreSetupPanel) — export preferences. Export-only:
    // the preview never changes; a DPI change re-renders the full page (and
    // the zoom tile) at the new resolution on the next use.
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
        fullDirty = true;                 // full page + tile follow on next use
        hiResTile_ = juce::Image();
        ++tileEpoch_;
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
        return processor.getScoreChannel(ModuleType::Timbre);
    }
    int boundScoreSlot_ = -1;

    Sp3ctraAudioProcessor& processor;

    std::array<timbregen::TimbreSlotParams, timbregen::kNumSlots> slots;
    timbregen::TimbrePageSettings pageSettings;
    int selectedSlot = 0;

    std::array<std::unique_ptr<SlotTab>, timbregen::kNumSlots> slotTabs;

    juce::Label    presetLabel, noteLabel, partialsLabel, slopeLabel, oddLabel,
                   inharmLabel, combLabel, combPosLabel, attackLabel, decayLabel,
                   hfDampLabel, levelLabel, wsLabel, lineLabel,
                   speedLabel, playHint, logLabel;
    juce::ComboBox presetCombo;
    juce::ToggleButton activeToggle, labelsToggle;
    TimbreIconToggle   loopBtn    { TimbreIconToggle::Glyph::Loop };
    TimbreIconToggle   reverseBtn { TimbreIconToggle::Glyph::Inverse };
    juce::Slider   noteSlider, partialsSlider, slopeSlider, oddSlider, inharmSlider,
                   combSlider, combPosSlider, attackSlider, decaySlider, hfDampSlider,
                   levelSlider, wsSlider, lineSlider, speedSlider;
    TimbreExportButton exportButton;
    bool exportAsPng_ = true;            // SETUP face: PNG (true) / JPEG
    bool exportBusy_  = false;           // one export at a time
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

    // Preview zoom state — 1 = whole band fitted; Cx/Cy = image fraction at
    // the frame centre while zoomed (wheel/pinch zoom, scrollbars navigate).
    double previewZoom_ = 1.0, previewCx_ = 0.5, previewCy_ = 0.5;
    juce::ScrollBar previewHScroll { false }, previewVScroll { true };

    // Hi-res tile of the visible band window while zoomed (see desiredTileSpec).
    juce::Image  hiResTile_;
    double       tileFx0_ = 0.0, tileFx1_ = 0.0, tileFy0_ = 0.0, tileFy1_ = 0.0;
    double       tileDensity_ = 0.0;    // tile px per band fraction (renew test)
    bool         tileRenderBusy_ = false;
    int          tileEpoch_ = 0;        // bumped on content change → voids in-flight tiles
    juce::uint32 lastViewChangeMs_ = 0;

    bool previewDirty = false, fullDirty = true, stateDirty = false;
    juce::uint32 lastEditMs = 0;

    bool framesAreOurs = false;          // our page currently sits in the score player
    int  loadedFrameCount = 0;
    int  scrubHead = -1;
    bool scrubbing = false, scrubAuditioning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimbreGenTabComponent)
};
