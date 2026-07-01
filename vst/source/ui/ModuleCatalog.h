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
    VideoScroll                    // VIDEO (waterfall probe — pass-through, slotted)
};

/** Behavioural role — drives the placement constraints. */
enum class ModuleRole { Source, Processor, Util, Synth };

/** Catalogue section a module belongs to. */
enum class ModuleCat  { SRC, MIDI, UTILS, SYNTH, OUT };

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
/** The whole catalogue, in canonical display order. */
inline const std::array<ModuleDesc, 12>& moduleTable()
{
    static const std::array<ModuleDesc, 12> table = {{
        // type                  category          role                  name                       colour       enableParam          id
        { ModuleType::Sp3ctra,     ModuleCat::SRC,   ModuleRole::Source,   "SP3CTRA",                 0xff68788f,  "",                  "Sp3ctra"  },
        { ModuleType::Image,       ModuleCat::SRC,   ModuleRole::Source,   "IMAGE",                   0xff68788f,  "",                  "Image"    },
        { ModuleType::Video,       ModuleCat::SRC,   ModuleRole::Source,   "VIDEO",                   0xff68788f,  "",                  "Video"    },
        { ModuleType::Pitch,       ModuleCat::MIDI,  ModuleRole::Processor,"PITCH",                   0xffe06bb8,  "luxpitchEnabled",   "Pitch"    },
        { ModuleType::Mask,        ModuleCat::MIDI,  ModuleRole::Processor,"MASK",                    0xff6be0d0,  "luxmaskEnabled",    "Mask"     },
        { ModuleType::Sampler,     ModuleCat::UTILS, ModuleRole::Util,     "SAMPLER",                 0xffe09040,  "luxSamplerEnabled", "Sampler"  },
        { ModuleType::Score,       ModuleCat::UTILS, ModuleRole::Util,     "SCORE",                   0xffe0a24a,  "",                  "Score"    },
        { ModuleType::Sequencer,   ModuleCat::UTILS, ModuleRole::Util,     "SEQUENCER",               0xff7ac0e0,  "seqEnabled",        "Sequencer" },
        { ModuleType::LuxStral,    ModuleCat::SYNTH, ModuleRole::Synth,    "\xE2\x99\xAA LUXSTRAL",   0xff4fa3e0,  "deviceEnabled",     "LuxStral" },
        { ModuleType::LuxSynth,    ModuleCat::SYNTH, ModuleRole::Synth,    "\xE2\x99\xAA LUXSYNTH",   0xffb07af0,  "luxsynthEnabled",   "LuxSynth" },
        { ModuleType::LuxWave,     ModuleCat::SYNTH, ModuleRole::Synth,    "\xE2\x99\xAA LUXWAVE",    0xff8fd05a,  "luxwaveEnabled",    "LuxWave"  },
        { ModuleType::VideoScroll, ModuleCat::OUT,   ModuleRole::Processor,"VIDEO SCROLL",            0xff5ad0c8,  "",                  "VideoScroll" },
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

/** Human label for a catalogue section header. */
inline const char* moduleCatLabel(ModuleCat c)
{
    switch (c)
    {
        case ModuleCat::SRC:   return "SRC";
        case ModuleCat::MIDI:  return "MIDI";
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
