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
 *   • the SEQUENCER state (non-APVTS trees) and the shared sampler prefs
 *     (enable LED, export options);
 *   • the score-family GENERATOR state (loaded file, rendered page, offline
 *     generation settings) — non-APVTS, persisted per player slot by the
 *     generator pages themselves ("<type>GenState{N}" / the SCORE child tree).
 *     Only the score-family TRANSPORT banks are in the manifest.
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

// MIDI TAP instance bank ("midiTap{slot}_*"). One source of truth shared by the
// zone-3 page, the right-band MIDI MIX strip and the RT config push. Unlike
// VideoScroll there is NO separate mixer bank: the strip's "level" lives here
// (videoMix* only exists because it predates the module-bank convention).
inline juce::String mtParam(int slot, const char* suffix)
{ return "midiTap" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

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

inline juce::String ctParam(int slot, const char* suffix)
{ return "luxcentro" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String dvParam(int slot, const char* suffix)
{ return "luxdrive" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

inline juce::String dcbParam(int slot, const char* suffix)
{ return "luxdcblock" + juce::String(juce::jlimit(0, 7, slot)) + "_" + suffix; }

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

// Score-family TRANSPORT banks (P7 — instances indépendantes par chaîne).
// Each SCORE-family INSTANCE owns its play / speed / loop / reverse, keyed by
// its player-pool slot: two SCORE modules in two chains no longer drag each
// other's transport. Slot 0 keeps the LEGACY per-type ids ("scoreSpeed",
// "timbreLoop"…) so existing sessions, DAW automation lanes and MIDI mappings
// load unchanged; slots 1..7 own "<prefix>{N}_<suffix>".
// Suffixes: "Playing" | "Speed" | "Loop" | "Reverse".
inline const char* scoreXportPrefix(ModuleType t)
{
    switch (t)
    {
        case ModuleType::Timbre:    return "timbre";
        case ModuleType::MidiScore: return "midiScore";
        case ModuleType::Voice:     return "voice";
        default:                    return "score";
    }
}

inline juce::String scoreXportParam(ModuleType t, int slot, const char* suffix)
{
    const juce::String p(scoreXportPrefix(t));
    return slot <= 0 ? p + suffix
                     : p + juce::String(juce::jlimit(1, 7, slot)) + "_" + suffix;
}

/** Inverse of scoreXportParam: parses @p id back to its (type, slot, suffix).
 *  Returns false for anything that is not a score-family transport id — so the
 *  parameter dispatcher rejects "scoreActive3" and friends in one cheap pass
 *  instead of rebuilding 4×8 candidate ids per parameter change. */
inline bool parseScoreXportParam(const juce::String& id, ModuleType& type,
                                 int& slot, juce::String& suffix)
{
    // "midiScore" must be tested before "score"/"voice" would ever match it;
    // no prefix here is a prefix of another, so plain order is enough.
    for (ModuleType t : kScoreFamily)
    {
        const juce::String p(scoreXportPrefix(t));
        if (! id.startsWith(p))
            continue;
        juce::String rest = id.substring(p.length());
        int s = 0;
        if (rest.isNotEmpty() && rest[0] >= '1' && rest[0] <= '7')
        {
            if (! rest.substring(1).startsWith("_"))
                continue;                       // "score1Foo" is not a bank id
            s    = rest[0] - '0';
            rest = rest.substring(2);
        }
        if (rest != "Playing" && rest != "Speed"
            && rest != "Loop"  && rest != "Reverse")
            continue;                           // e.g. "scoreActive3", "scoreEq…"
        type = t; slot = s; suffix = rest;
        return true;
    }
    return false;
}

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
        case ModuleType::Centroid:  return ctParam(slot, suffix);
        case ModuleType::Drive:     return dvParam(slot, suffix);
        case ModuleType::DcBlock:   return dcbParam(slot, suffix);
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
        "NumPoints", "BackgroundMode",
    };
    inline const char* const kHarmo[] = {
        "Enabled", "Mode", "Root", "Scale",
        "Strength", "Width", "Slope", "Glide",
        "BackgroundMode",
    };
    inline const char* const kCentro[] = {
        "Enabled", "Floor", "Thickness", "Edge",
        "Band0", "Band1", "Band2", "Band3", "Band4",
        "Band5", "Band6", "Band7", "Band8",
        "NumPoints",   // output EQ — same node model as the kEq bank
        "BackgroundMode",
    };
    inline const char* const kDrive[] = {
        "Enabled", "Gamma", "Saturation", "Floor", "ContrastMin", "InvertMode",
        "Band0", "Band1", "Band2", "Band3", "Band4",
        "Band5", "Band6", "Band7", "Band8",
        "NumPoints",   // output EQ — same node model as the kEq bank
        "BackgroundMode",
    };
    inline const char* const kDcBlock[] = {
        "Enabled", "Amount", "BackgroundMode",
    };
    inline const char* const kVideoScroll[] = {
        "mode", "speed", "linePos", "thickness", "zoom", "fade", "gamma",
        "compress", "invert",   // "invert" = legacy bool, migrated to "invertMode"
        "invertMode", "colorMode", "bgR", "bgG", "bgB", "paused", "enabled",
        "MixLevel", "MixBlend",   // → videoMix{N}_level / _blend
    };
    // MIDI TAP — "what counts as a note" only. The TIMEBASE and the FILE are
    // the MIDI MIX master's business (flat midiRec*/midiTempo* params), so
    // nothing here is per-take state.
    inline const char* const kMidiTap[] = {
        "enabled", "arm", "level",
        // detection
        "mode", "source", "threshold", "hysteresis", "relative", "smooth", "peakOnly",
        "maxPoly",
        // timing (ms — converted to lines against the measured line rate)
        "attackMs", "releaseMs", "minOnMs", "maxOnMs",
        // pitch mapping
        "transpose", "noteLo", "noteHi", "rangePolicy", "backgroundMode",
        // velocity + routing
        "velCurve", "velSpan", "velFixed", "channel",
        // dense ("black MIDI") — velocity-tracking restrikes
        "dense", "retrigMs", "retrigDelta",
    };
    // Purge 2026-08-05: the per-OUT conditioning knobs (negative / dcBlocking /
    // gamma / intensity / rangeDb) are gone — conditioning lives in the chain
    // modules (LEVELS, DC), the decode window on the SETUP face (luxstralRangeDb).
    // 2026-08-13: contrastMin followed — the variance dimming is the LEVELS
    // module's CONTRAST knob now (visual domain, the sound follows the picture).
    inline const char* const kLsOut[] = {
        "enabled",
    };
    inline const char* const kLxLwOut[] = {
        "enabled",
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
        "Enabled", "RecMode", "PlayMode", "NumBanks",
        // Per-engine sequencer settings follow the module too. "SeqTransport"
        // is DELIBERATELY absent (command transport, forced to Stop on restore
        // — same rationale as kScoreXport's "Playing").
        "SeqBpm", "SeqNumSteps", "SeqLoop", "SeqDawSync", "SeqBeatsPerStep",
        // REC/PLAY/SAVE bind params removed — those actions are now mapped through
        // the unified MIDI-Learn engine (right-click the buttons in the editor).
    };
    // Score-family transport bank. "Playing" is DELIBERATELY absent: it is a
    // command transport (forced back to Stop on every restore), so letting the
    // chain-owned VALUES / chain type-memory carry it would auto-start a
    // reading when a session or a preset is loaded.
    inline const char* const kScoreXport[] = {
        "Speed", "Loop", "Reverse",
    };

    inline juce::String lpId(int s, const char* x) { return lpParam(s, x); }
    inline juce::String lmId(int s, const char* x) { return lmParam(s, x); }
    inline juce::String rvId(int s, const char* x) { return rvParam(s, x); }
    inline juce::String ecId(int s, const char* x) { return ecParam(s, x); }
    inline juce::String eqId(int s, const char* x) { return eqParam(s, x); }
    inline juce::String hmId(int s, const char* x) { return hmParam(s, x); }
    inline juce::String ctId(int s, const char* x) { return ctParam(s, x); }
    inline juce::String dvId(int s, const char* x) { return dvParam(s, x); }
    inline juce::String dcbId(int s, const char* x) { return dcbParam(s, x); }
    inline juce::String lsId(int s, const char* x) { return lsOutParam(s, x); }
    inline juce::String lxId(int s, const char* x) { return lxOutParam(s, x); }
    inline juce::String lwId(int s, const char* x) { return lwOutParam(s, x); }
    inline juce::String lgId(int s, const char* x) { return lgOutParam(s, x); }
    inline juce::String fsId(int s, const char* x) { return fsEngineParam(s, x); }
    inline juce::String imgId(int s, const char* x) { return imgSrcParam(s, x); }
    inline juce::String vidId(int s, const char* x) { return vidSrcParam(s, x); }
    inline juce::String camId(int s, const char* x) { return camSrcParam(s, x); }
    inline juce::String scId(int s, const char* x) { return scoreXportParam(ModuleType::Score,     s, x); }
    inline juce::String tbId(int s, const char* x) { return scoreXportParam(ModuleType::Timbre,    s, x); }
    inline juce::String msId(int s, const char* x) { return scoreXportParam(ModuleType::MidiScore, s, x); }
    inline juce::String voId(int s, const char* x) { return scoreXportParam(ModuleType::Voice,     s, x); }
    inline juce::String mtId(int s, const char* x) { return mtParam(s, x); }
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
    { ModuleType::Centroid,    "luxcentro",   8,
      module_param_manifest_detail::kCentro,
      (int) std::size(module_param_manifest_detail::kCentro),
      &module_param_manifest_detail::ctId },
    { ModuleType::Drive,       "luxdrive",    8,
      module_param_manifest_detail::kDrive,
      (int) std::size(module_param_manifest_detail::kDrive),
      &module_param_manifest_detail::dvId },
    { ModuleType::DcBlock,     "luxdcblock",  8,
      module_param_manifest_detail::kDcBlock,
      (int) std::size(module_param_manifest_detail::kDcBlock),
      &module_param_manifest_detail::dcbId },
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
    // P7 — score-family transports, one bank per player-pool slot (slot 0 =
    // legacy per-type ids). Being in the manifest is what makes the CHAIN own
    // them: J2 snapshots them into the ModuleInstance VALUES and J3 remembers
    // them per chain when the module leaves and comes back.
    { ModuleType::Score,       "score",       8,
      module_param_manifest_detail::kScoreXport,
      (int) std::size(module_param_manifest_detail::kScoreXport),
      &module_param_manifest_detail::scId },
    { ModuleType::Timbre,      "timbre",      8,
      module_param_manifest_detail::kScoreXport,
      (int) std::size(module_param_manifest_detail::kScoreXport),
      &module_param_manifest_detail::tbId },
    { ModuleType::MidiScore,   "midiScore",   8,
      module_param_manifest_detail::kScoreXport,
      (int) std::size(module_param_manifest_detail::kScoreXport),
      &module_param_manifest_detail::msId },
    { ModuleType::Voice,       "voice",       8,
      module_param_manifest_detail::kScoreXport,
      (int) std::size(module_param_manifest_detail::kScoreXport),
      &module_param_manifest_detail::voId },
    { ModuleType::MidiTap,     "midiTap",     8,
      module_param_manifest_detail::kMidiTap,
      (int) std::size(module_param_manifest_detail::kMidiTap),
      &module_param_manifest_detail::mtId },
};

/** Manifest entry for a module type, or nullptr when the type carries no
 *  per-instance bank (the SP3CTRA source). */
inline const ModuleParamManifest* moduleParamManifest(ModuleType t)
{
    for (const auto& m : kModuleParamManifest)
        if (m.type == t)
            return &m;
    return nullptr;
}
