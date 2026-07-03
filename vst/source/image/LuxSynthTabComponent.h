/**
 * @file LuxSynthTabComponent.h
 * @brief LUXSYNTH module — the WHOLE module UI on one page, laid out in 2 columns
 *        (same charter as LuxStralTabComponent).
 *
 *   ┌ Volume ════════════════════ ┐   ┌ LUXSYNTH -- FFT ADDITIVE ┐
 *   │ LEFT                │ RIGHT  │
 *   │ ┌ IMAGE ─────────┐  │ ┌ ANALYSIS ─────┐
 *   │ │ Negative/DC    │  │ │ BLOB DETECTION │
 *   │ │ Gamma/Contrast │  │ │  Ampl/Pix/...  │
 *   │ └────────────────┘  │ │ FFT            │
 *   │ ┌ OSCILLATORS ───┐  │ │  Bins/Smooth   │
 *   │ │ VOLUME ADSR    │  │ └───────────────┘
 *   │ │  [ env curve ] │  │ ┌ FILTER & LFO ─┐
 *   │ │ Oscillators    │  │ │ FILTER ADSR    │
 *   │ └────────────────┘  │ │  [ env curve ] │
 *   │                     │ │ Cutoff/Depth/..│
 *   │                     │ └───────────────┘
 *
 * The page was previously split across two stacked components (image page +
 * AudioSynthPanel); the module is now a single self-contained component.
 *
 * Signal flow: IMAGE conditioning → OSCILLATORS (FFT-additive voice + volume
 * ADSR) on the left; BLOB/FFT ANALYSIS + FILTER (ADSR + LFO) on the right.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/AudioPanelWidgets.h"      // AudioPanelLayout + AudioPanelUI (shared look)
#include "../ui/EnvelopeEditorComponent.h"
#include "VisualizerMode.h"

class LuxSynthTabComponent : public juce::Component
{
public:
    explicit LuxSynthTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        auto& apvts = p.getAPVTS();

        // ── Source selector — RETIRED (source follows chain placement) ──────
        // The combo + attachment are kept (not made visible) so the param
        // plumbing survives for the future modular-chain routing.
        sourceCombo.addItem("Chain 1", 1);
        sourceCombo.addItem("Chain 2", 2);
        sourceAttach.reset(new CmbAttach(apvts, "luxsynthSource", sourceCombo));

        // ── Master Volume (top of left column) ────────────────────────────
        initLabel(volumeLabel, "Volume");
        initSlider(volumeSlider);
        volumeAttach.reset(new SldAttach(apvts, "luxsynthVolume", volumeSlider));

        // ── IMAGE — conditioning (label is the toggle text itself) ──────────
        negativeToggle.setButtonText("Negative");
        addAndMakeVisible(negativeToggle);
        negativeAttach.reset(new BtnAttach(apvts, "luxsynthInversion", negativeToggle));

        dcBlockToggle.setButtonText("DC Blocking");
        addAndMakeVisible(dcBlockToggle);
        dcBlockAttach.reset(new BtnAttach(apvts, "luxsynthAcRemoval", dcBlockToggle));

        initLabel(gammaLabel, "Gamma");
        initSlider(gammaSlider);
        gammaAttach.reset(new SldAttach(apvts, "luxsynthGammaValue", gammaSlider));

        initLabel(contrastMinLabel, "Contrast Min");
        initSlider(contrastMinSlider);
        contrastMinAttach.reset(new SldAttach(apvts, "samplerContrastMin", contrastMinSlider));

        // ── OSCILLATORS — additive voice: volume ADSR + oscillator count ─────
        volEnv = std::make_unique<EnvelopeEditorComponent>(
            apvts, juce::Colour(0xff66ccaa),
            "luxsynthAttackMs", "luxsynthDecayMs", "luxsynthSustainLevel", "luxsynthReleaseMs",
            "luxsynthAttackCurve", "luxsynthDecayCurve", "luxsynthReleaseCurve");
        addAndMakeVisible(*volEnv);

        AudioPanelUI::initKnob(numOscSlider);
        addAndMakeVisible(numOscSlider);
        numOscAttach.reset(new SldAttach(apvts, "luxsynthNumOscillators", numOscSlider));

        // ── ANALYSIS — blob detection (LuxSynth-only params) ────────────────
        // Amplitude threshold — normalised brightness [0..1]: pixels brighter
        // than this fraction of max amplitude are considered active.
        initLabel(blobThreshLabel, "Ampl. Thr.");
        initSlider(blobThreshSlider);
        blobThreshAttach.reset(new SldAttach(apvts, "lxBlobThreshold", blobThreshSlider));
        // Pixel threshold — minimum blob span in CIS pixels (width filter).
        initLabel(blobMinWidthLabel, "Pix. Thr.");
        initSlider(blobMinWidthSlider);
        blobMinWidthAttach.reset(new SldAttach(apvts, "lxBlobMinWidth", blobMinWidthSlider));
        initLabel(blobMergeGapLabel, "Merge Gap");
        initSlider(blobMergeGapSlider);
        blobMergeGapAttach.reset(new SldAttach(apvts, "lxBlobMergeGap", blobMergeGapSlider));
        // Color Split — 0% = gap-based merge only, 100% = any colour divergence
        // breaks a blob (independent of Merge Gap).
        initLabel(blobColorSplitLabel, "Color Split");
        initSlider(blobColorSplitSlider);
        blobColorSplitAttach.reset(new SldAttach(apvts, "lxBlobColorSplit", blobColorSplitSlider));

        // ── ANALYSIS — FFT (bins map 1:1 to additive oscillators) ───────────
        initLabel(fftBinsLabel, "FFT Bins");
        addAndMakeVisible(fftBinsCombo);
        fftBinsCombo.addItem("32  - fast",    1);
        fftBinsCombo.addItem("64",            2);
        fftBinsCombo.addItem("128 - default", 3);
        fftBinsCombo.addItem("256 - quality", 4);
        fftBinsAttach.reset(new CmbAttach(apvts, "lxFftBins", fftBinsCombo));

        // Temporal averaging of FFT magnitudes: 0 = reactive, 1 = smooth.
        initLabel(fftSmoothingLabel, "Smoothing");
        initSlider(fftSmoothingSlider);
        fftSmoothingAttach.reset(new SldAttach(apvts, "lxFftSmoothing", fftSmoothingSlider));

        // ── FILTER & LFO — filter ADSR + modulation knobs ───────────────────
        fltEnv = std::make_unique<EnvelopeEditorComponent>(
            apvts, juce::Colour(0xffcc88cc),
            "luxsynthFilterAttackMs", "luxsynthFilterDecayMs", "luxsynthFilterSustain", "luxsynthFilterReleaseMs",
            "luxsynthFilterAttackCurve", "luxsynthFilterDecayCurve", "luxsynthFilterReleaseCurve");
        addAndMakeVisible(*fltEnv);

        AudioPanelUI::initKnob(fltCutoffSlider, " Hz");
        addAndMakeVisible(fltCutoffSlider);
        fltCutoffAttach.reset(new SldAttach(apvts, "luxsynthFilterCutoff", fltCutoffSlider));

        AudioPanelUI::initKnob(fltDepthSlider);
        addAndMakeVisible(fltDepthSlider);
        fltDepthAttach.reset(new SldAttach(apvts, "luxsynthFilterEnvDepth", fltDepthSlider));

        AudioPanelUI::initKnob(lfoRateSlider, " Hz");
        addAndMakeVisible(lfoRateSlider);
        lfoRateAttach.reset(new SldAttach(apvts, "luxsynthLfoRate", lfoRateSlider));

        AudioPanelUI::initKnob(lfoDepthSlider);
        addAndMakeVisible(lfoDepthSlider);
        lfoDepthAttach.reset(new SldAttach(apvts, "luxsynthLfoDepth", lfoDepthSlider));
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const auto L = computeGeom(getWidth());
        using namespace AudioPanelUI;

        // ── Master Volume strip ─────────────────────────────────────────────
        {
            const auto r = L.volStrip.toFloat();
            g.setColour(juce::Colour(0xff182636));
            g.fillRoundedRectangle(r, 4.f);
            g.setColour(juce::Colour(0xff2c4055));
            g.drawRoundedRectangle(r, 4.f, 1.f);
        }

        // ── Module identity chip — mirrors the LuxStral engine chip ─────────
        {
            const juce::Colour tagCol(0xffb07af0);   // LUXSYNTH rack accent
            const auto chip = L.moduleChip.toFloat();
            g.setColour(tagCol.withAlpha(0.12f));
            g.fillRoundedRectangle(chip, 4.f);
            g.setColour(tagCol.withAlpha(0.55f));
            g.drawRoundedRectangle(chip, 4.f, 1.f);
            g.setColour(tagCol);
            g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            g.drawText("LUXSYNTH  --  FFT ADDITIVE", L.moduleChip,
                       juce::Justification::centred);
        }

        // ── LEFT: IMAGE ─────────────────────────────────────────────────────
        drawSectionBg(g, L.imgBg.getX(), L.imgBg.getY(), L.imgBg.getWidth(), L.imgBg.getHeight());
        drawBadge(g, L.imgBadge.getX(), L.imgBadge.getY(), L.imgBadge.getWidth(),
                  0xff20303c, 0xff7aade0, "IMAGE");

        // ── LEFT: OSCILLATORS ───────────────────────────────────────────────
        drawSectionBg(g, L.oscBg.getX(), L.oscBg.getY(), L.oscBg.getWidth(), L.oscBg.getHeight());
        drawBadge(g, L.oscBadge.getX(), L.oscBadge.getY(), L.oscBadge.getWidth(),
                  0xff1a3a3a, 0xff66ccaa, "OSCILLATORS");
        drawEnvCaption(g, L.oscBadge.getX() + kSecInsetX, L.oscCaptionY,
                       L.oscBadge.getWidth() - 2 * kSecInsetX, 0xff66ccaa, "VOLUME  ADSR");
        drawKnobLabel(g, L.oscGridX, L.oscGridW, L.oscGridY, 0, "Oscill.");

        // ── RIGHT: ANALYSIS (blob detection + FFT, dual-caption card) ───────
        drawSectionBg(g, L.anaBg.getX(), L.anaBg.getY(), L.anaBg.getWidth(), L.anaBg.getHeight());
        drawBadge(g, L.anaBadge.getX(), L.anaBadge.getY(), L.anaBadge.getWidth(),
                  0xff3a2620, 0xffd07040, "ANALYSIS");
        {
            const int adx = L.rightX + kSecInsetX;
            const int adw = L.colW - 2 * kSecInsetX;
            drawEnvCaption(g, adx, L.anaBlobCaptionY, adw, 0xffd07040, "BLOB DETECTION");
            g.setColour(juce::Colour(0xff2a2a40));
            g.fillRect(adx, L.anaDividerY, adw, 1);
            drawEnvCaption(g, adx, L.anaFftCaptionY, adw, 0xffe06868,
                           "FFT  --  1 bin -> 1 oscillator");
        }

        // ── RIGHT: FILTER & LFO ─────────────────────────────────────────────
        drawSectionBg(g, L.fltBg.getX(), L.fltBg.getY(), L.fltBg.getWidth(), L.fltBg.getHeight());
        drawBadge(g, L.fltBadge.getX(), L.fltBadge.getY(), L.fltBadge.getWidth(),
                  0xff32203a, 0xffcc88cc, "FILTER & LFO");
        drawEnvCaption(g, L.fltBadge.getX() + kSecInsetX, L.fltCaptionY,
                       L.fltBadge.getWidth() - 2 * kSecInsetX, 0xffcc88cc, "FILTER  ADSR");
        {
            // Short labels — half-column knob cells are ~48 px wide.
            static const char* const lbls[] = { "Cutoff", "Env Amt", "LFO Rate", "LFO Amt" };
            for (int i = 0; i < 4; ++i) drawKnobLabel(g, L.fltGridX, L.fltGridW, L.fltGridY, i, lbls[i]);
        }
    }

    //==========================================================================
    void resized() override
    {
        const auto L = computeGeom(getWidth());

        // LEFT
        volumeLabel.setBounds(L.volLabel);
        volumeSlider.setBounds(L.volSlider);

        negativeToggle.setBounds(L.negToggle);
        dcBlockToggle.setBounds(L.dcToggle);
        gammaLabel.setBounds(L.gammaLabel);          gammaSlider.setBounds(L.gammaSlider);
        contrastMinLabel.setBounds(L.contrastLabel); contrastMinSlider.setBounds(L.contrastSlider);

        volEnv->setBounds(L.oscEnv);
        AudioPanelUI::placeKnob(numOscSlider, L.oscGridX, L.oscGridW, L.oscGridY, 0);

        // RIGHT
        blobThreshLabel.setBounds(L.anaLabel[0]);     blobThreshSlider.setBounds(L.anaCtrl[0]);
        blobMinWidthLabel.setBounds(L.anaLabel[1]);   blobMinWidthSlider.setBounds(L.anaCtrl[1]);
        blobMergeGapLabel.setBounds(L.anaLabel[2]);   blobMergeGapSlider.setBounds(L.anaCtrl[2]);
        blobColorSplitLabel.setBounds(L.anaLabel[3]); blobColorSplitSlider.setBounds(L.anaCtrl[3]);
        fftBinsLabel.setBounds(L.anaLabel[4]);        fftBinsCombo.setBounds(L.anaCtrl[4]);
        fftSmoothingLabel.setBounds(L.anaLabel[5]);   fftSmoothingSlider.setBounds(L.anaCtrl[5]);

        fltEnv->setBounds(L.fltEnv);
        AudioPanelUI::placeKnob(fltCutoffSlider, L.fltGridX, L.fltGridW, L.fltGridY, 0);
        AudioPanelUI::placeKnob(fltDepthSlider,  L.fltGridX, L.fltGridW, L.fltGridY, 1);
        AudioPanelUI::placeKnob(lfoRateSlider,   L.fltGridX, L.fltGridW, L.fltGridY, 2);
        AudioPanelUI::placeKnob(lfoDepthSlider,  L.fltGridX, L.fltGridW, L.fltGridY, 3);
    }

private:
    [[maybe_unused]] Sp3ctraAudioProcessor& processor;

    // ── Vertical layout tokens (same charter as LuxStralTabComponent) ───────
    static constexpr int kTopPad    = 6;
    static constexpr int kColGap    = 16;                          // between columns
    static constexpr int kHeaderH   = 30;                          // Volume strip / chip
    static constexpr int kBadgeH    = Sp3ctraTheme::kSectionH;     // 24
    static constexpr int kBadgeGap  = Sp3ctraTheme::kSectionGap;   // 4
    static constexpr int kRowH      = Sp3ctraTheme::kControlH;     // 22
    static constexpr int kRowGap    = Sp3ctraTheme::kRowGap;       // 4
    static constexpr int kSecGapV   = 10;                          // between sections
    static constexpr int kSecPadB   = 8;                           // section bottom pad
    static constexpr int kSecInsetX = 8;                           // content inset
    static constexpr int kLabelW    = 96;                          // slider label column
    static constexpr int kDivGap    = 10;                          // blob/FFT divider gap
    static constexpr int kCapH      = AudioPanelLayout::kEnvCaptionH; // 13
    static constexpr int kKnobH     = AudioPanelLayout::kKnobCellH;   // 71
    static constexpr int kEnvH      = AudioPanelLayout::kEnvH;        // 124
    static constexpr int kEnvGap    = AudioPanelLayout::kEnvGap;      // 10

    static constexpr int kImgSecH = kBadgeH + kBadgeGap + (3 * kRowH + 2 * kRowGap) + kSecPadB;      // 110
    static constexpr int kOscSecH = kBadgeH + kBadgeGap + kCapH + kEnvH + kEnvGap + kKnobH + kSecPadB; // 254
    static constexpr int kAnaSecH = kBadgeH + kBadgeGap + kCapH + (4 * kRowH + 3 * kRowGap) + kDivGap
                                  + kCapH + (2 * kRowH + kRowGap) + kSecPadB;                        // 220
    static constexpr int kFltSecH = kBadgeH + kBadgeGap + kCapH + kEnvH + kEnvGap + kKnobH + kSecPadB; // 254

    static constexpr int kLeftColH  = kHeaderH + kSecGapV + kImgSecH + kSecGapV + kOscSecH;          // 414
    static constexpr int kRightColH = kHeaderH + kSecGapV + kAnaSecH + kSecGapV + kFltSecH;          // 524

public:
    /** Natural content height — the taller of the two columns. */
    static constexpr int kPreferredH =
        kTopPad + (kLeftColH > kRightColH ? kLeftColH : kRightColH) + kSecPadB;

private:
    // ── Resolved layout (single source for paint + resized) ─────────────────
    struct Geom
    {
        int gx = 0, gw = 0, colW = 0, leftX = 0, rightX = 0;
        // left
        juce::Rectangle<int> volStrip, volLabel, volSlider;
        juce::Rectangle<int> imgBg, imgBadge, negToggle, dcToggle,
                             gammaLabel, gammaSlider, contrastLabel, contrastSlider;
        juce::Rectangle<int> oscBg, oscBadge, oscEnv;
        int oscCaptionY = 0, oscGridX = 0, oscGridW = 0, oscGridY = 0;
        // right
        juce::Rectangle<int> moduleChip;
        juce::Rectangle<int> anaBg, anaBadge;
        int anaBlobCaptionY = 0, anaDividerY = 0, anaFftCaptionY = 0;
        juce::Rectangle<int> anaLabel[6], anaCtrl[6];   // 4 blob rows + 2 FFT rows
        juce::Rectangle<int> fltBg, fltBadge, fltEnv;
        int fltCaptionY = 0, fltGridX = 0, fltGridW = 0, fltGridY = 0;
    };

    Geom computeGeom(int w) const
    {
        Geom L{};
        const int gx     = Sp3ctraTheme::kHPad;
        const int gw     = w - 2 * Sp3ctraTheme::kHPad;
        const int colW   = (gw - kColGap) / 2;
        const int leftX  = gx;
        const int rightX = gx + colW + kColGap;
        const int gap    = Sp3ctraTheme::kGap;
        L.gx = gx; L.gw = gw; L.colW = colW; L.leftX = leftX; L.rightX = rightX;

        // ── LEFT COLUMN ─────────────────────────────────────────────────────
        {
            const int cx = leftX + kSecInsetX;
            const int cw = colW - 2 * kSecInsetX;
            int y = kTopPad;

            // Volume strip
            L.volStrip = { leftX - 2, y, colW + 4, kHeaderH };
            {
                const int vy = y + (kHeaderH - kRowH) / 2;
                L.volLabel  = { cx, vy, kLabelW, kRowH };
                L.volSlider = { cx + kLabelW + gap, vy, cw - kLabelW - gap, kRowH };
            }
            y += kHeaderH + kSecGapV;

            // IMAGE
            L.imgBg    = { leftX - 2, y, colW + 4, kImgSecH };
            L.imgBadge = { leftX, y, colW, kBadgeH };
            int cy = y + kBadgeH + kBadgeGap;
            {
                const int half = (cw - gap) / 2;
                L.negToggle = { cx, cy, half, kRowH };
                L.dcToggle  = { cx + half + gap, cy, half, kRowH };
                cy += kRowH + kRowGap;
            }
            L.gammaLabel  = { cx, cy, kLabelW, kRowH };
            L.gammaSlider = { cx + kLabelW + gap, cy, cw - kLabelW - gap, kRowH };
            cy += kRowH + kRowGap;
            L.contrastLabel  = { cx, cy, kLabelW, kRowH };
            L.contrastSlider = { cx + kLabelW + gap, cy, cw - kLabelW - gap, kRowH };
            y += kImgSecH + kSecGapV;

            // OSCILLATORS
            L.oscBg    = { leftX - 2, y, colW + 4, kOscSecH };
            L.oscBadge = { leftX, y, colW, kBadgeH };
            cy = y + kBadgeH + kBadgeGap;
            L.oscCaptionY = cy;
            cy += kCapH;
            L.oscEnv = { cx, cy, cw, kEnvH };
            cy += kEnvH + kEnvGap;
            L.oscGridX = cx; L.oscGridW = cw; L.oscGridY = cy;
        }

        // ── RIGHT COLUMN ────────────────────────────────────────────────────
        {
            const int cx = rightX + kSecInsetX;
            const int cw = colW - 2 * kSecInsetX;
            int y = kTopPad;

            // Module identity chip row — mirrors the Volume strip height.
            L.moduleChip = { rightX - 2, y, colW + 4, kHeaderH };
            y += kHeaderH + kSecGapV;

            // ANALYSIS — blob rows, divider, FFT rows.
            L.anaBg    = { rightX - 2, y, colW + 4, kAnaSecH };
            L.anaBadge = { rightX, y, colW, kBadgeH };
            int cy = y + kBadgeH + kBadgeGap;
            L.anaBlobCaptionY = cy;
            cy += kCapH;
            for (int i = 0; i < 4; ++i)
            {
                L.anaLabel[i] = { cx, cy, kLabelW, kRowH };
                L.anaCtrl[i]  = { cx + kLabelW + gap, cy, cw - kLabelW - gap, kRowH };
                cy += kRowH + kRowGap;
            }
            cy += kDivGap - kRowGap;
            L.anaDividerY   = cy - kDivGap / 2;
            L.anaFftCaptionY = cy;
            cy += kCapH;
            for (int i = 4; i < 6; ++i)
            {
                L.anaLabel[i] = { cx, cy, kLabelW, kRowH };
                L.anaCtrl[i]  = { cx + kLabelW + gap, cy, cw - kLabelW - gap, kRowH };
                cy += kRowH + kRowGap;
            }
            y += kAnaSecH + kSecGapV;

            // FILTER & LFO
            L.fltBg    = { rightX - 2, y, colW + 4, kFltSecH };
            L.fltBadge = { rightX, y, colW, kBadgeH };
            cy = y + kBadgeH + kBadgeGap;
            L.fltCaptionY = cy;
            cy += kCapH;
            L.fltEnv = { cx, cy, cw, kEnvH };
            cy += kEnvH + kEnvGap;
            L.fltGridX = cx; L.fltGridW = cw; L.fltGridY = cy;
        }

        return L;
    }

    void initLabel(juce::Label& lbl, const juce::String& text)
    {
        lbl.setText(text, juce::dontSendNotification);
        lbl.setJustificationType(juce::Justification::centredRight);
        lbl.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(lbl);
    }

    void initSlider(juce::Slider& s)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, Sp3ctraTheme::kControlH);
        addAndMakeVisible(s);
    }

    // ── Controls ────────────────────────────────────────────────────────────
    juce::Slider       volumeSlider;                               // master (left top)
    juce::ComboBox     sourceCombo;                                // retired (plumbing only)
    juce::ToggleButton negativeToggle, dcBlockToggle;
    juce::Label        volumeLabel, gammaLabel, contrastMinLabel;
    juce::Slider       gammaSlider, contrastMinSlider;

    // OSCILLATORS (left)
    std::unique_ptr<EnvelopeEditorComponent> volEnv;
    juce::Slider       numOscSlider;

    // ANALYSIS — blob detection + FFT (right)
    juce::Label    blobThreshLabel, blobMinWidthLabel, blobMergeGapLabel, blobColorSplitLabel;
    juce::Slider   blobThreshSlider, blobMinWidthSlider, blobMergeGapSlider, blobColorSplitSlider;
    juce::Label    fftBinsLabel, fftSmoothingLabel;
    juce::ComboBox fftBinsCombo;
    juce::Slider   fftSmoothingSlider;

    // FILTER & LFO (right)
    std::unique_ptr<EnvelopeEditorComponent> fltEnv;
    juce::Slider       fltCutoffSlider, fltDepthSlider, lfoRateSlider, lfoDepthSlider;

    // ── Attachments ───────────────────────────────────────────────────────
    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CmbAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<CmbAttach> sourceAttach, fftBinsAttach;
    std::unique_ptr<SldAttach> volumeAttach, gammaAttach, contrastMinAttach,
                               numOscAttach,
                               blobThreshAttach, blobMinWidthAttach,
                               blobMergeGapAttach, blobColorSplitAttach,
                               fftSmoothingAttach,
                               fltCutoffAttach, fltDepthAttach,
                               lfoRateAttach, lfoDepthAttach;
    std::unique_ptr<BtnAttach> negativeAttach, dcBlockAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxSynthTabComponent)
};
