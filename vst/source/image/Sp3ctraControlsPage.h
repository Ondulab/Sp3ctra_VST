/**
 * @file Sp3ctraControlsPage.h
 * @brief ZONE 3 (CONTROLS face) of the SP3CTRA source block — the CIS as a
 *        MIDI controller, and what the VST sends back to the device.
 *
 * SENSOR: the inertial unit itself, read from and written to the DEVICE over
 *   the LINK SESSION (SLP CFG_GET / CFG_SET / CAL_START), not the web server:
 *   an HTTP POST needs the admin password (random, generated on the device's
 *   first boot, shown once on its screen) while the bound session is already
 *   the proof of ownership. Gyro / accel full scale + Calibrate. The full
 *   scale is not decoration — it BOUNDS the Min/Max windows below, so a range
 *   the hardware can never reach (a +/-2000 dps window on a +/-250 dps device)
 *   is unreachable here.
 * MIDI OUT: one row per control (SW1..SW3, GYRO X/Y/Z, TILT P/R, the HIT
 *   family — every shock, then one row per struck face — and the FACE
 *   selector; only the bar's four LONG faces exist, its 25 mm ends are
 *   neither a resting position nor a playing surface):
 *   live meter (30 Hz, with peak hold) · Type · Ch · Num · Min · Max · bipolar.
 *   The HIT rows default to Velocity — a CC pulse carrying how hard the blow
 *   was, back to 0 within 15 ms so the control follows the impulse and no
 *   more; CC / Note / Toggle remain the flat trigger flavours. The row's meter
 *   holds longer than the pulse on purpose, or a light tap would be invisible.
 *   FACE has no Min/Max window; its row is followed by the FACE VALUE TABLE —
 *   one bar per resting face (% of the MIDI course), so any face can emit any
 *   value, at full resolution when the row's type is CC 14-bit.
 *   Ch / Num are INTERNAL identifiers (this MIDI never leaves the plugin) and
 *   stay folded away behind the "ids" button unless a collision with a real
 *   controller has to be resolved.
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
#include "../Sp3ctraLookAndFeel.h"
#include "../ui/ModuleCatalog.h"
#include "../ui/ParamIdentity.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/Sp3ctraBarSlider.h"
#include "../communication/link/Sp3ctraLink.h"
#include "../midi/MidiLearnAttachment.h"
#include "../midi/HidMidiMapper.h"

class Sp3ctraControlsPage : public juce::Component,
                            private juce::Timer,
                            private juce::ChangeListener
{
public:
    static inline const uint32_t kAccentARGB = moduleColour(ModuleType::Sp3ctra).getARGB();

    static constexpr int kRowH   = 22;
    static constexpr int kHeadH  = ModuleChrome::kLabelH;   ///< column-header strip
    static constexpr int kFoldW  = 168;                     ///< the advanced fold button
    static constexpr int kFoldH  = 16;
    static constexpr int kRowGap = 3;
    static constexpr int kNumControls = HidMidiMapper::NumControls;

    static constexpr int kSensorH   = ModuleChrome::kSectionCaptionH
                                    + ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;
    static constexpr int kMidiOutH  = ModuleChrome::kSectionCaptionH + kHeadH
                                    + kNumControls * (kRowH + kRowGap)
                                    + 2 * (ModuleChrome::kBoxRowH + ModuleChrome::kRowGap); // + FACE table + deadzone/smooth rows
    static constexpr int kFeedbackH = ModuleChrome::kSectionCaptionH
                                    + 2 * (ModuleChrome::kBoxRowH + ModuleChrome::kRowGap);
    static constexpr int kPreferredH = ModuleChrome::kPageTop + kSensorH + kMidiOutH + kFeedbackH
                                     + ModuleChrome::kPagePad;

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

            r.meter.button   = button && ! HidMidiMapper::isEvent(c);
            r.meter.unipolar = HidMidiMapper::isEvent(c);
            addAndMakeVisible(r.meter);

            // The HIT rows carry a strike strength, so they add Velocity: a
            // CC pulse whose VALUE is the force of the blow. CC / Note /
            // Toggle stay the flat trigger flavours.
            if (button) { r.type.addItem("Off", 1); r.type.addItem("CC", 2); r.type.addItem("Note", 3); r.type.addItem("Toggle", 4);
                          if (HidMidiMapper::isEvent(c)) r.type.addItem("Velocity", 5); }
            else        { r.type.addItem("Off", 1); r.type.addItem("CC", 2); r.type.addItem("CC 14b", 3); }
            addAndMakeVisible(r.type);
            r.typeAttach = std::make_unique<APVTS::ComboBoxAttachment>(apvts, HidMidiMapper::paramId(c, "Type"), r.type);

            r.chan.setNumDecimalPlacesToDisplay(0);
            addAndMakeVisible(r.chan);
            r.chanAttach = std::make_unique<APVTS::SliderAttachment>(apvts, HidMidiMapper::paramId(c, "Chan"), r.chan);
            r.num.setNumDecimalPlacesToDisplay(0);
            addAndMakeVisible(r.num);
            r.numAttach = std::make_unique<APVTS::SliderAttachment>(apvts, HidMidiMapper::paramId(c, "Num"), r.num);

            if (! button && c != HidMidiMapper::Face)
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
                // No button text: the switch fills its column, and the "bip"
                // header names it. A tooltip carries the meaning.
                r.bipolar.setTooltip("Bipolar: the axis swings both ways around its centre.");
                addAndMakeVisible(r.bipolar);
                r.bipAttach = std::make_unique<APVTS::ButtonAttachment>(apvts, HidMidiMapper::paramId(c, "Bipolar"), r.bipolar);
            }
        }

        // FACE value table: one bar per resting face, % of the MIDI course.
        for (int f = 0; f < HidMidiMapper::kNumFaces; ++f)
        {
            auto& s = faceVal_[(size_t) f];
            s.setTextValueSuffix(" %");
            s.setNumDecimalPlacesToDisplay(1);
            s.setTooltip(juce::String("Value the FACE row emits while the device rests on this face, "
                                      "as a percentage of the MIDI course. In CC 14-bit the 0.01 % "
                                      "step reaches nearly every one of the 16384 values."));
            addAndMakeVisible(s);
            faceValAttach_[(size_t) f] = std::make_unique<APVTS::SliderAttachment>(
                apvts, HidMidiMapper::faceParamId(f), s);
        }

        deadzone.setTextValueSuffix(" %");  deadzone.setNumDecimalPlacesToDisplay(1);
        addAndMakeVisible(deadzone);
        deadzoneAttach = std::make_unique<APVTS::SliderAttachment>(apvts, juce::String(HidMidiMapper::kPrefix) + "Deadzone", deadzone);
        smooth.setTextValueSuffix(" ms");   smooth.setNumDecimalPlacesToDisplay(0);
        addAndMakeVisible(smooth);
        smoothAttach = std::make_unique<APVTS::SliderAttachment>(apvts, juce::String(HidMidiMapper::kPrefix) + "SmoothMs", smooth);

        // ── SENSOR (device, over the link session) ──────────────────────────
        // The device owns these values; nothing here is saved with the session.
        for (auto* t : { "+/-2000 dps", "+/-1000 dps", "+/-500 dps", "+/-250 dps",
                         "+/-125 dps", "+/-62.5 dps", "+/-31.25 dps", "+/-15.625 dps" })
            gyroFs.addItem(t, gyroFs.getNumItems() + 1);
        gyroFs.onChange = [this]
        {
            if (applyingRemote_) return;
            const int idx = gyroFs.getSelectedId() - 1;
            applyGyroFullScale(idx);
            writeCfg(SLP_CFG_GYRO_FS, (uint32_t) idx, "Gyro range set - the device recalibrates itself");
        };
        addAndMakeVisible(gyroFs);

        for (auto* t : { "+/-16 g", "+/-8 g", "+/-4 g", "+/-2 g" })
            accelFs.addItem(t, accelFs.getNumItems() + 1);
        accelFs.onChange = [this]
        {
            if (applyingRemote_) return;
            writeCfg(SLP_CFG_ACCEL_FS, (uint32_t) (accelFs.getSelectedId() - 1),
                     "Accel range set - the device recalibrates itself");
        };
        addAndMakeVisible(accelFs);

        calibrate.setButtonText("Calibrate");
        calibrate.onClick = [this]
        {
            if (auto* l = processor.getLink()) l->requestCalibration(SLP_CAL_IMU);
            setSensorStatus("Calibrating - keep the device still (~1.5 s)");
        };
        addAndMakeVisible(calibrate);

        sensorStatus.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        sensorStatus.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(sensorStatus);
        setSensorEnabled(false);
        setSensorStatus("No session");

        // Ch / Num are internal identifiers — folded away by default.
        showIds_ = (bool) processor.getAPVTS().state.getProperty("hidShowIds", false);
        ids.setButtonText("ADVANCED - MIDI ch / num");
        ids.setClickingTogglesState(true);
        ids.setToggleState(showIds_, juce::dontSendNotification);
        ids.setTooltip("Show the MIDI channel / number each row emits. They are internal "
                       "(this MIDI never leaves the plugin) - only change them to avoid a "
                       "collision with a real controller you have already learnt.");
        ids.onClick = [this]
        {
            showIds_ = ids.getToggleState();
            // Rides along with the session state, like the editor's layout props.
            processor.getAPVTS().state.setProperty("hidShowIds", showIds_, nullptr);
            resized();
            repaint();
        };
        addAndMakeVisible(ids);

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
        if (auto* l = processor.getLink()) l->addChangeListener(this);
        startTimer(33);
        refreshBadges();
    }

    ~Sp3ctraControlsPage() override
    {
        stopTimer();
        processor.getMidiMap().removeChangeListener(this);
        if (auto* l = processor.getLink()) l->removeChangeListener(this);
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const juce::Colour accent(kAccentARGB);
        const int pad = ModuleChrome::kPagePad;
        const int w   = getWidth();
        int y = ModuleChrome::kPageTop;

        ModuleChrome::drawSectionCaption(g, y, w, accent, "SENSOR - THE INERTIAL UNIT");
        y += ModuleChrome::kSectionCaptionH;
        ModuleChrome::drawBoxLabel(g, gyroFs,    accent, "Gyro range");
        ModuleChrome::drawBoxLabel(g, accelFs,   accent, "Accel range");
        ModuleChrome::drawBoxLabel(g, calibrate, accent, "Zero the sensor");
        ModuleChrome::drawBoxLabel(g, sensorStatus, accent, "device");
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;

        ModuleChrome::drawSectionCaption(g, y, w, accent,
                                         mapper.hidAlive() ? "MIDI OUT - THE CIS AS A CONTROLLER"
                                                           : "MIDI OUT - (no HID stream: device not bound)");
        y += ModuleChrome::kSectionCaptionH;

        // The advanced block (ch / num) reads as one zone, not as two more
        // columns: a faint panel behind it, from the headers to the last row.
        if (showIds_)
        {
            const auto& first = rows_[0];
            const auto& last  = rows_[(size_t) kNumControls - 1];
            const juce::Rectangle<float> band ((float) first.chan.getX() - 5.0f,
                                               (float) y - 2.0f,
                                               (float) (first.num.getRight() - first.chan.getX()) + 10.0f,
                                               (float) (last.num.getBottom() + 4 - y));
            g.setColour(accent.withAlpha(0.05f));
            g.fillRoundedRectangle(band, 4.0f);
            g.setColour(accent.withAlpha(0.18f));
            g.drawRoundedRectangle(band.reduced(0.5f), 4.0f, 1.0f);
        }

        // column header
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.setColour(accent.withAlpha(0.6f));
        auto header = [&] (const juce::Component& ref, const char* text)
        {
            if (! ref.isVisible()) return;
            g.drawText(text, ref.getX(), y, ref.getWidth(), kHeadH, juce::Justification::centred, false);
        };
        const auto& r0 = rows_[3];   // a continuous row shows every column
        header(r0.meter, "live"); header(r0.type, "type"); header(r0.chan, "ch"); header(r0.num, "num");
        header(r0.min, "min"); header(r0.max, "max"); header(r0.bipolar, "bip");
        y += kHeadH + kNumControls * (kRowH + kRowGap);

        // FACE table: the labels name the faces; "FACE:" on the first one ties
        // the row to the selector right above it.
        for (int f = 0; f < HidMidiMapper::kNumFaces; ++f)
            ModuleChrome::drawBoxLabel(g, faceVal_[(size_t) f], accent,
                                       (f == 0 ? juce::String("FACE: ") : juce::String())
                                       + HidMidiMapper::faceName(f));
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;

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
        int y = ModuleChrome::kPageTop + ModuleChrome::kSectionCaptionH;

        ModuleChrome::layoutBoxRow(juce::Rectangle<int>(pad, y, w, ModuleChrome::kBoxRowH),
                                   { &gyroFs, &accelFs, &calibrate, &sensorStatus });
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;

        // The fold is a section option: it rides at the right end of the MIDI
        // OUT caption band, where there is room for a legible label.
        ids.setBounds(pad + w - kFoldW, y + (ModuleChrome::kSectionCaptionH - kFoldH) / 2, kFoldW, kFoldH);
        y += ModuleChrome::kSectionCaptionH;

        // fixed columns + weighted remainder
        // The bip toggle is a bare switch: its column must hold the whole track
        // the LookAndFeel draws (h x 1.9), or the ON knob is clipped away.
        // nameW is set by the longest label, badge included: "* HIT RIGHT".
        const int gap = 6, nameW = 76, meterW = 44;
        const int bipW = Sp3ctraLookAndFeel::toggleSwitchWidth(kRowH - 4);

        y += kHeadH;

        for (int c = 0; c < kNumControls; ++c)
        {
            auto& r = rows_[(size_t) c];
            const bool button = HidMidiMapper::isButton(c);
            // FACE has no Min/Max window: its mapping is the table row below.
            const bool faceRow = (c == HidMidiMapper::Face);
            r.chan.setVisible(showIds_); r.num.setVisible(showIds_);
            r.min.setVisible(! button && ! faceRow);
            r.max.setVisible(! button && ! faceRow);
            r.bipolar.setVisible(! button && ! faceRow);

            // Folding ch/num hands their width to min/max, NOT to type: the
            // combo keeps the same share of the row either way.
            const float freed = showIds_ ? 0.0f : (1.0f + 1.1f) * 0.5f;
            juce::Component* flex[5] { &r.type, nullptr, nullptr, nullptr, nullptr };
            float weights[5] { 2.0f, 0, 0, 0, 0 };                  // type [ch num] min max
            int n = 1;
            if (showIds_) { flex[n] = &r.chan; weights[n++] = 1.0f;
                            flex[n] = &r.num;  weights[n++] = 1.1f; }
            // The face row keeps the min/max slots in the layout (invisible):
            // the columns stay aligned, and the empty window under "min/max"
            // says "mapped by the table below" instead of looking broken.
            flex[n] = &r.min; weights[n++] = 1.5f + freed;
            flex[n] = &r.max; weights[n++] = 1.5f + freed;
            float wsum = 0; for (int i = 0; i < n; ++i) wsum += weights[i];
            const int flexW = juce::jmax(120, w - nameW - meterW - bipW - (n + 2) * gap);

            int x = pad;
            r.name .setBounds(x, y, nameW, kRowH);  x += nameW + gap;
            r.meter.setBounds(x, y + 3, meterW, kRowH - 6); x += meterW + gap;
            for (int i = 0; i < n; ++i)
            {
                const int cw = (int) (flexW * weights[i] / wsum);
                flex[i]->setBounds(x, y + 2, cw, kRowH - 4);
                x += cw + gap;
            }
            r.bipolar.setBounds(x, y + 2, bipW, kRowH - 4);
            y += kRowH + kRowGap;
        }

        ModuleChrome::layoutBoxRow(juce::Rectangle<int>(pad, y, w, ModuleChrome::kBoxRowH),
                                   { &faceVal_[0], &faceVal_[1], &faceVal_[2],
                                     &faceVal_[3], &faceVal_[4] });
        y += ModuleChrome::kBoxRowH + ModuleChrome::kRowGap;

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
        bool  unipolar = false;   ///< HIT rows: a force bar, 0 at the left
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
            else if (unipolar)
            {
                // How hard the blow was, held by a decaying peak: a soft tap
                // has to LOOK soft, not just "something happened".
                g.setColour(accent.withAlpha(0.85f));
                g.fillRect(b.getX() + 1.0f, b.getY() + 1.0f, value * (b.getWidth() - 2.0f), b.getHeight() - 2.0f);
                g.setColour(accent);
                g.fillRect(b.getX() + 1.0f + peak * (b.getWidth() - 3.0f), b.getY(), 2.0f, b.getHeight());
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
            if (r.meter.unipolar)                       r.meter.peak = (v >= r.meter.peak) ? v : r.meter.peak * 0.96f;
            else if (HidMidiMapper::isButton(c) || dev >= pk) r.meter.peak = v;
            else r.meter.peak += (0.5f - r.meter.peak) * 0.04f;   // ~1 s back to the centre
            r.meter.repaint();
        }
        if (++badgeTick_ >= 15)
        {
            badgeTick_ = 0;
            refreshBadges();
            if (sessionOpen_) refreshSensor();   // also retires a transient status line
            repaint();
        }
    }

    void changeListenerCallback(juce::ChangeBroadcaster* src) override
    {
        if (src == (juce::ChangeBroadcaster*) processor.getLink()) refreshSensor();
        else                                                       refreshBadges();
    }

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
            // "4 VIDEO · DC BLOCK · Amount" — the shared identity text
            // (ui/ParamIdentity.h), never the bank-tagged APVTS name.
            juce::String pretty = target;
            if (mapped) pretty = describeParam(processor, target).targetText();
            r.name.setText((mapped ? juce::String(juce::CharPointer_UTF8("\xe2\x97\x8f ")) : juce::String("   "))
                           + HidMidiMapper::controlName(c), juce::dontSendNotification);
            r.name.setColour(juce::Label::textColourId,
                             mapped ? juce::Colour(kAccentARGB) : juce::Colour(Sp3ctraTheme::kColTextMuted));
            r.name.setTooltip(mapped ? "Drives: " + pretty
                                     : juce::String("Not learnt yet: right-click a control, MIDI Learn, then use the CIS"));
        }
    }

    //── SENSOR: the device side of the page (link session, message thread) ───
    // CFG_GET on show (and whenever the session comes back), CFG_SET on edit,
    // CAL_START on Calibrate. The device answers every write with the value it
    // actually stored, so the combos always end up showing the truth.
    void visibilityChanged()      override { syncDeviceSession(); }
    void parentHierarchyChanged() override { syncDeviceSession(); }

    void syncDeviceSession()
    {
        const bool showing = isShowing();
        if (showing == sessionOpen_) return;
        sessionOpen_ = showing;
        if (showing) { pullSensor(); refreshSensor(); }
    }

    void pullSensor()
    {
        if (auto* l = processor.getLink())
            l->requestConfig({ SLP_CFG_GYRO_FS, SLP_CFG_ACCEL_FS });
    }

    void writeCfg(uint16_t id, uint32_t value, const juce::String& saying)
    {
        auto* l = processor.getLink();
        if (l == nullptr) return;
        slp_cfg_item it {};
        it.id = id; it.type = SLP_CFG_U8; it.value = value;
        l->writeConfig({ it });
        setSensorStatus(saying);
    }

    /** Mirror the link: bound or not, and what the device answered for the two
     *  full-scale ids. Driven by the link's change messages. */
    void refreshSensor()
    {
        auto* l = processor.getLink();
        const bool bound = l != nullptr && l->isBound();
        if (bound != connected_)
        {
            connected_ = bound;
            setSensorEnabled(bound);
            if (bound) pullSensor();          // a fresh session: ask again
        }

        Sp3ctraLink::CfgValue g {}, a {};
        const bool haveG = bound && l->configValue(SLP_CFG_GYRO_FS,  g);
        const bool haveA = bound && l->configValue(SLP_CFG_ACCEL_FS, a);
        if (haveG || haveA)
        {
            const juce::ScopedValueSetter<bool> guard(applyingRemote_, true);
            if (haveG)
            {
                gyroFs.setSelectedId(juce::jlimit(1, 8, (int) g.value + 1), juce::dontSendNotification);
                applyGyroFullScale((int) g.value);
            }
            if (haveA)
                accelFs.setSelectedId(juce::jlimit(1, 4, (int) a.value + 1), juce::dontSendNotification);
        }

        if (! bound)                          setSensorStatus("No session");
        else if (! (haveG && haveA))          setSensorStatus("Reading " + l->status().deviceIp + " ...");
        else if (! statusIsTransient())       setSensorStatus(l->status().deviceIp);
    }

    /** True while the status line carries a message worth leaving up (a write
     *  we just sent, a calibration in flight) rather than the device address. */
    bool statusIsTransient() const
    {
        return juce::Time::getMillisecondCounter() - statusStampMs_ < 2500u;
    }

    void setSensorStatus(const juce::String& text)
    {
        statusStampMs_ = juce::Time::getMillisecondCounter();
        sensorStatus.setText(text, juce::dontSendNotification);
        sensorStatus.setColour(juce::Label::textColourId,
                               juce::Colour(connected_ ? kAccentARGB : Sp3ctraTheme::kColTextMuted));
    }

    void setSensorEnabled(bool on)
    {
        gyroFs.setEnabled(on); accelFs.setEnabled(on); calibrate.setEnabled(on);
    }

    /** The gyro rows can only map what the hardware can produce: a window wider
     *  than the full scale would waste the MIDI course on a course the sensor
     *  saturates before reaching. Narrowing the bars clamps such a window back
     *  to the physical limit. TILT is an angle (always +/-90 deg) and the accel
     *  full scale only sets its resolution, so neither is bounded here. */
    void applyGyroFullScale(int fsIndex)
    {
        static constexpr float kDps[8] = { 2000.0f, 1000.0f, 500.0f, 250.0f,
                                           125.0f, 62.5f, 31.25f, 15.625f };
        const float fs = kDps[juce::jlimit(0, 7, fsIndex)];
        for (int c = HidMidiMapper::GyrX; c <= HidMidiMapper::GyrZ; ++c)
            for (auto* bar : { &rows_[(size_t) c].min, &rows_[(size_t) c].max })
                bar->setRange(-fs, fs, bar->getInterval());
    }

    Sp3ctraAudioProcessor& processor;
    HidMidiMapper&         mapper;
    std::array<Row, kNumControls> rows_;
    std::array<Sp3ctraBarSlider, HidMidiMapper::kNumFaces> faceVal_;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>,
               HidMidiMapper::kNumFaces> faceValAttach_;
    Sp3ctraBarSlider deadzone, smooth;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> deadzoneAttach, smoothAttach;
    std::array<LedRow, 3> leds_;
    juce::ComboBox   oledMode;
    Sp3ctraBarSlider oledHold;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> oledModeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   oledHoldAttach;
    int badgeTick_ = 0;

    // SENSOR band + the ch/num fold
    juce::ComboBox   gyroFs, accelFs;
    juce::TextButton calibrate, ids;
    juce::Label      sensorStatus;
    bool     applyingRemote_ = false, connected_ = false;
    bool     sessionOpen_ = false, showIds_ = false;
    uint32_t statusStampMs_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Sp3ctraControlsPage)
};
