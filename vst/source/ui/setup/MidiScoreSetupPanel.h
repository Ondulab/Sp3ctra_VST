/**
 * @file MidiScoreSetupPanel.h
 * @brief SETUP face of the MIDI SCORE block (zone 3).
 *
 * Export preferences moved off the PLAY page: image format (PNG/JPEG), sheet
 * size (A4 portrait / A3 landscape / FULL single sheet) and printer DPI.
 * They are export-only — the preview and the loaded playback frames never
 * change — so they live in the PLAY page's persisted JSON state
 * (midiScoreGenState), edited here through its accessors, not in the APVTS.
 *
 * The PLAY page keeps a single "Export image" button; when the piece is
 * longer than one page at the chosen sheet size, that button opens the
 * export-zone picker (movable page-wide frame over the whole strip).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../UITheme.h"
#include "../../image/MidiScoreGenTabComponent.h"

class MidiScoreSetupPanel : public juce::Component
{
public:
    static constexpr int kPreferredH = 190;

    MidiScoreSetupPanel(MidiScoreGenTabComponent& playPage, juce::Colour accentColour)
        : page_(playPage), accent(accentColour)
    {
        auto initLabel = [this](juce::Label& l, const juce::String& text)
        {
            l.setText(text, juce::dontSendNotification);
            l.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
            l.setColour(juce::Label::textColourId, juce::Colour(0xffb8c4d0));
            addAndMakeVisible(l);
        };

        initLabel(formatLabel, "Image format:");
        formatCombo.addItem("PNG",  1);
        formatCombo.addItem("JPEG", 2);
        formatCombo.onChange = [this]
        { page_.setExportFormatPng(formatCombo.getSelectedId() == 1); };
        addAndMakeVisible(formatCombo);

        initLabel(sheetLabel, "Sheet size:");
        sheetCombo.addItem("A4 portrait",         1);
        sheetCombo.addItem("A3 landscape",        2);
        sheetCombo.addItem("FULL (whole piece)",  3);
        sheetCombo.setTooltip("A4/A3: fixed sheets, printable at 100% scale. "
                              "Pieces longer than one page open a zone picker "
                              "on Export. FULL: one sheet stretched to hold "
                              "the whole piece (large files at high DPI).");
        sheetCombo.onChange = [this]
        { page_.setExportPageFormat(sheetCombo.getSelectedId() - 1); };
        addAndMakeVisible(sheetCombo);

        initLabel(dpiLabel, "Printer DPI:");
        for (int d : { 200, 300, 400, 600, 800 })
            dpiCombo.addItem(juce::String(d), d);
        dpiCombo.setTooltip("Export resolution only — 400 DPI matches the "
                            "CIS sensor (print at 100% to play in tune).");
        dpiCombo.onChange = [this]
        { page_.setExportDpi(dpiCombo.getSelectedId()); };
        addAndMakeVisible(dpiCombo);

        hintLabel.setText("Exports land in the session's exports/ folder, "
                          "DPI-stamped and calibrated for the SAMPLER.",
                          juce::dontSendNotification);
        hintLabel.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        hintLabel.setColour(juce::Label::textColourId, juce::Colour(0xff8890a0));
        addAndMakeVisible(hintLabel);

        refresh();
    }

    /** Re-reads the PLAY page's export prefs — the panel may be shown long
     *  after a session restore changed them behind its back. */
    void refresh()
    {
        formatCombo.setSelectedId(page_.exportFormatIsPng() ? 1 : 2,
                                  juce::dontSendNotification);
        sheetCombo.setSelectedId(juce::jlimit(0, 2, page_.exportPageFormat()) + 1,
                                 juce::dontSendNotification);
        dpiCombo.setSelectedId(page_.exportDpi(), juce::dontSendNotification);
    }

    void visibilityChanged() override
    {
        if (isVisible())
            refresh();
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(accent);
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSection)).boldened());
        g.drawText("MIDI SCORE -- SETUP", 12, 8, getWidth() - 24, 22,
                   juce::Justification::centredLeft, false);
        g.setColour(juce::Colour(0xff2a2a40));
        g.drawLine(12.f, 34.f, (float) getWidth() - 12.f, 34.f, 1.f);

        g.setColour(juce::Colour(0xff8a94a4));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        g.drawText("EXPORT", 12, 40, getWidth() - 24, 16,
                   juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        const int ch   = Sp3ctraTheme::kControlH;
        const int lblW = 110;
        const int w    = juce::jmin(300, getWidth() - lblW - 24);
        int y = 60;
        auto row = [&](juce::Label& l, juce::Component& c)
        {
            l.setBounds(12, y, lblW, ch);
            c.setBounds(12 + lblW + 6, y, w, ch);
            y += ch + 6;
        };
        row(formatLabel, formatCombo);
        row(sheetLabel,  sheetCombo);
        row(dpiLabel,    dpiCombo);
        hintLabel.setBounds(12, y + 4, getWidth() - 24, ch);
    }

private:
    MidiScoreGenTabComponent& page_;
    juce::Colour accent;

    juce::Label    formatLabel, sheetLabel, dpiLabel, hintLabel;
    juce::ComboBox formatCombo, sheetCombo, dpiCombo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiScoreSetupPanel)
};
