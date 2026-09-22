/**
 * @file DiffEditorComponent.h
 * @brief Interactive editor for the LuxDiff DIFF stage (reference subtraction).
 *
 * Mirrors the GainEditorComponent feel AND its visual language: the graphic
 * shows the REAL input stream as two live layers (30 Hz):
 *
 *   • rémanence (ghost fill) — ui_in_peak, the slow-release envelope of the
 *     recent maxima: the HIGHS of the stream stay readable for ~1-2 s;
 *   • live line — ui_in_now, the fast-release profile: the stream breathing,
 *     its dips show the LOWS against the remanent highs.
 *
 * On top of them sits the REFERENCE profile (ui_ref, dashed — what CAPTURE
 * froze), and the accent output curve is the remanent envelope minus the
 * reference (ADD: what appears; ABS: what appears or vanishes), so the
 * picture stays steady while the stream moves. Until the instance has seen
 * a stream, a fixed demo line (the same three masses as CENTROID/LEVELS/
 * GAIN) keeps the editor readable.
 *
 * The stream view is DISPLAY-ONLY: the controls live in their own row below
 * the frame — Amount (Sp3ctraBarSlider), Mode (combo), Follow (combo: Hold /
 * Track), Time (the Track lag, lines), CAPTURE and CLEAR (action buttons —
 * MIDI-learnable through the processor's virtual sink).
 *
 * Skeleton (frame, caption, box row + labels) = ModuleChrome; the controls
 * take the shared handle colour.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <functional>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "ModuleEditorChrome.h"
#include "Sp3ctraControls.h"
#include "Sp3ctraBarSlider.h"
#include "../processing/lux_diff.h"   // self-manages extern "C" linkage

class DiffEditorComponent : public juce::Component,
                            private juce::Timer
{
public:
    static constexpr int kGraphH     = 150;  // the graphic frame alone
    // frame + gap + label + box row
    static constexpr int kPreferredH = kGraphH + ModuleChrome::kBelowFrameH;

    DiffEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                        juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        // Mode choices — must match the AudioParameterChoice order (Add, Abs).
        modeCombo.addItemList({ "Add", "Abs" }, 1);
        modeCombo.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(modeCombo);
        // Follow choices — must match the AudioParameterChoice order (Hold, Track).
        followCombo.addItemList({ "Hold", "Track" }, 1);
        followCombo.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(followCombo);

        // Action buttons — plain chrome at rest, the CAPTURE button lights
        // while a learning window runs (toggle state driven by the timer).
        for (auto* b : { &captureBtn, &clearBtn })
        {
            b->setColour(juce::TextButton::buttonColourId,   juce::Colour(0xff2a2a2a));
            b->setColour(juce::TextButton::buttonOnColourId,
                         juce::Colour(Sp3ctraTheme::kColHandle).withMultipliedBrightness(0.45f));
            b->setColour(juce::TextButton::textColourOffId,  juce::Colour(0xffb0b0b0));
            b->setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
            b->setClickingTogglesState(false);
            addAndMakeVisible(*b);
        }
        captureBtn.onClick = [this] { if (onCapture) onCapture(); };
        clearBtn.onClick   = [this] { if (onClear)   onClear();   };

        // Unbound until the owning tab calls setInstance() with the selected
        // instance's bank ids (luxdiff{slot}_*).
        startTimerHz(30);   // fluid layers — the rémanence lives on screen
    }

    ~DiffEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — set once (before the first setInstance);
     *  the right-click popups then follow every rebind. */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** Action hooks — the owning tab routes them to the processor (which
     *  owns the request path to the image thread). */
    std::function<void()> onCapture;
    std::function<void()> onClear;

    /** (Re)bind the controls to one instance's bank and point the live layers
     *  at that instance's pool slot. `captureId` / `clearId` are the VIRTUAL
     *  MIDI targets of the two buttons (not APVTS params). */
    void setInstance(int slot,
                     const juce::String& amountId, const juce::String& modeId,
                     const juce::String& followId, const juce::String& timeId,
                     const juce::String& captureId, const juce::String& clearId)
    {
        slot_ = juce::jlimit(0, 7, slot);
        amount.attach.reset();
        mode.attach.reset();
        follow.attach.reset();
        boxAAtt.reset();
        boxTAtt.reset();
        modeAtt.reset();
        followAtt.reset();
        bind(amount, amountId);
        bind(mode,   modeId);
        // Follow gates the Time box: a lag only means something while tracking.
        follow.bind(apvts, followId, [this](float v)
                    { boxT.setEnabled(v >= 0.5f); repaint(); });
        initBox(boxA, amountId, boxAAtt, 100.0);
        initBox(boxT, timeId,   boxTAtt, 256.0);
        modeAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            apvts, modeId, modeCombo);
        followAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            apvts, followId, followCombo);
        boxT.setEnabled(follow.value >= 0.5f);

        learn_.clear();
        if (midiMap_ != nullptr)
        {
            learn_.push_back(std::make_unique<MidiLearnAttachment>(*midiMap_, boxA,        amountId));
            learn_.push_back(std::make_unique<MidiLearnAttachment>(*midiMap_, modeCombo,   modeId));
            learn_.push_back(std::make_unique<MidiLearnAttachment>(*midiMap_, followCombo, followId));
            learn_.push_back(std::make_unique<MidiLearnAttachment>(*midiMap_, boxT,        timeId));
            learn_.push_back(std::make_unique<MidiLearnAttachment>(*midiMap_, captureBtn,  captureId));
            learn_.push_back(std::make_unique<MidiLearnAttachment>(*midiMap_, clearBtn,    clearId));
        }
        syncButtons();
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
        ModuleChrome::layoutBoxRow(row, { &boxA, &modeCombo, &followCombo, &boxT,
                                          &captureBtn, &clearBtn });
    }

    void paint(juce::Graphics& g) override
    {
        ModuleChrome::drawFrame(g, frameRect_, accent);

        const Geometry geo = computeGeometry();
        const LuxDiffState& dst = *lux_diff_instance(slot_);
        if (geo.valid)
        {
            const bool live = (dst.config.enabled != 0 && dst.diff_active != 0);
            const bool real = dst.ui_in_valid != 0;
            const bool ref  = dst.ref_valid != 0;

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

            // Reference — what CAPTURE froze (or what TRACK is following),
            // dashed white: the profile the stream is measured against.
            if (ref)
            {
                juce::Path rp;
                for (int i = 0; i < kView; ++i)
                {
                    const float x = xOf(geo, (float) i);
                    const float y = yOf(geo, refAt(i));
                    if (i == 0) rp.startNewSubPath(x, y);
                    else        rp.lineTo(x, y);
                }
                const float dashes[] = { 4.0f, 3.0f };
                juce::Path dashed;
                juce::PathStrokeType(1.2f).createDashedStroke(dashed, rp, dashes, 2);
                g.setColour(juce::Colours::white.withAlpha(0.85f));
                g.fillPath(dashed);
            }

            // Output = the remanent envelope minus the reference (per mode),
            // filled in accent — the result of the current Amount / Mode.
            {
                juce::Path out;
                out.startNewSubPath(xOf(geo, 0.0f), geo.botY);
                for (int i = 0; i < kView; ++i)
                {
                    const float d = peakAt((float) i) - geo.amount * (ref ? refAt(i) : 0.0f);
                    const float e = geo.absMode ? std::abs(d) : juce::jmax(0.0f, d);
                    out.lineTo(xOf(geo, (float) i), yOf(geo, juce::jmin(1.0f, e)));
                }
                out.lineTo(xOf(geo, (float) (kView - 1)), geo.botY);
                out.closeSubPath();
                g.setColour(accent.withAlpha(live ? 0.45f : 0.28f));
                g.fillPath(out);
                g.setColour(accent.withAlpha(live ? 0.95f : 0.65f));
                g.strokePath(out, juce::PathStrokeType(1.2f));
            }

            // Learning progress — a thin bar along the plot bottom.
            if (dst.learning != 0)
            {
                const float p = lux_diff_learn_progress(&dst);
                g.setColour(juce::Colour(Sp3ctraTheme::kColHandle).withAlpha(0.9f));
                g.fillRect(geo.plot.getX(), geo.botY + 2.0f,
                           geo.plot.getWidth() * p, 2.0f);
            }
        }

        ModuleChrome::drawCaption(g, frameRect_, accent, "IN - REF");
        const bool tracking = dst.config.follow == LUX_DIFF_FOLLOW_TRACK;
        ModuleChrome::drawReadout(g, frameRect_, accent,
            dst.learning != 0 ? "CAPTURING " + juce::String(juce::roundToInt(
                                    lux_diff_learn_progress(&dst) * 100.0f)) + " %"
          : tracking          ? (dst.ref_valid ? "TRACKING  " + juce::String(dst.config.track_lines) + " lines"
                                               : "TRACKING - waiting for the stream")
          : dst.ref_valid     ? "REF ARMED  " + juce::String(dst.ref_px) + " px"
          :                     "NO REFERENCE - press CAPTURE",
            dst.learning != 0 ? 0.95f : 0.6f);
        ModuleChrome::drawBoxLabel(g, boxA,        accent, "Amount");
        ModuleChrome::drawBoxLabel(g, modeCombo,   accent, "Mode");
        ModuleChrome::drawBoxLabel(g, followCombo, accent, "Follow");
        ModuleChrome::drawBoxLabel(g, boxT,        accent, "Time");
        ModuleChrome::drawBoxLabel(g, captureBtn,  accent, "Reference");
        ModuleChrome::drawBoxLabel(g, clearBtn,    accent, "Reference");
    }

private:
    /** View width — one column per profile bin (the C engine publishes the
     *  stream's max-hold profile at this resolution). */
    static constexpr int kView = LUX_DIFF_UI_BINS;

    struct Geometry
    {
        juce::Rectangle<float> plot;
        float topY = 0, botY = 0;
        float amount  = 1.0f;      // 0..1 fraction subtracted
        bool  absMode = false;
        bool  valid   = false;
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
        const LuxDiffState& dst = *lux_diff_instance(slot_);
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
        const LuxDiffState& dst = *lux_diff_instance(slot_);
        const int i = juce::jlimit(0, kView - 1, (int) x);
        return juce::jlimit(0.0f, 1.0f, dst.ui_in_now[i]);
    }

    /** Reference layer — the captured profile (static). */
    float refAt(int i) const
    {
        const LuxDiffState& dst = *lux_diff_instance(slot_);
        return juce::jlimit(0.0f, 1.0f, dst.ui_ref[juce::jlimit(0, kView - 1, i)]);
    }

    Geometry computeGeometry() const
    {
        Geometry geo;
        if (graphRect_.getWidth() < 30.0f || graphRect_.getHeight() < 16.0f) return geo;
        geo.plot = ModuleChrome::plotOf(frameRect_);
        geo.topY = geo.plot.getY();
        geo.botY = geo.plot.getBottom();
        geo.amount  = juce::jlimit(0.0f, 1.0f, amount.value * 0.01f);
        geo.absMode = mode.value >= 0.5f;
        geo.valid = true;
        return geo;
    }

    float xOf(const Geometry& geo, float demoPx) const
    { return geo.plot.getX() + (demoPx / (float) (kView - 1)) * geo.plot.getWidth(); }

    float yOf(const Geometry& geo, float energyN) const
    { return geo.botY - juce::jlimit(0.0f, 1.0f, energyN) * (geo.botY - geo.topY); }

    //==========================================================================
    /** Button faces follow the C state: CAPTURE lit while learning, CLEAR
     *  only enabled when there is a reference to drop. */
    void syncButtons()
    {
        const LuxDiffState& dst = *lux_diff_instance(slot_);
        captureBtn.setToggleState(dst.learning != 0, juce::dontSendNotification);
        clearBtn.setEnabled(dst.ref_valid != 0);
    }

    void timerCallback() override
    {
        if (!isShowing()) return;
        syncButtons();
        repaint();
    }

    //==========================================================================
    /** The shared parameter binding — ui/Sp3ctraControls.h. Besides the
     *  attachment and the mirrored value it carries the EDIT HEAT: any
     *  change, from the box below, from a MIDI CC or from automation, lights
     *  the handle that owns it. */
    using Bound = Sp3ctraControls::Bound;

    void bind(Bound& bnd, const juce::String& id)
    {
        bnd.bind(apvts, id, [this](float) { repaint(); });
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

    Bound amount, mode, follow;
    Sp3ctraBarSlider boxA, boxT;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> boxAAtt, boxTAtt;
    juce::ComboBox modeCombo, followCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAtt, followAtt;
    juce::TextButton captureBtn { "CAPTURE" };
    juce::TextButton clearBtn   { "CLEAR" };
    MidiMappingEngine* midiMap_ = nullptr;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learn_;

    juce::Rectangle<float> frameRect_;   // the graphic window (frame only)
    juce::Rectangle<float> graphRect_;   // graph area inside the frame (ModuleChrome::graphOf)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiffEditorComponent)
};
