#include "EngineAudioPanels.h"

// ── Shared layout helpers — rotary-knob grid (mockup-validated layout) ────────
//    Sections stack full-width; continuous params are knobs in a fixed-column
//    grid, on/off params sit in a toggle strip under the section badge.
namespace
{
    using namespace AudioPanelLayout;
    using namespace AudioPanelUI;   // initKnob/placeKnob/drawBadge/... (shared header)

    constexpr int kHPad = Sp3ctraTheme::kHPad;
}

//==============================================================================
// AudioWavePanel — LUXWAVE module page, 2 columns (LUXSTRAL visual language)
//==============================================================================
AudioWavePanel::AudioWavePanel(Sp3ctraAudioProcessor& p)
{
    auto& apvts = p.getAPVTS();

    // Enable toggle removed — power lives in the rack LED + zone-3 header.

    // ── Master Volume (top of left column, mirrors LUXSTRAL's strip) ────────
    volumeLabel.setText("Volume", juce::dontSendNotification);
    volumeLabel.setJustificationType(juce::Justification::centredRight);
    volumeLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(volumeLabel);

    luxwaveVolumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    luxwaveVolumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50,
                                        Sp3ctraTheme::kControlH);
    addAndMakeVisible(luxwaveVolumeSlider);
    luxwaveVolumeAttachment = std::make_unique<SldAttach>(apvts, "luxwaveVolume", luxwaveVolumeSlider);

    // ── WAVETABLE — scan direction + amplitude ADSR voice ────────────────────
    addAndMakeVisible(lwScanModeCombo);
    lwScanModeCombo.addItem("Forward",   1);
    lwScanModeCombo.addItem("Ping-Pong", 2);
    lwScanModeCombo.addItem("Random",    3);
    lwScanModeAttach = std::make_unique<CmbAttach>(apvts, "luxwaveScanMode", lwScanModeCombo);

    // Amplitude ADSR as a draggable envelope editor with curve bend handles.
    volEnv = std::make_unique<EnvelopeEditorComponent>(
        apvts, juce::Colour(0xffddaa44),
        "luxwaveAttackMs", "luxwaveDecayMs", "luxwaveSustainLevel", "luxwaveReleaseMs",
        "luxwaveAttackCurve", "luxwaveDecayCurve", "luxwaveReleaseCurve");
    addAndMakeVisible(*volEnv);

    initKnob(lwAmplitudeSlider);       addAndMakeVisible(lwAmplitudeSlider);
    lwAmplitudeAttach = std::make_unique<SldAttach>(apvts, "luxwaveAmplitude", lwAmplitudeSlider);

    // ── FILTER — per-voice lowpass, cutoff swept by the amplitude ADSR ───────
    initKnob(lwFltCutoffSlider, " Hz"); addAndMakeVisible(lwFltCutoffSlider);
    lwFltCutoffAttach = std::make_unique<SldAttach>(apvts, "luxwaveFilterCutoff", lwFltCutoffSlider);
    initKnob(lwFltDepthSlider, " Hz");  addAndMakeVisible(lwFltDepthSlider);
    lwFltDepthAttach = std::make_unique<SldAttach>(apvts, "luxwaveFilterEnvDepth", lwFltDepthSlider);

    // ── LFO — global vibrato ─────────────────────────────────────────────────
    initKnob(lwLfoRateSlider, " Hz");  addAndMakeVisible(lwLfoRateSlider);
    lwLfoRateAttach = std::make_unique<SldAttach>(apvts, "luxwaveLfoRate", lwLfoRateSlider);
    initKnob(lwLfoDepthSlider);        addAndMakeVisible(lwLfoDepthSlider);
    lwLfoDepthAttach = std::make_unique<SldAttach>(apvts, "luxwaveLfoDepth", lwLfoDepthSlider);
}

AudioWavePanel::Geom AudioWavePanel::computeGeom(int w) const
{
    Geom L{};
    const int gx   = kHPad;
    const int gw   = w - 2 * kHPad;
    const int gap  = Sp3ctraTheme::kGap;
    L.colW   = (gw - kColGap) / 2;
    L.leftX  = gx;
    L.rightX = gx + L.colW + kColGap;

    // ── LEFT COLUMN ─────────────────────────────────────────────────────────
    {
        const int cx = L.leftX + kSecInsetX;
        const int cw = L.colW - 2 * kSecInsetX;
        int y = kTopPad;

        // Volume strip
        L.volStrip = { L.leftX - 2, y, L.colW + 4, kHeaderH };
        {
            const int vy = y + (kHeaderH - kRowH) / 2;
            L.volLabel  = { cx, vy, kLabelW, kRowH };
            L.volSlider = { cx + kLabelW + gap, vy, cw - kLabelW - gap, kRowH };
        }
        y += kHeaderH + kSecGapV;

        // WAVETABLE
        L.waveBg    = { L.leftX - 2, y, L.colW + 4, kWaveSecH };
        L.waveBadge = { L.leftX, y, L.colW, kBadgeH };
        int cy = y + kBadgeH + kBadgeGap;
        L.scanCaption = { cx, cy, 40, kRowH };
        L.scanCombo   = { cx + 44, cy, 130, kRowH };
        cy += kRowH + kToggleGap;
        L.waveCaptionY = cy;
        cy += kCapH;
        L.env = { cx, cy, cw, kEnvH };
        cy += kEnvH + kEnvGap;
        L.waveGridX = cx; L.waveGridW = cw; L.waveGridY = cy;
    }

    // ── RIGHT COLUMN ────────────────────────────────────────────────────────
    {
        const int cx = L.rightX + kSecInsetX;
        const int cw = L.colW - 2 * kSecInsetX;
        int y = kTopPad;

        // Module identity chip — mirrors the Volume strip's header row.
        L.chip = { L.rightX - 2, y, L.colW + 4, kHeaderH };
        y += kHeaderH + kSecGapV;

        // FILTER
        L.fltBg    = { L.rightX - 2, y, L.colW + 4, kFltSecH };
        L.fltBadge = { L.rightX, y, L.colW, kBadgeH };
        int cy = y + kBadgeH + kBadgeGap;
        L.fltCaptionY = cy;
        cy += kCapH;
        L.fltGridX = cx; L.fltGridW = cw; L.fltGridY = cy;
        y += kFltSecH + kSecGapV;

        // LFO
        L.lfoBg    = { L.rightX - 2, y, L.colW + 4, kLfoSecH };
        L.lfoBadge = { L.rightX, y, L.colW, kBadgeH };
        cy = y + kBadgeH + kBadgeGap;
        L.lfoCaptionY = cy;
        cy += kCapH;
        L.lfoGridX = cx; L.lfoGridW = cw; L.lfoGridY = cy;
    }

    return L;
}

void AudioWavePanel::paint(juce::Graphics& g)
{
    const auto L = computeGeom(getWidth());

    // ── Master Volume strip (amber-tinted variant of LUXSTRAL's) ────────────
    {
        const auto r = L.volStrip.toFloat();
        g.setColour(juce::Colour(0xff262014));
        g.fillRoundedRectangle(r, 4.f);
        g.setColour(juce::Colour(0xff453722));
        g.drawRoundedRectangle(r, 4.f, 1.f);
    }

    // ── Module identity chip (top of the right column) ──────────────────────
    {
        const juce::Colour tagCol(0xffddaa44);
        const auto chip = L.chip.toFloat();
        g.setColour(tagCol.withAlpha(0.12f));
        g.fillRoundedRectangle(chip, 4.f);
        g.setColour(tagCol.withAlpha(0.55f));
        g.drawRoundedRectangle(chip, 4.f, 1.f);
        g.setColour(tagCol);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("LUXWAVE  --  OPTICAL WAVETABLE", L.chip, juce::Justification::centred);
    }

    // ── LEFT: WAVETABLE ──────────────────────────────────────────────────────
    drawSectionBg(g, L.waveBg.getX(), L.waveBg.getY(), L.waveBg.getWidth(), L.waveBg.getHeight());
    drawBadge(g, L.waveBadge.getX(), L.waveBadge.getY(), L.waveBadge.getWidth(),
              0xff3a2a1a, 0xffddaa44, "WAVETABLE");
    g.setColour(juce::Colour(0xff888888));
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
    g.drawText("Scan", L.scanCaption, juce::Justification::centredLeft, true);
    drawEnvCaption(g, L.waveGridX, L.waveCaptionY, L.waveGridW, 0xffddaa44, "AMPLITUDE  ADSR");
    drawKnobLabel(g, L.waveGridX, L.waveGridW, L.waveGridY, 0, "Amplitude");

    // ── RIGHT: FILTER ────────────────────────────────────────────────────────
    drawSectionBg(g, L.fltBg.getX(), L.fltBg.getY(), L.fltBg.getWidth(), L.fltBg.getHeight());
    drawBadge(g, L.fltBadge.getX(), L.fltBadge.getY(), L.fltBadge.getWidth(),
              0xff2f2030, 0xffcc88cc, "FILTER");
    drawEnvCaption(g, L.fltGridX, L.fltCaptionY, L.fltGridW, 0xffcc88cc,
                   "LOWPASS  --  ADSR MODULATED");
    {
        static const char* const lbls[] = { "Cutoff", "Env Depth" };
        for (int i = 0; i < 2; ++i) drawKnobLabel(g, L.fltGridX, L.fltGridW, L.fltGridY, i, lbls[i]);
    }

    // ── RIGHT: LFO ───────────────────────────────────────────────────────────
    drawSectionBg(g, L.lfoBg.getX(), L.lfoBg.getY(), L.lfoBg.getWidth(), L.lfoBg.getHeight());
    drawBadge(g, L.lfoBadge.getX(), L.lfoBadge.getY(), L.lfoBadge.getWidth(),
              0xff1a3a3a, 0xff66ccaa, "LFO");
    drawEnvCaption(g, L.lfoGridX, L.lfoCaptionY, L.lfoGridW, 0xff66ccaa,
                   "VIBRATO  --  PITCH MOD");
    {
        static const char* const lbls[] = { "Rate", "Depth" };
        for (int i = 0; i < 2; ++i) drawKnobLabel(g, L.lfoGridX, L.lfoGridW, L.lfoGridY, i, lbls[i]);
    }
}

void AudioWavePanel::resized()
{
    const auto L = computeGeom(getWidth());

    // LEFT
    volumeLabel.setBounds(L.volLabel);
    luxwaveVolumeSlider.setBounds(L.volSlider);
    lwScanModeCombo.setBounds(L.scanCombo);
    volEnv->setBounds(L.env);
    placeKnob(lwAmplitudeSlider, L.waveGridX, L.waveGridW, L.waveGridY, 0);

    // RIGHT
    placeKnob(lwFltCutoffSlider, L.fltGridX, L.fltGridW, L.fltGridY, 0);
    placeKnob(lwFltDepthSlider,  L.fltGridX, L.fltGridW, L.fltGridY, 1);
    placeKnob(lwLfoRateSlider,   L.lfoGridX, L.lfoGridW, L.lfoGridY, 0);
    placeKnob(lwLfoDepthSlider,  L.lfoGridX, L.lfoGridW, L.lfoGridY, 1);
}
