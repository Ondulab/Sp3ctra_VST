/**
 * @file EqHandleMidiTargets.h
 * @brief Virtual MIDI-mapping targets for the "selected EQ handle" model.
 *
 * The typed-handle EQ banks (EQUALIZER / CENTROID / LEVELS output EQ) expose
 * only THREE mappable continuous targets per instance — Freq / Gain / Width —
 * which drive whichever handle is currently SELECTED in the ShapeEq editor.
 * The selection lives in a processor atomic (set by the UI on click), so the
 * three CCs keep steering the last-selected handle even with the editor
 * closed. The mapping engine reaches the real APVTS handle param through the
 * processor's IVirtualMidiSink (same bridge the sampler uses —
 * SamplerMidiTargets), which forwards to setValueNotifyingHost on the
 * resolved parameter.
 *
 *   paramId  : "eqh:{prefix}{slot}:{which}"   e.g. "eqh:luxeq3:freq",
 *              prefix ∈ luxeq / luxcentro / luxdrive, slot 0..7,
 *              which ∈ freq / gain / width
 *   targetId : kFlag | (family << 8) | (slot << 4) | which
 *
 * kFlag (bit 28) is disjoint from every SamplerMidiTargets encoding (their
 * highest used bit is band << 24 with band <= 8 → bit 27), so the processor
 * can route a targetId to the right resolver without ambiguity.
 */
#pragma once

#include <juce_core/juce_core.h>

namespace EqHandleMidiTargets
{
    constexpr int kFlag = 0x10000000;

    enum Family { FamilyEq = 0, FamilyCentro = 1, FamilyDrive = 2, FamilyCount };
    enum Which  { WhichFreq = 0, WhichGain = 1, WhichWidth = 2, WhichCount };
    constexpr int kSlots = 8;   // pool instances per family (CHAIN_MAX_CHAINS)

    inline const char* familyPrefix(int f) noexcept
    {
        switch (f)
        {
            case FamilyEq:     return "luxeq";
            case FamilyCentro: return "luxcentro";
            case FamilyDrive:  return "luxdrive";
            default:           return "";
        }
    }

    inline const char* whichToken(int w) noexcept
    {
        switch (w)
        {
            case WhichFreq:  return "freq";
            case WhichGain:  return "gain";
            case WhichWidth: return "width";
            default:         return "";
        }
    }

    /** The Sh{h} param suffix a `which` addresses (handle picked at apply
     *  time from the selection atomic). */
    inline juce::String paramSuffix(int handle, int w)
    {
        return "Sh" + juce::String(handle)
             + (w == WhichFreq ? "Freq" : w == WhichGain ? "Gain" : "Width");
    }

    inline juce::String makeId(int family, int slot, int w)
    {
        return "eqh:" + juce::String(familyPrefix(family)) + juce::String(slot)
             + ":" + whichToken(w);
    }

    inline int  encode (int family, int slot, int w) noexcept
    { return kFlag | ((family & 0xF) << 8) | ((slot & 0xF) << 4) | (w & 0xF); }
    inline bool isEqHandle(int t) noexcept { return (t & kFlag) != 0; }
    inline int  tFamily(int t) noexcept { return (t >> 8) & 0xF; }
    inline int  tSlot  (int t) noexcept { return (t >> 4) & 0xF; }
    inline int  tWhich (int t) noexcept { return t & 0xF; }

    /** Resolve a synthetic id → targetId (>= 0), or -1 if not ours / malformed.
     *  Message thread (uses String tokenisation). */
    inline int resolve(const juce::String& id)
    {
        if (! id.startsWith("eqh:")) return -1;
        const auto parts = juce::StringArray::fromTokens(id, ":", "");
        if (parts.size() != 3) return -1;

        int family = -1, slot = -1;
        for (int f = 0; f < FamilyCount; ++f)
        {
            const juce::String pfx(familyPrefix(f));
            if (parts[1].startsWith(pfx))
            {
                const juce::String ss = parts[1].substring(pfx.length());
                if (! ss.containsOnly("0123456789") || ss.isEmpty()) continue;
                const int s = ss.getIntValue();
                if (s < 0 || s >= kSlots) return -1;
                family = f; slot = s;
                break;
            }
        }
        if (family < 0) return -1;
        // "luxeq" is a prefix of nothing else, but "luxcentro"/"luxdrive"
        // both start beyond "lux" — startsWith order above is safe because
        // the three prefixes are mutually non-prefixing.

        for (int w = 0; w < WhichCount; ++w)
            if (parts[2] == whichToken(w))
                return encode(family, slot, w);
        return -1;
    }
}
