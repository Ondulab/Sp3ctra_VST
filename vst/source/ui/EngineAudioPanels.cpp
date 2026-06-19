#include "EngineAudioPanels.h"

// ── Shared layout helpers (mirrors the former PluginEditor SYNTH-page math,
//    with kPageTop folded out: panel-local y starts at 0) ─────────────────────
namespace
{
    constexpr int kHPad       = Sp3ctraTheme::kHPad;
    constexpr int kColGap     = 18;
    constexpr int kSectionH   = Sp3ctraTheme::kSectionH;
    constexpr int kSectionGap = Sp3ctraTheme::kSectionGap;
    constexpr int kRowH       = Sp3ctraTheme::kControlH;
    constexpr int kRowStep    = Sp3ctraTheme::kRowStep;
    constexpr int kLabelW     = Sp3ctraTheme::kLabelW;
    constexpr int kCtrlOffset = kLabelW + 8;

    int colWidth(int totalW) noexcept { return (totalW - 2 * kHPad - kColGap) / 2; }
    int colLX()              noexcept { return kHPad; }
    int colRX(int totalW)    noexcept { return kHPad + colWidth(totalW) + kColGap; }
    int rowsStartY()         noexcept { return kSectionH + kSectionGap; }

    void initSlider(juce::Slider& s, const char* suffix = nullptr)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false,
                          Sp3ctraTheme::kTbStd, Sp3ctraTheme::kTextBoxH);
        if (suffix) s.setTextValueSuffix(suffix);
    }

    void drawBadge(juce::Graphics& g, int x, int w,
                   juce::uint32 bg, juce::uint32 fg, const char* text)
    {
        g.setColour(juce::Colour(bg));
        g.fillRoundedRectangle(juce::Rectangle<int>(x, 0, w, kSectionH).toFloat(), 3.f);
        g.setColour(juce::Colour(fg));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText(text, juce::Rectangle<int>(x + 6, 0, w - 12, kSectionH),
                   juce::Justification::centredLeft, true);
    }

    void drawRowLabels(juce::Graphics& g, int x, int count, const char* const* labels)
    {
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.setColour(juce::Colour(0xffb8c4d0));
        for (int i = 0; i < count; ++i)
            g.drawText(labels[i],
                       juce::Rectangle<int>(x, rowsStartY() + i * kRowStep, kLabelW, kRowH),
                       juce::Justification::centredRight, true);
    }
}

//==============================================================================
// AudioStralPanel
//==============================================================================
AudioStralPanel::AudioStralPanel(Sp3ctraAudioProcessor& p)
{
    auto& apvts = p.getAPVTS();

    deviceOnToggle.setButtonText("Active");
    addAndMakeVisible(deviceOnToggle);
    deviceOnAttachment = std::make_unique<BtnAttach>(apvts, "deviceEnabled", deviceOnToggle);

    initSlider(luxstralVolumeSlider);
    addAndMakeVisible(luxstralVolumeSlider);
    luxstralVolumeAttachment = std::make_unique<SldAttach>(apvts, "luxstralVolume", luxstralVolumeSlider);

    initSlider(attackSlider, " ms");
    addAndMakeVisible(attackSlider);
    attackAttachment = std::make_unique<SldAttach>(apvts, "luxstralAttackMs", attackSlider);

    initSlider(releaseSlider, " ms");
    addAndMakeVisible(releaseSlider);
    releaseAttachment = std::make_unique<SldAttach>(apvts, "luxstralReleaseMs", releaseSlider);

    stereoEnableToggle.setButtonText("Active");
    addAndMakeVisible(stereoEnableToggle);
    stereoEnableAttachment = std::make_unique<BtnAttach>(apvts, "luxstralStereoEnable", stereoEnableToggle);

    initSlider(stereoTempSlider);
    addAndMakeVisible(stereoTempSlider);
    stereoTempAttachment = std::make_unique<SldAttach>(apvts, "luxstralStereoTempAmp", stereoTempSlider);

    initSlider(sumExpSlider);
    addAndMakeVisible(sumExpSlider);
    sumExpAttachment = std::make_unique<SldAttach>(apvts, "luxstralSummationResponseExp", sumExpSlider);

    initSlider(noiseGateSlider);
    addAndMakeVisible(noiseGateSlider);
    noiseGateAttachment = std::make_unique<SldAttach>(apvts, "luxstralNoiseGateThreshold", noiseGateSlider);

    // ── StrokeForge (right column) ────────────────────────────────────────────
    sfEnabledToggle.setButtonText("StrokeForge Active");
    addAndMakeVisible(sfEnabledToggle);
    sfEnabledAttachment = std::make_unique<BtnAttach>(apvts, "sfEnabled", sfEnabledToggle);

    initSlider(sfMorphWidthSlider);
    addAndMakeVisible(sfMorphWidthSlider);
    sfMorphWidthAttachment = std::make_unique<SldAttach>(apvts, "sfMorphWidthScale", sfMorphWidthSlider);

    initSlider(sfFocusSigmaSlider, " notes");
    addAndMakeVisible(sfFocusSigmaSlider);
    sfFocusSigmaAttachment = std::make_unique<SldAttach>(apvts, "sfBlobFocusSigma", sfFocusSigmaSlider);

    initSlider(sfSpectralThreshSlider, " notes");
    addAndMakeVisible(sfSpectralThreshSlider);
    sfSpectralThreshAttachment = std::make_unique<SldAttach>(apvts, "sfSpectralWidthThreshold", sfSpectralThreshSlider);

    sfFocusOnlyToggle.setButtonText("Focus Without Morph");
    addAndMakeVisible(sfFocusOnlyToggle);
    sfFocusOnlyAttachment = std::make_unique<BtnAttach>(apvts, "sfFocusOnly", sfFocusOnlyToggle);
}

void AudioStralPanel::paint(juce::Graphics& g)
{
    const int W   = getWidth();
    const int cw  = colWidth(W);
    const int lxp = colLX();
    const int rxp = colRX(W);

    // AUDIOSTRAL section badge (left column)
    drawBadge(g, lxp, cw, 0xff1c3755, 0xff7ab0f0, "AUDIOSTRAL");

    static const char* const lsLbls[] = {
        "Device On", "Volume", "Attack", "Release",
        "Stereo", "Stereo Temp.", "Sum. Exp.", "Noise Gate"
    };
    drawRowLabels(g, lxp, 8, lsLbls);

    // ── StrokeForge section (right column) ────────────────────────────────────
    constexpr int kSF_ROWS = 5;
    const auto sfBox = juce::Rectangle<int>(rxp, 0, cw,
        kSectionH + kSectionGap + kSF_ROWS * kRowStep + kSectionGap).toFloat();

    g.setColour(juce::Colour(0xff131320));
    g.fillRoundedRectangle(sfBox, 4.f);
    g.setColour(juce::Colour(0xff2a2a40));
    g.drawRoundedRectangle(sfBox, 4.f, 1.f);

    g.setColour(juce::Colour(0xff2c1f4a));
    g.fillRoundedRectangle(
        juce::Rectangle<int>(rxp + 4, 4, cw - 8, kSectionH - 2).toFloat(), 3.f);
    g.setColour(juce::Colour(0xffb07af0));
    g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
    g.drawText("STROKEFORGE  --  Waveform Morphing  (Sine -> Square)",
               juce::Rectangle<int>(rxp + 10, 4, cw - 20, kSectionH - 2),
               juce::Justification::centredLeft, true);

    static const char* const sfLbls[kSF_ROWS] = {
        "Enable", "Square at Width", "Focus Sigma",
        "Spectral Threshold", "Focus Only (spectral)"
    };
    drawRowLabels(g, rxp + 4, kSF_ROWS, sfLbls);
}

void AudioStralPanel::resized()
{
    const int W   = getWidth();
    const int cw  = colWidth(W);
    const int rsy = rowsStartY();

    // Left column
    {
        const int cx = colLX() + kCtrlOffset;
        const int cwL = juce::jmax(80, cw - kCtrlOffset);
        int cy = rsy;
        deviceOnToggle      .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        luxstralVolumeSlider.setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        attackSlider        .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        releaseSlider       .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        stereoEnableToggle  .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        stereoTempSlider    .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        sumExpSlider        .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        noiseGateSlider     .setBounds(cx, cy, cwL, kRowH);
    }

    // Right column (StrokeForge)
    {
        const int rxp = colRX(W);
        const int cx  = rxp + kCtrlOffset + 4;
        const int cw2 = juce::jmax(80, cw - kCtrlOffset - 12);
        int cy = rsy;
        sfEnabledToggle       .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        sfMorphWidthSlider    .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        sfFocusSigmaSlider    .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        sfSpectralThreshSlider.setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        sfFocusOnlyToggle     .setBounds(cx, cy, cw2, kRowH);
    }
}

//==============================================================================
// AudioSynthPanel
//==============================================================================
AudioSynthPanel::AudioSynthPanel(Sp3ctraAudioProcessor& p)
{
    auto& apvts = p.getAPVTS();

    lxEnableToggle.setButtonText("Active");
    addAndMakeVisible(lxEnableToggle);
    lxEnableAttachment = std::make_unique<BtnAttach>(apvts, "luxsynthEnabled", lxEnableToggle);

    initSlider(luxsynthVolumeSlider);
    addAndMakeVisible(luxsynthVolumeSlider);
    luxsynthVolumeAttachment = std::make_unique<SldAttach>(apvts, "luxsynthVolume", luxsynthVolumeSlider);

    initSlider(lxAttackSlider, " ms");   addAndMakeVisible(lxAttackSlider);
    lxAttackAttach = std::make_unique<SldAttach>(apvts, "luxsynthAttackMs", lxAttackSlider);
    initSlider(lxDecaySlider, " ms");    addAndMakeVisible(lxDecaySlider);
    lxDecayAttach = std::make_unique<SldAttach>(apvts, "luxsynthDecayMs", lxDecaySlider);
    initSlider(lxSustainSlider);         addAndMakeVisible(lxSustainSlider);
    lxSustainAttach = std::make_unique<SldAttach>(apvts, "luxsynthSustainLevel", lxSustainSlider);
    initSlider(lxReleaseSlider, " ms");  addAndMakeVisible(lxReleaseSlider);
    lxReleaseAttach = std::make_unique<SldAttach>(apvts, "luxsynthReleaseMs", lxReleaseSlider);

    initSlider(lxFltAttackSlider, " ms");  addAndMakeVisible(lxFltAttackSlider);
    lxFltAttackAttach = std::make_unique<SldAttach>(apvts, "luxsynthFilterAttackMs", lxFltAttackSlider);
    initSlider(lxFltDecaySlider, " ms");   addAndMakeVisible(lxFltDecaySlider);
    lxFltDecayAttach = std::make_unique<SldAttach>(apvts, "luxsynthFilterDecayMs", lxFltDecaySlider);
    initSlider(lxFltSustainSlider);        addAndMakeVisible(lxFltSustainSlider);
    lxFltSustainAttach = std::make_unique<SldAttach>(apvts, "luxsynthFilterSustain", lxFltSustainSlider);
    initSlider(lxFltReleaseSlider, " ms"); addAndMakeVisible(lxFltReleaseSlider);
    lxFltReleaseAttach = std::make_unique<SldAttach>(apvts, "luxsynthFilterReleaseMs", lxFltReleaseSlider);
    initSlider(lxFltCutoffSlider, " Hz");  addAndMakeVisible(lxFltCutoffSlider);
    lxFltCutoffAttach = std::make_unique<SldAttach>(apvts, "luxsynthFilterCutoff", lxFltCutoffSlider);
    initSlider(lxFltDepthSlider);          addAndMakeVisible(lxFltDepthSlider);
    lxFltDepthAttach = std::make_unique<SldAttach>(apvts, "luxsynthFilterEnvDepth", lxFltDepthSlider);

    initSlider(lxNumOscSlider);   addAndMakeVisible(lxNumOscSlider);
    lxNumOscAttach = std::make_unique<SldAttach>(apvts, "luxsynthNumOscillators", lxNumOscSlider);

    initSlider(lxLfoRateSlider, " Hz");  addAndMakeVisible(lxLfoRateSlider);
    lxLfoRateAttach = std::make_unique<SldAttach>(apvts, "luxsynthLfoRate", lxLfoRateSlider);
    initSlider(lxLfoDepthSlider);        addAndMakeVisible(lxLfoDepthSlider);
    lxLfoDepthAttach = std::make_unique<SldAttach>(apvts, "luxsynthLfoDepth", lxLfoDepthSlider);
}

void AudioSynthPanel::paint(juce::Graphics& g)
{
    const int W   = getWidth();
    const int cw  = colWidth(W);
    const int lxp = colLX();
    const int rxp = colRX(W);

    // Left column badge — VOLUME ADSR (teal)
    drawBadge(g, lxp, cw, 0xff1a3a3a, 0xff66ccaa, "AUDIOSYNTH  --  Volume ADSR + Spectral");

    static const char* const lxLeftLbls[] = {
        "Enable", "Volume", "Attack", "Decay", "Sustain", "Release",
        "Oscillators", "LFO Rate"
    };
    drawRowLabels(g, lxp, 8, lxLeftLbls);

    // Right column badge — FILTER ADSR (purple-pink)
    drawBadge(g, rxp, cw, 0xff2a1a3a, 0xffcc88cc, "FILTER ADSR + LFO");

    static const char* const lxRightLbls[] = {
        "Flt. Attack", "Flt. Decay", "Flt. Sustain", "Flt. Release",
        "Cutoff", "Env Depth", "LFO Depth"
    };
    drawRowLabels(g, rxp + 4, 7, lxRightLbls);
}

void AudioSynthPanel::resized()
{
    const int W   = getWidth();
    const int cw  = colWidth(W);
    const int rsy = rowsStartY();

    // Left column = vol ADSR + spectral
    {
        const int cx  = colLX() + kCtrlOffset;
        const int cwL = juce::jmax(80, cw - kCtrlOffset);
        int cy = rsy;
        lxEnableToggle      .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        luxsynthVolumeSlider.setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lxAttackSlider      .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lxDecaySlider       .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lxSustainSlider     .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lxReleaseSlider     .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lxNumOscSlider      .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lxLfoRateSlider     .setBounds(cx, cy, cwL, kRowH);
    }

    // Right column = filter ADSR + LFO
    {
        const int rxp = colRX(W);
        const int cx  = rxp + kCtrlOffset + 4;
        const int cw2 = juce::jmax(80, cw - kCtrlOffset - 12);
        int cy = rsy;
        lxFltAttackSlider .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxFltDecaySlider  .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxFltSustainSlider.setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxFltReleaseSlider.setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxFltCutoffSlider .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxFltDepthSlider  .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lxLfoDepthSlider  .setBounds(cx, cy, cw2, kRowH);
    }
}

//==============================================================================
// AudioWavePanel
//==============================================================================
AudioWavePanel::AudioWavePanel(Sp3ctraAudioProcessor& p)
{
    auto& apvts = p.getAPVTS();

    lwEnableToggle.setButtonText("Active");
    addAndMakeVisible(lwEnableToggle);
    lwEnableAttachment = std::make_unique<BtnAttach>(apvts, "luxwaveEnabled", lwEnableToggle);

    initSlider(luxwaveVolumeSlider);
    addAndMakeVisible(luxwaveVolumeSlider);
    luxwaveVolumeAttachment = std::make_unique<SldAttach>(apvts, "luxwaveVolume", luxwaveVolumeSlider);

    initSlider(lwAttackSlider, " ms");   addAndMakeVisible(lwAttackSlider);
    lwAttackAttach = std::make_unique<SldAttach>(apvts, "luxwaveAttackMs", lwAttackSlider);
    initSlider(lwDecaySlider, " ms");    addAndMakeVisible(lwDecaySlider);
    lwDecayAttach = std::make_unique<SldAttach>(apvts, "luxwaveDecayMs", lwDecaySlider);
    initSlider(lwSustainSlider);         addAndMakeVisible(lwSustainSlider);
    lwSustainAttach = std::make_unique<SldAttach>(apvts, "luxwaveSustainLevel", lwSustainSlider);
    initSlider(lwReleaseSlider, " ms");  addAndMakeVisible(lwReleaseSlider);
    lwReleaseAttach = std::make_unique<SldAttach>(apvts, "luxwaveReleaseMs", lwReleaseSlider);

    initSlider(lwFltCutoffSlider, " Hz"); addAndMakeVisible(lwFltCutoffSlider);
    lwFltCutoffAttach = std::make_unique<SldAttach>(apvts, "luxwaveFilterCutoff", lwFltCutoffSlider);
    initSlider(lwFltDepthSlider, " Hz");  addAndMakeVisible(lwFltDepthSlider);
    lwFltDepthAttach = std::make_unique<SldAttach>(apvts, "luxwaveFilterEnvDepth", lwFltDepthSlider);

    initSlider(lwLfoRateSlider, " Hz");  addAndMakeVisible(lwLfoRateSlider);
    lwLfoRateAttach = std::make_unique<SldAttach>(apvts, "luxwaveLfoRate", lwLfoRateSlider);
    initSlider(lwLfoDepthSlider);        addAndMakeVisible(lwLfoDepthSlider);
    lwLfoDepthAttach = std::make_unique<SldAttach>(apvts, "luxwaveLfoDepth", lwLfoDepthSlider);

    initSlider(lwAmplitudeSlider);       addAndMakeVisible(lwAmplitudeSlider);
    lwAmplitudeAttach = std::make_unique<SldAttach>(apvts, "luxwaveAmplitude", lwAmplitudeSlider);

    addAndMakeVisible(lwScanModeCombo);
    lwScanModeCombo.addItem("Forward",   1);
    lwScanModeCombo.addItem("Ping-Pong", 2);
    lwScanModeCombo.addItem("Random",    3);
    lwScanModeAttach = std::make_unique<CmbAttach>(apvts, "luxwaveScanMode", lwScanModeCombo);
}

void AudioWavePanel::paint(juce::Graphics& g)
{
    const int W   = getWidth();
    const int cw  = colWidth(W);
    const int lxp = colLX();
    const int rxp = colRX(W);

    // Left column badge — Wavetable ADSR (orange-gold)
    drawBadge(g, lxp, cw, 0xff3a2a1a, 0xffddaa44, "AUDIOWAVE  --  Wavetable ADSR + Scan");

    static const char* const lwLeftLbls[] = {
        "Enable", "Volume", "Amplitude", "Attack", "Decay", "Sustain", "Release",
        "Scan Mode"
    };
    drawRowLabels(g, lxp, 8, lwLeftLbls);

    // Right column badge — Filter + LFO (teal-dark)
    drawBadge(g, rxp, cw, 0xff1a2a3a, 0xff66aacc, "FILTER + LFO");

    static const char* const lwRightLbls[] = {
        "Flt. Cutoff", "Flt. Env Depth", "LFO Rate", "LFO Depth"
    };
    drawRowLabels(g, rxp + 4, 4, lwRightLbls);
}

void AudioWavePanel::resized()
{
    const int W   = getWidth();
    const int cw  = colWidth(W);
    const int rsy = rowsStartY();

    // Left column = ADSR + Volume
    {
        const int cx  = colLX() + kCtrlOffset;
        const int cwL = juce::jmax(80, cw - kCtrlOffset);
        int cy = rsy;
        lwEnableToggle     .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        luxwaveVolumeSlider.setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lwAmplitudeSlider  .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lwAttackSlider     .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lwDecaySlider      .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lwSustainSlider    .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lwReleaseSlider    .setBounds(cx, cy, cwL, kRowH); cy += kRowStep;
        lwScanModeCombo    .setBounds(cx, cy, cwL, kRowH);
    }

    // Right column = Filter + LFO
    {
        const int rxp = colRX(W);
        const int cx  = rxp + kCtrlOffset + 4;
        const int cw2 = juce::jmax(80, cw - kCtrlOffset - 12);
        int cy = rsy;
        lwFltCutoffSlider.setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lwFltDepthSlider .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lwLfoRateSlider  .setBounds(cx, cy, cw2, kRowH); cy += kRowStep;
        lwLfoDepthSlider .setBounds(cx, cy, cw2, kRowH);
    }
}
