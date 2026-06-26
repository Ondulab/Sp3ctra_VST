#include "EngineAudioPanels.h"

// ── Shared layout helpers — rotary-knob grid (mockup-validated layout) ────────
//    Sections stack full-width; continuous params are knobs in a fixed-column
//    grid, on/off params sit in a toggle strip under the section badge.
namespace
{
    using namespace AudioPanelLayout;
    using namespace AudioPanelUI;   // initKnob/placeKnob/drawBadge/... (shared header)

    constexpr int kHPad     = Sp3ctraTheme::kHPad;
    constexpr int kSectionH = Sp3ctraTheme::kSectionH;
    constexpr int kSecGap   = Sp3ctraTheme::kSectionGap;
    constexpr int kCtrlH    = Sp3ctraTheme::kControlH;

    // Vertical anatomy of a section that hosts an envelope editor (top at 0):
    // badge → enable strip → caption → editor → knob grid.
    int shStripY()   { return kSectionH + kSecGap; }
    int shCaptionY() { return shStripY() + kCtrlH + kToggleGap; }
    int shEnvY()     { return shCaptionY() + kEnvCaptionH; }
    int shGridY()    { return shEnvY() + kEnvH + kEnvGap; }
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
