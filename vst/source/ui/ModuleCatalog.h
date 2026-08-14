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
 *   UTILS — SAMPLER / SCORE          (advanced frame players; the sampler's
 *           step sequencer is INTERNAL to each engine — no rack module)
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
    Sampler, Score,                // UTILS (the retired "Sequencer" rack module
                                   //        is dropped on load by its string id)
    LuxStral, LuxSynth, LuxWave,   // SYNTH
    VideoScroll,                   // VIDEO (waterfall probe — pass-through, slotted)
    Camera,                        // SRC (appended to keep table indices stable)
    Reverb, Echo,                  // FX (appended to keep table indices stable)
    Timbre,                        // UTILS (appended to keep table indices stable)
    Equalizer,                     // FX (appended to keep table indices stable)
    MidiScore,                     // UTILS (appended to keep table indices stable)
    Voice,                         // UTILS (appended to keep table indices stable)
    Harmonize,                     // FX — SCALE quantizer (appended to keep table indices stable)
    LuxGrain,                      // OUT — "→ LUXGRAIN" granular send (appended to keep table indices stable)
    MidiTap,                       // OUT — MIDI note probe (appended to keep table indices stable)
    Centroid,                      // FX — CENTROID mass→barycentre simplifier (appended to keep table indices stable)
    Drive,                         // FX — LEVELS gain/saturation/floor (appended to keep table indices
                                   // stable; renamed DRIVE → LEVELS, enum/persist id stay "Drive")
    DcBlock                        // FX — DC BLOCK per-line mean removal (appended to keep table indices stable)
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
enum class ModuleCat  { SRC, MIDI, FX, UTILS, SYNTH, Out };  // "Out": OUT is a Windows macro

/** Immutable per-type metadata. A module carries no colour of its own: its
 *  colour IS its category's (moduleCatColour via moduleColour) — every piece
 *  of module UI inherits it from there, never from a local constant. */
struct ModuleDesc
{
    ModuleType   type;
    ModuleCat    category;
    ModuleRole   role;
    const char*  displayName;     ///< UTF-8 ("SP3CTRA", "\xE2\x99\xAA LUXSTRAL"…)
    const char*  enableParamId;   ///< APVTS enable param, "" when none
    const char*  id;              ///< stable string id for persistence
};

//==============================================================================
/** The whole catalogue. Table order MUST match the enum order (descFor indexes
 *  by ordinal); the catalogue panel buckets rows by category for display. */
inline const std::array<ModuleDesc, 24>& moduleTable()
{
    static const std::array<ModuleDesc, 24> table = {{
        // type                  category          role                  name                       enableParam          id
        { ModuleType::Sp3ctra,     ModuleCat::SRC,   ModuleRole::Source,   "SP3CTRA",                 "",                  "Sp3ctra"  },
        // Media sources: the type-level id is slot 0's LEGACY global param
        // (P5-M3 pooling) — the rack LED and the face-bar power switch resolve
        // the SELECTED instance's per-slot bank instead (imgSrc{N}_Enabled…).
        { ModuleType::Image,       ModuleCat::SRC,   ModuleRole::Source,   "IMAGE",                   "imgSrcEnabled",     "Image"    },
        { ModuleType::Video,       ModuleCat::SRC,   ModuleRole::Source,   "VIDEO",                   "vidSrcEnabled",     "Video"    },
        // Pitch/Mask/Reverb/Echo/EQ enable lives in the PER-INSTANCE bank
        // (luxpitch{slot}_Enabled…): the rack block and the zone-3 power switch
        // resolve it from the selected instance's pool slot, not from here.
        { ModuleType::Pitch,       ModuleCat::MIDI,  ModuleRole::Processor,"PITCH",                   "",                  "Pitch"    },
        { ModuleType::Mask,        ModuleCat::MIDI,  ModuleRole::Processor,"MASK",                    "",                  "Mask"     },
        { ModuleType::Sampler,     ModuleCat::UTILS, ModuleRole::Util,     "SAMPLER",                 "luxSamplerEnabled", "Sampler"  },
        { ModuleType::Score,       ModuleCat::UTILS, ModuleRole::Util,     "SCORE",                   "",                  "Score"    },
        // Synth-split P2 — the three synths are OUT/send modules in the rack
        // (the flux leaves the chain toward the global engine, which lives in
        // the ZONE-5 dock): OUT category, arrow-prefixed names.
        { ModuleType::LuxStral,    ModuleCat::Out,   ModuleRole::Synth,    "\xE2\x86\x92 LUXSTRAL",   "deviceEnabled",     "LuxStral" },
        { ModuleType::LuxSynth,    ModuleCat::Out,   ModuleRole::Synth,    "\xE2\x86\x92 LUXSYNTH",   "luxsynthEnabled",   "LuxSynth" },
        { ModuleType::LuxWave,     ModuleCat::Out,   ModuleRole::Synth,    "\xE2\x86\x92 LUXWAVE",    "luxwaveEnabled",    "LuxWave"  },
        { ModuleType::VideoScroll, ModuleCat::Out,   ModuleRole::Processor,"VIDEO SCROLL",            "",                  "VideoScroll" },
        { ModuleType::Camera,      ModuleCat::SRC,   ModuleRole::Source,   "CAMERA",                  "camSrcEnabled",     "Camera"   },
        { ModuleType::Reverb,      ModuleCat::FX,    ModuleRole::Processor,"REVERB",                  "",                  "Reverb"   },
        { ModuleType::Echo,        ModuleCat::FX,    ModuleRole::Processor,"ECHO",                    "",                  "Echo"     },
        { ModuleType::Timbre,      ModuleCat::UTILS, ModuleRole::Util,     "TIMBRE",                  "",                  "Timbre"   },
        { ModuleType::Equalizer,   ModuleCat::FX,    ModuleRole::Processor,"EQ",                      "",                  "Equalizer" },
        { ModuleType::MidiScore,   ModuleCat::UTILS, ModuleRole::Util,     "MIDI SCORE",              "",                  "MidiScore" },
        { ModuleType::Voice,       ModuleCat::UTILS, ModuleRole::Util,     "VOICE",                   "",                  "Voice"    },
        { ModuleType::Harmonize,   ModuleCat::FX,    ModuleRole::Processor,"SCALE",                   "",                  "Harmonize" },
        { ModuleType::LuxGrain,    ModuleCat::Out,   ModuleRole::Synth,    "\xE2\x86\x92 LUXGRAIN",   "luxgrainEnabled",   "LuxGrain" },
        // MIDI TAP is a PROBE, not a send: no "→" prefix (that marks a flux
        // leaving toward an audio ENGINE) and Processor role, exactly like
        // VIDEO SCROLL. Its enable lives in the PER-INSTANCE bank
        // (midiTap{slot}_enabled), so enableParamId stays empty here.
        { ModuleType::MidiTap,     ModuleCat::Out,   ModuleRole::Processor,"MIDI TAP",                "",                  "MidiTap"  },
        { ModuleType::Centroid,    ModuleCat::FX,    ModuleRole::Processor,"CENTROID",                "",                  "Centroid" },
        // Display name LEVELS (was DRIVE) — the persist id keeps "Drive" so
        // sessions saved under the old name load unchanged.
        { ModuleType::Drive,       ModuleCat::FX,    ModuleRole::Processor,"LEVELS",                  "",                  "Drive"    },
        { ModuleType::DcBlock,     ModuleCat::FX,    ModuleRole::Processor,"DC BLOCK",                "",                  "DcBlock"  },
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

/** One colour per catalogue section — THE palette of the whole module UI. */
inline juce::Colour moduleCatColour(ModuleCat c)
{
    switch (c)
    {
        case ModuleCat::SRC:   return juce::Colour(0xff00d9ff);   // electric cyan
        case ModuleCat::MIDI:  return juce::Colour(0xffff2ed0);   // hot magenta
        case ModuleCat::FX:    return juce::Colour(0xffb44dff);   // neon violet
        case ModuleCat::UTILS: return juce::Colour(0xffffb020);   // vivid amber
        case ModuleCat::SYNTH: return juce::Colour(0xff45ff8c);   // neon green
        case ModuleCat::Out:   return juce::Colour(0xff45ff8c);   // neon green
    }
    return juce::Colour(0xff00d9ff);
}

/** A module's colour = its category's colour. Single inheritance point for ALL
 *  module-tinted chrome — catalogue chips, rack blocks, zone-3 PLAY/SETUP
 *  pages, mixer strips. Never restate a module hue as a local constant. */
inline juce::Colour moduleColour(ModuleType t)       { return moduleCatColour(descFor(t).category); }
inline juce::String moduleEnableParam(ModuleType t)  { return juce::String(descFor(t).enableParamId); }
inline juce::String moduleDisplayName(ModuleType t)  { return juce::String::fromUTF8(descFor(t).displayName); }
inline ModuleRole   moduleRole(ModuleType t)         { return descFor(t).role; }
inline ModuleCat    moduleCategory(ModuleType t)     { return descFor(t).category; }

/** Catalogue display rank inside a section. The table is frozen in enum order
 *  (descFor indexes by ordinal), so late-appended modules use this to slot into
 *  their natural place — a stable sort keeps table order among equals. Today:
 *  the two PROBES (VIDEO SCROLL, MIDI TAP) close the OUT section so the "→"
 *  engine sends (incl. the appended LUXGRAIN) stay grouped above them. */
inline int          moduleCatalogueRank(ModuleType t)
{ return (t == ModuleType::VideoScroll || t == ModuleType::MidiTap) ? 1 : 0; }

/** True when a module needs incoming MIDI to do anything — i.e. it consumes
 *  NoteOn/NoteOff in processBlock. Keyed on the MIDI catalogue section (PITCH /
 *  MASK today), so any future MIDI-category module inherits the picto for free.
 *  Drives the DIN badge drawn by the catalogue chips and the rack blocks. */
inline bool         moduleNeedsMidi(ModuleType t)    { return moduleCategory(t) == ModuleCat::MIDI; }

/** True for audio-flux processors of the FX catalogue section. Drives the
 *  boxed-"FX" badge drawn by the catalogue chips and the rack blocks, the
 *  same way moduleNeedsMidi drives the keyboard badge. */
inline bool         moduleIsFx(ModuleType t)         { return moduleCategory(t) == ModuleCat::FX; }

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

    /** Draws "FX" boxed in a small rounded square, tinted `c`. Flags the audio
     *  processors of the FX section (see moduleIsFx) the way drawMidiKeyboard
     *  flags MIDI-input modules. Letters-in-a-box rather than a pictogram so
     *  it stays unambiguous at badge size. */
    inline void drawFxBadge(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour c)
    {
        const float w    = area.getWidth();
        const float boxH = juce::jmin(area.getHeight(), w * 0.9f);
        const juce::Rectangle<float> body(area.getX(), area.getCentreY() - boxH * 0.5f, w, boxH);

        const float stroke = juce::jmax(0.9f, w * 0.08f);
        const float radius = juce::jmax(1.0f, w * 0.16f);

        g.setColour(c);
        g.drawRoundedRectangle(body.reduced(stroke * 0.5f), radius, stroke);

        g.setFont(juce::Font(juce::FontOptions(boxH * 0.72f)).boldened());
        g.drawText("FX", body.toNearestInt(), juce::Justification::centred, false);
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
        case ModuleCat::Out:   return "OUT";
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
