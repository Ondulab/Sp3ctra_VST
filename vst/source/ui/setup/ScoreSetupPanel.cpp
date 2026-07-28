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

    // ── Image processing — only the PhonoPaper-conforming Dynamic Range ────
    initLabel(dynLabel, "Dynamic Range (dB)");
    initSlider(dynSlider, 20.0, 120.0, 1.0, settings.dynamicRangeDB);
    dynSlider.onValueChange = [this] { settings.dynamicRangeDB = dynSlider.getValue(); };

    // ── Print size — spectro band height = CIS sensor length ───────────────
    printSectionLabel.setText("Print Size", juce::dontSendNotification);
    printSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    printSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    addAndMakeVisible(printSectionLabel);

    heightManualToggle.setButtonText("Manual (override CIS height)");
    heightManualToggle.setTooltip(
        "Locked to the Sp3ctra CIS sensor length (219.456 mm = 8.64\"). Print the\n"
        "score at 100% / real size so the band height matches the sensor and plays\n"
        "in tune. Enable only to compensate a printer that can't scale to 100%.");
    heightManualToggle.setToggleState(settings.spectroHeightManual != 0, juce::dontSendNotification);
    heightManualToggle.onClick = [this]
    {
        const bool man = heightManualToggle.getToggleState();
        settings.spectroHeightManual = man ? 1 : 0;
        if (! man)
            settings.spectroHeightMM = SCORE_CIS_HEIGHT_MM;   // re-lock to the sensor
        refreshHeightControls();
    };
    addAndMakeVisible(heightManualToggle);

    initLabel(heightLabel, "Band Height (mm)");
    initSlider(heightSlider, 180.0, 260.0, 0.001, settings.spectroHeightMM);
    heightSlider.setNumDecimalPlacesToDisplay(3);
    heightSlider.setTextValueSuffix(" mm");
    heightSlider.onValueChange = [this]
    {
        if (settings.spectroHeightManual)
            settings.spectroHeightMM = heightSlider.getValue();
    };

    // ── Export — format / sheet size / DPI (moved off the PLAY page) ───────
    exportSectionLabel.setText("Export", juce::dontSendNotification);
    exportSectionLabel.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSettings)).boldened());
    exportSectionLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
    addAndMakeVisible(exportSectionLabel);

    initLabel(formatLabel, "Image Format");
    formatCombo.addItem("PNG",  1);
    formatCombo.addItem("JPEG", 2);
    formatCombo.setSelectedId(
        (bool) apvts.state.getProperty("scoreExportPng", true) ? 1 : 2,
        juce::dontSendNotification);
    formatCombo.onChange = [this]
    {
        apvts.state.setProperty("scoreExportPng",
                                formatCombo.getSelectedId() == 1, nullptr);
    };
    addAndMakeVisible(formatCombo);

    initLabel(pageLabel, "Page");
    pageCombo.addItem("A4 Portrait",  1);
    pageCombo.addItem("A3 Landscape", 2);
    pageCombo.addItem("Selection",    3);
    pageCombo.setTooltip("A4/A3: fixed sheets — the waveform picker slides a "
                         "one-page window over the file. Selection: pick a "
                         "free region in the waveform (drag its edges to "
                         "resize); the sheet stretches to hold it. Takes "
                         "effect on the next GENERATE.");
    pageCombo.setSelectedId(juce::jlimit(0, 2, settings.pageFormat) + 1,
                            juce::dontSendNotification);
    pageCombo.onChange = [this]
    {
        // The PLAY page's timer mirrors this into the region-picker window.
        settings.pageFormat = pageCombo.getSelectedId() - 1;
    };
    addAndMakeVisible(pageCombo);

    initLabel(dpiLabel, "Printer DPI");
    for (int d : { 200, 300, 400, 600, 800 })
        dpiCombo.addItem(juce::String(d), d);
    dpiCombo.setTooltip("Output resolution — 400 DPI matches the CIS sensor "
                        "(print at 100% to play in tune). Takes effect on the "
                        "next GENERATE.");
    dpiCombo.setSelectedId((int) settings.printerDpi, juce::dontSendNotification);
    dpiCombo.onChange = [this]
    { settings.printerDpi = (double) juce::jmax(72, dpiCombo.getSelectedId()); };
    addAndMakeVisible(dpiCombo);

    refreshFreqControls();
    refreshHeightControls();
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

void ScoreSetupPanel::refreshHeightControls()
{
    const bool man = settings.spectroHeightManual != 0;

    heightManualToggle.setToggleState(man, juce::dontSendNotification);
    heightSlider.setEnabled(man);

    // Grey the control when locked to the sensor length (read-only display).
    const float a = man ? 1.0f : 0.45f;
    heightSlider.setAlpha(a);
    heightLabel.setAlpha(a);

    // Show the live override, or the locked CIS length when following the sensor.
    heightSlider.setValue(man ? settings.spectroHeightMM : SCORE_CIS_HEIGHT_MM,
                          juce::dontSendNotification);
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

    // ── Image processing (PhonoPaper-conforming) ──
    labelled(dynLabel, dynSlider);

    area.removeFromTop(Sp3ctraTheme::kRowGap);   // small breather

    // ── Print size (CIS band height) ──
    printSectionLabel.setBounds(row());
    { auto r = row(); heightManualToggle.setBounds(r.removeFromLeft(labelW + Sp3ctraTheme::kGap + ctrlW)); }
    labelled(heightLabel, heightSlider);

    area.removeFromTop(Sp3ctraTheme::kRowGap);   // small breather

    // ── Export (format / sheet / DPI) ──
    exportSectionLabel.setBounds(row());
    labelled(formatLabel, formatCombo);
    labelled(pageLabel,   pageCombo);
    labelled(dpiLabel,    dpiCombo);
}
