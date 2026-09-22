/**
 * @file ParamIdentity.h
 * @brief THE human identity of a mappable parameter — whose it is and what it
 *        is called — and the one label that paints it.
 *
 *   CC 32   (4) VIDEO · DC BLOCK · Amount
 *   ─────   ─────────   ────────   ──────
 *   event   chain       module     parameter
 *   white   pastille + name in     module     muted text
 *           the chain colour       colour
 *
 * Before this file every reader of a parameter name rolled its own: the MIDI
 * MAP rows glued "MODULE · <APVTS name>", the CIS OLED stripped bank
 * prefixes with heuristics of its own, the CONTROLS page tooltips showed the
 * raw APVTS name ("DC2 Amount"). The APVTS name is HOST-facing — it must be
 * unique across the 8 instances of a bank, hence the "DC2 " tag — and is
 * never what a musician should read: the chain and the module already say
 * where the parameter lives.
 *
 *   • ParamNaming::bareName  — strips exactly the tag ModuleCatalog put there
 *                              (moduleAbbrev) plus the words that repeat the
 *                              module, for APVTS params;
 *   • ParamNaming::virtualName — names the non-APVTS targets (sampler slots,
 *                              selected EQ handle, FREEZE) from their ids;
 *   • describeParam          — resolves event / chain / module / name from the
 *                              processor (MIDI map, navTargetForParam, chain
 *                              model) into a ParamIdentity;
 *   • ParamIdentityLabel     — paints it, left or right aligned, the
 *                              parameter name giving way first when narrow;
 *                              the MIDI event never gives way (which
 *                              controller drives the row is the one thing
 *                              that must always be readable).
 *
 * Message thread only (String work, chain-model reads).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <vector>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/EqHandleMidiTargets.h"
#include "../midi/LfoBank.h"
#include "../sampler/SamplerMidiTargets.h"
#include "ChainIdentity.h"
#include "ModuleCatalog.h"

//==============================================================================
namespace ParamNaming
{
    /** Words a param name may carry that just repeat the module's identity —
     *  the chain + module already say it ("LS OUT1 On" on a LuxStral send,
     *  "Voice 2 Play", "Score Active 2" on any score-family instance — the
     *  pool param is named "Score" whoever owns the slot). */
    inline bool isModuleWord(ModuleType t, const juce::String& w)
    {
        const juce::String u = w.toUpperCase();
        if (u == moduleAbbrev(t))
            return true;
        switch (t)
        {
            case ModuleType::LuxStral:  return u == "LUXSTRAL";
            case ModuleType::LuxSynth:  return u == "LUXSYNTH";
            case ModuleType::LuxWave:   return u == "LUXWAVE";
            case ModuleType::LuxGrain:  return u == "LUXGRAIN";
            case ModuleType::Sampler:   return u == "LUXSAMPLER";
            case ModuleType::Score:
            case ModuleType::Voice:
            case ModuleType::MidiScore:
            case ModuleType::Timbre:    return u == "SCORE" || u == "VOICE"
                                            || u == "MIDI"  || u == "TIMBRE";
            case ModuleType::Image:
            case ModuleType::Video:
            case ModuleType::Camera:    return u == "IMAGE" || u == "VIDEO"
                                            || u == "CAMERA" || u == "SRC";
            case ModuleType::VideoScroll: return u == "VIDEO" || u == "SCROLL";
            case ModuleType::Sp3ctra:   return u == "SP3CTRA" || u == "CIS";
            default:                    return false;
        }
    }

    /** Strip the per-instance prefix shapes a bank param name may carry when
     *  the module is NOT known from the id ("DC2 ", "VS0 ", "LuxSampler B ",
     *  "Image Src 1 "...). Unknown shapes come back unchanged. */
    inline juce::String stripBankPrefix(const juce::String& name)
    {
        juce::StringArray tok = juce::StringArray::fromTokens(name, " ", "");
        if (tok.size() >= 2)
        {
            const juce::String t0 = tok[0];
            // "DC2" / "RV0" / "VS3" — leading capitals then digits.
            int letters = 0;
            while (letters < t0.length() && juce::CharacterFunctions::isUpperCase(t0[letters])) ++letters;
            bool tagLike = letters > 0 && letters < t0.length();
            for (int j = letters; tagLike && j < t0.length(); ++j)
                tagLike = juce::CharacterFunctions::isDigit(t0[j]);
            if (tagLike)
            { tok.remove(0); return tok.joinIntoString(" "); }
            if (t0 == "LuxSampler")
            {
                tok.remove(0);
                if (tok.size() > 1 && (tok[0] == "B" || tok[0].containsOnly("0123456789")))
                    tok.remove(0);
                return tok.joinIntoString(" ");
            }
            // "Image Src 1 …" / "Video Src 1 …" / "Camera Src 1 …"
            if (tok.size() >= 4 && tok[1] == "Src" && tok[2].containsOnly("0123456789"))
            { tok.removeRange(0, 3); return tok.joinIntoString(" "); }
        }
        return name;
    }

    /** The BARE parameter name — what a human reads once the chain and the
     *  module are shown beside it. `type` = the owning module when known
     *  (nullptr otherwise): then the catalogue's bank tag "<ABBREV><slot> "
     *  is removed deterministically, plus the words repeating the module and
     *  a leading slot number. Never empty: falls back to `fullName`. */
    inline juce::String bareName(const juce::String& fullName, const ModuleType* type)
    {
        juce::String s = fullName.trim();
        if (type != nullptr)
        {
            // 1. The catalogue tag: "DC2 Amount" → "Amount", "LS OUT1 Volume"
            //    → "OUT1 Volume" (the tag alone is also accepted).
            const juce::String ab(moduleAbbrev(*type));
            if (ab.isNotEmpty() && s.startsWith(ab))
            {
                int i = ab.length();
                while (i < s.length() && juce::CharacterFunctions::isDigit(s[i])) ++i;
                if (i < s.length() && s[i] == ' ')
                    s = s.substring(i + 1).trimStart();
            }
        }
        // 2. The irregular families (sampler engines, media sources).
        s = stripBankPrefix(s);
        // 3. Words repeating the module, then a leading slot number
        //    ("Score 2 Play" → "Play"). Always keep the last word.
        if (type != nullptr)
        {
            juce::StringArray tok = juce::StringArray::fromTokens(s, " ", "");
            while (tok.size() > 1 && isModuleWord(*type, tok[0]))
                tok.remove(0);
            while (tok.size() > 1 && tok[0].containsOnly("0123456789"))
                tok.remove(0);
            s = tok.joinIntoString(" ");
        }
        return s.isNotEmpty() ? s : fullName;
    }

    /** Name of a NON-APVTS (virtual) mapping target from its synthetic id:
     *  "smp:e0:s3:speed" → "S4 Speed", "eqh:luxeq3:freq" → "Handle Freq",
     *  "img:freeze" → "Freeze". Unknown shapes: the capitalised id tail. */
    inline juce::String virtualName(const juce::String& id)
    {
        if (id.startsWith("smp:e"))
        {
            if (const int t = SamplerMidiTargets::resolve(id); t >= 0)
            {
                const auto k = SamplerMidiTargets::tKind(t);
                juce::String s;
                if (! SamplerMidiTargets::isEngineWide(k))
                    s << "S" << juce::String(SamplerMidiTargets::tSlot(t) + 1) << " ";
                return s + SamplerMidiTargets::displayName(k);
            }
        }
        else if (id.startsWith("eqh:"))
        {
            if (const int t = EqHandleMidiTargets::resolve(id); t >= 0)
                switch (EqHandleMidiTargets::tWhich(t))
                {
                    case EqHandleMidiTargets::WhichFreq: return "Handle Freq";
                    case EqHandleMidiTargets::WhichGain: return "Handle Gain";
                    default:                             return "Handle Width";
                }
        }
        else if (id == Sp3ctraAudioProcessor::kImgFreezeMidiId)
            return "Freeze";
        else if (id.startsWith("lfo:"))
        {
            if (const int t = LfoMidiTargets::resolve(id); t >= 0)
                return LfoMidiTargets::whichName(LfoMidiTargets::tWhich(t));
        }
        else if (id.startsWith("luxdiff"))
        {
            // DIFF action buttons — luxdiff{slot}_Capture / _Clear (virtual,
            // bank-prefixed so they navigate like the bank's own params).
            if (id.endsWith("_Capture")) return "Capture";
            if (id.endsWith("_Clear"))   return "Clear";
        }

        const juce::String tail = id.fromLastOccurrenceOf(":", false, false);
        if (tail.isEmpty()) return id;
        return tail.substring(0, 1).toUpperCase() + tail.substring(1);
    }
} // namespace ParamNaming

//==============================================================================
/** One mappable parameter, described for a human. Built by describeParam(). */
struct ParamIdentity
{
    juce::String paramId;

    // ── MIDI event (eventType 0 = unmapped) ──────────────────────────────────
    int          eventType { 0 };    ///< 1 = Note, 2 = CC, 3 = LFO (MidiMappingEngine)
    int          channel   { 0 };    ///< 1..16
    int          number    { 0 };
    juce::String eventName;          ///< "CC 32" / "C#3" / "LFO 3"

    // ── Chain (chainNumber 0 = the module sits in no chain) ──────────────────
    int          chainNumber { 0 };  ///< 1-based rack index
    juce::String chainName;          ///< user label, or "CHAIN"
    juce::Colour chainColour;

    // ── Module ───────────────────────────────────────────────────────────────
    bool         hasModule { false };
    ModuleType   moduleType { ModuleType::Sp3ctra };
    juce::String moduleName;         ///< "DC BLOCK" (no "→" send arrow)
    juce::Colour moduleColour;

    // ── Parameter ────────────────────────────────────────────────────────────
    juce::String paramName;          ///< "Amount" — the bare name

    /** "CC 32 · ch 1" (empty when unmapped). */
    juce::String eventText() const
    {
        if (eventName.isEmpty()) return {};
        if (eventType == MidiMappingEngine::kTypeLfo)
            return eventName;   // an LFO has no channel — it is not an event
        return eventName + juce::String::fromUTF8(" \xC2\xB7 ch ") + juce::String(channel);
    }

    /** "4 VIDEO · DC BLOCK · Amount" — plain text for tooltips and menus. */
    juce::String targetText() const
    {
        const juce::String sep = juce::String::fromUTF8(" \xC2\xB7 ");
        juce::String s;
        if (chainNumber > 0) s << juce::String(chainNumber) << " " << chainName;
        if (hasModule)       s << (s.isEmpty() ? "" : sep) << moduleName;
        if (paramName.isNotEmpty()) s << (s.isEmpty() ? "" : sep) << paramName;
        return s;
    }

    /** Event + target, or the target alone when unmapped. */
    juce::String text() const
    {
        const juce::String ev = eventText();
        return ev.isEmpty() ? targetText() : ev + "  " + targetText();
    }
};

/** Resolve `paramId` — an APVTS id or a virtual target id — against the
 *  processor: the learnt event (MIDI map), the owning module and its chain
 *  (navTargetForParam + chain model), the bare name (ParamNaming). */
inline ParamIdentity describeParam(Sp3ctraAudioProcessor& proc, const juce::String& paramId)
{
    ParamIdentity id;
    id.paramId = paramId;

    if (int t = 0, c = 0, n = 0; proc.getMidiMap().getMappingFor(paramId, t, c, n))
    {
        id.eventType = t;
        id.channel   = c;
        id.number    = n;
        id.eventName = t == MidiMappingEngine::kTypeLfo
                         ? "LFO " + juce::String(n + 1)
                     : t == MidiMappingEngine::kTypeNote
                         ? juce::MidiMessage::getMidiNoteName(n, true, true, 3)
                         : "CC " + juce::String(n);
    }

    // An LFO's own parameter has no chain and no module: the LFO IS its
    // owner, so it takes the module segment (in the modulation hue).
    if (const int lt = LfoMidiTargets::resolve(paramId); lt >= 0)
    {
        id.hasModule    = true;
        id.moduleName   = "LFO " + juce::String(LfoMidiTargets::tIndex(lt) + 1);
        id.moduleColour = juce::Colour(Sp3ctraTheme::kColMod);
        id.paramName    = ParamNaming::virtualName(paramId);
        return id;
    }

    const auto nav = proc.navTargetForParam(paramId);
    if (nav.valid)
    {
        id.hasModule    = true;
        id.moduleType   = nav.type;
        id.moduleName   = moduleDisplayName(nav.type)
                              .removeCharacters(juce::String::fromUTF8("\xE2\x86\x92")).trim();
        id.moduleColour = moduleColour(nav.type);

        if (const int chain = proc.chainIndexForInstance(nav.instanceId); chain >= 0)
        {
            id.chainNumber = chain + 1;
            id.chainName   = ChainIdentity::label(
                proc.getChainModel().chains[(size_t) chain].name);
            id.chainColour = ChainIdentity::colour(chain);
        }
    }

    if (auto* p = proc.getAPVTS().getParameter(paramId))
        id.paramName = ParamNaming::bareName(p->getName(64),
                                             id.hasModule ? &id.moduleType : nullptr);
    else
        id.paramName = ParamNaming::virtualName(paramId);
    return id;
}

//==============================================================================
/** The identity label — one line: event, chain pastille + name, module,
 *  parameter. Same grammar wherever a MIDI parameter is named on screen. */
namespace ParamIdentityLabel
{
    constexpr float kFont      = Sp3ctraTheme::kFontBadge;   ///< 12 px — a 24 px row
    constexpr int   kPastilleD = 14;   ///< compact pastille (rack: 16)
    constexpr int   kGapEvent  = 8;    ///< event → chain
    constexpr int   kGapDot    = 4;    ///< pastille → chain name

    /// The MIDI event — WHITE, never a chain or module hue: chain 1 is amber
    /// like the "mapped" badge, so an amber event would read as a chain.
    constexpr juce::uint32 kColEvent = 0xffeeeeee;
    /// The parameter name and the " · " separators — muted text.
    constexpr juce::uint32 kColParam = 0xff9aa6ba;

    namespace detail
    {
        struct Seg
        {
            juce::String text;
            juce::Font   font { juce::FontOptions(kFont) };
            juce::Colour colour;
            int  width    { 0 };
            int  gapAfter { 0 };
            bool pastille { false };   ///< draws the numbered disc (text = number)
            bool locked   { false };   ///< never dropped, never ellipsized (the event)
            bool sep      { false };   ///< a " · " — dropped with the segment after it
            bool elastic  { false };   ///< may ellipsize (the parameter name)
        };

        inline juce::Font bold()  { return juce::Font(juce::FontOptions(kFont)).boldened(); }
        inline juce::Font plain() { return juce::Font(juce::FontOptions(kFont)); }
        inline int textW(const juce::Font& f, const juce::String& s)
        { return (int) std::ceil(juce::GlyphArrangement::getStringWidth(f, s)); }

        inline std::vector<Seg> build(const ParamIdentity& id, bool withEvent)
        {
            std::vector<Seg> v;
            auto add = [&v](juce::String t, juce::Font f, juce::Colour c, int gap,
                            bool sep = false, bool elastic = false)
            {
                Seg s;
                s.text = std::move(t); s.font = std::move(f); s.colour = c;
                s.width = textW(s.font, s.text); s.gapAfter = gap;
                s.sep = sep; s.elastic = elastic;
                v.push_back(std::move(s));
            };
            const juce::String dot   = juce::String::fromUTF8(" \xC2\xB7 ");
            const juce::Colour sepC  = juce::Colour(kColParam).withAlpha(0.5f);

            if (withEvent && id.eventName.isNotEmpty())
            {
                add(id.eventName, bold(),
                    juce::Colour(id.eventType == MidiMappingEngine::kTypeLfo
                                     ? Sp3ctraTheme::kColMod : kColEvent),
                    kGapEvent);
                v.back().locked = true;   // "CC 20" is never traded for room
            }

            bool needSep = false;
            if (id.chainNumber > 0)
            {
                Seg p;
                p.pastille = true; p.text = juce::String(id.chainNumber);
                p.colour = id.chainColour; p.width = kPastilleD; p.gapAfter = kGapDot;
                v.push_back(p);
                add(id.chainName, bold(), id.chainColour, 0);
                needSep = true;
            }
            if (id.hasModule)
            {
                if (needSep) add(dot, plain(), sepC, 0, true);
                add(id.moduleName, bold(), id.moduleColour, 0);
                needSep = true;
            }
            if (id.paramName.isNotEmpty())
            {
                if (needSep) add(dot, plain(), sepC, 0, true);
                add(id.paramName, plain(), juce::Colour(kColParam), 0, false, true);
            }
            return v;
        }

        inline int total(const std::vector<Seg>& v)
        {
            int w = 0;
            for (const auto& s : v) w += s.width + s.gapAfter;
            return w;
        }
    } // namespace detail

    /** Width the whole label needs (no truncation). */
    inline int width(const ParamIdentity& id, bool withEvent = true)
    { return detail::total(detail::build(id, withEvent)); }

    /** Paint the label inside `area` (vertically centred). `rightAlign` packs
     *  it against the right edge (the collapsed MIDI MAP readout). When the
     *  room is short the parameter name ellipsizes, then goes, then the
     *  module, then the chain — the MIDI EVENT is never dropped nor
     *  ellipsized, it is what identifies the row. `alpha` fades everything. */
    inline void draw(juce::Graphics& g, juce::Rectangle<int> area, const ParamIdentity& id,
                     bool withEvent, bool rightAlign, float alpha = 1.0f)
    {
        auto segs = detail::build(id, withEvent);
        if (segs.empty() || area.isEmpty()) return;

        int total = detail::total(segs);
        // Elastic first: shrink the parameter name down to an ellipsis + a
        // couple of glyphs.
        if (total > area.getWidth())
            for (auto& s : segs)
                if (s.elastic)
                {
                    const int minW = detail::textW(s.font, juce::String::fromUTF8("\xE2\x80\xA6")) + 14;
                    const int nw   = s.width - (total - area.getWidth());
                    if (nw >= minW) { total -= s.width - nw; s.width = nw; }
                    break;
                }
        // Then drop trailing segments (with their separator) until it fits.
        // A LOCKED segment (the MIDI event) is never dropped: a row that has
        // lost its parameter name is still usable, one that has lost "CC 20"
        // no longer says which controller it belongs to.
        while (total > area.getWidth() && segs.size() > 1 && ! segs.back().locked)
        {
            total -= segs.back().width + segs.back().gapAfter;
            segs.pop_back();
            if (! segs.empty() && segs.back().sep)
            {
                total -= segs.back().width + segs.back().gapAfter;
                segs.pop_back();
            }
        }
        // A pastille left trailing by the drops names nothing (a numbered
        // disc with no chain beside it): it goes with them.
        while (segs.size() > 1 && segs.back().pastille)
        {
            total -= segs.back().width + segs.back().gapAfter;
            segs.pop_back();
        }
        // A lone pastille left behind by the drops is meaningless: keep the
        // chain name with it or draw the first segment ellipsized.
        if (segs.size() == 1 && segs.front().pastille)
            return;
        // Last resort: squeeze the leading segment — never a locked one. An
        // event ellipsized to "C…" says less than one running past the edge.
        if (total > area.getWidth() && ! segs.front().locked)
        {
            auto& s = segs.front();
            total -= s.width - area.getWidth();
            s.width = area.getWidth();
            s.elastic = true;
        }

        int x = rightAlign ? area.getRight() - total : area.getX();
        for (const auto& s : segs)
        {
            if (s.pastille)
            {
                const float d = (float) kPastilleD;
                ChainIdentity::drawPastille(g, { (float) x, (float) area.getCentreY() - d * 0.5f, d, d },
                                            s.text.getIntValue(), s.colour, alpha);
            }
            else
            {
                g.setFont(s.font);
                g.setColour(s.colour.withMultipliedAlpha(alpha));
                g.drawText(s.text, x, area.getY(), s.width, area.getHeight(),
                           juce::Justification::centredLeft, s.elastic);
            }
            x += s.width + s.gapAfter;
        }
    }
} // namespace ParamIdentityLabel
