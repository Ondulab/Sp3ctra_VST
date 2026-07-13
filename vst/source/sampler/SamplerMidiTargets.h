/**
 * @file SamplerMidiTargets.h
 * @brief Virtual MIDI-mapping targets for the LuxSampler per-slot play params.
 *
 * The sampler's play parameters (Speed, Loop, EQ floor, fades…) live inside the
 * LuxSampler engine, NOT in the APVTS, so the generic MidiMappingEngine cannot
 * reach them as RangedAudioParameters. This namespace bridges the gap: it maps a
 * synthetic paramId string ↔ an opaque targetId and reads/writes the value
 * directly on a LuxSampler. The processor implements IVirtualMidiSink on top of
 * these helpers (adding the message-thread action pulses for REC / PLAY / SAVE).
 *
 * "Fixed slot per button": the synthetic id encodes ONE slot (the slot that was
 * selected when the user did MIDI-Learn), so a controller always drives that
 * exact slot — independent of which slot the editor later shows.
 *
 *   paramId  : "smp:e{E}:s{S}:{token}"   per-slot   (E engine 0/1, S slot 0..11)
 *              "smp:e{E}:{token}"         engine-wide (overdub)
 *   targetId : (engine << 16) | (slot << 8) | kind
 *
 * The persisted identity is the STRING (token), so the Kind enum ints may be
 * reordered freely without breaking saved sessions.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cmath>
#include "../luxsampler/LuxSampler.h"

namespace SamplerMidiTargets
{
    // The slot EQ is a 9-node octave grid over the fixed 65.41–16744 Hz span
    // (see ScoreEqComponent / LuxSampler::kEqBands).
    constexpr int kEqBands = 9;

    // Scoped so the enumerators never collide with LuxSampler's LoopMode /
    // FadeCurveType types used in the read/apply casts below.
    enum class Kind
    {
        Speed = 0, LoopMode, LoopXf, Img, Floor, Resume,
        FadeInType, FadeInPow, FadeOutType, FadeOutPow,
        Overdub,                     // engine-wide (slot ignored)
        Rec, Play, Save, Clear,      // action targets (slot-addressed)
        EqBand,                      // per-slot EQ band gain (band in id bits 24+)
        MixMode,                     // per-bank composite rule (Mix/Add/Darken)
        KindCount
    };

    /** Stable synthetic-id token per kind — this is what gets persisted. */
    inline const char* token(Kind k) noexcept
    {
        switch (k)
        {
            case Kind::Speed:       return "speed";
            case Kind::LoopMode:    return "loopmode";
            case Kind::LoopXf:      return "loopxf";
            case Kind::Img:         return "img";
            case Kind::Floor:       return "floor";
            case Kind::Resume:      return "resume";
            case Kind::FadeInType:  return "faintype";
            case Kind::FadeInPow:   return "fainpow";
            case Kind::FadeOutType: return "fouttype";
            case Kind::FadeOutPow:  return "foutpow";
            case Kind::Overdub:     return "overdub";
            case Kind::Rec:         return "rec";
            case Kind::Play:        return "play";
            case Kind::Save:        return "save";
            case Kind::Clear:       return "clear";
            case Kind::EqBand:      return "eq";   // real id is "eq{band}" (makeEqBandId)
            case Kind::MixMode:     return "mixmode";
            default:                return "";
        }
    }

    inline bool isEngineWide(Kind k) noexcept { return k == Kind::Overdub; }
    inline bool isAction    (Kind k) noexcept
    { return k == Kind::Rec || k == Kind::Play || k == Kind::Save || k == Kind::Clear; }

    // ── Synthetic paramId ↔ targetId ─────────────────────────────────────────
    inline juce::String makeId(int engine, int slot, Kind k)
    {
        juce::String s = "smp:e" + juce::String(engine);
        if (! isEngineWide(k)) s += ":s" + juce::String(slot);
        return s + ":" + token(k);
    }

    inline int  encode (int engine, int slot, Kind k) noexcept
    { return (engine << 16) | ((slot & 0xFF) << 8) | (int) k; }
    inline int  tEngine(int t) noexcept { return (t >> 16) & 0xFF; }
    inline int  tSlot  (int t) noexcept { return (t >> 8)  & 0xFF; }
    inline Kind tKind  (int t) noexcept { return static_cast<Kind>(t & 0xFF); }

    // EQ band targets carry the band index in the high byte (0..8).
    inline juce::String makeEqBandId(int engine, int slot, int band)
    { return "smp:e" + juce::String(engine) + ":s" + juce::String(slot)
           + ":eq" + juce::String(band); }
    inline int encodeEq(int engine, int slot, int band) noexcept
    { return (band << 24) | (engine << 16) | ((slot & 0xFF) << 8) | (int) Kind::EqBand; }
    inline int tBand(int t) noexcept { return (t >> 24) & 0xFF; }

    /** Resolve a synthetic id → targetId (>= 0), or -1 if not ours / malformed.
     *  Message thread (uses String tokenisation). */
    inline int resolve(const juce::String& id)
    {
        if (! id.startsWith("smp:e")) return -1;
        juce::StringArray parts = juce::StringArray::fromTokens(id, ":", "");
        if (parts.size() < 3 || ! parts[1].startsWith("e")) return -1;

        const int engine = parts[1].substring(1).getIntValue();
        if (engine < 0 || engine > 1) return -1;

        const bool perSlot = parts[2].startsWith("s");
        int slot = 0;
        juce::String tok;
        if (perSlot)
        {
            if (parts.size() < 4) return -1;
            slot = parts[2].substring(1).getIntValue();
            tok  = parts[3];
            if (slot < 0 || slot >= LuxSamplerConstants::NUM_SLOTS) return -1;
        }
        else
            tok = parts[2];

        // EQ band: "eq{band}" (per-slot only).
        if (perSlot && tok.startsWith("eq") && tok.length() > 2)
        {
            const juce::String bs = tok.substring(2);
            if (! bs.containsOnly("0123456789")) return -1;
            const int band = bs.getIntValue();
            if (band < 0 || band >= kEqBands) return -1;
            return encodeEq(engine, slot, band);
        }

        for (int k = 0; k < (int) Kind::KindCount; ++k)
        {
            const Kind kind = static_cast<Kind>(k);
            if (kind == Kind::EqBand) continue;   // handled above (needs a band)
            if (tok == token(kind))
            {
                // Per-slot form must match a per-slot kind and vice-versa.
                if (isEngineWide(kind) == perSlot) return -1;
                return encode(engine, slot, kind);
            }
        }
        return -1;
    }

    // ── Kind → step/kind code (mirrors RangedAudioParameter::getNumSteps) ──────
    //   >= 1 value target · -1 momentary action · -2 one-shot action
    inline int steps(Kind k) noexcept
    {
        switch (k)
        {
            case Kind::Speed: case Kind::LoopXf: case Kind::Img: case Kind::Floor:
            case Kind::FadeInPow: case Kind::FadeOutPow:
            case Kind::EqBand:                             return 0;    // continuous
            case Kind::Resume: case Kind::Overdub:         return 2;    // 2-state
            case Kind::LoopMode:                           return 4;    // NONE/LOOP/INV/PING
            case Kind::MixMode:                            return 3;    // MIX/ADD/DARKEN
            case Kind::FadeInType: case Kind::FadeOutType: return 4;    // LIN/EXP/LOG/S
            case Kind::Rec: case Kind::Play:               return -1;   // momentary action
            case Kind::Save: case Kind::Clear:             return -2;   // one-shot action
            default:                                       return 0;
        }
    }

    // Continuous ranges (with skew) — mirror the SlotEditor sliders so a CC knob
    // feels the same as dragging (1.0× speed / power at knob centre).
    inline const juce::NormalisableRange<float>& speedRange()
    {
        static const juce::NormalisableRange<float> r = []
        { juce::NormalisableRange<float> rr(0.01f, 32.0f); rr.setSkewForCentre(1.0f); return rr; }();
        return r;
    }
    inline const juce::NormalisableRange<float>& powerRange()
    {
        static const juce::NormalisableRange<float> r = []
        { juce::NormalisableRange<float> rr(0.1f, 10.0f); rr.setSkewForCentre(1.0f); return rr; }();
        return r;
    }

    /** Read a per-slot / engine-wide VALUE target, normalised to 0..1. RT-safe. */
    inline float read(LuxSampler& fs, int slot, Kind k) noexcept
    {
        switch (k)
        {
            case Kind::Speed:       return speedRange().convertTo0to1(fs.getSlotSpeed(slot));
            case Kind::LoopXf:      return juce::jlimit(0.0f, 1.0f, fs.getSlotLoopOverlap(slot) * 2.0f);
            case Kind::Img:         return juce::jlimit(0.0f, 1.0f, 1.0f - fs.getSlotBrightnessLift(slot));
            case Kind::Floor:       return fs.getSlotEqFloor(slot);
            case Kind::FadeInPow:   return powerRange().convertTo0to1(fs.getSlotAttackCurvePower(slot));
            case Kind::FadeOutPow:  return powerRange().convertTo0to1(fs.getSlotDecayCurvePower(slot));
            case Kind::Resume:      return fs.getSlotResumeMode(slot) ? 1.0f : 0.0f;
            case Kind::Overdub:     return fs.getOverdubMode()        ? 1.0f : 0.0f;
            case Kind::LoopMode:    return (float) (int) fs.getSlotLoopMode(slot)        / 3.0f;
            case Kind::MixMode:     return (float) (int) fs.getSlotMixMode(slot)         / 2.0f;
            case Kind::FadeInType:  return (float) (int) fs.getSlotAttackCurveType(slot) / 3.0f;
            case Kind::FadeOutType: return (float) (int) fs.getSlotDecayCurveType(slot)  / 3.0f;
            default:                return 0.0f;
        }
    }

    /** Apply a normalised value (0..1) to a per-slot / engine-wide VALUE target.
     *  RT-safe (LuxSampler setters are atomic stores). */
    inline void apply(LuxSampler& fs, int slot, Kind k, float n) noexcept
    {
        n = juce::jlimit(0.0f, 1.0f, n);
        switch (k)
        {
            case Kind::Speed:       fs.setSlotSpeed(slot, speedRange().convertFrom0to1(n));            break;
            case Kind::LoopXf:      fs.setSlotLoopOverlap(slot, n * 0.5f);                              break;
            case Kind::Img:         fs.setSlotBrightnessLift(slot, 1.0f - n);                           break;
            case Kind::Floor:       fs.setSlotEqFloor(slot, n);                                         break;
            case Kind::FadeInPow:   fs.setSlotAttackCurvePower(slot, powerRange().convertFrom0to1(n));  break;
            case Kind::FadeOutPow:  fs.setSlotDecayCurvePower(slot, powerRange().convertFrom0to1(n));   break;
            case Kind::Resume:      fs.setSlotResumeMode(slot, n >= 0.5f);                              break;
            case Kind::Overdub:     fs.setOverdubMode(n >= 0.5f);                                       break;
            case Kind::LoopMode:    fs.setSlotLoopMode(slot, (LoopMode) (int) std::lround(n * 3.0f));   break;
            case Kind::MixMode:     fs.setSlotMixMode(slot, (SlotMixMode) (int) std::lround(n * 2.0f)); break;
            case Kind::FadeInType:  fs.setSlotAttackCurveType(slot, (FadeCurveType) (int) std::lround(n * 3.0f)); break;
            case Kind::FadeOutType: fs.setSlotDecayCurveType(slot,  (FadeCurveType) (int) std::lround(n * 3.0f)); break;
            default: break;
        }
    }
}
