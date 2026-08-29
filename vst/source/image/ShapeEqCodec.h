/**
 * @file ShapeEqCodec.h
 * @brief String codec for the typed-handle EQ (shape_eq.h) — the non-APVTS
 *        persistence channel (sampler slot EQ, SCORE / VOICE / MIDI SCORE
 *        generator tabs, .fslot files).
 *
 * Format (version tag first):
 *   "H2|{minF}|{maxF}|{levelDb}|t,f,g,w;t,f,g,w;t,f,g,w;t,f,g,w"
 * with exactly SHAPE_EQ_MAX_HANDLES records (t = type, f = freq01,
 * g = gain_db, w = width01) and levelDb = the whole-EQ gain fader.
 * H1 payloads (no level field) still decode, with level = 0.
 *
 * decode() returns false on anything else — including every legacy spline
 * string ("minF|maxF|g0;g1;…") — and leaves the output untouched: callers
 * reset to flat (all handles Off). That IS the migration policy: old band
 * curves reload flat, by design (no curve fitting).
 */
#pragma once

#include <juce_core/juce_core.h>
#include "../processing/shape_eq.h"

namespace ShapeEqCodec
{
    inline juce::String encode(const ShapeEqHandle* h, float levelDb,
                               double minF, double maxF)
    {
        juce::String s;
        s << "H2|" << juce::String(minF, 3) << '|' << juce::String(maxF, 3)
          << '|' << juce::String(levelDb, 2) << '|';
        for (int i = 0; i < SHAPE_EQ_MAX_HANDLES; ++i)
        {
            if (i) s << ';';
            s << h[i].type << ','
              << juce::String(h[i].freq01, 4) << ','
              << juce::String(h[i].gain_db, 2) << ','
              << juce::String(h[i].width01, 4);
        }
        return s;
    }

    inline bool decode(const juce::String& s, ShapeEqHandle* out,
                       float& levelDb, double& minF, double& maxF)
    {
        const bool h2 = s.startsWith("H2|");
        if (! h2 && ! s.startsWith("H1|"))
            return false;   // legacy spline / empty → caller resets to flat
        const auto parts = juce::StringArray::fromTokens(s, "|", "");
        if (parts.size() != (h2 ? 5 : 4)) return false;
        const double lo = parts[1].getDoubleValue();
        const double hi = parts[2].getDoubleValue();
        if (lo <= 0.0 || hi <= lo) return false;
        const float lvl = h2 ? juce::jlimit(-SHAPE_EQ_DB_MAX, SHAPE_EQ_DB_MAX,
                                            parts[3].getFloatValue())
                             : 0.0f;
        const auto recs = juce::StringArray::fromTokens(parts[h2 ? 4 : 3],
                                                        ";", "");
        if (recs.size() != SHAPE_EQ_MAX_HANDLES) return false;

        ShapeEqHandle parsed[SHAPE_EQ_MAX_HANDLES];
        for (int i = 0; i < SHAPE_EQ_MAX_HANDLES; ++i)
        {
            const auto f = juce::StringArray::fromTokens(recs[i], ",", "");
            if (f.size() != 4) return false;
            parsed[i].type    = juce::jlimit(0, SHAPE_EQ_NUM_TYPES - 1,
                                             f[0].getIntValue());
            parsed[i].freq01  = juce::jlimit(0.0f, 1.0f, f[1].getFloatValue());
            parsed[i].gain_db = juce::jlimit(-SHAPE_EQ_DB_MAX, SHAPE_EQ_DB_MAX,
                                             f[2].getFloatValue());
            parsed[i].width01 = juce::jlimit(0.0f, 1.0f, f[3].getFloatValue());
        }
        for (int i = 0; i < SHAPE_EQ_MAX_HANDLES; ++i)
            out[i] = parsed[i];
        levelDb = lvl;
        minF = lo; maxF = hi;
        return true;
    }
}
