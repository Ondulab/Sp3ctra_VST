/**
 * @file ModuleParamManifest.h
 * @brief Single source of truth for every module's per-instance APVTS bank
 *        (chantier « chain porteuse des réglages », J1).
 *
 * Before this file, the type→params mapping was scattered: kInsertBanks in
 * PluginProcessor.cpp (Pitch/Mask/Reverb/Echo/EQ), the OUT suffix list local
 * to the constructor, the VideoScroll suffixes inline in the layout and the
 * sampler ids in fsEngineParam call sites. Every consumer that needs "all the
 * params of THIS module instance" (per-chain settings memory, listeners,
 * legacy migration — and soon the chain-owned VALUES snapshot/projection and
 * the .sp3chain presets) iterates THIS table instead.
 *
 * An entry describes one module type's bank family:
 *   • numSlots     — independent instances (8 for pooled/sends/VideoScroll,
 *                    2 for the sampler engines),
 *   • suffixes     — manifest names, unique per module (they become the
 *                    VALUES attribute names in the chain tree, J2),
 *   • paramId(slot, suffix) — the APVTS id of that bank member.
 *
 * NOT in the manifest (documented, out of the chain-owned scope):
 *   • the global ENGINE params (luxstral*, luxsynth*, luxwave*, sf*,
 *     spctr* …) — the engines live in the AUDIO MIX zone, not in a chain;
 *   • the source modules (SP3CTRA/IMAGE/VIDEO/CAMERA + acqGate*) — engine
 *     singletons with global params (V1 decision C);
 *   • SCORE/SEQUENCER state (non-APVTS trees) and the shared sampler prefs
 *     (enable LED, export options).
 */
#pragma once

#include "ModuleCatalog.h"   // ModuleType
#include <cstring>
#include <iterator>          // std::size

//==============================================================================
// Per-instance bank id builders — the historical helpers, unchanged ids.
// (Moved here from PluginProcessor.h so the manifest and every UI consumer
// share one definition; PluginProcessor.h re-exports them via its include.)
//==============================================================================

// VideoScroll instance bank ("videoScroll{slot}_*") + its mixer voice
// ("videoMix{slot}_*"). One source of truth shared by the contextual panel,
// the per-instance renderer and the right-band mixer.
inline juce::String vsParam(int slot, const char* suffix)
{ return "videoScroll" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String vsMixParam(int slot, const char* suffix)
{ return "videoMix" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

// Pooled-insert (Pitch/Mask/Reverb/Echo/EQ) per-instance banks. Each instance
// owns a state-pool slot 0..7 (modulePoolSlots_, keyed by the ModuleInstance
// UUID); its play params live under "luxpitch{slot}_*" etc. Suffixes match
// the legacy per-type ids ("luxpitchAttackMs" → "luxpitch{N}_AttackMs").
inline juce::String lpParam(int slot, const char* suffix)
{ return "luxpitch" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String lmParam(int slot, const char* suffix)
{ return "luxmask" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String rvParam(int slot, const char* suffix)
{ return "luxreverb" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String ecParam(int slot, const char* suffix)
{ return "luxecho" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String eqParam(int slot, const char* suffix)
{ return "luxeq" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String hmParam(int slot, const char* suffix)
{ return "luxharmo" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

// Media source banks (P5-M3) — slot 0 keeps the LEGACY global ids
// ("imgSrcPos"…) so existing sessions and automation lanes load unchanged;
// slots 1..7 own "imgSrc{N}_<suffix>". (VIDEO/CAMERA follow the same recipe
// when their per-slot engines land.)
inline juce::String imgSrcParam(int slot, const char* suffix)
{
    return slot <= 0 ? "imgSrc" + juce::String(suffix)
                     : "imgSrc" + juce::String(juce::jlimit(1, 7, slot))
                          + "_" + suffix;
}

inline juce::String vidSrcParam(int slot, const char* suffix)
{
    return slot <= 0 ? "vidSrc" + juce::String(suffix)
                     : "vidSrc" + juce::String(juce::jlimit(1, 7, slot))
                          + "_" + suffix;
}

inline juce::String camSrcParam(int slot, const char* suffix)
{
    return slot <= 0 ? "camSrc" + juce::String(suffix)
                     : "camSrc" + juce::String(juce::jlimit(1, 7, slot))
                          + "_" + suffix;
}

// Score-player pool per-slot ACTIVE (module enable). The score family
// (SCORE / TIMBRE / MIDI SCORE / VOICE) shares one 8-slot player pool; each
// instance's rack LED toggles ITS slot's active state (NOT the transport).
// Deactivating stops playback while remembering the head; reactivating
// resumes it. Default ON, PERSISTED (unlike the play transports it is not
// forced back off on session restore).
inline juce::String scoreActiveParam(int slot)
{ return "scoreActive" + juce::String(juce::jlimit(0, 7, slot)); }

// Engine-send (OUT) conditioning banks — one per send instance (M6 pools).
inline juce::String lsOutParam(int slot, const char* suffix)
{ return "luxstralOut" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String lxOutParam(int slot, const char* suffix)
{ return "luxsynthOut" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String lwOutParam(int slot, const char* suffix)
{ return "luxwaveOut" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String lgOutParam(int slot, const char* suffix)
{ return "luxgrainOut" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

// Sampler per-engine bank (P6 — engines ×8): engines 0/1 keep the legacy
// "luxSampler*" / "luxSamplerB*" ids (sessions and MIDI mappings load
// unchanged), engines 2..7 own "luxSampler{N}_*".
// Play params only — the enable LED, export prefs and output dir stay shared.
inline juce::String fsEngineParam(int engine, const char* suffix)
{
    if (engine == 1)
        return juce::String("luxSamplerB") + suffix;
    if (engine >= 2)
        return "luxSampler" + juce::String(juce::jlimit(2, 7, engine)) + "_" + suffix;
    return juce::String("luxSampler") + suffix;
}

/** Bank id for any pooled insert type; empty for non-pooled types. */
inline juce::String insertBankParam(ModuleType t, int slot, const char* suffix)
{
    switch (t)
    {
        case ModuleType::Pitch:     return lpParam(slot, suffix);
        case ModuleType::Mask:      return lmParam(slot, suffix);
        case ModuleType::Reverb:    return rvParam(slot, suffix);
        case ModuleType::Echo:      return ecParam(slot, suffix);
        case ModuleType::Equalizer: return eqParam(slot, suffix);
        case ModuleType::Harmonize: return hmParam(slot, suffix);
        default:                    return {};
    }
}

//==============================================================================
// The manifest
//==============================================================================

struct ModuleParamManifest
{
    ModuleType         type;
    const char*        bankPrefix;    ///< legacy/type prefix ("luxpitch"…) — the
                                      ///< pre-bank legacy id is prefix+suffix
    int                numSlots;      ///< independent instance banks
    const char* const* suffixes;      ///< manifest names (unique per module)
    int                numSuffixes;
    juce::String     (*paramId)(int slot, const char* suffix);
};

namespace module_param_manifest_detail
{
    inline const char* const kPitch[] = {
        "Enabled", "Polyphony", "BackgroundMode", "CouplingMode",
        "FreePixelsPerST", "PitchBendRange",
        "AttackMs", "DecayMs", "SustainLevel", "ReleaseMs",
        "AttackCurve", "DecayCurve", "ReleaseCurve",
        "GlideMs", "LfoRate", "LfoDepth", "VelocityCoupling",
        "MidiChannel", "OctaveOffset", "ReferenceNote",
    };
    inline const char* const kMask[] = {
        "Enabled", "Polyphony", "BackgroundMode", "CouplingMode",
        "FreePixelsPerST", "PitchBendRange",
        "FilterWidth", "FilterOffset", "FilterSlope",
        "AttackMs", "DecayMs", "SustainLevel", "ReleaseMs",
        "AttackCurve", "DecayCurve", "ReleaseCurve",
        "GlideMs", "LfoPosRate", "LfoPosDepth", "VelocityCoupling",
        "MidiChannel", "OctaveOffset", "ReferenceNote",
    };
    inline const char* const kReverb[] = {
        "Enabled", "Decay", "Diffusion", "Mix", "BackgroundMode",
    };
    inline const char* const kEcho[] = {
        "Enabled", "Delay", "Feedback", "Mix", "BackgroundMode",
    };
    inline const char* const kEq[] = {
        "Enabled",
        "Band0", "Band1", "Band2", "Band3", "Band4",
        "Band5", "Band6", "Band7", "Band8",
        "BackgroundMode",
    };
    inline const char* const kHarmo[] = {
        "Enabled", "Mode", "Root", "Scale",
        "Strength", "Width", "Slope", "Glide",
        "BackgroundMode",
    };
    inline const char* const kVideoScroll[] = {
        "mode", "speed", "linePos", "thickness", "zoom", "fade",
        "compress", "invert",   // "invert" = legacy bool, migrated to "invertMode"
        "invertMode", "colorMode", "bgR", "bgG", "bgB", "paused", "enabled",
        "MixLevel", "MixBlend",   // → videoMix{N}_level / _blend
    };
    inline const char* const kLsOut[] = {
        "negative", "dcBlocking", "gamma", "contrastMin", "rangeDb",
        "intensity", "enabled",
    };
    inline const char* const kLxLwOut[] = {
        "negative", "dcBlocking", "gamma", "intensity", "enabled",
    };
    inline const char* const kImage[] = {
        "Pos", "Duration", "Loop", "Play", "Enabled", "ScanStart", "ScanEnd",
        "Rotate",
    };
    inline const char* const kVideo[] = {
        "Line", "Speed", "Loop", "Play", "Enabled",
    };
    inline const char* const kCamera[] = {
        "Line", "Enabled",
    };
    inline const char* const kSampler[] = {
        "MidiChannel", "OctaveOffset", "MaxDuration",
        // REC/PLAY/SAVE bind params removed — those actions are now mapped through
        // the unified MIDI-Learn engine (right-click the buttons in the editor).
    };

    inline juce::String lpId(int s, const char* x) { return lpParam(s, x); }
    inline juce::String lmId(int s, const char* x) { return lmParam(s, x); }
    inline juce::String rvId(int s, const char* x) { return rvParam(s, x); }
    inline juce::String ecId(int s, const char* x) { return ecParam(s, x); }
    inline juce::String eqId(int s, const char* x) { return eqParam(s, x); }
    inline juce::String hmId(int s, const char* x) { return hmParam(s, x); }
    inline juce::String lsId(int s, const char* x) { return lsOutParam(s, x); }
    inline juce::String lxId(int s, const char* x) { return lxOutParam(s, x); }
    inline juce::String lwId(int s, const char* x) { return lwOutParam(s, x); }
    inline juce::String lgId(int s, const char* x) { return lgOutParam(s, x); }
    inline juce::String fsId(int s, const char* x) { return fsEngineParam(s, x); }
    inline juce::String imgId(int s, const char* x) { return imgSrcParam(s, x); }
    inline juce::String vidId(int s, const char* x) { return vidSrcParam(s, x); }
    inline juce::String camId(int s, const char* x) { return camSrcParam(s, x); }
    inline juce::String vsId(int s, const char* x)
    {
        if (std::strcmp(x, "MixLevel") == 0) return vsMixParam(s, "level");
        if (std::strcmp(x, "MixBlend") == 0) return vsMixParam(s, "blend");
        return vsParam(s, x);
    }
}

inline const ModuleParamManifest kModuleParamManifest[] = {
    { ModuleType::Pitch,       "luxpitch",    8,
      module_param_manifest_detail::kPitch,
      (int) std::size(module_param_manifest_detail::kPitch),
      &module_param_manifest_detail::lpId },
    { ModuleType::Mask,        "luxmask",     8,
      module_param_manifest_detail::kMask,
      (int) std::size(module_param_manifest_detail::kMask),
      &module_param_manifest_detail::lmId },
    { ModuleType::Reverb,      "luxreverb",   8,
      module_param_manifest_detail::kReverb,
      (int) std::size(module_param_manifest_detail::kReverb),
      &module_param_manifest_detail::rvId },
    { ModuleType::Echo,        "luxecho",     8,
      module_param_manifest_detail::kEcho,
      (int) std::size(module_param_manifest_detail::kEcho),
      &module_param_manifest_detail::ecId },
    { ModuleType::Equalizer,   "luxeq",       8,
      module_param_manifest_detail::kEq,
      (int) std::size(module_param_manifest_detail::kEq),
      &module_param_manifest_detail::eqId },
    { ModuleType::Harmonize,   "luxharmo",    8,
      module_param_manifest_detail::kHarmo,
      (int) std::size(module_param_manifest_detail::kHarmo),
      &module_param_manifest_detail::hmId },
    { ModuleType::VideoScroll, "videoScroll", 8,
      module_param_manifest_detail::kVideoScroll,
      (int) std::size(module_param_manifest_detail::kVideoScroll),
      &module_param_manifest_detail::vsId },
    { ModuleType::LuxStral,    "luxstralOut", 8,
      module_param_manifest_detail::kLsOut,
      (int) std::size(module_param_manifest_detail::kLsOut),
      &module_param_manifest_detail::lsId },
    { ModuleType::LuxSynth,    "luxsynthOut", 8,
      module_param_manifest_detail::kLxLwOut,
      (int) std::size(module_param_manifest_detail::kLxLwOut),
      &module_param_manifest_detail::lxId },
    { ModuleType::LuxWave,     "luxwaveOut",  8,
      module_param_manifest_detail::kLxLwOut,
      (int) std::size(module_param_manifest_detail::kLxLwOut),
      &module_param_manifest_detail::lwId },
    { ModuleType::LuxGrain,    "luxgrainOut", 8,
      module_param_manifest_detail::kLxLwOut,
      (int) std::size(module_param_manifest_detail::kLxLwOut),
      &module_param_manifest_detail::lgId },
    { ModuleType::Sampler,     "luxSampler",  8,
      module_param_manifest_detail::kSampler,
      (int) std::size(module_param_manifest_detail::kSampler),
      &module_param_manifest_detail::fsId },
    // P5-M3 — media source instances (slot 0 = legacy ids).
    { ModuleType::Image,       "imgSrc",      8,
      module_param_manifest_detail::kImage,
      (int) std::size(module_param_manifest_detail::kImage),
      &module_param_manifest_detail::imgId },
    { ModuleType::Video,       "vidSrc",      8,
      module_param_manifest_detail::kVideo,
      (int) std::size(module_param_manifest_detail::kVideo),
      &module_param_manifest_detail::vidId },
    { ModuleType::Camera,      "camSrc",      8,
      module_param_manifest_detail::kCamera,
      (int) std::size(module_param_manifest_detail::kCamera),
      &module_param_manifest_detail::camId },
};

/** Manifest entry for a module type, or nullptr when the type carries no
 *  per-instance bank (the SP3CTRA source, Score/Timbre). */
inline const ModuleParamManifest* moduleParamManifest(ModuleType t)
{
    for (const auto& m : kModuleParamManifest)
        if (m.type == t)
            return &m;
    return nullptr;
}
