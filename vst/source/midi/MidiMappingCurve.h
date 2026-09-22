/**
 * @file MidiMappingCurve.h
 * @brief The transfer law of ONE MIDI mapping — input window, response
 *        curve, hysteresis — edited in the MIDI CURVE window, baked into a
 *        lock-free LUT by MidiMappingEngine, persisted in the <MAP> node.
 *
 * Pipeline of a continuous sweep (absolute CC / note velocity):
 *
 *   v01 = value / 127
 *     → input window   x = (v01 - inLo) / (inHi - inLo), clamped    IN MIN/MAX
 *     → hysteresis     backlash loop of width `hyst` on x (audio state)
 *     → shape          y = curve(x)          LINEAR / EXPO / POINTS / TABLE
 *     → output range   out = lo + (hi - lo) * y     the panel row's MIN/MAX
 *
 * The window is what a controller physically covers (a fader that only
 * reaches 100, the upper half of a knob); the shape is HOW it travels; the
 * hysteresis makes the response direction-dependent (and swallows ±1
 * jitter); the output range — kept on the mapping slot, not here — is
 * WHERE it lands. Toggle / cycle semantics ignore all of it.
 *
 * Shapes:
 *   • LINEAR  y = x
 *   • EXPO    y = x ^ k, k = 10 ^ shape (shape −1 … 1 → k 0.1 … 10): a slow
 *             start (k > 1) or a fast one (k < 1), one handle at the middle
 *   • POINTS  up to kMaxPoints breakpoints, piecewise LINEAR, blended
 *             toward a MONOTONE cubic (Fritsch–Carlson — no overshoot) by
 *             `smooth`
 *   • TABLE   one output per CC value (kTableN = 128), drawn freehand,
 *             box-blurred by `smooth` (a correspondence table, smoothed)
 *
 * This struct is the MESSAGE-THREAD description. bake() is the ONE
 * evaluator: the window draws from a baked copy and the engine applies a
 * baked copy, so what is drawn is what is applied. hystBranch() mirrors
 * the audio thread's backlash for the drawn rising / falling branches.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <cmath>
#include <vector>

struct MidiMappingCurve
{
    enum class Mode : int { Linear = 0, Expo = 1, Points = 2, Table = 3 };

    static constexpr int   kMaxPoints = 16;
    static constexpr int   kTableN    = 128;           ///< one entry per CC value
    static constexpr int   kLutN      = 257;           ///< baked resolution (x = i / 256)
    static constexpr float kMinInSpan = 1.0f / 127.0f; ///< IN MIN < IN MAX by ≥ one CC step
    static constexpr float kMaxHyst   = 0.5f;          ///< loop width cap (half the travel)
    static constexpr int   kMaxBlur   = 16;            ///< TABLE smoothing radius at smooth = 1

    Mode  mode   { Mode::Linear };
    float inLo   { 0.0f }, inHi { 1.0f };   ///< input window, fraction of 0..127
    float shape  { 0.0f };                  ///< EXPO exponent = 10^shape (−1 … 1)
    float hyst   { 0.0f };                  ///< backlash loop width (0 … kMaxHyst)
    float smooth { 0.0f };                  ///< POINTS: linear → cubic; TABLE: blur radius
    std::vector<juce::Point<float>> points { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
    std::array<float, kTableN>      table  {};

    MidiMappingCurve() { resetTable(); }

    //==========================================================================
    // Defaults
    //==========================================================================
    void resetTable() noexcept
    {
        for (int i = 0; i < kTableN; ++i)
            table[(size_t) i] = (float) i / (float) (kTableN - 1);
    }
    void resetPoints() { points = { { 0.0f, 0.0f }, { 1.0f, 1.0f } }; }

    /** RESET of the window: the shape data of every mode + hysteresis go
     *  back to identity; the input window and the mode itself stay. */
    void resetShape()
    {
        shape = 0.0f; hyst = 0.0f; smooth = 0.0f;
        resetPoints(); resetTable();
    }

    bool isDefaultPoints() const noexcept
    {
        return points.size() == 2
            && std::abs(points[0].y)        < 1e-3f
            && std::abs(points[1].y - 1.0f) < 1e-3f;
    }
    bool isIdentityTable() const noexcept
    {
        for (int i = 0; i < kTableN; ++i)
            if (std::abs(table[(size_t) i] - (float) i / (float) (kTableN - 1)) > 1e-3f)
                return false;
        return true;
    }
    /** True when the mapping applies NO transfer beyond the output range:
     *  linear, full window, no hysteresis (the panel row's gear stays dim). */
    bool isNeutral() const noexcept
    {
        return mode == Mode::Linear && inLo <= 0.0f && inHi >= 1.0f && hyst <= 0.0f;
    }

    float exponent() const noexcept
    { return std::pow(10.0f, juce::jlimit(-1.0f, 1.0f, shape)); }

    //==========================================================================
    // Evaluation helpers shared by the window and the engine
    //==========================================================================
    /** Input window: raw 0..1 → curve x 0..1 (flat outside the window). */
    float windowX(float v01) const noexcept
    {
        const float span = inHi - inLo;
        if (span <= 1e-6f) return v01 >= inHi ? 1.0f : 0.0f;
        return juce::jlimit(0.0f, 1.0f, (v01 - inLo) / span);
    }

    /** The backlash branch a steady sweep in direction `dir` (+1 rising,
     *  −1 falling, 0 none) follows — EXACTLY the audio thread's law once the
     *  loop has been traversed: rising lags by the loop width, falling
     *  leads by it, both renormalised so the extremes are still reached. */
    static float hystBranch(float x, float hystWidth, int dir) noexcept
    {
        const float h = 0.5f * juce::jlimit(0.0f, kMaxHyst, hystWidth);
        if (h <= 0.0f || dir == 0) return juce::jlimit(0.0f, 1.0f, x);
        const float xs = juce::jlimit(h, 1.0f - h, x - (float) dir * h);
        return juce::jlimit(0.0f, 1.0f, (xs - h) / (1.0f - 2.0f * h));
    }

    /** Linear interpolation into a baked LUT (kLutN samples over x 0..1). */
    static float lutAt(const float* lut, float x) noexcept
    {
        const float pos = juce::jlimit(0.0f, 1.0f, x) * (float) (kLutN - 1);
        const int   i   = juce::jmin((int) pos, kLutN - 2);
        const float t   = pos - (float) i;
        return lut[i] + (lut[i + 1] - lut[i]) * t;
    }

    /** Bake the SHAPE (no window, no hysteresis, no output range) into a
     *  kLutN table over x 0..1. THE evaluator. */
    void bake(std::array<float, kLutN>& lut) const
    {
        switch (mode)
        {
            case Mode::Linear:
                for (int i = 0; i < kLutN; ++i)
                    lut[(size_t) i] = (float) i / (float) (kLutN - 1);
                break;

            case Mode::Expo:
            {
                const float k = exponent();
                for (int i = 0; i < kLutN; ++i)
                    lut[(size_t) i] = std::pow((float) i / (float) (kLutN - 1), k);
                break;
            }

            case Mode::Points:
            {
                const auto pts = sanitizedPoints();
                float m[kMaxPoints];
                tangents(pts, m);
                const float s = juce::jlimit(0.0f, 1.0f, smooth);
                for (int i = 0; i < kLutN; ++i)
                    lut[(size_t) i] = pointsAt(pts, m, s, (float) i / (float) (kLutN - 1));
                break;
            }

            case Mode::Table:
            {
                std::array<float, kTableN> t;
                smoothedTable(t);
                for (int i = 0; i < kLutN; ++i)
                {
                    const float pos = (float) i / (float) (kLutN - 1) * (float) (kTableN - 1);
                    const int   j   = juce::jmin((int) pos, kTableN - 2);
                    const float f   = pos - (float) j;
                    lut[(size_t) i] = t[(size_t) j] + (t[(size_t) j + 1] - t[(size_t) j]) * f;
                }
                break;
            }
        }
        for (auto& v : lut) v = juce::jlimit(0.0f, 1.0f, v);
    }

    /** The TABLE after its blur (what is drawn and baked). */
    void smoothedTable(std::array<float, kTableN>& out) const
    {
        const int r = juce::roundToInt(juce::jlimit(0.0f, 1.0f, smooth) * (float) kMaxBlur);
        if (r <= 0) { out = table; return; }
        // Two clamped box passes = a triangular kernel, edges replicated.
        std::array<float, kTableN> tmp;
        auto pass = [r](const std::array<float, kTableN>& in, std::array<float, kTableN>& o)
        {
            for (int i = 0; i < kTableN; ++i)
            {
                float sum = 0.0f;
                for (int j = i - r; j <= i + r; ++j)
                    sum += in[(size_t) juce::jlimit(0, kTableN - 1, j)];
                o[(size_t) i] = sum / (float) (2 * r + 1);
            }
        };
        pass(table, tmp);
        pass(tmp, out);
    }

    /** POINTS, in order, with the endpoints pinned to x = 0 and x = 1 and at
     *  least two of them — what the editor and the bake agree on. */
    std::vector<juce::Point<float>> sanitizedPoints() const
    {
        std::vector<juce::Point<float>> pts = points;
        if (pts.size() > (size_t) kMaxPoints) pts.resize((size_t) kMaxPoints);
        for (auto& p : pts)
        {
            p.x = juce::jlimit(0.0f, 1.0f, p.x);
            p.y = juce::jlimit(0.0f, 1.0f, p.y);
        }
        std::stable_sort(pts.begin(), pts.end(),
                         [](const juce::Point<float>& a, const juce::Point<float>& b)
                         { return a.x < b.x; });
        if (pts.empty())      pts = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
        if (pts.size() == 1)  pts.push_back({ 1.0f, pts[0].y });
        pts.front().x = 0.0f;
        pts.back ().x = 1.0f;
        return pts;
    }

    //==========================================================================
    // Persistence — <MAP> attributes, written only when off-default so
    // pre-curve sessions round-trip byte-identical.
    //==========================================================================
    void writeTo(juce::ValueTree& map) const
    {
        if (mode != Mode::Linear) map.setProperty("curve",  (int) mode, nullptr);
        if (inLo   != 0.0f)       map.setProperty("inLo",   inLo,   nullptr);
        if (inHi   != 1.0f)       map.setProperty("inHi",   inHi,   nullptr);
        if (shape  != 0.0f)       map.setProperty("shape",  shape,  nullptr);
        if (hyst   != 0.0f)       map.setProperty("hyst",   hyst,   nullptr);
        if (smooth != 0.0f)       map.setProperty("smooth", smooth, nullptr);
        if (! isDefaultPoints())
        {
            juce::StringArray sa;
            for (const auto& p : sanitizedPoints())
                sa.add(juce::String(p.x, 4) + "," + juce::String(p.y, 4));
            map.setProperty("pts", sa.joinIntoString(";"), nullptr);
        }
        if (! isIdentityTable())
        {
            juce::StringArray sa;
            for (const float v : table) sa.add(juce::String(v, 4));
            map.setProperty("table", sa.joinIntoString(","), nullptr);
        }
    }

    static MidiMappingCurve readFrom(const juce::ValueTree& map)
    {
        MidiMappingCurve c;
        c.mode   = (Mode) juce::jlimit(0, 3, (int) map.getProperty("curve", 0));
        c.inLo   = juce::jlimit(0.0f, 1.0f, (float) (double) map.getProperty("inLo",   0.0));
        c.inHi   = juce::jlimit(0.0f, 1.0f, (float) (double) map.getProperty("inHi",   1.0));
        c.shape  = juce::jlimit(-1.0f, 1.0f, (float) (double) map.getProperty("shape", 0.0));
        c.hyst   = juce::jlimit(0.0f, kMaxHyst, (float) (double) map.getProperty("hyst", 0.0));
        c.smooth = juce::jlimit(0.0f, 1.0f, (float) (double) map.getProperty("smooth", 0.0));
        if (c.inHi < c.inLo + kMinInSpan) c.inHi = juce::jmin(1.0f, c.inLo + kMinInSpan);

        if (const auto pts = map.getProperty("pts", "").toString(); pts.isNotEmpty())
        {
            c.points.clear();
            for (const auto& tok : juce::StringArray::fromTokens(pts, ";", ""))
            {
                const auto xy = juce::StringArray::fromTokens(tok, ",", "");
                if (xy.size() == 2)
                    c.points.push_back({ xy[0].getFloatValue(), xy[1].getFloatValue() });
            }
            c.points = c.sanitizedPoints();
        }
        if (const auto tb = map.getProperty("table", "").toString(); tb.isNotEmpty())
        {
            const auto vals = juce::StringArray::fromTokens(tb, ",", "");
            if (vals.size() == kTableN)
                for (int i = 0; i < kTableN; ++i)
                    c.table[(size_t) i] = juce::jlimit(0.0f, 1.0f, vals[i].getFloatValue());
        }
        return c;
    }

private:
    //==========================================================================
    // POINTS interpolation — piecewise linear blended toward a monotone
    // cubic (Fritsch–Carlson tangents: the smoothed curve never overshoots
    // its breakpoints, so a rising table stays rising).
    //==========================================================================
    static void tangents(const std::vector<juce::Point<float>>& p, float* m) noexcept
    {
        const int n = (int) p.size();
        float d[kMaxPoints];
        for (int i = 0; i < n - 1; ++i)
        {
            const float dx = juce::jmax(1e-6f, p[(size_t) i + 1].x - p[(size_t) i].x);
            d[i] = (p[(size_t) i + 1].y - p[(size_t) i].y) / dx;
        }
        if (n == 2) { m[0] = m[1] = d[0]; return; }
        m[0]     = d[0];
        m[n - 1] = d[n - 2];
        for (int i = 1; i < n - 1; ++i)
            m[i] = (d[i - 1] * d[i] <= 0.0f) ? 0.0f : 0.5f * (d[i - 1] + d[i]);
        for (int i = 0; i < n - 1; ++i)
        {
            if (d[i] == 0.0f) { m[i] = 0.0f; m[i + 1] = 0.0f; continue; }
            const float a = m[i] / d[i], b = m[i + 1] / d[i];
            const float s = a * a + b * b;
            if (s > 9.0f)
            {
                const float t = 3.0f / std::sqrt(s);
                m[i]     = t * a * d[i];
                m[i + 1] = t * b * d[i];
            }
        }
    }

    static float pointsAt(const std::vector<juce::Point<float>>& p, const float* m,
                          float smoothAmt, float x) noexcept
    {
        const int n = (int) p.size();
        int i = 0;
        while (i < n - 2 && x > p[(size_t) i + 1].x) ++i;
        const auto& a = p[(size_t) i];
        const auto& b = p[(size_t) i + 1];
        const float h = b.x - a.x;
        if (h <= 1e-6f) return b.y;
        const float t   = juce::jlimit(0.0f, 1.0f, (x - a.x) / h);
        const float lin = a.y + (b.y - a.y) * t;
        if (smoothAmt <= 0.0f) return lin;
        const float t2 = t * t, t3 = t2 * t;
        const float h00 =  2.0f * t3 - 3.0f * t2 + 1.0f;
        const float h10 =         t3 - 2.0f * t2 + t;
        const float h01 = -2.0f * t3 + 3.0f * t2;
        const float h11 =         t3 -        t2;
        const float cub = h00 * a.y + h10 * h * m[i] + h01 * b.y + h11 * h * m[i + 1];
        return lin + (cub - lin) * smoothAmt;
    }
};
