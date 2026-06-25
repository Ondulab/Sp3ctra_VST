#include "ScoreSetupPanel.h"
#include "SetupHeader.h"
#include "../../UITheme.h"

ScoreSetupPanel::ScoreSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour)
    : proc(processor),
      apvts(processor.getAPVTS()),
      accent(accentColour),
      settings(processor.getScoreSettings())
{
    // ── Frequency range section (mirror LuxStral or manual override) ─────
    freqSectionLabel.setText("Frequency Range", juce::dontSendNotification);
    freqSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    freqSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    addAndMakeVisible(freqSectionLabel);

    manualToggle.setButtonText("Manual (override LuxStral)");
    manualToggle.setToggleState(proc.getScoreFreqOverride().manual, juce::dontSendNotification);
    manualToggle.onClick = [this]
    {
        auto& ov = proc.getScoreFreqOverride();
        ov.manual = manualToggle.getToggleState();
        if (ov.manual)
        {
            // Hand off smoothly: seed manual values from the displayed mirror.
            ov.tuning    = tuningSlider.getValue();
            ov.rootIndex = rootCombo.getSelectedId() - 1;
            ov.octaves   = (int) octavesSlider.getValue();
        }
        refreshFreqControls();
    };
    addAndMakeVisible(manualToggle);

    initLabel(tuningLabel, "Tuning (A4)");
    tuningSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    tuningSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, Sp3ctraTheme::kControlH);
    tuningSlider.setTextValueSuffix(" Hz");
    tuningSlider.setRange(415.0, 466.0, 0.1);
    tuningSlider.onValueChange = [this]
    {
        auto& ov = proc.getScoreFreqOverride();
        if (ov.manual) { ov.tuning = tuningSlider.getValue(); updateRangeInfo(); }
    };
    addAndMakeVisible(tuningSlider);

    initLabel(rootLabel, "Root Note");
    {
        const char* n[] = { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
        for (int oct = 1; oct <= 6; ++oct)
            for (int note = 0; note < 12; ++note)
                rootCombo.addItem(juce::String(n[note]) + juce::String(oct),
                                  (oct - 1) * 12 + note + 1);
    }
    rootCombo.onChange = [this]
    {
        auto& ov = proc.getScoreFreqOverride();
        if (ov.manual) { ov.rootIndex = rootCombo.getSelectedId() - 1; updateRangeInfo(); }
    };
    addAndMakeVisible(rootCombo);

    initLabel(octavesLabel, "Octaves");
    octavesSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    octavesSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, Sp3ctraTheme::kControlH);
    octavesSlider.setRange(1, 10, 1);
    octavesSlider.onValueChange = [this]
    {
        auto& ov = proc.getScoreFreqOverride();
        if (ov.manual) { ov.octaves = (int) octavesSlider.getValue(); updateRangeInfo(); }
    };
    addAndMakeVisible(octavesSlider);

    rangeInfoLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).italicised());
    rangeInfoLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible(rangeInfoLabel);

    // ── Image processing parameters ──────────────────────────────────────
    initLabel(dynLabel,      "Dynamic Range (dB)");
    initLabel(gammaLabel,    "Gamma");
    initLabel(contrastLabel, "Contrast");
    initLabel(boostLabel,    "HF Boost Alpha");
    initLabel(hpLabel,       "High-Pass Cutoff (Hz)");
    initLabel(gateLabel,     "Gate Threshold");

    initSlider(dynSlider,      20.0, 120.0,  1.0,  settings.dynamicRangeDB);
    initSlider(gammaSlider,    0.2, 3.0,     0.05, settings.gammaCorrection);
    initSlider(contrastSlider, 0.5, 4.0,     0.05, settings.contrastFactor);
    initSlider(boostSlider,    0.0, 1.0,     0.01, settings.highBoostAlpha);
    initSlider(hpSlider,       20.0, 1000.0, 1.0,  settings.highPassCutoffFreq);
    initSlider(gateSlider,     0.0, 0.9,     0.01, settings.noiseGateThreshold);

    dynSlider.onValueChange      = [this] { settings.dynamicRangeDB     = dynSlider.getValue();      };
    gammaSlider.onValueChange    = [this] { settings.gammaCorrection    = gammaSlider.getValue();    };
    contrastSlider.onValueChange = [this] { settings.contrastFactor     = contrastSlider.getValue(); };
    boostSlider.onValueChange    = [this] { settings.highBoostAlpha     = boostSlider.getValue();    };
    hpSlider.onValueChange       = [this] { settings.highPassCutoffFreq = hpSlider.getValue();       };
    gateSlider.onValueChange     = [this] { settings.noiseGateThreshold = gateSlider.getValue();     };

    initLabel(pageLabel, "Page Format");
    pageCombo.addItem("A4 Portrait", 1);
    pageCombo.addItem("A3 Landscape", 2);
    pageCombo.setSelectedId(settings.pageFormat == 1 ? 2 : 1, juce::dontSendNotification);
    pageCombo.onChange = [this] { settings.pageFormat = (pageCombo.getSelectedId() == 2) ? 1 : 0; };
    addAndMakeVisible(pageCombo);

    initLabel(overlapLabel, "Overlap");
    overlapCombo.addItem("Low",    1);
    overlapCombo.addItem("Medium", 2);
    overlapCombo.addItem("High",   3);
    overlapCombo.setSelectedId(settings.overlapPreset + 1, juce::dontSendNotification);
    overlapCombo.onChange = [this]
    { settings.overlapPreset = juce::jlimit(0, 2, overlapCombo.getSelectedId() - 1); };
    addAndMakeVisible(overlapCombo);

    initLabel(dpiLabel, "Printer DPI");
    for (int d : { 200, 300, 400, 600, 800 })
        dpiCombo.addItem(juce::String(d), d);
    dpiCombo.setSelectedId((int) settings.printerDpi, juce::dontSendNotification);
    dpiCombo.onChange = [this]
    { settings.printerDpi = (double) juce::jmax(72, dpiCombo.getSelectedId()); };
    addAndMakeVisible(dpiCombo);

    initToggle(boostToggle,  "HF Boost",   settings.enableHighBoost != 0);
    initToggle(hpToggle,     "High-Pass",  settings.enableHighPassFilter != 0);
    initToggle(normToggle,   "Normalize",  settings.enableNormalization != 0);
    initToggle(ditherToggle, "Dither",     settings.enableDithering != 0);
    initToggle(gateToggle,   "Noise Gate", settings.enableNoiseGate != 0);

    boostToggle.onClick  = [this] { settings.enableHighBoost      = boostToggle.getToggleState()  ? 1 : 0; };
    hpToggle.onClick     = [this] { settings.enableHighPassFilter = hpToggle.getToggleState()     ? 1 : 0; };
    normToggle.onClick   = [this] { settings.enableNormalization  = normToggle.getToggleState()   ? 1 : 0; };
    ditherToggle.onClick = [this] { settings.enableDithering      = ditherToggle.getToggleState() ? 1 : 0; };
    gateToggle.onClick   = [this] { settings.enableNoiseGate      = gateToggle.getToggleState()   ? 1 : 0; };

    refreshFreqControls();
    startTimerHz(5);   // mirror live LuxStral values while not in manual mode
}

ScoreSetupPanel::~ScoreSetupPanel() { stopTimer(); }

void ScoreSetupPanel::initLabel(juce::Label& l, const juce::String& text)
{
    l.setText(text, juce::dontSendNotification);
    l.setJustificationType(juce::Justification::centredRight);
    l.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
    addAndMakeVisible(l);
}

void ScoreSetupPanel::initSlider(juce::Slider& s, double lo, double hi, double step, double val)
{
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, Sp3ctraTheme::kControlH);
    s.setRange(lo, hi, step);
    s.setValue(val, juce::dontSendNotification);
    addAndMakeVisible(s);
}

void ScoreSetupPanel::initToggle(juce::ToggleButton& t, const juce::String& text, bool on)
{
    t.setButtonText(text);
    t.setToggleState(on, juce::dontSendNotification);
    addAndMakeVisible(t);
}

void ScoreSetupPanel::refreshFreqControls()
{
    const auto& ov = proc.getScoreFreqOverride();
    const bool man = ov.manual;

    manualToggle.setToggleState(man, juce::dontSendNotification);
    tuningSlider.setEnabled(man);
    rootCombo.setEnabled(man);
    octavesSlider.setEnabled(man);

    // Grey the whole control (track + thumb + textbox) when following LuxStral.
    const float a = man ? 1.0f : 0.45f;
    tuningSlider.setAlpha(a);
    rootCombo.setAlpha(a);
    octavesSlider.setAlpha(a);
    tuningLabel.setAlpha(a);
    rootLabel.setAlpha(a);
    octavesLabel.setAlpha(a);

    if (man)
    {
        tuningSlider.setValue(ov.tuning, juce::dontSendNotification);
        rootCombo.setSelectedId(ov.rootIndex + 1, juce::dontSendNotification);
        octavesSlider.setValue(ov.octaves, juce::dontSendNotification);
    }
    else
    {
        // Mirror LuxStral's current musical tuning (read-only display).
        if (auto* t = apvts.getRawParameterValue("luxstralTuning"))
            tuningSlider.setValue((double) t->load(), juce::dontSendNotification);
        if (auto* r = apvts.getRawParameterValue("luxstralRootNote"))
            rootCombo.setSelectedId((int) r->load() + 1, juce::dontSendNotification);
        if (auto* o = apvts.getRawParameterValue("luxstralNumOctaves"))
            octavesSlider.setValue((double) (int) o->load(), juce::dontSendNotification);
    }
    updateRangeInfo();
}

void ScoreSetupPanel::updateRangeInfo()
{
    double lo = 0.0, hi = 0.0;
    proc.getScoreFrequencyRange(lo, hi);
    rangeInfoLabel.setText("Range: " + juce::String(lo, 1) + " Hz  -  "
                               + juce::String(hi, 0) + " Hz",
                           juce::dontSendNotification);
}

void ScoreSetupPanel::timerCallback()
{
    // While following LuxStral, keep the read-only mirror live.
    if (!proc.getScoreFreqOverride().manual)
        refreshFreqControls();
}

void ScoreSetupPanel::paint(juce::Graphics& g)
{
    SetupUI::paintHeader(g, *this, "SCORE -- SETUP", accent);
}

void ScoreSetupPanel::resized()
{
    auto area = getLocalBounds().reduced(Sp3ctraTheme::kHPad, 0);
    area.removeFromTop(SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap);

    constexpr int rowH   = Sp3ctraTheme::kControlH;
    constexpr int gap    = Sp3ctraTheme::kRowGap * 2;
    constexpr int labelW = Sp3ctraTheme::kLabelW + 40;   // longer labels here
    const int ctrlW      = juce::jmin(260, area.getWidth() - labelW - Sp3ctraTheme::kGap);

    auto row = [&]() -> juce::Rectangle<int>
    {
        auto r = area.removeFromTop(rowH);
        area.removeFromTop(gap);
        return r;
    };
    auto labelled = [&](juce::Component& label, juce::Component& ctl)
    {
        auto r = row();
        label.setBounds(r.removeFromLeft(labelW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        ctl.setBounds(r.removeFromLeft(ctrlW));
    };

    // ── Frequency range section ──
    freqSectionLabel.setBounds(row());
    { auto r = row(); manualToggle.setBounds(r.removeFromLeft(labelW + Sp3ctraTheme::kGap + ctrlW)); }
    labelled(tuningLabel,  tuningSlider);
    labelled(rootLabel,    rootCombo);
    labelled(octavesLabel, octavesSlider);
    { auto r = row(); rangeInfoLabel.setBounds(r.removeFromLeft(labelW + Sp3ctraTheme::kGap + ctrlW)); }

    area.removeFromTop(Sp3ctraTheme::kRowGap);   // small breather

    // ── Image processing ──
    labelled(dynLabel,      dynSlider);
    labelled(gammaLabel,    gammaSlider);
    labelled(contrastLabel, contrastSlider);
    labelled(boostLabel,    boostSlider);
    labelled(hpLabel,       hpSlider);
    labelled(gateLabel,     gateSlider);
    labelled(pageLabel,     pageCombo);
    labelled(overlapLabel,  overlapCombo);
    labelled(dpiLabel,      dpiCombo);

    const int colW = juce::jmin(160, (area.getWidth() - Sp3ctraTheme::kGap) / 2);
    {
        auto r = row();
        boostToggle.setBounds(r.removeFromLeft(colW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        hpToggle.setBounds(r.removeFromLeft(colW));
    }
    {
        auto r = row();
        normToggle.setBounds(r.removeFromLeft(colW));
        r.removeFromLeft(Sp3ctraTheme::kGap);
        ditherToggle.setBounds(r.removeFromLeft(colW));
    }
    {
        auto r = row();
        gateToggle.setBounds(r.removeFromLeft(colW));
    }
}
