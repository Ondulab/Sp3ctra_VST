/**
 * @file Sp3ctraControlsPage.h
 * @brief ZONE 3 (CONTROLS face) of the SP3CTRA source block — the CIS as a
 *        MIDI controller, and what the VST sends back to the device.
 *
 * MIDI OUT: one row per control (SW1..SW3, ACC X/Y/Z, GYRO X/Y/Z, TILT P/R):
 *   live meter (30 Hz, with peak hold) · Type · Ch · Num · Min · Max · bipolar.
 *   A dot before the name lights when the MIDI this row produces is currently
 *   learnt on a parameter (tooltip = which one). There is no "learn" button
 *   here: right-click the DESTINATION control anywhere in the VST, then press
 *   the button / tilt the device — like any hardware controller.
 * FEEDBACK: LED1..3 mode (Off / Press / Follow / Manual) + Manual level,
 *   OLED overlay mode (Off / Chain / All) + hold time.
 *
 * docs/PLAN_SP3CTRA_LINK.md §12.3.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleCatalog.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/Sp3ctraBarSlider.h"
#include "../midi/MidiLearnAttachment.h"
#include "../midi/HidMidiMapper.h"

class Sp3ctraControlsPage : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener
{
public:
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Sp3ctra).getARGB();

    static constexpr int kRowH   = 22;
    static constexpr int kRowGap = 3;
    static constexpr int kNumControls = HidMidiMapper::NumControls;

    static constexpr int kMidiOutH  = ModuleChrome::kSectionCaptionH + ModuleChrome::kLabelH
                                    + kNumControls * (kRowH + kRowGap)
                                    + ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;      // + deadzone/smooth row
    static constexpr int kFeedbackH = ModuleChrome::kSectionCaptionH
                                    + 2 * (ModuleChrome::kBoxRowH + ModuleChrome::kRowGap);
    static constexpr int kPreferredH = ModuleChrome::kPageTop + kMidiOutH + kFeedbackH + ModuleChrome::kPagePad;

    explicit Sp3ctraControlsPage(Sp3ctraAudioProcessor& p)
        : processor(p), mapper(p.getHidMapper())
    {
        auto& apvts = processor.getAPVTS();
        using APVTS = juce::AudioProcessorValueTreeState;

        for (int c = 0; c < kNumControls; ++c)
        {
            auto& r = rows_[(size_t) c];
            const bool button = HidMidiMapper::isButton(c);

            r.name.setText(HidMidiMapper::controlName(c), juce::dontSendNotification);
            r.name.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            r.name.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(r.name);

            r.meter.button = button;
            addAndMakeVisible(r.meter);

            if (button) { r.type.addItem("Off", 1); r.type.addItem("CC", 2); r.type.addItem("Note", 3); r.type.addItem("Toggle", 4); }
            else        { r.type.addItem("Off", 1); r.type.addItem("CC", 2); r.type.addItem("CC 14b", 3); }
            addAndMakeVisible(r.type);
            r.typeAttach = std::make_unique<APVTS::ComboBoxAttachment>(apvts, HidMidiMapper::paramId(c, "Type"), r.type);

            r.chan.setNumDecimalPlacesToDisplay(0);
            addAndMakeVisible(r.chan);
            r.chanAttach = std::make_unique<APVTS::SliderAttachment>(apvts, HidMidiMapper::paramId(c, "Chan"), r.chan);
            r.num.setNumDecimalPlacesToDisplay(0);
            addAndMakeVisible(r.num);
            r.numAttach = std::make_unique<APVTS::SliderAttachment>(apvts, HidMidiMapper::paramId(c, "Num"), r.num);

            if (! button)
            {
                const juce::String unit = juce::String(" ") + HidMidiMapper::controlUnit(c);
                const int decimals = juce::String(HidMidiMapper::controlUnit(c)) == "g" ? 2 : 0;
                for (auto* s : { &r.min, &r.max })
                {
                    s->setTextValueSuffix(unit);
                    addAndMakeVisible(*s);
                }
                r.minAttach = std::make_unique<APVTS::SliderAttachment>(apvts, HidMidiMapper::paramId(c, "Min"), r.min);
                r.maxAttach = std::make_unique<APVTS::SliderAttachment>(apvts, HidMidiMapper::paramId(c, "Max"), r.max);
                r.min.setNumDecimalPlacesToDisplay(decimals);
                r.max.setNumDecimalPlacesToDisplay(decimals);
                r.bipolar.setButtonText(juce::CharPointer_UTF8("\xc2\xb1"));
                addAndMakeVisible(r.bipolar);
                r.bipAttach = std::make_unique<APVTS::ButtonAttachment>(apvts, HidMidiMapper::paramId(c, "Bipolar"), r.bipolar);
            }
        }

        deadzone.setTextValueSuffix(" %");  deadzone.setNumDecimalPlacesToDisplay(1);
        addAndMakeVisible(deadzone);
        deadzoneAttach = std::make_unique<APVTS::SliderAttachment>(apvts, juce::String(HidMidiMapper::kPrefix) + "Deadzone", deadzone);
        smooth.setTextValueSuffix(" ms");   smooth.setNumDecimalPlacesToDisplay(0);
        addAndMakeVisible(smooth);
        smoothAttach = std::make_unique<APVTS::SliderAttachment>(apvts, juce::String(HidMidiMapper::kPrefix) + "SmoothMs", smooth);

        // ── FEEDBACK ────────────────────────────────────────────────────────
        for (int i = 0; i < 3; ++i)
        {
            auto& l = leds_[(size_t) i];
            for (auto* s : { "Off", "Press", "Follow", "Manual" }) l.mode.addItem(s, l.mode.getNumItems() + 1);
            addAndMakeVisible(l.mode);
            l.modeAttach = std::make_unique<APVTS::ComboBoxAttachment>(apvts, "sp3ctraLed" + juce::String(i + 1) + "Mode", l.mode);
            l.level.setNumDecimalPlacesToDisplay(2);
            addAndMakeVisible(l.level);
            l.levelAttach = std::make_unique<APVTS::SliderAttachment>(apvts, "sp3ctraLed" + juce::String(i + 1) + "Level", l.level);
            l.learn = std::make_unique<MidiLearnAttachment>(processor.getMidiMap(), l.level, "sp3ctraLed" + juce::String(i + 1) + "Level");
        }
        for (auto* s : { "Off", "Chain", "All" }) oledMode.addItem(s, oledMode.getNumItems() + 1);
        addAndMakeVisible(oledMode);
        oledModeAttach = std::make_unique<APVTS::ComboBoxAttachment>(apvts, "sp3ctraOledMode", oledMode);
        oledHold.setTextValueSuffix(" ms"); oledHold.setNumDecimalPlacesToDisplay(0);
        addAndMakeVisible(oledHold);
        oledHoldAttach = std::make_unique<APVTS::SliderAttachment>(apvts, "sp3ctraOledHoldMs", oledHold);

        processor.getMidiMap().addChangeListener(this);
        startTimer(33);
        refreshBadges();
    }

    ~Sp3ctraControlsPage() override
    {
        stopTimer();
        processor.getMidiMap().removeChangeListener(this);
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent(kAccentARGB);
        const int pad = ModuleChrome::kPagePad;
        const int w   = getWidth();
        int y = ModuleChrome::kPageTop;

        ModuleChrome::drawSectionCaption(g, y, w, accent,
                                         mapper.hidAlive() ? "MIDI OUT - THE CIS AS A CONTROLLER"
                                                           : "MIDI OUT - (no HID stream: device not bound)");
        y += ModuleChrome::kSectionCaptionH;

        // column header
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.setColour(accent.withAlpha(0.6f));
        auto header = [&] (const juce::Component& ref, const char* text)
        {
            if (! ref.isVisible()) return;
            g.drawText(text, ref.getX(), y, ref.getWidth(), ModuleChrome::kLabelH, juce::Justification::centred, false);
        };
        const auto& r0 = rows_[3];   // a continuous row shows every column
        header(r0.meter, "live"); header(r0.type, "type"); header(r0.chan, "ch"); header(r0.num, "num");
        header(r0.min, "min"); header(r0.max, "max"); header(r0.bipolar, "bip");
        y += ModuleChrome::kLabelH + kNumControls * (kRowH + kRowGap);

        ModuleChrome::drawBoxLabel(g, deadzone, accent, "Deadzone (bipolar)");
        ModuleChrome::drawBoxLabel(g, smooth,   accent, "Smoothing");
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;

        ModuleChrome::drawSectionCaption(g, y, w, accent, "FEEDBACK - WHAT THE VST SENDS BACK");
        for (int i = 0; i < 3; ++i)
        {
            ModuleChrome::drawBoxLabel(g, leds_[(size_t) i].mode,  accent, "LED" + juce::String(i + 1) + " mode");
            ModuleChrome::drawBoxLabel(g, leds_[(size_t) i].level, accent, "LED" + juce::String(i + 1) + " level");
        }
        ModuleChrome::drawBoxLabel(g, oledMode, accent, "OLED overlay");
        ModuleChrome::drawBoxLabel(g, oledHold, accent, "OLED hold");
        juce::ignoreUnused(pad);
    }

    void resized() override
    {
        const int pad = ModuleChrome::kPagePad;
        const int w   = getWidth() - 2 * pad;
        int y = ModuleChrome::kPageTop + ModuleChrome::kSectionCaptionH + ModuleChrome::kLabelH;

        // fixed columns + weighted remainder
        const int gap = 6, nameW = 52, meterW = 44, bipW = 26;
        const float weights[5] = { 2.0f, 1.0f, 1.1f, 1.5f, 1.5f };   // type ch num min max
        float wsum = 0; for (float f : weights) wsum += f;
        const int flexW = juce::jmax(120, w - nameW - meterW - bipW - 7 * gap);

        for (int c = 0; c < kNumControls; ++c)
        {
            auto& r = rows_[(size_t) c];
            int x = pad;
            r.name .setBounds(x, y, nameW, kRowH);  x += nameW + gap;
            r.meter.setBounds(x, y + 3, meterW, kRowH - 6); x += meterW + gap;
            juce::Component* flex[5] = { &r.type, &r.chan, &r.num, &r.min, &r.max };
            for (int i = 0; i < 5; ++i)
            {
                const int cw = (int) (flexW * weights[i] / wsum);
                flex[i]->setBounds(x, y + 2, cw, kRowH - 4);
                x += cw + gap;
            }
            r.bipolar.setBounds(x, y + 2, bipW, kRowH - 4);
            const bool button = HidMidiMapper::isButton(c);
            r.min.setVisible(! button); r.max.setVisible(! button); r.bipolar.setVisible(! button);
            y += kRowH + kRowGap;
        }

        ModuleChrome::layoutBoxRow(juce::Rectangle<int>(pad, y, w, ModuleChrome::kBoxRowH), { &deadzone, &smooth, nullptr, nullptr });
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;

        y += ModuleChrome::kSectionCaptionH;
        ModuleChrome::layoutBoxRow(juce::Rectangle<int>(pad, y, w, ModuleChrome::kBoxRowH),
                                   { &leds_[0].mode, &leds_[0].level, &leds_[1].mode, &leds_[1].level, &leds_[2].mode, &leds_[2].level });
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;
        ModuleChrome::layoutBoxRow(juce::Rectangle<int>(pad, y, w, ModuleChrome::kBoxRowH), { &oledMode, &oledHold, nullptr, nullptr });
    }

private:
    //==========================================================================
    /** Live meter with peak hold — "what you look at" in the module colour. */
    struct HidMeter : public juce::Component
    {
        float value = 0.0f, peak = 0.0f;
        bool  button = false;
        void paint(juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();
            g.setColour(juce::Colour(Sp3ctraTheme::kColFrameBg));
            g.fillRoundedRectangle(b, 3.0f);
            const juce::Colour accent(kAccentARGB);
            if (button)
            {
                if (value > 0.5f) { g.setColour(accent); g.fillRoundedRectangle(b.reduced(1.0f), 3.0f); }
            }
            else
            {
                const float mid = b.getCentreX();
                const float xv  = b.getX() + value * b.getWidth();
                g.setColour(accent.withAlpha(0.85f));
                g.fillRect(juce::Rectangle<float>::leftTopRightBottom(juce::jmin(mid, xv), b.getY() + 1, juce::jmax(mid, xv), b.getBottom() - 1));
                g.setColour(accent.withAlpha(0.35f));
                g.fillRect(mid - 0.5f, b.getY(), 1.0f, b.getHeight());
                const float xp = b.getX() + peak * b.getWidth();
                g.setColour(accent);
                g.fillRect(xp - 1.0f, b.getY(), 2.0f, b.getHeight());
            }
            g.setColour(accent.withAlpha(0.25f));
            g.drawRoundedRectangle(b.reduced(0.5f), 3.0f, 1.0f);
        }
    };

    struct Row
    {
        juce::Label      name;
        HidMeter         meter;
        juce::ComboBox   type;
        Sp3ctraBarSlider chan, num, min, max;
        juce::ToggleButton bipolar;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   chanAttach, numAttach, minAttach, maxAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   bipAttach;
    };
    struct LedRow
    {
        juce::ComboBox   mode;
        Sp3ctraBarSlider level;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   levelAttach;
        std::unique_ptr<MidiLearnAttachment> learn;
    };

    void timerCallback() override
    {
        // meters: value follows the mapper, the peak decays (rémanence)
        for (int c = 0; c < kNumControls; ++c)
        {
            auto& r = rows_[(size_t) c];
            const float v = mapper.liveNorm(c);
            r.meter.value = v;
            const float dev = std::fabs(v - 0.5f);
            const float pk  = std::fabs(r.meter.peak - 0.5f);
            if (HidMidiMapper::isButton(c) || dev >= pk) r.meter.peak = v;
            else r.meter.peak += (0.5f - r.meter.peak) * 0.04f;   // ~1 s back to the centre
            r.meter.repaint();
        }
        if (++badgeTick_ >= 15) { badgeTick_ = 0; refreshBadges(); repaint(); }
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { refreshBadges(); }

    /** Name label: "● SW1" + tooltip when the row's MIDI drives a parameter. */
    void refreshBadges()
    {
        auto& mm = processor.getMidiMap();
        for (int c = 0; c < kNumControls; ++c)
        {
            auto& r = rows_[(size_t) c];
            int type = 0, ch = 0, num = 0;
            juce::String target;
            if (mapper.currentEvent(c, type, ch, num))
                target = mm.paramForEvent(type, ch, num);
            const bool mapped = target.isNotEmpty();
            juce::String pretty = target;
            if (auto* p = processor.getAPVTS().getParameter(target)) pretty = p->getName(40);
            r.name.setText((mapped ? juce::String(juce::CharPointer_UTF8("\xe2\x97\x8f ")) : juce::String("   "))
                           + HidMidiMapper::controlName(c), juce::dontSendNotification);
            r.name.setColour(juce::Label::textColourId,
                             mapped ? juce::Colour(kAccentARGB) : juce::Colour(Sp3ctraTheme::kColTextMuted));
            r.name.setTooltip(mapped ? "Drives: " + pretty
                                     : juce::String("Not learnt yet: right-click a control, MIDI Learn, then use the CIS"));
        }
    }

    Sp3ctraAudioProcessor& processor;
    HidMidiMapper&         mapper;
    std::array<Row, kNumControls> rows_;
    Sp3ctraBarSlider deadzone, smooth;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> deadzoneAttach, smoothAttach;
    std::array<LedRow, 3> leds_;
    juce::ComboBox   oledMode;
    Sp3ctraBarSlider oledHold;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> oledModeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   oledHoldAttach;
    int badgeTick_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraControlsPage)
};
