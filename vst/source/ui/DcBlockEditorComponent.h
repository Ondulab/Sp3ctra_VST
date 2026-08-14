/**
 * @file DcBlockEditorComponent.h
 * @brief Interactive editor for the LuxDcBlock DC BLOCK stage (Amount).
 *
 * Mirrors the DriveEditorComponent feel AND its visual language: the graphic
 * shows the REAL input stream as two live layers (30 Hz):
 *
 *   • rémanence (ghost fill) — ui_in_peak, the slow-release envelope of the
 *     recent maxima: the HIGHS of the stream stay readable for ~1-2 s;
 *   • live line — ui_in_now, the fast-release profile: the stream breathing,
 *     its dips show the LOWS against the remanent highs.
 *
 * A thin reference line marks the measured DC — the stream's smoothed mean
 * energy (ui_dc, published by the C engine). The accent output curve is the
 * remanent envelope minus the removed DC, so the picture stays steady while
 * the stream moves. Until the instance has seen a stream, a fixed demo line
 * (the same three masses as CENTROID/LEVELS) keeps the editor readable.
 *
 * The stream view is DISPLAY-ONLY: the BLOCK line (dashed, HORIZONTAL) sits
 * at amount × DC, the energy actually subtracted from every pixel — but it is
 * not grabbable. Anchoring a handle on the measured DC made the control jump
 * (no DC in the stream → the line snaps between 0 and full), so Amount is set
 * ONLY from the numeric box in its own row below the frame (double-click =
 * 100 %).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "Sp3ctraBarSlider.h"
#include "../processing/lux_dcblock.h"   // self-manages extern "C" linkage

class DcBlockEditorComponent : public juce::Component,
                               private juce::Timer
{
public:
    static constexpr int kGraphH     = 150;  // the graphic frame alone
    // frame + gap + label + box row
    static constexpr int kPreferredH = kGraphH + 6 + 10 + 18;

    DcBlockEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                           juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        // Unbound until the owning tab calls setInstance() with the selected
        // instance's bank ids (luxdcblock{slot}_*).
        startTimerHz(30);   // fluid layers — the rémanence lives on screen
    }

    ~DcBlockEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance);
     *  the right-click popups then follow every rebind. */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** (Re)bind the box to one instance's bank and point the live layers at
     *  that instance's pool slot. */
    void setInstance(int slot, const juce::String& amountId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        amt.attach.reset();
        boxAAtt.reset();
        bind(amt, amountId);
        initBox(boxA, amountId, boxAAtt, 100.0);

        learnA_.reset();
        if (midiMap_ != nullptr)
            learnA_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxA, amountId);
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    void resized() override
    {
        auto area = getLocalBounds();
        // Controls OUT of the graphic frame — box row below it.
        auto row = area.removeFromBottom(kLabelH + kBoxH);
        area.removeFromBottom(kRowGap);
        frameRect_ = area.toFloat();
        graphRect_ = area.reduced(6).toFloat();

        row.removeFromTop(kLabelH);
        boxA.setBounds(row.getX(), row.getY(), row.getWidth(), kBoxH);
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(juce::Colour(0xff20202a));
        g.fillRoundedRectangle(frameRect_.reduced(0.5f), 4.0f);
        g.setColour(accent.withAlpha(0.25f));
        g.drawRoundedRectangle(frameRect_.reduced(0.5f), 4.0f, 1.0f);

        const Geometry geo = computeGeometry();
        if (geo.valid)
        {
            const LuxDcBlockState& dst = *lux_dcblock_instance(slot_);
            const bool live = (dst.config.enabled != 0 && dst.dc_active != 0);
            const bool real = dst.ui_in_valid != 0;

            // Rémanence — slow-release envelope of the recent maxima, ghost
            // fill: the HIGHS of the stream stay readable (demo until a
            // stream has been seen).
            {
                juce::Path pk;
                pk.startNewSubPath(xOf(geo, 0.0f), geo.botY);
                for (int i = 0; i < kView; ++i)
                    pk.lineTo(xOf(geo, (float) i), yOf(geo, peakAt((float) i)));
                pk.lineTo(xOf(geo, (float) (kView - 1)), geo.botY);
                pk.closeSubPath();
                g.setColour(juce::Colours::white.withAlpha(0.06f));
                g.fillPath(pk);
                g.setColour(juce::Colours::white.withAlpha(0.25f));
                g.strokePath(pk, juce::PathStrokeType(1.0f));
            }

            // Live input — fast-release profile, the stream breathing: its
            // dips show the LOWS against the remanent highs.
            if (real)
            {
                juce::Path nw;
                for (int i = 0; i < kView; ++i)
                {
                    const float x = xOf(geo, (float) i);
                    const float y = yOf(geo, nowAt((float) i));
                    if (i == 0) nw.startNewSubPath(x, y);
                    else        nw.lineTo(x, y);
                }
                g.setColour(juce::Colours::white.withAlpha(0.55f));
                g.strokePath(nw, juce::PathStrokeType(1.1f));
            }

            // Output = the remanent envelope minus the removed DC, filled in
            // accent — the result of the current Amount.
            {
                juce::Path out;
                out.startNewSubPath(xOf(geo, 0.0f), geo.botY);
                for (int i = 0; i < kView; ++i)
                    out.lineTo(xOf(geo, (float) i),
                               yOf(geo, juce::jmax(0.0f, peakAt((float) i) - geo.cutN)));
                out.lineTo(xOf(geo, (float) (kView - 1)), geo.botY);
                out.closeSubPath();
                g.setColour(accent.withAlpha(live ? 0.45f : 0.28f));
                g.fillPath(out);
                g.setColour(accent.withAlpha(live ? 0.95f : 0.65f));
                g.strokePath(out, juce::PathStrokeType(1.2f));
            }

            // DC — thin reference line at the measured mean (what "100 %"
            // removes), labelled on the line.
            {
                const float y = yOf(geo, geo.dcN);
                g.setColour(juce::Colours::white.withAlpha(0.30f));
                g.drawLine(geo.plot.getX(), y, geo.plot.getRight(), y, 1.0f);
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
                g.setColour(juce::Colours::white.withAlpha(0.40f));
                const float ly = (y - 11.0f > geo.topY) ? y - 11.0f : y + 3.0f;
                g.drawText("DC", (int) geo.plot.getX() + 4, (int) ly,
                           40, 9, juce::Justification::centredLeft, false);
            }

            // BLOCK — dashed removed-energy line across the plot (HORIZONTAL,
            // same reading as the LEVELS floor), labelled on the line.
            // Display-only: Amount is set from the numeric box below.
            {
                const float y = yOf(geo, geo.cutN);
                const float dash[2] = { 4.0f, 3.0f };
                g.setColour(accent.withAlpha(0.7f));
                g.drawDashedLine(juce::Line<float>(geo.plot.getX(), y,
                                                   geo.plot.getRight(), y),
                                 dash, 2, 1.2f);
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
                g.setColour(accent.withAlpha(0.55f));
                const float ly = (y - 11.0f > geo.topY) ? y - 11.0f : y + 3.0f;
                g.drawText("Block", (int) (geo.plot.getRight() - 94.0f), (int) ly,
                           70, 9, juce::Justification::centredRight, false);
            }
        }

        g.setColour(accent.withAlpha(0.45f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        g.drawText("IN - OUT", (int) frameRect_.getX() + 8,
                   (int) frameRect_.getY() + 2,
                   110, 9, juce::Justification::centredLeft, false);

        g.setColour(accent.withAlpha(0.6f));
        {
            auto bb = boxA.getBounds();
            g.drawText("Amount", bb.getX(), bb.getY() - kLabelH, bb.getWidth(),
                       kLabelH, juce::Justification::centred, false);
        }
    }

private:
    /** View width — one column per profile bin (the C engine publishes the
     *  stream's max-hold profile at this resolution). */
    static constexpr int kView = LUX_DCBLOCK_UI_BINS;

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float topY = 0, botY = 0;
        float dcN  = 0;            // 0..1 — measured (or demo) DC level
        float cutN = 0;            // 0..1 — removed energy = amount * dcN
        bool  valid = false;
    };

    /** Fixed demo input: three masses (tall/narrow, small, broad faint) —
     *  identical to CentroEditorComponent::demoMass. */
    static float demoMass(float x)
    {
        auto gauss = [](float x0, float c0, float s0, float a0)
        {
            const float d = (x0 - c0) / s0;
            return a0 * std::exp(-0.5f * d * d);
        };
        const float m = gauss(x, 34.0f, 7.0f, 0.85f)
                      + gauss(x, 58.0f, 3.5f, 0.40f)
                      + gauss(x, 96.0f, 11.0f, 0.30f);
        return juce::jlimit(0.0f, 1.0f, m);
    }

    /** Mean of the demo profile — the demo's DC level. */
    static float demoDc()
    {
        static const float dc = []
        {
            float s = 0.0f;
            for (int i = 0; i < kView; ++i)
                s += demoMass((float) i);
            return s / (float) kView;
        }();
        return dc;
    }

    /** Rémanence layer — slow-release envelope of the recent maxima. The
     *  output preview rides THIS (a steady picture); the demo masses stand in
     *  until a stream has been seen. */
    float peakAt(float x) const
    {
        const LuxDcBlockState& dst = *lux_dcblock_instance(slot_);
        if (dst.ui_in_valid)
        {
            const int i = juce::jlimit(0, kView - 1, (int) x);
            return juce::jlimit(0.0f, 1.0f, dst.ui_in_peak[i]);
        }
        return demoMass(x);
    }

    /** Live layer — fast-release profile of the stream as it breathes. */
    float nowAt(float x) const
    {
        const LuxDcBlockState& dst = *lux_dcblock_instance(slot_);
        const int i = juce::jlimit(0, kView - 1, (int) x);
        return juce::jlimit(0.0f, 1.0f, dst.ui_in_now[i]);
    }

    Geometry computeGeometry() const
    {
        Geometry geo;
        if (graphRect_.getWidth() < 30.0f || graphRect_.getHeight() < 16.0f) return geo;
        const LuxDcBlockState& dst = *lux_dcblock_instance(slot_);
        geo.plot = graphRect_.reduced(8.0f, 7.0f);
        geo.topY = geo.plot.getY();
        geo.botY = geo.plot.getBottom();
        geo.dcN  = dst.ui_in_valid ? juce::jlimit(0.0f, 1.0f, dst.ui_dc)
                                   : demoDc();
        geo.cutN = juce::jlimit(0.0f, 100.0f, amt.value) * 0.01f * geo.dcN;
        geo.valid = true;
        return geo;
    }

    float xOf(const Geometry& geo, float demoPx) const
    { return geo.plot.getX() + (demoPx / (float) (kView - 1)) * geo.plot.getWidth(); }

    float yOf(const Geometry& geo, float energyN) const
    { return geo.botY - juce::jlimit(0.0f, 1.0f, energyN) * (geo.botY - geo.topY); }

    //==========================================================================
    void timerCallback() override
    {
        if (!isShowing()) return;
        repaint();
    }

    //==========================================================================
    struct Bound
    {
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attach;
        float value = 0.0f;   // read for drawing only — edits go through the box
    };

    void bind(Bound& bnd, const juce::String& id)
    {
        bnd.param = apvts.getParameter(id);
        jassert(bnd.param != nullptr);
        if (bnd.param == nullptr) return;
        bnd.attach = std::make_unique<juce::ParameterAttachment>(
            *bnd.param, [this, &bnd](float v) { bnd.value = v; repaint(); });
        bnd.attach->sendInitialUpdate();
    }

    void initBox(Sp3ctraBarSlider& box, const juce::String& id,
                 std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>& att,
                 double resetValue)
    {
        box.setAccent(accent);
        box.setDoubleClickReturnValue(true, resetValue);
        addAndMakeVisible(box);
        att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, id, box);
    }

    static constexpr int   kBoxH   = 18;
    static constexpr int   kLabelH = 10;
    static constexpr int   kRowGap = 6;

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live layers)

    Bound amt;
    Sp3ctraBarSlider boxA;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> boxAAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::unique_ptr<MidiLearnAttachment> learnA_;

    juce::Rectangle<float> frameRect_;   // the graphic window (frame only)
    juce::Rectangle<float> graphRect_;   // plot area inside the frame

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DcBlockEditorComponent)
};
