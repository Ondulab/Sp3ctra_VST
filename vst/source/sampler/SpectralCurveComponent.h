/**
 * @file SpectralCurveComponent.h
 * @brief Per-slot frequency-axis MIRROR curve editor for the SAMPLER (HF + LF).
 *
 * Two multi-point curves, mirrored around a central line (like the timeline):
 *   • UPPER lane — HF / treble curve. X = high-frequency band (mid → aigu),
 *                  level 1 (keep) at the top, level 0 (cut → white) at centre.
 *   • LOWER lane — LF / bass  curve. X = low-frequency  band (grave → mid),
 *                  level 1 (keep) at the bottom, level 0 (cut → white) at centre.
 * Together they tile the full spectrum: the LF band feeds the left half of the
 * pixels, the HF band the right half (see LuxSampler::rebuildFreqLut).
 *
 * Both curves are Catmull-Rom smoothed (same evaluator as the RT LUT). Editing:
 *   • click empty space in a lane  → add a point (draggable in the same gesture),
 *   • click a point without dragging → delete it (interior only),
 *   • drag a point → move it.
 * A faint backdrop shows the slot's energy per frequency (treble up, bass down).
 * onChange fires on every edit; the host reads both bands and pushes them.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include <algorithm>
#include <functional>
#include "../luxsampler/LuxSampler.h"
#include "../UITheme.h"

class SpectralCurveComponent : public juce::Component
{
public:
    static constexpr int kPreferredH = 170;

    explicit SpectralCurveComponent(juce::Colour accentColour = juce::Colour(0xffcc88ff))
        : accent(accentColour)
    {
        hf_ = { { 0.0f, 1.0f }, { 1.0f, 1.0f } };
        lf_ = { { 0.0f, 1.0f }, { 1.0f, 1.0f } };
    }

    /** Fired on every edit (drag / add / delete). */
    std::function<void()> onChange;

    /** Replace a band's points (band = LuxSamplerConstants::FREQ_BAND_LF / _HF). */
    void setBandPoints(int band, const SamplerSpectralPoint* p, int n)
    {
        auto& v = pts(band);
        v.clear();
        for (int i = 0; i < n; ++i)
            v.push_back({ juce::jlimit(0.0f, 1.0f, p[i].x),
                          juce::jlimit(0.0f, 1.0f, p[i].y) });
        sanitize(v);
        dragging = hovered = -1;
        repaint();
    }

    /** Copy a band's points into @p out (up to maxN). Returns the count. */
    int getBandPoints(int band, SamplerSpectralPoint* out, int maxN) const
    {
        const auto& v = pts(band);
        const int n = juce::jmin((int) v.size(), maxN);
        for (int i = 0; i < n; ++i) out[i] = v[(size_t) i];
        return n;
    }

    /** Backdrop: full-spectrum energy profile (0..1). Split across the two lanes. */
    void setSpectralProfile(const float* prof, int n)
    {
        if (prof == nullptr || n <= 0) profile_.clear();
        else                           profile_.assign(prof, prof + n);
        repaint();
    }

    bool isFlat() const noexcept
    {
        for (const auto& p : hf_) if (p.y < 0.999f) return false;
        for (const auto& p : lf_) if (p.y < 0.999f) return false;
        return true;
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        auto bf = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff20202a));
        g.fillRoundedRectangle(bf.reduced(0.5f), 4.0f);
        g.setColour(accent.withAlpha(0.25f));
        g.drawRoundedRectangle(bf.reduced(0.5f), 4.0f, 1.0f);

        const auto  plot = plotArea();
        const float cyf  = plot.getCentreY();

        // Energy backdrop — HF (treble) up from centre, LF (bass) down from centre.
        if (!profile_.empty())
        {
            const int np = (int) profile_.size();
            g.setColour(juce::Colours::white.withAlpha(0.08f));
            for (int x = (int) plot.getX(); x < (int) plot.getRight(); ++x)
            {
                const float xn = (plot.getWidth() > 1.0f)
                    ? (float) (x - (int) plot.getX()) / plot.getWidth() : 0.0f;
                // HF lane samples the treble half [0.5..1] of the profile.
                const int   hi = juce::jlimit(0, np - 1, (int) ((0.5f + xn * 0.5f) * (np - 1)));
                const float he = profile_[(size_t) hi] * (cyf - plot.getY());
                g.fillRect((float) x, cyf - he, 1.0f, he);
                // LF lane samples the bass half [0..0.5] of the profile.
                const int   li = juce::jlimit(0, np - 1, (int) ((xn * 0.5f) * (np - 1)));
                const float le = profile_[(size_t) li] * (plot.getBottom() - cyf);
                g.fillRect((float) x, cyf, 1.0f, le);
            }
        }

        // Centre (cut) line.
        g.setColour(juce::Colour(0x33ffffff));
        g.drawHorizontalLine((int) cyf, plot.getX(), plot.getRight());

        // Frequency thirds.
        for (int k = 1; k < 4; ++k)
        {
            const float x = plot.getX() + (float) k / 4.0f * plot.getWidth();
            g.setColour(juce::Colour(0x0cffffff));
            g.drawVerticalLine((int) x, plot.getY(), plot.getBottom());
        }

        drawBand(g, LuxSamplerConstants::FREQ_BAND_HF, plot);
        drawBand(g, LuxSamplerConstants::FREQ_BAND_LF, plot);

        // Title + labels + hint.
        g.setColour(accent.withAlpha(0.75f));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        g.drawText("HF", (int) plot.getX(), (int) bf.getY() + 2, 40, 10,
                   juce::Justification::left, false);
        g.drawText("LF", (int) plot.getX(), (int) plot.getBottom() - 1, 40, 10,
                   juce::Justification::left, false);
        g.setColour(accent.withAlpha(0.4f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.drawText("treble", (int) plot.getRight() - 40, (int) plot.getY() - 1, 40, 10,
                   juce::Justification::right, false);
        g.drawText("bass",  (int) plot.getRight() - 40, (int) plot.getBottom() - 1, 40, 10,
                   juce::Justification::right, false);
        if (isFlat())
        {
            g.setColour(juce::Colour(0xff55606f));
            g.drawText(juce::String::fromUTF8("click: +/-  \xc2\xb7  drag: move  \xc2\xb7  up=treble down=bass"), // ·
                       plot.getRight() - 320, (int) bf.getY() + 2, 320, 10,
                       juce::Justification::right, false);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const int band = laneAt(e.position);
        const int h    = nodeAt(band, e.position);
        if (h != hovered || band != hoverBand_) { hovered = h; hoverBand_ = band; repaint(); }
        setMouseCursor(h >= 0 ? juce::MouseCursor::DraggingHandCursor
                              : juce::MouseCursor::NormalCursor);
    }
    void mouseExit(const juce::MouseEvent&) override
    {
        if (hovered != -1) { hovered = -1; repaint(); }
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        pressPos_     = e.position;
        movedDrag_    = false;
        addedOnPress_ = false;
        dragBand_     = laneAt(e.position);
        auto& v       = pts(dragBand_);

        const int hit = nodeAt(dragBand_, e.position);
        if (hit >= 0) { dragging = hovered = hit; hoverBand_ = dragBand_; return; }

        if ((int) v.size() < LuxSamplerConstants::MAX_FREQ_PTS)
        {
            SamplerSpectralPoint np { juce::jlimit(0.001f, 0.999f, xToFreq(e.position.x)),
                                      laneYToLevel(dragBand_, e.position.y) };
            size_t ins = 1;
            while (ins < v.size() && v[ins].x < np.x) ++ins;
            v.insert(v.begin() + ins, np);
            dragging = hovered = (int) ins;
            hoverBand_ = dragBand_;
            addedOnPress_ = true;
            repaint();
            if (onChange) onChange();
        }
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        if (e.position.getDistanceFrom(pressPos_) > 3.0f) movedDrag_ = true;
        applyDrag(e);
    }
    void mouseUp(const juce::MouseEvent&) override
    {
        auto& v = pts(dragBand_);
        if (dragging > 0 && dragging < (int) v.size() - 1 && !movedDrag_ && !addedOnPress_)
        {
            v.erase(v.begin() + dragging);
            if (onChange) onChange();
        }
        dragging = hovered = -1;
        movedDrag_ = addedOnPress_ = false;
        repaint();
    }

private:
    std::vector<SamplerSpectralPoint>&       pts(int band)       { return band == LuxSamplerConstants::FREQ_BAND_HF ? hf_ : lf_; }
    const std::vector<SamplerSpectralPoint>& pts(int band) const { return band == LuxSamplerConstants::FREQ_BAND_HF ? hf_ : lf_; }

    static void sanitize(std::vector<SamplerSpectralPoint>& v)
    {
        if (v.size() < 2) v = { { 0.0f, 1.0f }, { 1.0f, 1.0f } };
        std::sort(v.begin(), v.end(),
                  [](const SamplerSpectralPoint& a, const SamplerSpectralPoint& b) { return a.x < b.x; });
        if ((int) v.size() > LuxSamplerConstants::MAX_FREQ_PTS)
            v.resize(LuxSamplerConstants::MAX_FREQ_PTS);
        v.front().x = 0.0f;
        v.back().x  = 1.0f;
    }

    juce::Rectangle<float> plotArea() const
    {
        return getLocalBounds().toFloat().reduced(6.0f)
                   .withTrimmedTop(11.0f).withTrimmedBottom(11.0f);
    }
    float freqToX(float f) const { const auto p = plotArea(); return p.getX() + f * p.getWidth(); }
    float xToFreq(float x) const
    { const auto p = plotArea(); return juce::jlimit(0.0f, 1.0f, (x - p.getX()) / juce::jmax(1.0f, p.getWidth())); }

    /** Level→Y within a band's lane (HF above centre, LF below). */
    float laneLevelToY(int band, float v) const
    {
        const auto p = plotArea();
        const float cy = p.getCentreY();
        return (band == LuxSamplerConstants::FREQ_BAND_HF)
            ? cy - v * (cy - p.getY())
            : cy + v * (p.getBottom() - cy);
    }
    float laneYToLevel(int band, float y) const
    {
        const auto p = plotArea();
        const float cy = p.getCentreY();
        return (band == LuxSamplerConstants::FREQ_BAND_HF)
            ? juce::jlimit(0.0f, 1.0f, (cy - y) / juce::jmax(1.0f, cy - p.getY()))
            : juce::jlimit(0.0f, 1.0f, (y - cy) / juce::jmax(1.0f, p.getBottom() - cy));
    }
    int laneAt(juce::Point<float> pt) const
    {
        return (pt.y <= plotArea().getCentreY())
            ? LuxSamplerConstants::FREQ_BAND_HF : LuxSamplerConstants::FREQ_BAND_LF;
    }
    int nodeAt(int band, juce::Point<float> pt) const
    {
        const auto& v = pts(band);
        for (size_t i = 0; i < v.size(); ++i)
        {
            const float x = freqToX(v[i].x), y = laneLevelToY(band, v[i].y);
            if (pt.getDistanceFrom({ x, y }) <= 10.0f) return (int) i;
        }
        return -1;
    }
    void applyDrag(const juce::MouseEvent& e)
    {
        auto& v = pts(dragBand_);
        if (dragging < 0 || dragging >= (int) v.size()) return;
        v[(size_t) dragging].y = laneYToLevel(dragBand_, e.position.y);
        const bool endpoint = (dragging == 0 || dragging == (int) v.size() - 1);
        if (!endpoint)
        {
            const float lo = v[(size_t) dragging - 1].x + 0.001f;
            const float hi = v[(size_t) dragging + 1].x - 0.001f;
            if (hi > lo) v[(size_t) dragging].x = juce::jlimit(lo, hi, xToFreq(e.position.x));
        }
        hovered = dragging; hoverBand_ = dragBand_;
        repaint();
        if (onChange) onChange();
    }

    void drawBand(juce::Graphics& g, int band, juce::Rectangle<float> plot)
    {
        const auto& v  = pts(band);
        const float cyf = plot.getCentreY();
        const int   steps = juce::jmax(2, (int) plot.getWidth());
        const SamplerSpectralPoint* pd = v.data();
        const int   pn = (int) v.size();

        juce::Path curve, fill;
        for (int s = 0; s <= steps; ++s)
        {
            const float xn = (float) s / (float) steps;
            const float y  = samplerSpectralCurveY(pd, pn, xn);
            const float px = plot.getX() + xn * plot.getWidth();
            const float py = laneLevelToY(band, y);
            if (s == 0) { curve.startNewSubPath(px, py); fill.startNewSubPath(px, cyf); fill.lineTo(px, py); }
            else        { curve.lineTo(px, py);          fill.lineTo(px, py); }
        }
        fill.lineTo(plot.getRight(), cyf);
        fill.closeSubPath();
        g.setColour(accent.withAlpha(0.12f));
        g.fillPath(fill);
        g.setColour(accent.withAlpha(0.9f));
        g.strokePath(curve, juce::PathStrokeType(1.6f));

        for (size_t i = 0; i < v.size(); ++i)
        {
            const float x = freqToX(v[i].x), y = laneLevelToY(band, v[i].y);
            const bool active = (hoverBand_ == band && ((int) i == hovered || (int) i == dragging));
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
    }

    std::vector<SamplerSpectralPoint> hf_, lf_;    // HF (treble) / LF (bass) curves
    std::vector<float>                profile_;    // full-spectrum energy backdrop
    juce::Colour accent;
    int  hovered = -1, dragging = -1;
    int  hoverBand_ = LuxSamplerConstants::FREQ_BAND_HF;
    int  dragBand_  = LuxSamplerConstants::FREQ_BAND_HF;
    juce::Point<float> pressPos_;
    bool movedDrag_   = false;
    bool addedOnPress_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralCurveComponent)
};
