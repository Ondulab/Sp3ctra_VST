/**
 * @file EqEditorComponent.h
 * @brief Interactive graphic-EQ curve editor for the LuxEq insert.
 *
 * Same visual idiom as ScoreEqComponent (draggable accent curve + node
 * handles, X = frequency log axis, Y = gain in dB, 0 dB at the centre) but
 * bound to APVTS params (host-automatable, MIDI-mappable) like the other FX
 * editors: one luxeq{slot}_Band{k} param per node, rebindable per instance.
 *
 * The node count is user-selectable (top-right dropdown → luxeq{slot}_NumPoints,
 * 2..LUX_EQ_NUM_BANDS, default 2 = one straight line) — the C engine spreads
 * the same nodes over the pixel axis, so the curve is positional whatever the
 * span. On a count change the current spline is resampled at the new node
 * positions so the drawn shape survives the re-grid.
 * Between nodes the curve is the SHARED Catmull-Rom spline (lux_eq_curve_db):
 * the path is sampled from the exact evaluator the RT LUT uses.
 * The curve brightens while the bound pool instance is shaping a stream —
 * by default the LuxEq pool; hosts embedding the editor for another engine's
 * EQ bank (CENTROID's output EQ) repoint `liveProvider` at their own pool.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <functional>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../processing/lux_eq.h"   // self-manages extern "C" linkage

class EqEditorComponent : public juce::Component,
                          private juce::Timer
{
public:
    static constexpr int   kPreferredH = 150;
    static constexpr float kGainRange  = LUX_EQ_GAIN_DB_MAX;   // ± dB

    EqEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                      juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        // Unbound until the owning tab calls setInstance() with the selected
        // instance's bank ids (luxeq{slot}_Band*).
        for (int n = 2; n <= LUX_EQ_NUM_BANDS; ++n)
            pointsCombo_.addItem(juce::String(n) + " pts", n);
        pointsCombo_.setSelectedId(numBands_, juce::dontSendNotification);
        pointsCombo_.onChange = [this] { pointsComboChanged(); };
        addAndMakeVisible(pointsCombo_);

        setRepaintsOnMouseActivity(true);
        startTimerHz(30);
    }

    ~EqEditorComponent() override { stopTimer(); }

    /** Optional MIDI-learn wiring — right-click near a node then targets that
     *  band's parameter (nearest node by x, like dragging). */
    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** Live-glow source: true while the engine instance bound to `slot` is
     *  actually applying this curve. Defaults to the LuxEq pool — replace it
     *  when the editor drives another engine's EQ bank. */
    std::function<bool(int slot)> liveProvider = [](int slot)
    {
        const LuxEqState& st = *lux_eq_instance(slot);
        return st.config.enabled != 0 && st.eq_active != 0;
    };

    /** (Re)bind the nodes to one instance's band params and point the live
     *  overlay at that instance's pool slot. `bandIds` must hold exactly
     *  LUX_EQ_NUM_BANDS ids, node order (low → high frequency);
     *  `numPointsId` is the instance's node-count choice param. */
    void setInstance(int slot, const juce::StringArray& bandIds,
                     const juce::String& numPointsId)
    {
        jassert(bandIds.size() == LUX_EQ_NUM_BANDS);
        slot_ = juce::jlimit(0, 7, slot);
        bandIds_ = bandIds;
        for (int b = 0; b < LUX_EQ_NUM_BANDS; ++b)
        {
            bands[(size_t) b].attach.reset();
            if (b < bandIds.size())
                bind(bands[(size_t) b], bandIds[b]);
        }

        // Node-count dropdown ← luxeq{slot}_NumPoints (choice "2".."9").
        pointsAttach_.reset();
        pointsLearn_.reset();
        if (auto* p = apvts.getParameter(numPointsId))
        {
            pointsAttach_ = std::make_unique<juce::ParameterAttachment>(
                *p, [this](float v)
                {
                    numBands_ = juce::jlimit(2, LUX_EQ_NUM_BANDS,
                                             2 + (int) std::lround(v));
                    pointsCombo_.setSelectedId(numBands_,
                                               juce::dontSendNotification);
                    repaint();
                });
            pointsAttach_->sendInitialUpdate();
            if (midiMap_ != nullptr)
                pointsLearn_ = std::make_unique<MidiLearnAttachment>(
                    *midiMap_, pointsCombo_, numPointsId);
        }
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        auto bf = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff20202a));
        g.fillRoundedRectangle(bf.reduced(0.5f), 4.0f);
        g.setColour(accent.withAlpha(0.25f));
        g.drawRoundedRectangle(bf.reduced(0.5f), 4.0f, 1.0f);

        const auto plot = plotArea();
        const bool live = liveProvider && liveProvider(slot_);

        // Horizontal gain grid: 0 dB centre (brighter) + ±12 / ±24.
        for (int db = -24; db <= 24; db += 12)
        {
            const float y = gainToY((float) db, plot);
            g.setColour(db == 0 ? juce::Colour(0x22ffffff) : juce::Colour(0x10ffffff));
            g.drawHorizontalLine((int) y, plot.getX(), plot.getRight());
            g.setColour(accent.withAlpha(0.35f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            g.drawText((db > 0 ? "+" : "") + juce::String(db),
                       (int) plot.getX() + 2, (int) y - 6, 26, 11,
                       juce::Justification::left, false);
        }

        // Vertical node grid + Hz labels (default instrument range — the
        // engine's curve is positional, labels are informative).
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        for (int i = 0; i < numBands_; ++i)
        {
            const float x = nodeX(i, plot);
            g.setColour(juce::Colour(0x0cffffff));
            g.drawVerticalLine((int) x, plot.getY(), plot.getBottom());
            if (i == 0 || i + 1 == numBands_ || (i % 2 == 0))
            {
                const double f = kMinFreq
                    * std::pow(2.0, 8.0 * (double) i / (double) (numBands_ - 1));
                const juce::String lbl = (f >= 1000.0)
                    ? juce::String(f / 1000.0, f >= 10000.0 ? 0 : 1) + "k"
                    : juce::String((int) std::lround(f));
                g.setColour(accent.withAlpha(0.4f));
                g.drawText(lbl, (int) x - 16, (int) plot.getBottom() + 1, 32, 10,
                           juce::Justification::centred, false);
            }
        }

        // Curve + filled area to the 0 dB line — sampled from the SAME
        // Catmull-Rom evaluator the engine's LUT uses (lux_eq_curve_db), so
        // the drawn spline is exactly the applied gain.
        float nodeDb[LUX_EQ_NUM_BANDS];
        for (int i = 0; i < numBands_; ++i)
            nodeDb[i] = bands[(size_t) i].value;
        juce::Path curve, fill;
        const float y0 = gainToY(0.0f, plot);
        const int steps = juce::jmax(48, (int) plot.getWidth() / 3);
        for (int s = 0; s <= steps; ++s)
        {
            const float u  = (float) s / (float) steps;
            const float x  = plot.getX() + u * plot.getWidth();
            const float y  = gainToY(lux_eq_curve_db(nodeDb, numBands_,
                                 u * (float) (numBands_ - 1)), plot);
            if (s == 0) { curve.startNewSubPath(x, y); fill.startNewSubPath(x, y0); fill.lineTo(x, y); }
            else        { curve.lineTo(x, y); fill.lineTo(x, y); }
        }
        fill.lineTo(plot.getRight(), y0);
        fill.closeSubPath();
        g.setColour(accent.withAlpha(live ? 0.16f : 0.10f));
        g.fillPath(fill);
        g.setColour(accent.withAlpha(live ? 0.95f : 0.55f));
        g.strokePath(curve, juce::PathStrokeType(1.6f));

        // Node handles.
        for (int i = 0; i < numBands_; ++i)
        {
            const float x = nodeX(i, plot);
            const float y = gainToY(bands[(size_t) i].value, plot);
            const bool active = (i == hovered || i == dragging);
            const float rad = 4.5f;
            if (active)
            {
                g.setColour(accent.withAlpha(0.25f));
                g.fillEllipse(x - rad - 2.5f, y - rad - 2.5f, 2 * (rad + 2.5f), 2 * (rad + 2.5f));
            }
            g.setColour(active ? accent.brighter(0.3f) : juce::Colour(0xff20202a));
            g.fillEllipse(x - rad, y - rad, 2 * rad, 2 * rad);
            g.setColour(active ? juce::Colours::white : accent.withAlpha(0.9f));
            g.drawEllipse(x - rad, y - rad, 2 * rad, 2 * rad, 1.4f);
        }

        // Title + hint.
        g.setColour(accent.withAlpha(0.75f));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        g.drawText("GAIN CURVE", (int) plot.getX(), (int) bf.getY() + 2, 90, 10,
                   juce::Justification::left, false);
        if (isFlat())
        {
            g.setColour(juce::Colour(0xff55606f));
            g.drawText(juce::String::fromUTF8("drag to shape  \xc2\xb7  double-click to reset"),
                       (int) plot.getRight() - kComboW - 6 - 220, (int) bf.getY() + 2, 220, 10,
                       juce::Justification::right, false);
        }
    }

    void resized() override
    {
        pointsCombo_.setBounds(getWidth() - 6 - kComboW, 1, kComboW, 15);
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        if (dragging != -1) return;
        const int h = nodeAt(e.position);
        if (h != hovered) { hovered = h; repaint(); }
        setMouseCursor(h == -1 ? juce::MouseCursor::NormalCursor
                               : juce::MouseCursor::UpDownResizeCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (dragging == -1 && hovered != -1) { hovered = -1; repaint(); }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        // Right-click: MIDI Learn menu for the nearest band's parameter.
        if (e.mods.isPopupMenu())
        {
            const int n = nearestNodeByX(e.position);
            if (midiMap_ != nullptr && n >= 0 && n < bandIds_.size())
                MidiLearnPopup::show(*midiMap_, bandIds_[n], this);
            return;
        }

        dragging = nearestNodeByX(e.position);
        hovered  = dragging;
        if (dragging < 0) return;
        auto& bnd = bands[(size_t) dragging];
        if (e.getNumberOfClicks() >= 2)
        {
            // One-shot reset to 0 dB — no gesture kept open.
            bnd.begin(); bnd.setGesture(0.0f); bnd.end();
            dragging = -1;
            repaint();
            return;
        }
        bnd.begin();
        applyDrag(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override { applyDrag(e); }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (dragging >= 0) bands[(size_t) dragging].end();
        dragging = -1;
        hovered  = nodeAt(e.position);
        repaint();
    }

private:
    // Default instrument span (C2 → ~16.7 kHz, 8 octaves) — label grid only.
    static constexpr double kMinFreq = 65.41;
    static constexpr int    kComboW  = 72;   // points dropdown (top-right corner)

    /** Dropdown edit — resample the current spline at the new node positions
     *  (shape preserved as closely as the new count allows), publish the new
     *  band values, then the new count. Host/preset changes of the NumPoints
     *  param bypass this and just re-grid (pointsAttach_ callback). */
    void pointsComboChanged()
    {
        const int newN = pointsCombo_.getSelectedId();
        if (newN < 2 || newN == numBands_ || pointsAttach_ == nullptr)
            return;
        float old[LUX_EQ_NUM_BANDS];
        const int oldN = juce::jlimit(2, LUX_EQ_NUM_BANDS, numBands_);
        for (int i = 0; i < oldN; ++i)
            old[i] = bands[(size_t) i].value;
        for (int i = 0; i < newN; ++i)
        {
            const float x = (float) i * (float) (oldN - 1) / (float) (newN - 1);
            if (bands[(size_t) i].attach)
                bands[(size_t) i].attach->setValueAsCompleteGesture(
                    lux_eq_curve_db(old, oldN, x));
        }
        pointsAttach_->setValueAsCompleteGesture((float) (newN - 2));
    }

    juce::Rectangle<float> plotArea() const
    {
        return getLocalBounds().toFloat().reduced(6.0f)
                               .withTrimmedTop(12.0f).withTrimmedBottom(12.0f);
    }
    float nodeX(int i, juce::Rectangle<float> p) const
    {
        return p.getX() + ((float) i / (float) (numBands_ - 1)) * p.getWidth();
    }
    float gainToY(float db, juce::Rectangle<float> p) const
    {
        return p.getCentreY() - (db / kGainRange) * (p.getHeight() * 0.5f);
    }
    float yToGain(float y, juce::Rectangle<float> p) const
    {
        const float g = (p.getCentreY() - y) / (p.getHeight() * 0.5f) * kGainRange;
        return juce::jlimit(-kGainRange, kGainRange, g);
    }
    int nearestNodeByX(juce::Point<float> pt) const
    {
        const auto p = plotArea();
        int best = -1; float bd = 1e9f;
        for (int i = 0; i < numBands_; ++i)
        {
            const float dx = std::abs(nodeX(i, p) - pt.x);
            if (dx < bd) { bd = dx; best = i; }
        }
        return best;
    }
    int nodeAt(juce::Point<float> pt) const
    {
        const auto p = plotArea();
        for (int i = 0; i < numBands_; ++i)
        {
            const float x = nodeX(i, p), y = gainToY(bands[(size_t) i].value, p);
            if (pt.getDistanceFrom({ x, y }) <= 11.0f) return i;
        }
        return -1;
    }
    void applyDrag(const juce::MouseEvent& e)
    {
        if (dragging < 0) return;
        bands[(size_t) dragging].setGesture(yToGain(e.position.y, plotArea()));
        hovered = dragging;
        repaint();
    }
    bool isFlat() const noexcept
    {
        for (int i = 0; i < numBands_; ++i)
            if (std::abs(bands[(size_t) i].value) > 0.01f) return false;
        return true;
    }

    //==========================================================================
    void timerCallback() override { if (isShowing()) repaint(); }

    //==========================================================================
    struct Bound
    {
        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attach;
        float value = 0.0f;
        void begin()            { if (attach) attach->beginGesture(); }
        void end()              { if (attach) attach->endGesture(); }
        void setGesture(float v){ if (attach) attach->setValueAsPartOfGesture(v); }
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

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live overlay)
    juce::StringArray bandIds_;            // per-band param ids (learn popup)
    MidiMappingEngine* midiMap_ = nullptr;

    std::array<Bound, LUX_EQ_NUM_BANDS> bands;

    // Node-count dropdown ↔ luxeq{slot}_NumPoints (2..LUX_EQ_NUM_BANDS).
    int numBands_ = 2;
    juce::ComboBox pointsCombo_;
    std::unique_ptr<juce::ParameterAttachment> pointsAttach_;
    std::unique_ptr<MidiLearnAttachment>       pointsLearn_;

    int hovered = -1, dragging = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EqEditorComponent)
};
