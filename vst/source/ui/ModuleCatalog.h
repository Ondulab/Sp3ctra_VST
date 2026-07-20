/**
 * @file ModuleCatalog.h
 * @brief Single source of truth describing every chain module (M6).
 *
 * The chain rack used to scatter a block's identity (display name, colour,
 * enable parameter, category) across switch(ChainBlockId) statements. This
 * table centralises it, keyed by a stable ModuleType, so the catalogue panel,
 * the rack blocks, the zone-3 page mapping and persistence all agree.
 *
 * Categories (left catalogue sections):
 *   SRC   — SP3CTRA / IMAGE / VIDEO   (chain input, optional, at most one)
 *   MIDI  — PITCH / MASK              (image-space processors, order matters)
 *   UTILS — SAMPLER / SCORE / SEQUENCER (advanced frame players + step driver)
 *   SYNTH — LUXSTRAL / LUXSYNTH / LUXWAVE (audio engines)
 *   OUT   — VIDEO SCROLL              (video output taps — multi-instance, slotted)
 *
 * Placement rules live in ChainModel (at most one Source role per chain; never
 * two instances of the same ModuleType in a chain — except multi-instance OUT
 * taps like VideoScroll; order is free).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

//==============================================================================
/** Stable identity of a module type. Persisted by string id (see moduleTypeId),
 *  never by ordinal, so reordering this enum can't corrupt saved sessions. */
enum class ModuleType
{
    Sp3ctra = 0, Image, Video,     // SRC
    Pitch, Mask,                   // MIDI
    Sampler, Score, Sequencer,     // UTILS
    LuxStral, LuxSynth, LuxWave,   // SYNTH
    VideoScroll,                   // VIDEO (waterfall probe — pass-through, slotted)
    Camera,                        // SRC (appended to keep table indices stable)
    Reverb, Echo,                  // FX (appended to keep table indices stable)
    Timbre,                        // UTILS (appended to keep table indices stable)
    Equalizer,                     // FX (appended to keep table indices stable)
    MidiScore,                     // UTILS (appended to keep table indices stable)
    Voice,                         // UTILS (appended to keep table indices stable)
    Harmonize,                     // FX — SCALE quantizer (appended to keep table indices stable)
    LuxGrain                       // OUT — "→ LUXGRAIN" granular send (appended to keep table indices stable)
};

/** SCORE / TIMBRE / MIDI SCORE / VOICE all audition through the
 *  score-player service. THE list — every site that
 *  gates on the family iterates it or calls isScoreFamily(), so adding the
 *  next member updates them all at once (the P4-M2 VOICE bug — a chain
 *  hosting only VOICE never became player-owned — came from one of four
 *  hand-maintained copies of this list missing the newest member). */
inline constexpr ModuleType kScoreFamily[] = {
    ModuleType::Score, ModuleType::Timbre,
    ModuleType::MidiScore, ModuleType::Voice
};

inline bool isScoreFamily(ModuleType t)
{
    for (auto f : kScoreFamily)
        if (t == f)
            return true;
    return false;
}

/** Behavioural role — drives the placement constraints. */
enum class ModuleRole { Source, Processor, Util, Synth };

/** Catalogue section a module belongs to. */
enum class ModuleCat  { SRC, MIDI, FX, UTILS, SYNTH, OUT };

/** Immutable per-type metadata. */
struct ModuleDesc
{
    ModuleType   type;
    ModuleCat    category;
    ModuleRole   role;
    const char*  displayName;     ///< UTF-8 ("SP3CTRA", "\xE2\x99\xAA LUXSTRAL"…)
    juce::uint32 colourArgb;      ///< identity colour
    const char*  enableParamId;   ///< APVTS enable param, "" when none
    const char*  id;              ///< stable string id for persistence
};

//==============================================================================
/** The whole catalogue. Table order MUST match the enum order (descFor indexes
 *  by ordinal); the catalogue panel buckets rows by category for display. */
inline const std::array<ModuleDesc, 21>& moduleTable()
{
    static const std::array<ModuleDesc, 21> table = {{
        // type                  category          role                  name                       colour       enableParam          id
        { ModuleType::Sp3ctra,     ModuleCat::SRC,   ModuleRole::Source,   "SP3CTRA",                 0xff68788f,  "",                  "Sp3ctra"  },
        // Media sources are engine singletons (V1 decision C): one global
        // ACTIVE param each — the rack LED and the PLAY-face toggle share it.
        { ModuleType::Image,       ModuleCat::SRC,   ModuleRole::Source,   "IMAGE",                   0xff68788f,  "imgSrcEnabled",     "Image"    },
        { ModuleType::Video,       ModuleCat::SRC,   ModuleRole::Source,   "VIDEO",                   0xff68788f,  "vidSrcEnabled",     "Video"    },
        // Pitch/Mask/Reverb/Echo/EQ enable lives in the PER-INSTANCE bank
        // (luxpitch{slot}_Enabled…): the rack block and the zone-3 power switch
        // resolve it from the selected instance's pool slot, not from here.
        { ModuleType::Pitch,       ModuleCat::MIDI,  ModuleRole::Processor,"PITCH",                   0xffe06bb8,  "",                  "Pitch"    },
        { ModuleType::Mask,        ModuleCat::MIDI,  ModuleRole::Processor,"MASK",                    0xff6be0d0,  "",                  "Mask"     },
        { ModuleType::Sampler,     ModuleCat::UTILS, ModuleRole::Util,     "SAMPLER",                 0xffe09040,  "luxSamplerEnabled", "Sampler"  },
        { ModuleType::Score,       ModuleCat::UTILS, ModuleRole::Util,     "SCORE",                   0xffe0a24a,  "",                  "Score"    },
        { ModuleType::Sequencer,   ModuleCat::UTILS, ModuleRole::Util,     "SEQUENCER",               0xff7ac0e0,  "seqEnabled",        "Sequencer" },
        // Synth-split P2 — the three synths are OUT/send modules in the rack
        // (the flux leaves the chain toward the global engine, which lives in
        // the ZONE-5 dock): OUT category, arrow-prefixed names.
        { ModuleType::LuxStral,    ModuleCat::OUT,   ModuleRole::Synth,    "\xE2\x86\x92 LUXSTRAL",   0xff4fa3e0,  "deviceEnabled",     "LuxStral" },
        { ModuleType::LuxSynth,    ModuleCat::OUT,   ModuleRole::Synth,    "\xE2\x86\x92 LUXSYNTH",   0xffb07af0,  "luxsynthEnabled",   "LuxSynth" },
        { ModuleType::LuxWave,     ModuleCat::OUT,   ModuleRole::Synth,    "\xE2\x86\x92 LUXWAVE",    0xff8fd05a,  "luxwaveEnabled",    "LuxWave"  },
        { ModuleType::VideoScroll, ModuleCat::OUT,   ModuleRole::Processor,"VIDEO SCROLL",            0xff5ad0c8,  "",                  "VideoScroll" },
        { ModuleType::Camera,      ModuleCat::SRC,   ModuleRole::Source,   "CAMERA",                  0xff68788f,  "camSrcEnabled",     "Camera"   },
        { ModuleType::Reverb,      ModuleCat::FX,    ModuleRole::Processor,"REVERB",                  0xff9d8ce0,  "",                  "Reverb"   },
        { ModuleType::Echo,        ModuleCat::FX,    ModuleRole::Processor,"ECHO",                    0xffe0c95a,  "",                  "Echo"     },
        { ModuleType::Timbre,      ModuleCat::UTILS, ModuleRole::Util,     "TIMBRE",                  0xffd97b52,  "",                  "Timbre"   },
        { ModuleType::Equalizer,   ModuleCat::FX,    ModuleRole::Processor,"EQ",                      0xffe0847a,  "",                  "Equalizer" },
        { ModuleType::MidiScore,   ModuleCat::UTILS, ModuleRole::Util,     "MIDI SCORE",              0xffc9a13e,  "",                  "MidiScore" },
        { ModuleType::Voice,       ModuleCat::UTILS, ModuleRole::Util,     "VOICE",                   0xffd06a9e,  "",                  "Voice"    },
        { ModuleType::Harmonize,   ModuleCat::FX,    ModuleRole::Processor,"SCALE",                   0xff8fb84f,  "",                  "Harmonize" },
        { ModuleType::LuxGrain,    ModuleCat::OUT,   ModuleRole::Synth,    "\xE2\x86\x92 LUXGRAIN",   0xffd0a25a,  "luxgrainEnabled",   "LuxGrain" },
    }};
    return table;
}

/** Descriptor for a type (table order matches the enum order, so index = (int)t). */
inline const ModuleDesc& descFor(ModuleType t)
{
    return moduleTable()[(size_t) t];
}

//==============================================================================
// Convenience accessors — every consumer goes through these.
//==============================================================================
inline juce::Colour moduleColour(ModuleType t)       { return juce::Colour(descFor(t).colourArgb); }
inline juce::String moduleEnableParam(ModuleType t)  { return juce::String(descFor(t).enableParamId); }
inline juce::String moduleDisplayName(ModuleType t)  { return juce::String::fromUTF8(descFor(t).displayName); }
inline ModuleRole   moduleRole(ModuleType t)         { return descFor(t).role; }
inline ModuleCat    moduleCategory(ModuleType t)     { return descFor(t).category; }

/** True when a module needs incoming MIDI to do anything — i.e. it consumes
 *  NoteOn/NoteOff in processBlock. Keyed on the MIDI catalogue section (PITCH /
 *  MASK today), so any future MIDI-category module inherits the picto for free.
 *  Drives the DIN badge drawn by the catalogue chips and the rack blocks. */
inline bool         moduleNeedsMidi(ModuleType t)    { return moduleCategory(t) == ModuleCat::MIDI; }

/** Stable persistence id for a type ("Pitch", "LuxStral"…). */
inline const char*  moduleTypeId(ModuleType t)       { return descFor(t).id; }

/** Parse a persisted string id back to a ModuleType. Returns false on unknown
 *  ids (e.g. a session saved by a newer build) so callers can drop them. */
inline bool moduleTypeFromId(const juce::String& s, ModuleType& out)
{
    for (const auto& d : moduleTable())
        if (s == d.id) { out = d.type; return true; }
    return false;
}

//==============================================================================
// Shared module glyphs — drawn identically by the catalogue chips and the rack
// blocks so a module reads the same wherever it appears.
//==============================================================================
namespace ModuleIcons
{
    /** Draws a tiny piano keyboard inside `area`, tinted `c`. Flags modules that
     *  require a MIDI input / are keyboard-played (see moduleNeedsMidi): the
     *  white-key bed is outlined with two black keys (the classic group-of-2).
     *  Deliberately minimal so it stays legible at badge size. */
    inline void drawMidiKeyboard(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour c)
    {
        const float w   = area.getWidth();
        const float kbH = juce::jmin(area.getHeight(), w * 0.82f);
        const juce::Rectangle<float> body(area.getX(), area.getCentreY() - kbH * 0.5f, w, kbH);

        const float stroke = juce::jmax(0.9f, w * 0.09f);
        const float radius = juce::jmax(1.0f, w * 0.12f);

        // White-key bed.
        g.setColour(c);
        g.drawRoundedRectangle(body.reduced(stroke * 0.5f), radius, stroke);

        // White-key dividers (3 keys → 2 lines).
        constexpr int whiteKeys = 3;
        const float kw = body.getWidth() / (float) whiteKeys;
        for (int i = 1; i < whiteKeys; ++i)
        {
            const float x = body.getX() + kw * (float) i;
            g.drawLine(x, body.getY() + stroke, x, body.getBottom() - stroke, stroke * 0.85f);
        }

        // Two black keys on the upper ~55 %, at the C#/D# boundaries.
        const float bkW = kw * 0.52f;
        const float bkH = body.getHeight() * 0.55f;
        for (int boundary : { 1, 2 })
        {
            const float cx = body.getX() + kw * (float) boundary;
            g.fillRect(juce::Rectangle<float>(cx - bkW * 0.5f, body.getY(), bkW, bkH));
        }
    }
}

/** Human label for a catalogue section header. */
inline const char* moduleCatLabel(ModuleCat c)
{
    switch (c)
    {
        case ModuleCat::SRC:   return "SRC";
        case ModuleCat::MIDI:  return "MIDI";
        case ModuleCat::FX:    return "FX";
        case ModuleCat::UTILS: return "UTILS";
        case ModuleCat::SYNTH: return "SYNTH";
        case ModuleCat::OUT:   return "OUT";
    }
    return "";
}

//==============================================================================
// Drag-and-drop payload — shared vocabulary between the catalogue (drag source)
// and the chain rack (drop target). Two kinds:
//   "catalog"  — a new module dragged from the left catalogue  → carries type
//   "rackMove" — an existing rack block being reordered/moved   → carries uuid
//==============================================================================
namespace ModuleDrag
{
    inline juce::var fromCatalogue(ModuleType t)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("kind", "catalog");
        o->setProperty("moduleType", (int) t);
        return juce::var(o);
    }

    inline juce::var fromRackMove(const juce::Uuid& id)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("kind", "rackMove");
        o->setProperty("uuid", id.toString());
        return juce::var(o);
    }

    inline juce::String kind(const juce::var& v)
    {
        if (auto* o = v.getDynamicObject())
            return o->getProperty("kind").toString();
        return {};
    }

    inline bool isCatalogue(const juce::var& v) { return kind(v) == "catalog"; }
    inline bool isRackMove (const juce::var& v) { return kind(v) == "rackMove"; }

    inline bool moduleType(const juce::var& v, ModuleType& out)
    {
        if (auto* o = v.getDynamicObject())
        {
            const int idx = (int) o->getProperty("moduleType");
            if (idx >= 0 && idx < (int) moduleTable().size())
            {
                out = (ModuleType) idx;
                return true;
            }
        }
        return false;
    }

    inline juce::Uuid uuid(const juce::var& v)
    {
        if (auto* o = v.getDynamicObject())
            return juce::Uuid(o->getProperty("uuid").toString());
        return {};
    }
}
