/**
 * @file LuxStralTabComponent.h
 * @brief LUXSTRAL module — the WHOLE module UI on one page, laid out in 2 columns.
 *
 *   ┌ Volume ════════════════════ ┐   (left column, master output)
 *   │ LEFT                │ RIGHT  │
 *   │ ┌ IMAGE ─────────┐  │ ┌ STEREO ──────┐
 *   │ │ Negative/DC    │  │ │ [Stereo] Temp │
 *   │ │ Gamma/Contrast │  │ └───────────────┘
 *   │ └────────────────┘  │ ┌ STROKEFORGE ──┐
 *   │ ┌ OSCILLATORS ───┐  │ │ BLOB DETECTION │
 *   │ │ ATTACK/RELEASE │  │ │  Ampl/Pix/...  │
 *   │ │  [ env curve ] │  │ │ MORPHING       │
 *   │ │ Sum Exp / Gate │  │ │  Sq@W/Focus/.. │
 *   │ └────────────────┘  │ └───────────────┘
 *
 * The page was previously split across two stacked components (image page +
 * AudioStralPanel); the 2-column request mixes image and audio content within
 * each column, so the module is now a single self-contained component.
 *
 * Signal flow: IMAGE conditioning → OSCILLATORS (additive voice + A/R) on the
 * left; STEREO spatialisation + STROKEFORGE (blob-driven morphing) on the right.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/AudioPanelWidgets.h"      // AudioPanelLayout + AudioPanelUI (shared look)
#include "../ui/EnvelopeEditorComponent.h"
#include "VisualizerMode.h"

class LuxStralTabComponent : public juce::Component
{
public:
    explicit LuxStralTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        auto& apvts = p.getAPVTS();

        // ── Source selector — RETIRED (source follows chain placement) ──────
        sourceCombo.addItem("Chain 1", 1);
        sourceCombo.addItem("Chain 2", 2);
        sourceAttach.reset(new CmbAttach(apvts, "luxstralSource", sourceCombo));

        // ── Master Volume (top of left column) ────────────────────────────
        initLabel(volumeLabel, "Volume");
        initSlider(luxstralVolumeSlider);

        // ── IMAGE — conditioning (label is the toggle text itself) ──────────
        negativeToggle.setButtonText("Negative");
        addAndMakeVisible(negativeToggle);

        dcBlockToggle.setButtonText("DC Blocking");
        addAndMakeVisible(dcBlockToggle);

        initLabel(gammaLabel, "Gamma");
        initSlider(gammaSlider);

        initLabel(contrastMinLabel, "Contrast Min");
        initSlider(contrastMinSlider);

        // ── OSCILLATORS — additive voice: Sum Exp / Noise Gate knobs ─────────
        // (the A/R envelope editor is created per-engine in bindEngineParams)
        AudioPanelUI::initKnob(sumExpSlider);
        addAndMakeVisible(sumExpSlider);

        AudioPanelUI::initKnob(noiseGateSlider);
        addAndMakeVisible(noiseGateSlider);

        // ── STEREO — spatialisation (enable toggle lives in the badge) ───────
        stereoEnableToggle.setButtonText({});
        addAndMakeVisible(stereoEnableToggle);

        AudioPanelUI::initKnob(stereoTempSlider);
        addAndMakeVisible(stereoTempSlider);

        // Per-engine attachments (Volume / IMAGE / OSCILLATORS / STEREO) —
        // bound to luxstral* (A) or luxstralB* (B) depending on the selected
        // rack instance. StrokeForge below stays SHARED between engines.
        bindEngineParams();

        // ── STROKEFORGE — blob detection (sliders) ──────────────────────────
        initLabel(blobThreshLabel, "Ampl. Thr.");
        initSlider(blobThreshSlider);
        blobThreshAttach.reset(new SldAttach(apvts, "spctrBlobThreshold", blobThreshSlider));
        initLabel(blobMinWidthLabel, "Pix. Thr.");
        initSlider(blobMinWidthSlider);
        blobMinWidthAttach.reset(new SldAttach(apvts, "spctrBlobMinWidth", blobMinWidthSlider));
        initLabel(blobMergeGapLabel, "Merge Gap");
        initSlider(blobMergeGapSlider);
        blobMergeGapAttach.reset(new SldAttach(apvts, "spctrBlobMergeGap", blobMergeGapSlider));
        initLabel(blobColorSplitLabel, "Color Split");
        initSlider(blobColorSplitSlider);
        blobColorSplitAttach.reset(new SldAttach(apvts, "spctrBlobColorSplit", blobColorSplitSlider));

        // ── STROKEFORGE — morphing (enable toggle lives in the badge) ────────
        sfEnabledToggle.setButtonText({});
        addAndMakeVisible(sfEnabledToggle);
        sfEnabledAttach.reset(new BtnAttach(apvts, "sfEnabled", sfEnabledToggle));

        sfFocusOnlyToggle.setButtonText("Focus Only");
        addAndMakeVisible(sfFocusOnlyToggle);
        sfFocusOnlyAttach.reset(new BtnAttach(apvts, "sfFocusOnly", sfFocusOnlyToggle));

        AudioPanelUI::initKnob(sfMorphWidthSlider);
        addAndMakeVisible(sfMorphWidthSlider);
        sfMorphWidthAttach.reset(new SldAttach(apvts, "sfMorphWidthScale", sfMorphWidthSlider));

        AudioPanelUI::initKnob(sfFocusSigmaSlider, " notes");
        addAndMakeVisible(sfFocusSigmaSlider);
        sfFocusSigmaAttach.reset(new SldAttach(apvts, "sfBlobFocusSigma", sfFocusSigmaSlider));

        AudioPanelUI::initKnob(sfSpectralThreshSlider, " notes");
        addAndMakeVisible(sfSpectralThreshSlider);
        sfSpectralThreshAttach.reset(new SldAttach(apvts, "sfSpectralWidthThreshold", sfSpectralThreshSlider));

        // Dependent-control dimming.
        stereoEnableToggle.onClick = [this] { updateStereoEnablement(); };
        sfEnabledToggle.onClick     = [this] { updateStrokeForgeEnablement(); };
        updateStereoEnablement();
        updateStrokeForgeEnablement();
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

        // ── Engine identity chip (M8) — A vs B unmistakable at a glance ─────
        // Top of the right column, mirroring the Volume strip's header row.
        {
            const bool isB = (engineIndex_ == 1);
            const juce::Colour tagCol = isB ? juce::Colour(0xffe0a35a)   // amber = B
                                            : juce::Colour(0xff7ab0f0);  // blue  = A
            const auto chip = L.engineChip.toFloat();
            g.setColour(tagCol.withAlpha(0.12f));
            g.fillRoundedRectangle(chip, 4.f);
            g.setColour(tagCol.withAlpha(0.55f));
            g.drawRoundedRectangle(chip, 4.f, 1.f);
            g.setColour(tagCol);
            g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
            g.drawText(isB ? "LUXSTRAL  --  ENGINE B" : "LUXSTRAL  --  ENGINE A",
                       L.engineChip, juce::Justification::centred);
        }

        // ── LEFT: IMAGE ─────────────────────────────────────────────────────
        drawSectionBg(g, L.imgBg.getX(), L.imgBg.getY(), L.imgBg.getWidth(), L.imgBg.getHeight());
        drawBadge(g, L.imgBadge.getX(), L.imgBadge.getY(), L.imgBadge.getWidth(),
                  0xff20303c, 0xff7aade0, "IMAGE");

        // ── LEFT: OSCILLATORS ───────────────────────────────────────────────
        drawSectionBg(g, L.oscBg.getX(), L.oscBg.getY(), L.oscBg.getWidth(), L.oscBg.getHeight());
        drawBadge(g, L.oscBadge.getX(), L.oscBadge.getY(), L.oscBadge.getWidth(),
                  0xff1c3755, 0xff7ab0f0, "OSCILLATORS");
        drawEnvCaption(g, L.oscBadge.getX() + kSecInsetX, L.oscCaptionY,
                       L.oscBadge.getWidth() - 2 * kSecInsetX, 0xff7ab0f0, "ATTACK / RELEASE");
        {
            static const char* const lbls[] = { "Sum Exp", "Noise Gate" };
            for (int i = 0; i < 2; ++i) drawKnobLabel(g, L.oscGridX, L.oscGridW, L.oscGridY, i, lbls[i]);
        }

        // ── RIGHT: STEREO (enable toggle sits in the badge) ─────────────────
        const bool stOn = stereoEnableToggle.getToggleState();
        drawSectionBg(g, L.stBg.getX(), L.stBg.getY(), L.stBg.getWidth(), L.stBg.getHeight());
        drawBadge(g, L.stBadge.getX(), L.stBadge.getY(), L.stBadge.getWidth(),
                  0xff1c3755, 0xff7ab0f0, "STEREO");
        drawKnobLabel(g, L.stGridX, L.stGridW, L.stGridY, 0, "Stereo Temp",
                      stOn ? 0xffb8c4d0 : kDimText);

        // ── RIGHT: STROKEFORGE (enable in badge; blob detection + morphing) ─
        const bool sfOn = sfEnabledToggle.getToggleState();
        const juce::uint32 cap1 = sfOn ? 0xff8888e0 : kDimText;   // BLOB DETECTION
        const juce::uint32 cap2 = sfOn ? 0xffb07af0 : kDimText;   // MORPHING
        const juce::uint32 klbl = sfOn ? 0xffb8c4d0 : kDimText;   // knob labels
        drawSectionBg(g, L.sfBg.getX(), L.sfBg.getY(), L.sfBg.getWidth(), L.sfBg.getHeight());
        // StrokeForge settings are SHARED between engines A and B (single
        // analysis config) — say so explicitly on the B page.
        drawBadge(g, L.sfBadge.getX(), L.sfBadge.getY(), L.sfBadge.getWidth(),
                  0xff2a2a40, 0xff8888e0,
                  engineIndex_ == 1 ? "STROKEFORGE (SHARED A+B)" : "STROKEFORGE");

        const int sdx = L.rightX + kSecInsetX;
        const int sdw = L.colW - 2 * kSecInsetX;
        drawEnvCaption(g, sdx, L.sfBlobCaptionY, sdw, cap1, "BLOB DETECTION");
        g.setColour(juce::Colour(0xff2a2a40));
        g.fillRect(sdx, L.sfDividerY, sdw, 1);
        drawEnvCaption(g, sdx, L.sfMorphCaptionY, sdw, cap2, "MORPHING  --  Sine -> Square");
        {
            static const char* const lbls[] = { "Square @W", "Focus Sigma", "Spectral Thr" };
            for (int i = 0; i < 3; ++i) drawKnobLabel(g, L.sfGridX, L.sfGridW, L.sfGridY, i, lbls[i], klbl);
        }
    }

    //==========================================================================
    void resized() override
    {
        const auto L = computeGeom(getWidth());

        // LEFT
        volumeLabel.setBounds(L.volLabel);
        luxstralVolumeSlider.setBounds(L.volSlider);

        negativeToggle.setBounds(L.negToggle);
        dcBlockToggle.setBounds(L.dcToggle);
        gammaLabel.setBounds(L.gammaLabel);          gammaSlider.setBounds(L.gammaSlider);
        contrastMinLabel.setBounds(L.contrastLabel); contrastMinSlider.setBounds(L.contrastSlider);

        arEnv->setBounds(L.env);
        AudioPanelUI::placeKnob(sumExpSlider,    L.oscGridX, L.oscGridW, L.oscGridY, 0);
        AudioPanelUI::placeKnob(noiseGateSlider, L.oscGridX, L.oscGridW, L.oscGridY, 1);

        // RIGHT
        stereoEnableToggle.setBounds(L.stBadgeToggle);
        AudioPanelUI::placeKnob(stereoTempSlider, L.stGridX, L.stGridW, L.stGridY, 0);

        blobThreshLabel.setBounds(L.blobLabel[0]);     blobThreshSlider.setBounds(L.blobSlider[0]);
        blobMinWidthLabel.setBounds(L.blobLabel[1]);   blobMinWidthSlider.setBounds(L.blobSlider[1]);
        blobMergeGapLabel.setBounds(L.blobLabel[2]);   blobMergeGapSlider.setBounds(L.blobSlider[2]);
        blobColorSplitLabel.setBounds(L.blobLabel[3]); blobColorSplitSlider.setBounds(L.blobSlider[3]);

        sfEnabledToggle.setBounds(L.sfBadgeToggle);
        sfFocusOnlyToggle.setBounds(L.sfFocusToggle);
        AudioPanelUI::placeKnob(sfMorphWidthSlider,     L.sfGridX, L.sfGridW, L.sfGridY, 0);
        AudioPanelUI::placeKnob(sfFocusSigmaSlider,     L.sfGridX, L.sfGridW, L.sfGridY, 1);
        AudioPanelUI::placeKnob(sfSpectralThreshSlider, L.sfGridX, L.sfGridW, L.sfGridY, 2);
    }

    /** Set by the editor: re-evaluate which contextual top-bandeau panels are
     *  shown (COLOR ⟺ Stereo, BLOB ⟺ StrokeForge) when a toggle flips. */
    std::function<void()> onVisualizerSourcesChanged;

    /** Bind the per-engine controls to LuxStral A (0) or B (1) — fired by the
     *  rack on selection (M8). Volume / IMAGE / OSCILLATORS / STEREO rebind to
     *  the luxstral* / luxstralB* parameter sets; STROKEFORGE stays shared. */
    void setEngineIndex(int idx)
    {
        idx = (idx == 1) ? 1 : 0;
        if (idx == engineIndex_)
            return;
        engineIndex_ = idx;
        bindEngineParams();
        resized();               // place the recreated envelope editor
        updateStereoEnablement();
        updateStrokeForgeEnablement();
        repaint();
    }

    int engineIndex() const noexcept { return engineIndex_; }

private:
    Sp3ctraAudioProcessor& processor;
    int engineIndex_ = 0;    // 0 = LuxStral A (luxstral*), 1 = B (luxstralB*)

    /** Per-engine parameter ID: "luxstral"+base (A) or "luxstralB"+base (B). */
    juce::String pid(const char* base) const
    {
        return (engineIndex_ == 1 ? juce::String("luxstralB")
                                  : juce::String("luxstral")) + base;
    }

    /** (Re)create every per-engine attachment + the A/R envelope editor. */
    void bindEngineParams()
    {
        auto& apvts = processor.getAPVTS();

        volumeAttach.reset(new SldAttach(apvts, pid("Volume"), luxstralVolumeSlider));
        negativeAttach.reset(new BtnAttach(apvts, pid("Inversion"), negativeToggle));
        dcBlockAttach.reset(new BtnAttach(apvts, pid("AcRemoval"), dcBlockToggle));
        gammaAttach.reset(new SldAttach(apvts, pid("GammaValue"), gammaSlider));
        contrastMinAttach.reset(new SldAttach(apvts, pid("ContrastMin"), contrastMinSlider));

        // The envelope editor captures its parameter IDs at construction —
        // recreate it against the selected engine's Attack/Release params.
        arEnv = std::make_unique<EnvelopeEditorComponent>(
            apvts, juce::Colour(0xff7ab0f0),
            pid("AttackMs"), juce::String(), juce::String(), pid("ReleaseMs"));
        addAndMakeVisible(*arEnv);

        sumExpAttach.reset(new SldAttach(apvts, pid("SummationResponseExp"), sumExpSlider));
        noiseGateAttach.reset(new SldAttach(apvts, pid("NoiseGateThreshold"), noiseGateSlider));
        stereoEnableAttach.reset(new BtnAttach(apvts, pid("StereoEnable"), stereoEnableToggle));
        stereoTempAttach.reset(new SldAttach(apvts, pid("StereoTempAmp"), stereoTempSlider));
    }

    // ── Vertical layout tokens ──────────────────────────────────────────────
    static constexpr int kTopPad    = 6;
    static constexpr int kColGap    = 16;                          // between columns
    static constexpr int kHeaderH   = 30;                          // Volume strip
    static constexpr int kBadgeH    = Sp3ctraTheme::kSectionH;     // 24
    static constexpr int kBadgeGap  = Sp3ctraTheme::kSectionGap;   // 4
    static constexpr int kRowH      = Sp3ctraTheme::kControlH;     // 22
    static constexpr int kRowGap    = Sp3ctraTheme::kRowGap;       // 4
    static constexpr int kSecGapV   = 10;                          // between sections
    static constexpr int kSecPadB   = 8;                           // section bottom pad
    static constexpr int kSecInsetX = 8;                           // content inset
    static constexpr int kLabelW    = 96;                          // slider label column
    static constexpr int kDivGap    = 10;                          // blob/morph divider gap
    static constexpr int kCapH      = AudioPanelLayout::kEnvCaptionH; // 13
    static constexpr int kToggleGap = AudioPanelLayout::kToggleGap;   // 6
    static constexpr int kKnobH     = AudioPanelLayout::kKnobCellH;   // 71
    static constexpr int kEnvH      = AudioPanelLayout::kEnvH;        // 124
    static constexpr int kEnvGap    = AudioPanelLayout::kEnvGap;      // 10
    static constexpr juce::uint32 kDimText = 0xff5a5a66;             // greyed labels/captions

    static constexpr int kImgSecH    = kBadgeH + kBadgeGap + (3 * kRowH + 2 * kRowGap) + kSecPadB;     // 110
    static constexpr int kOscSecH    = kBadgeH + kBadgeGap + kCapH + kEnvH + kEnvGap + kKnobH + kSecPadB; // 254
    static constexpr int kStereoSecH = kBadgeH + kBadgeGap + kKnobH + kSecPadB;                        // 107
    static constexpr int kSfSecH     = kBadgeH + kBadgeGap + kCapH + (4 * kRowH + 3 * kRowGap) + kDivGap
                                     + kCapH + kRowH + kToggleGap + kKnobH + kSecPadB;                 // 271

    static constexpr int kLeftColH   = kHeaderH + kSecGapV + kImgSecH + kSecGapV + kOscSecH;           // 414
    // Right column starts with the engine-identity chip (same height as the
    // Volume strip) so both columns share the top header row (M8).
    static constexpr int kRightColH  = kHeaderH + kSecGapV + kStereoSecH + kSecGapV + kSfSecH;         // 456

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
        juce::Rectangle<int> oscBg, oscBadge, env;
        int oscCaptionY = 0, oscGridX = 0, oscGridW = 0, oscGridY = 0;
        // right
        juce::Rectangle<int> engineChip;
        juce::Rectangle<int> stBg, stBadge, stBadgeToggle;
        int stGridX = 0, stGridW = 0, stGridY = 0;
        juce::Rectangle<int> sfBg, sfBadge, sfBadgeToggle;
        int sfBlobCaptionY = 0, sfDividerY = 0, sfMorphCaptionY = 0;
        juce::Rectangle<int> blobLabel[4], blobSlider[4];
        juce::Rectangle<int> sfFocusToggle;
        int sfGridX = 0, sfGridW = 0, sfGridY = 0;
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

        // Enable-toggle rectangle docked at the right edge of a section badge.
        auto badgeToggle = [](juce::Rectangle<int> badge)
        {
            constexpr int tw = 44, th = 20;
            return juce::Rectangle<int>(badge.getRight() - tw - 8,
                                        badge.getY() + (badge.getHeight() - th) / 2, tw, th);
        };

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
            L.env = { cx, cy, cw, kEnvH };
            cy += kEnvH + kEnvGap;
            L.oscGridX = cx; L.oscGridW = cw; L.oscGridY = cy;
        }

        // ── RIGHT COLUMN ────────────────────────────────────────────────────
        {
            const int cx = rightX + kSecInsetX;
            const int cw = colW - 2 * kSecInsetX;
            int y = kTopPad;

            // Engine identity chip row (M8) — mirrors the Volume strip height.
            L.engineChip = { rightX - 2, y, colW + 4, kHeaderH };
            y += kHeaderH + kSecGapV;

            // STEREO — enable toggle in the badge; content is just the knob.
            L.stBg          = { rightX - 2, y, colW + 4, kStereoSecH };
            L.stBadge       = { rightX, y, colW, kBadgeH };
            L.stBadgeToggle = badgeToggle(L.stBadge);
            int cy = y + kBadgeH + kBadgeGap;
            L.stGridX = cx; L.stGridW = cw; L.stGridY = cy;
            y += kStereoSecH + kSecGapV;

            // STROKEFORGE — enable toggle in the badge; blob sliders, then
            // morphing (Focus Only + knobs).
            L.sfBg          = { rightX - 2, y, colW + 4, kSfSecH };
            L.sfBadge       = { rightX, y, colW, kBadgeH };
            L.sfBadgeToggle = badgeToggle(L.sfBadge);
            cy = y + kBadgeH + kBadgeGap;
            L.sfBlobCaptionY = cy;
            cy += kCapH;
            for (int i = 0; i < 4; ++i)
            {
                L.blobLabel[i]  = { cx, cy, kLabelW, kRowH };
                L.blobSlider[i] = { cx + kLabelW + gap, cy, cw - kLabelW - gap, kRowH };
                cy += kRowH + kRowGap;
            }
            cy += kDivGap - kRowGap;
            L.sfDividerY      = cy - kDivGap / 2;
            L.sfMorphCaptionY = cy;
            cy += kCapH;
            L.sfFocusToggle = { cx, cy, (cw - gap) / 2, kRowH };
            cy += kRowH + kToggleGap;
            L.sfGridX = cx; L.sfGridW = cw; L.sfGridY = cy;
        }

        return L;
    }

    void updateStereoEnablement()
    {
        stereoTempSlider.setEnabled(stereoEnableToggle.getToggleState());
        repaint();  // refresh the Stereo Temp label tint
        if (onVisualizerSourcesChanged) onVisualizerSourcesChanged();
    }

    void updateStrokeForgeEnablement()
    {
        const bool on = sfEnabledToggle.getToggleState();
        // The whole StrokeForge section is inert when disabled (blob detection
        // is gated off in the core too) — grey the blob sliders, morph knobs,
        // and the Focus Only option (including their value text boxes, which
        // JUCE does not dim on its own).
        auto greySlider = [on](juce::Slider& s, juce::uint32 onText)
        {
            s.setEnabled(on);
            s.setColour(juce::Slider::textBoxTextColourId,
                        juce::Colour(on ? onText : kDimText));
        };
        greySlider(blobThreshSlider,      0xffd6dde6);
        greySlider(blobMinWidthSlider,    0xffd6dde6);
        greySlider(blobMergeGapSlider,    0xffd6dde6);
        greySlider(blobColorSplitSlider,  0xffd6dde6);
        greySlider(sfMorphWidthSlider,    0xffa0c4e8);
        greySlider(sfFocusSigmaSlider,    0xffa0c4e8);
        greySlider(sfSpectralThreshSlider,0xffa0c4e8);
        sfFocusOnlyToggle.setEnabled(on);

        juce::Label* labels[] = { &blobThreshLabel, &blobMinWidthLabel,
                                  &blobMergeGapLabel, &blobColorSplitLabel };
        for (auto* l : labels)
            l->setColour(juce::Label::textColourId, juce::Colour(on ? 0xffb8c4d0 : kDimText));

        repaint();  // refresh the painted captions / knob labels
        if (onVisualizerSourcesChanged) onVisualizerSourcesChanged();
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
    juce::Slider       luxstralVolumeSlider;                       // master (left top)
    juce::ComboBox     sourceCombo;                                // retired (plumbing only)
    juce::ToggleButton negativeToggle, dcBlockToggle;
    juce::Label        volumeLabel, gammaLabel, contrastMinLabel;
    juce::Slider       gammaSlider, contrastMinSlider;

    // OSCILLATORS (left)
    std::unique_ptr<EnvelopeEditorComponent> arEnv;
    juce::Slider       sumExpSlider, noiseGateSlider;

    // STEREO (right)
    juce::ToggleButton stereoEnableToggle;
    juce::Slider       stereoTempSlider;

    // STROKEFORGE — blob detection (right)
    juce::Label  blobThreshLabel, blobMinWidthLabel, blobMergeGapLabel, blobColorSplitLabel;
    juce::Slider blobThreshSlider, blobMinWidthSlider, blobMergeGapSlider, blobColorSplitSlider;
    // STROKEFORGE — morphing (right)
    juce::ToggleButton sfEnabledToggle, sfFocusOnlyToggle;
    juce::Slider       sfMorphWidthSlider, sfFocusSigmaSlider, sfSpectralThreshSlider;

    // ── Attachments ───────────────────────────────────────────────────────
    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CmbAttach = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<CmbAttach> sourceAttach;
    std::unique_ptr<SldAttach> volumeAttach, gammaAttach, contrastMinAttach,
                               sumExpAttach, noiseGateAttach, stereoTempAttach,
                               blobThreshAttach, blobMinWidthAttach,
                               blobMergeGapAttach, blobColorSplitAttach,
                               sfMorphWidthAttach, sfFocusSigmaAttach, sfSpectralThreshAttach;
    std::unique_ptr<BtnAttach> negativeAttach, dcBlockAttach, stereoEnableAttach,
                               sfEnabledAttach, sfFocusOnlyAttach;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LuxStralTabComponent)
};
