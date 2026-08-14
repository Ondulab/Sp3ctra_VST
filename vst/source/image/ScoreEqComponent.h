/**
 * @file ScoreEqComponent.h
 * @brief Graphic EQ curve editor for the SCORE image (PLAY page).
 *
 * Same visual idiom as the PITCH ADSR editor (EnvelopeEditorComponent): a dark
 * rounded panel with a draggable accent curve and node handles. Here the X axis
 * is FREQUENCY (logarithmic, like the instrument's oscillator bank) and the Y
 * axis is GAIN in dB (±kGainRange, 0 dB at the vertical centre).
 *
 * It edits the GENERATED IMAGE, never the source WAV: the host queries
 * gainDbAtFreq() per image row and shifts that row's darkness by gain/dynRange.
 * The node count is user-selectable (top-right dropdown, kMinPoints..kMaxPoints,
 * default 2 = one straight line); nodes spread evenly over the log-freq span.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <cmath>
#include <functional>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"   // optional right-click MIDI-Learn per band
#include "EqCurve.h"                       // shared Catmull-Rom evaluator (LuxEq look)

class ScoreEqComponent : public juce::Component
{
public:
    static constexpr int   kPreferredH = 150;
    static constexpr float kGainRange  = 24.0f;   // ± dB
    static constexpr int   kMinPoints  = 2;       // straight line (default)
    static constexpr int   kMaxPoints  = 9;       // octave grid of the 8-oct span

    explicit ScoreEqComponent(juce::Colour accentColour) : accent(accentColour)
    {
        for (int n = kMinPoints; n <= kMaxPoints; ++n)
            pointsCombo.addItem(juce::String(n) + " pts", n);
        pointsCombo.setSelectedId(numPoints_, juce::dontSendNotification);
        pointsCombo.onChange = [this]
        {
            const int n = pointsCombo.getSelectedId();
            if (n < kMinPoints || n == numPoints_) return;
            setNumPoints(n);
            if (onChange) onChange();
        };
        addAndMakeVisible(pointsCombo);

        setRange(65.41, 16744.04);
    }

    /** Called whenever the curve changes (drag / reset). */
    std::function<void()> onChange;

    /** Enable right-click "MIDI Learn" on the nearest band node. @p idFn maps a
     *  band index (0-based, left→right) to the paramId to learn. Pass a null
     *  engine to disable (default) — right-click then keeps its drag behaviour. */
    void setBandMidiLearn(MidiMappingEngine* mm, std::function<juce::String(int)> idFn)
    { eqLearnMap_ = mm; eqLearnIdFn_ = std::move(idFn); }

    /** Number of band nodes (== the dropdown's point count). */
    int numBands() const noexcept { return (int) gains.size(); }

    /** Rebuild the node grid for a new frequency span (resets gains).
     *  No-op when the span is unchanged, so regenerating with the same musical
     *  range keeps the curve the user drew. */
    void setRange(double minHz, double maxHz)
    {
        if (minHz <= 0.0 || maxHz <= minHz) return;
        if (! gains.empty()
            && std::abs(minHz - minF) < 1e-9 && std::abs(maxHz - maxF) < 1e-9)
            return;
        minF = minHz; maxF = maxHz;
        rebuildGrid();
        gains.assign(freqs.size(), 0.0f);
        repaint();
    }

    /** Current node count (2 = one straight line … kMaxPoints). */
    int numPoints() const noexcept { return numPoints_; }

    /** Re-grid the curve onto @p n nodes (dropdown / host). The existing
     *  spline is resampled at the new node positions so the drawn shape is
     *  preserved as closely as n nodes allow. */
    void setNumPoints(int n)
    {
        n = juce::jlimit(kMinPoints, kMaxPoints, n);
        if (n == numPoints_ && (int) gains.size() == n) { syncCombo(); return; }
        const std::vector<float> old = gains;
        numPoints_ = n;
        rebuildGrid();
        gains.assign((size_t) n, 0.0f);
        if (old.size() >= 2)
            for (int i = 0; i < n; ++i)
                gains[(size_t) i] = eqCurveDbAt(old.data(), (int) old.size(),
                                                (float) i * (float) (old.size() - 1)
                                                    / (float) (n - 1),
                                                kGainRange);
        syncCombo();
        repaint();
    }

    /** Serialise the curve as "minF|maxF|g0;g1;…" for the plugin state blob. */
    juce::String encodeState() const
    {
        juce::String s;
        s << juce::String(minF, 3) << '|' << juce::String(maxF, 3) << '|';
        for (size_t i = 0; i < gains.size(); ++i)
        {
            if (i) s << ';';
            s << juce::String(gains[i], 2);
        }
        return s;
    }

    /** Restore a curve written by encodeState(). Adopts the stored node count
     *  (legacy octave grids beyond kMaxPoints are resampled down). Returns
     *  false (curve left untouched) on any mismatch. */
    bool decodeState(const juce::String& s)
    {
        const auto parts = juce::StringArray::fromTokens(s, "|", "");
        if (parts.size() != 3) return false;
        const double lo = parts[0].getDoubleValue();
        const double hi = parts[1].getDoubleValue();
        if (lo <= 0.0 || hi <= lo) return false;
        const auto gs = juce::StringArray::fromTokens(parts[2], ";", "");
        if (gs.size() < 2) return false;
        std::vector<float> loaded((size_t) gs.size());
        for (int i = 0; i < gs.size(); ++i)
            loaded[(size_t) i] = juce::jlimit(-kGainRange, kGainRange,
                                              gs[i].getFloatValue());
        minF = lo; maxF = hi;
        numPoints_ = juce::jlimit(kMinPoints, kMaxPoints, gs.size());
        rebuildGrid();
        gains.assign((size_t) numPoints_, 0.0f);
        for (int i = 0; i < numPoints_; ++i)
            gains[(size_t) i] = ((int) loaded.size() == numPoints_)
                ? loaded[(size_t) i]
                : eqCurveDbAt(loaded.data(), (int) loaded.size(),
                              (float) i * (float) (loaded.size() - 1)
                                  / (float) (numPoints_ - 1), kGainRange);
        syncCombo();
        repaint();
        return true;
    }

    /** Reset every band to 0 dB. */
    void reset()
    {
        std::fill(gains.begin(), gains.end(), 0.0f);
        repaint();
        if (onChange) onChange();
    }

    bool isFlat() const noexcept
    {
        for (float g : gains) if (std::abs(g) > 0.01f) return false;
        return true;
    }

    /** True while a node is being dragged — host defers the heavy image reapply. */
    bool isDragging() const noexcept { return dragging != -1; }

    /** Gain (dB) at an arbitrary frequency — smooth Catmull-Rom spline through
     *  the nodes (same evaluator family as the LuxEq module), sampled in
     *  log-freq node space so what paint() draws is exactly what applies. */
    float gainDbAtFreq(double hz) const noexcept
    {
        if (freqs.size() < 2) return gains.empty() ? 0.0f : gains.front();
        if (hz <= freqs.front()) return gains.front();
        if (hz >= freqs.back())  return gains.back();
        const double x = std::log(hz / minF) / std::log(maxF / minF)
                       * (double) (gains.size() - 1);
        return eqCurveDbAt(gains.data(), (int) gains.size(), (float) x, kGainRange);
    }

    /** Curve snapshot accessors — lets hosts capture the curve for use on a
     *  background thread (see MidiScoreGenTabComponent's playback shaping). */
    double getMinFreq() const noexcept { return minF; }
    double getMaxFreq() const noexcept { return maxF; }
    const std::vector<float>& getGains() const noexcept { return gains; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        auto bf = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff20202a));
        g.fillRoundedRectangle(bf.reduced(0.5f), 4.0f);
        g.setColour(accent.withAlpha(0.25f));
        g.drawRoundedRectangle(bf.reduced(0.5f), 4.0f, 1.0f);

        const auto plot = plotArea();

        // Horizontal gain grid: 0 dB centre (brighter) + ±12 / ±24.
        for (int db = -24; db <= 24; db += 12)
        {
            const float y = gainToY(db, plot);
            g.setColour(db == 0 ? juce::Colour(0x22ffffff) : juce::Colour(0x10ffffff));
            g.drawHorizontalLine((int) y, plot.getX(), plot.getRight());
            g.setColour(accent.withAlpha(0.35f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
            g.drawText((db > 0 ? "+" : "") + juce::String(db),
                       (int) plot.getX() + 2, (int) y - 6, 26, 11,
                       juce::Justification::left, false);
        }

        // Vertical octave grid + Hz labels.
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        for (size_t i = 0; i < freqs.size(); ++i)
        {
            const float x = freqToX(freqs[i], plot);
            g.setColour(juce::Colour(0x0cffffff));
            g.drawVerticalLine((int) x, plot.getY(), plot.getBottom());
            if (i == 0 || i + 1 == freqs.size() || (i % 2 == 0))
            {
                const double f = freqs[i];
                const juce::String lbl = (f >= 1000.0)
                    ? juce::String(f / 1000.0, f >= 10000.0 ? 0 : 1) + "k"
                    : juce::String((int) std::lround(f));
                g.setColour(accent.withAlpha(0.4f));
                g.drawText(lbl, (int) x - 16, (int) plot.getBottom() + 1, 32, 10,
                           juce::Justification::centred, false);
            }
        }

        // Curve + filled area to the 0 dB line — sampled from the SAME
        // Catmull-Rom evaluator gainDbAtFreq() uses (shared with the LuxEq
        // module's editor), so the drawn spline is exactly the applied gain.
        if (gains.size() >= 2)
        {
            juce::Path curve, fill;
            const float y0 = gainToY(0.0f, plot);
            const int steps = juce::jmax(48, (int) plot.getWidth() / 3);
            for (int s = 0; s <= steps; ++s)
            {
                const float u = (float) s / (float) steps;
                const float x = plot.getX() + u * plot.getWidth();
                const float y = gainToY(eqCurveDbAt(gains.data(), (int) gains.size(),
                                                    u * (float) (gains.size() - 1),
                                                    kGainRange), plot);
                if (s == 0) { curve.startNewSubPath(x, y); fill.startNewSubPath(x, y0); fill.lineTo(x, y); }
                else        { curve.lineTo(x, y); fill.lineTo(x, y); }
            }
            fill.lineTo(plot.getRight(), y0);
            fill.closeSubPath();
            g.setColour(accent.withAlpha(0.12f));
            g.fillPath(fill);
            g.setColour(accent.withAlpha(0.9f));
            g.strokePath(curve, juce::PathStrokeType(1.6f));
        }

        // Node handles (ADSR style).
        for (size_t i = 0; i < freqs.size(); ++i)
        {
            const float x = freqToX(freqs[i], plot);
            const float y = gainToY(gains[i], plot);
            const bool active = ((int) i == hovered || (int) i == dragging);
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
        g.drawText("IMAGE EQ", (int) plot.getX(), (int) bf.getY() + 2, 80, 10,
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
        pointsCombo.setBounds(getWidth() - 6 - kComboW, 1, kComboW, 15);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const int h = nodeAt(e.position);
        if (h != hovered) { hovered = h; repaint(); }
    }
    void mouseExit(const juce::MouseEvent&) override
    {
        if (hovered != -1) { hovered = -1; repaint(); }
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        // Right-click: MIDI Learn menu for the nearest band (when configured).
        if (e.mods.isPopupMenu() && eqLearnMap_ != nullptr && eqLearnIdFn_)
        {
            const int n = nearestNodeByX(e.position);
            if (n >= 0) MidiLearnPopup::show(*eqLearnMap_, eqLearnIdFn_(n), this);
            return;
        }

        dragging = nearestNodeByX(e.position);
        if (e.getNumberOfClicks() >= 2 && dragging >= 0)
        {
            gains[(size_t) dragging] = 0.0f;
            repaint();
            if (onChange) onChange();
            return;
        }
        applyDrag(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override { applyDrag(e); }
    void mouseUp(const juce::MouseEvent&) override
    {
        if (dragging != -1) { dragging = -1; repaint(); }
    }

private:
    static constexpr int kComboW = 72;   // points dropdown (top-right corner)

    /** Even log-frequency node grid over [minF, maxF] for numPoints_ nodes. */
    void rebuildGrid()
    {
        freqs.resize((size_t) numPoints_);
        const double r = maxF / minF;
        for (int i = 0; i < numPoints_; ++i)
            freqs[(size_t) i] = minF * std::pow(r, (double) i
                                                   / (double) (numPoints_ - 1));
    }

    void syncCombo()
    {
        pointsCombo.setSelectedId(numPoints_, juce::dontSendNotification);
    }

    juce::Rectangle<float> plotArea() const
    {
        return getLocalBounds().toFloat().reduced(6.0f).withTrimmedTop(12.0f).withTrimmedBottom(12.0f);
    }
    float freqToX(double hz, juce::Rectangle<float> p) const
    {
        const double t = std::log(hz / minF) / std::log(maxF / minF);
        return p.getX() + (float) t * p.getWidth();
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
        for (size_t i = 0; i < freqs.size(); ++i)
        {
            const float dx = std::abs(freqToX(freqs[i], p) - pt.x);
            if (dx < bd) { bd = dx; best = (int) i; }
        }
        return best;
    }
    int nodeAt(juce::Point<float> pt) const
    {
        const auto p = plotArea();
        for (size_t i = 0; i < freqs.size(); ++i)
        {
            const float x = freqToX(freqs[i], p), y = gainToY(gains[i], p);
            if (pt.getDistanceFrom({ x, y }) <= 11.0f) return (int) i;
        }
        return -1;
    }
    void applyDrag(const juce::MouseEvent& e)
    {
        if (dragging < 0) return;
        gains[(size_t) dragging] = yToGain(e.position.y, plotArea());
        hovered = dragging;
        repaint();
        if (onChange) onChange();
    }

    juce::Colour accent;
    double minF = 65.41, maxF = 16744.04;
    int numPoints_ = kMinPoints;
    juce::ComboBox pointsCombo;
    std::vector<double> freqs;
    std::vector<float>  gains;
    int hovered = -1, dragging = -1;

    // Optional per-band right-click MIDI-Learn (null → disabled; SCORE leaves it off).
    MidiMappingEngine*                eqLearnMap_ = nullptr;
    std::function<juce::String(int)>  eqLearnIdFn_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreEqComponent)
};
