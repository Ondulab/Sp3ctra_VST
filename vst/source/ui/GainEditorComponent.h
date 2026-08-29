/**
 * @file GainEditorComponent.h
 * @brief Interactive editor for the LuxGain GAIN stage (Gain dB).
 *
 * Mirrors the DcBlockEditorComponent feel AND its visual language: the graphic
 * shows the REAL input stream as two live layers (30 Hz):
 *
 *   • rémanence (ghost fill) — ui_in_peak, the slow-release envelope of the
 *     recent maxima: the HIGHS of the stream stay readable for ~1-2 s;
 *   • live line — ui_in_now, the fast-release profile: the stream breathing,
 *     its dips show the LOWS against the remanent highs.
 *
 * The accent output curve is the remanent envelope times the current gain
 * (clamped to full scale), so the picture stays steady while the stream
 * moves. Until the instance has seen a stream, a fixed demo line (the same
 * three masses as CENTROID/LEVELS/DC BLOCK) keeps the editor readable.
 *
 * The stream view is DISPLAY-ONLY: Gain is set from the numeric box in its
 * own row below the frame (Sp3ctraBarSlider — double-click cycles
 * min/centre/max, centre = 0 dB = unity).
 *
 * Skeleton (frame, caption, box row + label) = ModuleChrome; the box takes
 * the shared handle colour.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "ModuleEditorChrome.h"
#include "Sp3ctraBarSlider.h"
#include "../processing/lux_gain.h"   // self-manages extern "C" linkage

class GainEditorComponent : public juce::Component,
                            private juce::Timer
{
public:
    static constexpr int kGraphH     = 150;  // the graphic frame alone
    // frame + gap + label + box row
    static constexpr int kPreferredH = kGraphH + ModuleChrome::kBelowFrameH;

    GainEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                        juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        // Unbound until the owning tab calls setInstance() with the selected
        // instance's bank ids (luxgain{slot}_*).
        startTimerHz(30);   // fluid layers — the rémanence lives on screen
    }

    ~GainEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance);
     *  the right-click popups then follow every rebind. */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** (Re)bind the box to one instance's bank and point the live layers at
     *  that instance's pool slot. */
    void setInstance(int slot, const juce::String& gainId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        gain.attach.reset();
        boxGAtt.reset();
        bind(gain, gainId);
        initBox(boxG, gainId, boxGAtt, 0.0);

        learnG_.reset();
        if (midiMap_ != nullptr)
            learnG_ = std::make_unique<MidiLearnAttachment>(*midiMap_, boxG, gainId);
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    void resized() override
    {
        auto area = getLocalBounds();
        // Controls OUT of the graphic frame — box row below it.
        auto row = area.removeFromBottom(ModuleChrome::kBoxRowH);
        area.removeFromBottom(ModuleChrome::kRowGap);
        frameRect_ = area.toFloat();
        graphRect_ = ModuleChrome::graphOf(frameRect_);
        ModuleChrome::layoutBoxRow(row, { &boxG });
    }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawFrame(g, frameRect_, accent);

        const Geometry geo = computeGeometry();
        if (geo.valid)
        {
            const LuxGainState& gst = *lux_gain_instance(slot_);
            const bool live = (gst.config.enabled != 0 && gst.gain_active != 0);
            const bool real = gst.ui_in_valid != 0;

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

            // Output = the remanent envelope times the current gain (clamped),
            // filled in accent — the result of the current Gain.
            {
                juce::Path out;
                out.startNewSubPath(xOf(geo, 0.0f), geo.botY);
                for (int i = 0; i < kView; ++i)
                    out.lineTo(xOf(geo, (float) i),
                               yOf(geo, juce::jmin(1.0f, peakAt((float) i) * geo.gainLin)));
                out.lineTo(xOf(geo, (float) (kView - 1)), geo.botY);
                out.closeSubPath();
                g.setColour(accent.withAlpha(live ? 0.45f : 0.28f));
                g.fillPath(out);
                g.setColour(accent.withAlpha(live ? 0.95f : 0.65f));
                g.strokePath(out, juce::PathStrokeType(1.2f));
            }
        }

        ModuleChrome::drawCaption(g, frameRect_, accent, "IN - OUT");
        ModuleChrome::drawBoxLabel(g, boxG, accent, "Gain");
    }

private:
    /** View width — one column per profile bin (the C engine publishes the
     *  stream's max-hold profile at this resolution). */
    static constexpr int kView = LUX_GAIN_UI_BINS;

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float topY = 0, botY = 0;
        float gainLin = 1.0f;      // linear factor of the current Gain (dB)
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

    /** Rémanence layer — slow-release envelope of the recent maxima. The
     *  output preview rides THIS (a steady picture); the demo masses stand in
     *  until a stream has been seen. */
    float peakAt(float x) const
    {
        const LuxGainState& gst = *lux_gain_instance(slot_);
        if (gst.ui_in_valid)
        {
            const int i = juce::jlimit(0, kView - 1, (int) x);
            return juce::jlimit(0.0f, 1.0f, gst.ui_in_peak[i]);
        }
        return demoMass(x);
    }

    /** Live layer — fast-release profile of the stream as it breathes. */
    float nowAt(float x) const
    {
        const LuxGainState& gst = *lux_gain_instance(slot_);
        const int i = juce::jlimit(0, kView - 1, (int) x);
        return juce::jlimit(0.0f, 1.0f, gst.ui_in_now[i]);
    }

    Geometry computeGeometry() const
    {
        Geometry geo;
        if (graphRect_.getWidth() < 30.0f || graphRect_.getHeight() < 16.0f) return geo;
        geo.plot = ModuleChrome::plotOf(frameRect_);
        geo.topY = geo.plot.getY();
        geo.botY = geo.plot.getBottom();
        geo.gainLin = std::pow(10.0f, gain.value / 20.0f);
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
        box.setDoubleClickReturnValue(true, resetValue);
        addAndMakeVisible(box);
        att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(apvts, id, box);
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live layers)

    Bound gain;
    Sp3ctraBarSlider boxG;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> boxGAtt;
    MidiMappingEngine* midiMap_ = nullptr;
    std::unique_ptr<MidiLearnAttachment> learnG_;

    juce::Rectangle<float> frameRect_;   // the graphic window (frame only)
    juce::Rectangle<float> graphRect_;   // graph area inside the frame (ModuleChrome::graphOf)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GainEditorComponent)
};
