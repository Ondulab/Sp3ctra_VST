#include "EngineAudioPanels.h"

// ── Shared layout helpers — rotary-knob grid (mockup-validated layout) ────────
//    Sections stack full-width; continuous params are knobs in a fixed-column
//    grid, on/off params sit in a toggle strip under the section badge.
namespace
{
    using namespace AudioPanelLayout;

    constexpr int kHPad     = Sp3ctraTheme::kHPad;
    constexpr int kSectionH = Sp3ctraTheme::kSectionH;
    constexpr int kSecGap   = Sp3ctraTheme::kSectionGap;
    constexpr int kCtrlH    = Sp3ctraTheme::kControlH;

    /** Rotary knob: accent-arc cadran + value text-box below (Sp3ctraLookAndFeel). */
    void initKnob(juce::Slider& s, const char* suffix = nullptr)
    {
        s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, kKnobValH);
        s.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        s.setColour(juce::Slider::textBoxTextColourId,       juce::Colour(0xffa0c4e8));
        if (suffix) s.setTextValueSuffix(suffix);
    }

    /** Cell rectangle for knob #idx in a grid starting at (gx, gy) of width gw. */
    juce::Rectangle<int> knobCell(int gx, int gw, int gy, int idx)
    {
        const int cellW = (gw - (kKnobCols - 1) * kKnobGapX) / kKnobCols;
        const int col = idx % kKnobCols;
        const int row = idx / kKnobCols;
        return { gx + col * (cellW + kKnobGapX), gy + row * kKnobRowStep, cellW, kKnobCellH };
    }

    /** Place a knob slider (rotary + value box) into its grid cell. */
    void placeKnob(juce::Slider& s, int gx, int gw, int gy, int idx)
    {
        const auto c = knobCell(gx, gw, gy, idx);
        s.setBounds(c.getX(), c.getY(), c.getWidth(), kKnobArea + kKnobValH);
    }

    /** Draw the name label under knob #idx (the cell's bottom strip). */
    void drawKnobLabel(juce::Graphics& g, int gx, int gw, int gy, int idx, const char* text)
    {
        const auto c = knobCell(gx, gw, gy, idx);
        g.setColour(juce::Colour(0xffb8c4d0));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        g.drawText(text,
                   juce::Rectangle<int>(c.getX(), c.getY() + kKnobArea + kKnobValH,
                                        c.getWidth(), kKnobLblH),
                   juce::Justification::centred, true);
    }

    /** Subtle rounded backdrop grouping one stacked section. */
    void drawSectionBg(juce::Graphics& g, int x, int y, int w, int h)
    {
        const auto r = juce::Rectangle<int>(x, y, w, h).toFloat();
        g.setColour(juce::Colour(0xff131320));
        g.fillRoundedRectangle(r, 4.f);
        g.setColour(juce::Colour(0xff2a2a40));
        g.drawRoundedRectangle(r, 4.f, 1.f);
    }

    /** Coloured section badge at (x, y). */
    void drawBadge(juce::Graphics& g, int x, int y, int w,
                   juce::uint32 bg, juce::uint32 fg, const char* text)
    {
        g.setColour(juce::Colour(bg));
        g.fillRoundedRectangle(juce::Rectangle<int>(x, y, w, kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(fg));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText(text, juce::Rectangle<int>(x + 8, y, w - 16, kSectionH),
                   juce::Justification::centredLeft, true);
    }

    /** Small accented caption above an envelope editor. */
    void drawEnvCaption(juce::Graphics& g, int x, int y, int w,
                        juce::uint32 fg, const char* text)
    {
        g.setColour(juce::Colour(fg).withAlpha(0.85f));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
        g.drawText(text, x + 2, y, w - 4, kEnvCaptionH,
                   juce::Justification::centredLeft, true);
    }

    // Section geometry: y offsets relative to the section's top.
    int stripY(int secTop) { return secTop + kSectionH + kSecGap; }
    int gridYWithToggles(int secTop) { return stripY(secTop) + kToggleStripH + kToggleGap; }

    // Vertical anatomy of a section that hosts an envelope editor (top at 0):
    // badge → enable strip → caption → editor → knob grid.
    int shStripY()   { return kSectionH + kSecGap; }
    int shCaptionY() { return shStripY() + kCtrlH + kToggleGap; }
    int shEnvY()     { return shCaptionY() + kEnvCaptionH; }
    int shGridY()    { return shEnvY() + kEnvH + kEnvGap; }
}

//==============================================================================
// AudioStralPanel
//==============================================================================
AudioStralPanel::AudioStralPanel(Sp3ctraAudioProcessor& p)
{
    auto& apvts = p.getAPVTS();

    // Device-On toggle removed — power lives in the rack LED + zone-3 header.
    stereoEnableToggle.setButtonText("Stereo");
    addAndMakeVisible(stereoEnableToggle);
    stereoEnableAttachment = std::make_unique<BtnAttach>(apvts, "luxstralStereoEnable", stereoEnableToggle);

    // Attack/Release as a draggable AR envelope editor (empty decay/sustain IDs).
    arEnv = std::make_unique<EnvelopeEditorComponent>(
        apvts, juce::Colour(0xff7ab0f0),
        "luxstralAttackMs", juce::String(), juce::String(), "luxstralReleaseMs");
    addAndMakeVisible(*arEnv);

    initKnob(luxstralVolumeSlider);
    addAndMakeVisible(luxstralVolumeSlider);
    luxstralVolumeAttachment = std::make_unique<SldAttach>(apvts, "luxstralVolume", luxstralVolumeSlider);

    initKnob(stereoTempSlider);
    addAndMakeVisible(stereoTempSlider);
    stereoTempAttachment = std::make_unique<SldAttach>(apvts, "luxstralStereoTempAmp", stereoTempSlider);

    initKnob(sumExpSlider);
    addAndMakeVisible(sumExpSlider);
    sumExpAttachment = std::make_unique<SldAttach>(apvts, "luxstralSummationResponseExp", sumExpSlider);

    initKnob(noiseGateSlider);
    addAndMakeVisible(noiseGateSlider);
    noiseGateAttachment = std::make_unique<SldAttach>(apvts, "luxstralNoiseGateThreshold", noiseGateSlider);

    // ── StrokeForge (stacked section) ─────────────────────────────────────────
    sfEnabledToggle.setButtonText("StrokeForge");
    addAndMakeVisible(sfEnabledToggle);
    sfEnabledAttachment = std::make_unique<BtnAttach>(apvts, "sfEnabled", sfEnabledToggle);

    initKnob(sfMorphWidthSlider);
    addAndMakeVisible(sfMorphWidthSlider);
    sfMorphWidthAttachment = std::make_unique<SldAttach>(apvts, "sfMorphWidthScale", sfMorphWidthSlider);

    initKnob(sfFocusSigmaSlider, " notes");
    addAndMakeVisible(sfFocusSigmaSlider);
    sfFocusSigmaAttachment = std::make_unique<SldAttach>(apvts, "sfBlobFocusSigma", sfFocusSigmaSlider);

    initKnob(sfSpectralThreshSlider, " notes");
    addAndMakeVisible(sfSpectralThreshSlider);
    sfSpectralThreshAttachment = std::make_unique<SldAttach>(apvts, "sfSpectralWidthThreshold", sfSpectralThreshSlider);

    sfFocusOnlyToggle.setButtonText("Focus Only");
    addAndMakeVisible(sfFocusOnlyToggle);
    sfFocusOnlyAttachment = std::make_unique<BtnAttach>(apvts, "sfFocusOnly", sfFocusOnlyToggle);
}

void AudioStralPanel::paint(juce::Graphics& g)
{
    const int gx = kHPad;
    const int gw = getWidth() - 2 * kHPad;

    // Section A — AUDIOSTRAL (AR envelope + knobs)
    const int secAH = shGridY() + gridH(4);
    drawSectionBg(g, gx - 2, 0, gw + 4, secAH);
    drawBadge(g, gx, 0, gw, 0xff1c3755, 0xff7ab0f0, "AUDIOSTRAL");
    drawEnvCaption(g, gx, shCaptionY(), gw, 0xff7ab0f0, "ATTACK / RELEASE");
    {
        const int gy = shGridY();
        static const char* const lbls[] = { "Volume", "Stereo Temp", "Sum Exp", "Noise Gate" };
        for (int i = 0; i < 4; ++i) drawKnobLabel(g, gx, gw, gy, i, lbls[i]);
    }

    // Section B — STROKEFORGE
    const int bTop = secAH + kSecGapV;
    drawSectionBg(g, gx - 2, bTop, gw + 4, sectionH(3, true));
    drawBadge(g, gx, bTop, gw, 0xff2c1f4a, 0xffb07af0,
              "STROKEFORGE  --  Waveform Morphing (Sine -> Square)");
    {
        const int gy = gridYWithToggles(bTop);
        static const char* const lbls[] = { "Square @W", "Focus Sigma", "Spectral Thr" };
        for (int i = 0; i < 3; ++i) drawKnobLabel(g, gx, gw, gy, i, lbls[i]);
    }
}

void AudioStralPanel::resized()
{
    const int gx = kHPad;
    const int gw = getWidth() - 2 * kHPad;

    // Section A — AUDIOSTRAL
    stereoEnableToggle.setBounds(gx, shStripY(), kToggleW, kCtrlH);
    arEnv->setBounds(gx, shEnvY(), gw, kEnvH);
    {
        const int gy = shGridY();
        placeKnob(luxstralVolumeSlider, gx, gw, gy, 0);
        placeKnob(stereoTempSlider,     gx, gw, gy, 1);
        placeKnob(sumExpSlider,         gx, gw, gy, 2);
        placeKnob(noiseGateSlider,      gx, gw, gy, 3);
    }

    // Section B — STROKEFORGE
    const int secAH = shGridY() + gridH(4);
    const int bTop  = secAH + kSecGapV;
    sfEnabledToggle  .setBounds(gx,               stripY(bTop), kToggleW, kCtrlH);
    sfFocusOnlyToggle.setBounds(gx + kToggleW + 6, stripY(bTop), kToggleW, kCtrlH);
    {
        const int gy = gridYWithToggles(bTop);
        placeKnob(sfMorphWidthSlider,    gx, gw, gy, 0);
        placeKnob(sfFocusSigmaSlider,    gx, gw, gy, 1);
        placeKnob(sfSpectralThreshSlider, gx, gw, gy, 2);
    }
}

//==============================================================================
// AudioSynthPanel
//==============================================================================
AudioSynthPanel::AudioSynthPanel(Sp3ctraAudioProcessor& p)
{
    auto& apvts = p.getAPVTS();

    // Enable toggle removed — power lives in the rack LED + zone-3 header.

    // Volume & Filter ADSRs as draggable envelope editors with curve bend handles.
    volEnv = std::make_unique<EnvelopeEditorComponent>(
        apvts, juce::Colour(0xff66ccaa),
        "luxsynthAttackMs", "luxsynthDecayMs", "luxsynthSustainLevel", "luxsynthReleaseMs",
        "luxsynthAttackCurve", "luxsynthDecayCurve", "luxsynthReleaseCurve");
    addAndMakeVisible(*volEnv);
    fltEnv = std::make_unique<EnvelopeEditorComponent>(
        apvts, juce::Colour(0xffcc88cc),
        "luxsynthFilterAttackMs", "luxsynthFilterDecayMs", "luxsynthFilterSustain", "luxsynthFilterReleaseMs",
        "luxsynthFilterAttackCurve", "luxsynthFilterDecayCurve", "luxsynthFilterReleaseCurve");
    addAndMakeVisible(*fltEnv);

    // Continuous params remain rotary knobs.
    initKnob(luxsynthVolumeSlider);
    addAndMakeVisible(luxsynthVolumeSlider);
    luxsynthVolumeAttachment = std::make_unique<SldAttach>(apvts, "luxsynthVolume", luxsynthVolumeSlider);

    initKnob(lxFltCutoffSlider, " Hz");  addAndMakeVisible(lxFltCutoffSlider);
    lxFltCutoffAttach = std::make_unique<SldAttach>(apvts, "luxsynthFilterCutoff", lxFltCutoffSlider);
    initKnob(lxFltDepthSlider);          addAndMakeVisible(lxFltDepthSlider);
    lxFltDepthAttach = std::make_unique<SldAttach>(apvts, "luxsynthFilterEnvDepth", lxFltDepthSlider);

    initKnob(lxNumOscSlider);   addAndMakeVisible(lxNumOscSlider);
    lxNumOscAttach = std::make_unique<SldAttach>(apvts, "luxsynthNumOscillators", lxNumOscSlider);

    initKnob(lxLfoRateSlider, " Hz");  addAndMakeVisible(lxLfoRateSlider);
    lxLfoRateAttach = std::make_unique<SldAttach>(apvts, "luxsynthLfoRate", lxLfoRateSlider);
    initKnob(lxLfoDepthSlider);        addAndMakeVisible(lxLfoDepthSlider);
    lxLfoDepthAttach = std::make_unique<SldAttach>(apvts, "luxsynthLfoDepth", lxLfoDepthSlider);
}

void AudioSynthPanel::paint(juce::Graphics& g)
{
    const int gx = kHPad;
    const int gw = getWidth() - 2 * kHPad;
    const int half = (gw - 10) / 2;
    const int dy = kToggleStripH + kToggleGap;  // empty enable strip reclaimed

    drawSectionBg(g, gx - 2, 0, gw + 4, getHeight() - 2);
    drawBadge(g, gx, 0, gw, 0xff1a3a3a, 0xff66ccaa, "AUDIOSYNTH");

    drawEnvCaption(g, gx,            shCaptionY() - dy, half, 0xff66ccaa, "VOLUME  ADSR");
    drawEnvCaption(g, gx + half + 10, shCaptionY() - dy, half, 0xffcc88cc, "FILTER  ADSR");

    const int gy = shGridY() - dy;
    static const char* const lbls[] = { "Volume", "Cutoff", "Env Depth",
                                        "Oscill.", "LFO Rate", "LFO Depth" };
    for (int i = 0; i < 6; ++i) drawKnobLabel(g, gx, gw, gy, i, lbls[i]);
}

void AudioSynthPanel::resized()
{
    const int gx = kHPad;
    const int gw = getWidth() - 2 * kHPad;
    const int half = (gw - 10) / 2;
    const int dy = kToggleStripH + kToggleGap;  // empty enable strip reclaimed

    volEnv->setBounds(gx,             shEnvY() - dy, half, kEnvH);
    fltEnv->setBounds(gx + half + 10, shEnvY() - dy, half, kEnvH);

    const int gy = shGridY() - dy;
    placeKnob(luxsynthVolumeSlider, gx, gw, gy, 0);
    placeKnob(lxFltCutoffSlider,    gx, gw, gy, 1);
    placeKnob(lxFltDepthSlider,     gx, gw, gy, 2);
    placeKnob(lxNumOscSlider,       gx, gw, gy, 3);
    placeKnob(lxLfoRateSlider,      gx, gw, gy, 4);
    placeKnob(lxLfoDepthSlider,     gx, gw, gy, 5);
}

//==============================================================================
// AudioWavePanel
//==============================================================================
AudioWavePanel::AudioWavePanel(Sp3ctraAudioProcessor& p)
{
    auto& apvts = p.getAPVTS();

    // Enable toggle removed — power lives in the rack LED + zone-3 header.
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

    // Continuous params remain rotary knobs.
    initKnob(luxwaveVolumeSlider);
    addAndMakeVisible(luxwaveVolumeSlider);
    luxwaveVolumeAttachment = std::make_unique<SldAttach>(apvts, "luxwaveVolume", luxwaveVolumeSlider);

    initKnob(lwAmplitudeSlider);       addAndMakeVisible(lwAmplitudeSlider);
    lwAmplitudeAttach = std::make_unique<SldAttach>(apvts, "luxwaveAmplitude", lwAmplitudeSlider);

    initKnob(lwFltCutoffSlider, " Hz"); addAndMakeVisible(lwFltCutoffSlider);
    lwFltCutoffAttach = std::make_unique<SldAttach>(apvts, "luxwaveFilterCutoff", lwFltCutoffSlider);
    initKnob(lwFltDepthSlider, " Hz");  addAndMakeVisible(lwFltDepthSlider);
    lwFltDepthAttach = std::make_unique<SldAttach>(apvts, "luxwaveFilterEnvDepth", lwFltDepthSlider);

    initKnob(lwLfoRateSlider, " Hz");  addAndMakeVisible(lwLfoRateSlider);
    lwLfoRateAttach = std::make_unique<SldAttach>(apvts, "luxwaveLfoRate", lwLfoRateSlider);
    initKnob(lwLfoDepthSlider);        addAndMakeVisible(lwLfoDepthSlider);
    lwLfoDepthAttach = std::make_unique<SldAttach>(apvts, "luxwaveLfoDepth", lwLfoDepthSlider);
}

void AudioWavePanel::paint(juce::Graphics& g)
{
    const int gx = kHPad;
    const int gw = getWidth() - 2 * kHPad;

    drawSectionBg(g, gx - 2, 0, gw + 4, getHeight() - 2);
    drawBadge(g, gx, 0, gw, 0xff3a2a1a, 0xffddaa44, "AUDIOWAVE");

    // "Scan" caption before the combo (now at the left of the strip).
    g.setColour(juce::Colour(0xff888888));
    g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
    g.drawText("Scan", juce::Rectangle<int>(gx, shStripY() - 1, 40, kCtrlH),
               juce::Justification::centredLeft, true);

    drawEnvCaption(g, gx, shCaptionY(), gw, 0xffddaa44, "AMPLITUDE  ADSR");

    const int gy = shGridY();
    static const char* const lbls[] = { "Volume", "Amplitude", "Flt Cutoff",
                                        "Flt Env Depth", "LFO Rate", "LFO Depth" };
    for (int i = 0; i < 6; ++i) drawKnobLabel(g, gx, gw, gy, i, lbls[i]);
}

void AudioWavePanel::resized()
{
    const int gx = kHPad;
    const int gw = getWidth() - 2 * kHPad;

    lwScanModeCombo.setBounds(gx + 40, shStripY(), 130, kCtrlH);

    volEnv->setBounds(gx, shEnvY(), gw, kEnvH);

    const int gy = shGridY();
    placeKnob(luxwaveVolumeSlider, gx, gw, gy, 0);
    placeKnob(lwAmplitudeSlider,   gx, gw, gy, 1);
    placeKnob(lwFltCutoffSlider,   gx, gw, gy, 2);
    placeKnob(lwFltDepthSlider,    gx, gw, gy, 3);
    placeKnob(lwLfoRateSlider,     gx, gw, gy, 4);
    placeKnob(lwLfoDepthSlider,    gx, gw, gy, 5);
}
