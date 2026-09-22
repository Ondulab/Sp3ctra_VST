/**
 * @file DeviceFeedback.cpp
 * @brief VST → CIS feedback (LEDs + OLED overlay) — see header.
 */
#include "DeviceFeedback.h"
#include "../PluginProcessor.h"
#include "../communication/link/Sp3ctraLink.h"
#include "../midi/HidMidiMapper.h"
#include "../ui/ParamIdentity.h"                 // ParamNaming (shared with the UI)
#include "../sampler/SamplerMidiTargets.h"
#include "../luxsampler/LuxSampler.h"
#if __has_include("Sp3ctraVersion.h")
 #include "Sp3ctraVersion.h"
#endif

namespace
{
    constexpr int      kMaxItems        = 3;
    constexpr uint32_t kOverlayMinGapMs = 50;     // <= 20 Hz
    constexpr uint32_t kOverlayRefreshMs = 250;   // keep the device TTL alive
    constexpr uint32_t kLedResendMs     = 2000;
    constexpr uint32_t kEligCacheMs     = 1000;
    constexpr uint32_t kStartupMuteMs   = 3000;   // the restore storm is not "editing"
    constexpr uint32_t kGreetingMs      = 1500;   // blink + "SP3CTRA LINK" stay put this long

    uint32_t nowMs() { return (uint32_t) juce::Time::getMillisecondCounter(); }

    bool isPlumbing (const juce::String& id)
    {
        static const char* const prefixes[] = {
            "udpPort", "udpByte", "deviceIpByte", "sensorDpi", "logLevel", "luxstralNumWorkers",
            "midiFollowParam", "sp3ctraOled", "sp3ctraLed", "sp3ctraHid"
        };
        for (auto* p : prefixes) if (id.startsWith (p)) return true;
        return false;
    }

    // (Module codes + bank-prefix stripping: ModuleCatalog::moduleAbbrev and
    // ParamNaming in ui/ParamIdentity.h — the same naming the interface
    // shows, so the OLED and the MIDI MAP agree on what a parameter is called.)

    /** Drop "(...)" groups — secondary info the 14-char label can't afford. */
    juce::String stripParens (juce::String s)
    {
        for (;;)
        {
            const int a = s.indexOfChar ('(');
            const int b = s.indexOfChar (')');
            if (a < 0 || b <= a) break;
            s = (s.substring (0, a) + " " + s.substring (b + 1)).trim();
        }
        return s;
    }

    /** Word abbreviations, applied ONLY when the composed label overflows the
     *  protocol's 14 chars — common short names stay untouched. */
    juce::String shortenWord (const juce::String& u)
    {
        static const std::pair<const char*, const char*> kMap[] = {
            { "POSITION", "POS" },     { "THICKNESS", "THICK" },
            { "COMPRESSION", "COMPR" },{ "SENSITIVITY", "SENS" },
            { "HARMONICS", "HARM" },   { "HARMONIC", "HARM" },
            { "ENVELOPE", "ENV" },     { "DENSITY", "DENS" },
            { "MATERIAL", "MAT" },     { "OCTAVE", "OCT" },
            { "CHANNEL", "CH" },       { "DURATION", "DUR" },
            { "TRANSPORT", "XPORT" },  { "SELECTION", "SEL" },
            { "LATENCY", "LAT" },      { "VISUALIZER", "VIZ" },
            { "ADAPTIVE", "ADAPT" },   { "CONTRAST", "CONTR" },
            { "COHERENCE", "COHER" },  { "RELEASE", "REL" },
            { "SUSTAIN", "SUS" },      { "OSCILLATORS", "OSC" },
            { "BALANCE", "BAL" },      { "OPACITY", "OPAC" },
            { "WINDOW", "WIN" },       { "INVERTED", "INV" },
            { "EQUAL-LOUDNESS", "EQ-LOUD" }, { "ACQUISITION", "ACQ" },
        };
        for (const auto& [w, sh] : kMap)
            if (u == w) return sh;
        return u;
    }
}

//==============================================================================
DeviceFeedback::DeviceFeedback (Sp3ctraAudioProcessor& p) : proc_ (p) {}

void DeviceFeedback::prepare()
{
    const auto& params = proc_.getParameters();
    numParams_ = params.size();
    touchMs_.reset (new std::atomic<uint32_t>[(size_t) juce::jmax (1, numParams_)]);
    ids_.clear();
    ids_.reserve ((size_t) numParams_);
    for (int i = 0; i < numParams_; ++i)
    {
        touchMs_[(size_t) i].store (0, std::memory_order_relaxed);
        auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*> (params[i]);
        ids_.push_back (wid ? wid->getParameterID() : juce::String());
    }
    suppressUntilMs_ = nowMs() + kStartupMuteMs;
}

void DeviceFeedback::noteParamTouched (int paramIndex) noexcept
{
    if (touchMs_ == nullptr || paramIndex < 0 || paramIndex >= numParams_)
        return;
    touchMs_[(size_t) paramIndex].store (nowMs(), std::memory_order_relaxed);
    touchFlag_.store (1, std::memory_order_release);
}

void DeviceFeedback::noteVirtualTouched (int targetId) noexcept
{
    const uint32_t w = vRingW_.fetch_add (1, std::memory_order_acq_rel) % (uint32_t) kVirtualRing;
    vRing_[w].store (((uint64_t) nowMs() << 32) | (uint32_t) targetId, std::memory_order_release);
    touchFlag_.store (1, std::memory_order_release);
}

//==============================================================================
bool DeviceFeedback::eligible (int index, int mode)
{
    const juce::String& id = ids_[(size_t) index];
    if (id.isEmpty() || isPlumbing (id))
        return false;
    if (mode == 2)                         // All
        return true;

    // Chain: the SP3CTRA module's own transport + everything in a chain that
    // hosts an IN SP3CTRA. Resolution is cached (rack topology is slow-moving).
    const uint32_t now = nowMs();
    auto it = eligCache_.find (index);
    if (it != eligCache_.end() && (now - it->second.second) < kEligCacheMs)
        return it->second.first;

    bool ok = id.startsWith ("image") || id.startsWith ("rawFreeze");
    if (! ok)
    {
        const auto t = proc_.navTargetForParam (id);
        if (t.valid)
            ok = (t.type == ModuleType::Sp3ctra) || proc_.instanceChainHostsSp3ctra (t.instanceId);
    }
    eligCache_[index] = { ok, now };
    return ok;
}

bool DeviceFeedback::virtualEligible (int targetId, int mode)
{
    if (mode == 2)                         // All
        return true;

    const uint32_t now = nowMs();
    auto it = vEligCache_.find (targetId);
    if (it != vEligCache_.end() && (now - it->second.second) < kEligCacheMs)
        return it->second.first;

    // Chain: the sampler module instance hosting this engine sits in a chain
    // with an IN SP3CTRA (same resolution as APVTS ids, via the synthetic id).
    const auto id = SamplerMidiTargets::makeId (SamplerMidiTargets::tEngine (targetId),
                                               SamplerMidiTargets::tSlot (targetId),
                                               SamplerMidiTargets::tKind (targetId));
    const auto t = proc_.navTargetForParam (id);
    const bool ok = t.valid && proc_.instanceChainHostsSp3ctra (t.instanceId);
    vEligCache_[targetId] = { ok, now };
    return ok;
}

bool DeviceFeedback::composeVirtualItem (int targetId, Sp3ctraLink::OverlayItem& it)
{
    using namespace SamplerMidiTargets;
    const Kind k = tKind (targetId);
    const int  e = tEngine (targetId);
    const int  s = tSlot (targetId);
    auto* fs = proc_.getSampler (e);
    if (fs == nullptr)
        return false;

    const char* name = nullptr;
    switch (k)
    {
        case Kind::Speed:       name = "SPEED";     break;
        case Kind::LoopMode:    name = "LOOP";      break;
        case Kind::LoopFwd:     name = "LOOP FWD";  break;   // relabelled below from live state
        case Kind::LoopBwd:     name = "LOOP BWD";  break;
        case Kind::LoopRepeat:  name = "LOOP RPT";  break;
        case Kind::Img:         name = "LEVEL";     break;
        case Kind::Floor:       name = "FLOOR";     break;
        case Kind::Resume:      name = "RESUME";    break;
        case Kind::FadeInType:  name = "FIN CURVE"; break;
        case Kind::FadeInPow:   name = "FIN POW";   break;
        case Kind::FadeOutType: name = "FOUT CURVE";break;
        case Kind::FadeOutPow:  name = "FOUT POW";  break;
        case Kind::Overdub:     name = "OVERDUB";   break;
        case Kind::SelEqFreq:   name = "EQ FREQ";   break;
        case Kind::SelEqGain:   name = "EQ GAIN";   break;
        case Kind::SelEqWidth:  name = "EQ WIDTH";  break;
        case Kind::MixMode:     name = "MIX";       break;
        case Kind::CropStart:   name = "CROP IN";   break;
        case Kind::CropEnd:     name = "CROP OUT";  break;
        case Kind::FadeInLen:   name = "FADE IN";   break;
        case Kind::FadeOutLen:  name = "FADE OUT";  break;
        case Kind::Rec:         name = "REC";       break;   // actions: show the
        case Kind::Play:        name = "PLAY";      break;   // resulting state
        case Kind::Save:        name = "SAVE";      break;
        case Kind::Clear:       name = "CLEAR";     break;
        default:                return false;
    }

    // In a chain: "5 S4 SPEED" — inverted chain number + boxed module token
    // (the chain identifies the engine). Otherwise: "SA4 SPEED" — engine
    // letter + 1-based bank. Engine-wide kinds skip the bank ("5 SMP OVERDUB").
    const auto nav   = proc_.navTargetForParam (makeId (e, s, k));
    const int  chain = nav.valid ? proc_.chainIndexForInstance (nav.instanceId) : -1;
    juce::String label;
    if (chain >= 0)
    {
        label = juce::String (chain + 1) + " ";
        if (isEngineWide (k)) label << "SMP";
        else                  label << "S" << juce::String (s + 1);
        it.tagInvert = true;
    }
    else
    {
        label = "S";
        label << juce::String::charToString ((juce::juce_wchar) ('A' + juce::jlimit (0, 7, e)));
        if (! isEngineWide (k))
            label << juce::String (s + 1);
    }
    label << " " << name;

    const auto pct = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; };
    const auto onOff = [] (bool b) { return juce::String (b ? "ON" : "OFF"); };
    static const char* const fadeNames[] = { "LIN", "EXP", "LOG", "S" };
    static const char* const loopNames[] = { "NONE", "FWD", "BWD", "PP", "1x BWD", "1x RT" };
    static const char* const mixNames[]  = { "MIX", "ADD", "DARK" };

    float norm = read (*fs, s, k);
    juce::String value;
    bool bar = true;
    bool f = false, b = false, r = false;

    switch (k)
    {
        case Kind::Speed:       value = juce::String (fs->getSlotSpeed (s), 2) + "x";          break;
        case Kind::Img:         value = pct (norm);                                            break;
        case Kind::Floor:       value = pct (norm);                                            break;
        case Kind::FadeInPow:   value = juce::String (fs->getSlotAttackCurvePower (s), 2);     break;
        case Kind::FadeOutPow:  value = juce::String (fs->getSlotDecayCurvePower (s), 2);      break;
        case Kind::FadeInType:  value = fadeNames[juce::jlimit (0, 3, (int) fs->getSlotAttackCurveType (s))]; bar = false; break;
        case Kind::FadeOutType: value = fadeNames[juce::jlimit (0, 3, (int) fs->getSlotDecayCurveType (s))];  bar = false; break;
        case Kind::LoopMode:    value = loopNames[juce::jlimit (0, 5, (int) fs->getSlotLoopMode (s))];        bar = false; break;
        case Kind::LoopFwd: case Kind::LoopBwd: case Kind::LoopRepeat:
            decomposeLoopMode (fs->getSlotLoopMode (s), f, b, r);
            value = onOff (k == Kind::LoopFwd ? f : k == Kind::LoopBwd ? b : r);
            bar = false;
            break;
        case Kind::Resume:      value = onOff (fs->getSlotResumeMode (s)); bar = false;        break;
        case Kind::Overdub:     value = onOff (fs->getOverdubMode());      bar = false;        break;
        case Kind::MixMode:     value = mixNames[juce::jlimit (0, 2, (int) fs->getSlotMixMode (s))]; bar = false; break;
        case Kind::SelEqFreq: case Kind::SelEqGain: case Kind::SelEqWidth:
        {
            const int which = (k == Kind::SelEqFreq) ? 0 : (k == Kind::SelEqGain) ? 1 : 2;
            norm  = fs->getSlotEqHandleParam (s, fs->getSlotEqSelHandle (s), which);
            value = pct (norm);
            break;
        }
        case Kind::Rec:         value = onOff (fs->getSlotState (s) == SlotState::RECORDING); bar = false; break;
        case Kind::Play:        value = (fs->getSlotState (s) == SlotState::PLAYING) ? "PLAY" : "STOP";
                                bar = false; break;
        case Kind::Save:        value = "SAVED";   bar = false; break;
        case Kind::Clear:       value = "CLEARED"; bar = false; break;
        default:                value = pct (norm);                                            break;
    }

    it.label = label;
    it.value = value;
    it.norm  = bar ? juce::jlimit (0.0f, 1.0f, norm) : -1.0f;
    it.bipolar = false;
    return true;
}

void DeviceFeedback::tick()
{
    auto* link = proc_.getLink();
    if (link == nullptr)
        return;

    const auto st = link->status();
    const uint32_t now = nowMs();
    const bool bound = st.state == Sp3ctraLink::State::Bound;
    const bool rebound = bound && (! wasBound_ || st.generation != lastLinkGen_);
    lastLinkGen_ = st.generation;
    wasBound_ = bound;
    if (! bound)
    {
        overlayActive_ = false;
        lastOverlaySig_.clear();
        for (auto& l : leds_) l = {};
        return;
    }

    if (rebound)
    {
        // The instrument acknowledges the host: a short greeting on the OLED
        // and one blink of the three backlights.
        Sp3ctraLink::OverlayItem hello;
        hello.label = "SP3CTRA LINK";
#ifdef SP3CTRA_VERSION_STRING
        hello.value = juce::String ("v") + SP3CTRA_VERSION_STRING;
#else
        hello.value = "VST";
#endif
        hello.highlight = true;
        link->setOverlay ({ hello }, 2000);
        overlayActive_ = true;
        lastOverlaySendMs_ = now;
        lastOverlaySig_ = "hello";
        for (int i = 0; i < 3; ++i)
        {
            slp_led_cmd blink {};
            blink.brightness_1 = 100; blink.time_1_ms = 120;
            blink.brightness_2 = 0;   blink.time_2_ms = 120;
            blink.blink_count = 2;
            link->setLed (i, blink);
        }
        leds_[0] = leds_[1] = leds_[2] = {};
        // Hold it: the feedback queue is coalesced (last writer wins), so the
        // regular LED / overlay state must not overwrite the greeting on the
        // very next tick.
        greetingUntilMs_ = now + kGreetingMs;
        return;
    }

    if (now < greetingUntilMs_)
        return;   // greeting still on screen

    tickOverlay (*link, now);
    tickLeds (*link, now, false);
}

//==============================================================================
void DeviceFeedback::tickOverlay (Sp3ctraLink& link, uint32_t now)
{
    auto& apvts = proc_.getAPVTS();
    const int mode = (int) apvts.getRawParameterValue ("sp3ctraOledMode")->load();   // 0 Off, 1 Chain, 2 All
    const uint32_t holdMs = (uint32_t) juce::jmax (100.0f, apvts.getRawParameterValue ("sp3ctraOledHoldMs")->load());

    if (mode != lastOverlayMode_) { eligCache_.clear(); vEligCache_.clear(); lastOverlayMode_ = mode; }

    // ── collect fresh touches ────────────────────────────────────────────────
    if (touchFlag_.exchange (0, std::memory_order_acq_rel) != 0 && now >= suppressUntilMs_ && mode != 0
        && ! proc_.isBulkParamApplyActive())
    {
        for (int i = 0; i < numParams_; ++i)
        {
            const uint32_t t = touchMs_[(size_t) i].load (std::memory_order_relaxed);
            if (t == 0 || (now - t) > holdMs) continue;
            bool known = false;
            for (auto& r : recent_) if (r.index == i) { r.touchedMs = juce::jmax (r.touchedMs, t); known = true; }
            if (! known && eligible (i, mode))
                recent_.push_back ({ i, -1, t });
        }

        // Virtual (non-APVTS) sampler targets, stamped into the small ring.
        for (auto& slot : vRing_)
        {
            const uint64_t packed = slot.exchange (0, std::memory_order_acq_rel);
            if (packed == 0) continue;
            const int      vt = (int) (uint32_t) packed;
            const uint32_t t  = (uint32_t) (packed >> 32);
            if ((now - t) > holdMs) continue;
            bool known = false;
            for (auto& r : recent_) if (r.vt == vt && r.index < 0) { r.touchedMs = juce::jmax (r.touchedMs, t); known = true; }
            if (! known && virtualEligible (vt, mode))
                recent_.push_back ({ -1, vt, t });
        }
    }

    // ── expire, order by recency, keep 3 ─────────────────────────────────────
    recent_.erase (std::remove_if (recent_.begin(), recent_.end(),
                                   [&] (const Recent& r) { return (now - r.touchedMs) > holdMs; }),
                   recent_.end());
    std::sort (recent_.begin(), recent_.end(), [] (const Recent& a, const Recent& b) { return a.touchedMs > b.touchedMs; });
    if ((int) recent_.size() > kMaxItems) recent_.resize ((size_t) kMaxItems);

    if (mode == 0 || recent_.empty())
    {
        if (overlayActive_)
        {
            link.clearOverlay();
            overlayActive_ = false;
            lastOverlaySig_.clear();
        }
        return;
    }

    // ── compose ──────────────────────────────────────────────────────────────
    std::vector<Sp3ctraLink::OverlayItem> items;
    juce::String sig;
    const auto& params = proc_.getParameters();
    for (size_t k = 0; k < recent_.size(); ++k)
    {
        Sp3ctraLink::OverlayItem it;
        if (recent_[k].index < 0)
        {
            if (! composeVirtualItem (recent_[k].vt, it))
                continue;
        }
        else
        {
            auto* p = params[recent_[k].index];
            if (p == nullptr) continue;

            // OLED naming: params of a module that sits in a chain show as
            // "3 DC AMOUNT" — chain number inverted, module code boxed
            // (SLP_OVL_TAG_INVERT). Words repeating the module identity and
            // the redundant slot number are dropped; a module-enable param
            // (…Enabled / …Active / …On) shows ENABLED/DISABLED as its value.
            const juce::String full = p->getName (64);
            const juce::String& pid = ids_[(size_t) recent_[k].index];
            const bool isBool = dynamic_cast<juce::AudioParameterBool*> (p) != nullptr;
            const auto nav    = proc_.navTargetForParam (pid);
            const int  chain  = nav.valid ? proc_.chainIndexForInstance (nav.instanceId) : -1;
            const char* ab    = chain >= 0 ? moduleAbbrev (nav.type) : nullptr;

            // The SP3CTRA transport (imageFreezeMode 0=play/1=hold/2=stop) IS
            // the module's power + freeze: show it like every other enable.
            if (pid == "imageFreezeMode" && ab != nullptr)
            {
                const int m = juce::roundToInt (p->getValue() * 2.0f);   // 0..2 int param, normalised
                it.label = juce::String (chain + 1) + " " + ab;
                it.tagInvert = true;
                it.value = m == 0 ? "ENABLED" : m == 1 ? "FROZEN" : "DISABLED";
                it.norm  = -1.0f;
                it.highlight = (k == 0);
                sig << it.label << '|' << it.value << "|-1.000|" << (it.highlight ? 'H' : '-') << ';';
                items.push_back (it);
                continue;
            }

            // Shared naming (ui/ParamIdentity.h): the bank tag, the words
            // repeating the module and the slot number go — the chain tag +
            // module code already say all that, as the identity label does
            // on screen.
            juce::StringArray tok = juce::StringArray::fromTokens (
                stripParens (ParamNaming::bareName (full, ab != nullptr ? &nav.type : nullptr)),
                " ", "");
            bool enableLike = false;
            if (isBool)
            {
                while (tok.size() > 0 && tok[tok.size() - 1].containsOnly ("0123456789"))
                    tok.remove (tok.size() - 1);
                const juce::String last = tok.size() > 0 ? tok[tok.size() - 1].toUpperCase() : juce::String();
                if (last == "ON" || last == "ENABLED" || last == "ENABLE" || last == "ACTIVE")
                {
                    enableLike = true;
                    tok.remove (tok.size() - 1);
                }
            }

            if (ab != nullptr)
            {
                juce::String rest = tok.joinIntoString (" ").toUpperCase();
                const juce::String head = juce::String (chain + 1) + " " + ab;
                if (head.length() + 1 + rest.length() > 14)
                {
                    // Overflow: abbreviate the long words (POSITION → POS…).
                    juce::StringArray sh;
                    for (const auto& t : tok)
                        sh.add (shortenWord (t.toUpperCase()));
                    rest = sh.joinIntoString (" ");
                }
                it.label = head + (rest.isEmpty() ? juce::String() : " " + rest);
                it.tagInvert = true;
            }
            else
                it.label = full.substring (0, 14);

            if (enableLike)
                it.value = p->getValue() >= 0.5f ? "ENABLED" : "DISABLED";
            else
            {
                // Value field is 10 chars: prefer a parenthesised short form
                // ("Foo (BAR)" → "BAR") over blunt truncation.
                juce::String v = p->getCurrentValueAsText();
                if      (v == "Fundamental")       v = "Fund.";
                else if (v == "Inverted Waveform") v = "Inv.Wave";
                else if (v == "LuxSynth/LuxWave")  v = "LX/LW";
                if (v.length() > 10)
                {
                    const juce::String inner =
                        v.fromFirstOccurrenceOf ("(", false, false)
                         .upToFirstOccurrenceOf (")", false, false).trim();
                    if (inner.isNotEmpty() && inner.length() <= 10)
                        v = inner;
                }
                it.value = v.substring (0, 10);
            }
            if (isBool)
                it.norm = -1.0f;                              // no bar for switches
            else
                it.norm = p->getValue();
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            {
                const auto& r = rp->getNormalisableRange();
                it.bipolar = r.start < 0.0f && r.end > 0.0f;
            }
        }
        it.highlight = (k == 0);
        sig << it.label << '|' << it.value << '|' << juce::String (it.norm, 3) << '|' << (it.highlight ? 'H' : '-') << ';';
        items.push_back (it);
    }

    const bool changed = (sig != lastOverlaySig_);
    const bool refresh = (now - lastOverlaySendMs_) >= kOverlayRefreshMs;
    if ((changed && (now - lastOverlaySendMs_) >= kOverlayMinGapMs) || refresh)
    {
        link.setOverlay (items, (int) holdMs + 200);
        lastOverlaySig_ = sig;
        lastOverlaySendMs_ = now;
        overlayActive_ = true;
    }
}

//==============================================================================
void DeviceFeedback::tickLeds (Sp3ctraLink& link, uint32_t now, bool resendAll)
{
    auto& apvts = proc_.getAPVTS();
    auto& mapper = proc_.getHidMapper();
    auto& mm = proc_.getMidiMap();

    for (int i = 0; i < 3; ++i)
    {
        auto& L = leds_[i];
        const int mode = (int) apvts.getRawParameterValue ("sp3ctraLed" + juce::String (i + 1) + "Mode")->load();
        int brightness = 0;
        uint8_t flags = 0;

        switch (mode)
        {
            case 0:   // Off: dark, and the device must not light it on press either
                brightness = 0; flags = SLP_LED_NO_LOCAL_PRESS; break;
            case 1:   // Press: device-local behaviour
                brightness = 0; flags = 0; break;
            case 2:   // Follow the learnt parameter
            {
                flags = SLP_LED_NO_LOCAL_PRESS;
                int type = 0, ch = 0, num = 0;
                juce::String target;
                if (mapper.currentEvent (i, type, ch, num))
                    target = mm.paramForEvent (type, ch, num);
                float norm = 0.0f; bool twoState = false, found = false;
                if (target.isNotEmpty())
                {
                    if (auto* p = apvts.getParameter (target))
                    {
                        norm = p->getValue(); found = true;
                        twoState = (dynamic_cast<juce::AudioParameterBool*> (p) != nullptr) || p->getNumSteps() == 2;
                    }
                    else
                    {
                        const int vt = proc_.virtualResolve (target);
                        if (vt >= 0) { norm = proc_.virtualRead (vt); found = true; twoState = proc_.virtualSteps (vt) == 2; }
                    }
                }
                brightness = ! found ? 0 : twoState ? (norm > 0.5f ? 100 : 0) : juce::roundToInt (norm * 100.0f);
                break;
            }
            default:  // Manual
                flags = SLP_LED_NO_LOCAL_PRESS;
                brightness = juce::roundToInt (apvts.getRawParameterValue ("sp3ctraLed" + juce::String (i + 1) + "Level")->load() * 100.0f);
                break;
        }

        const bool changed = mode != L.mode || brightness != L.brightness || flags != L.flags;
        const bool periodic = (mode != 1) && (now - L.sentMs) >= kLedResendMs;
        if (resendAll || changed || periodic)
        {
            slp_led_cmd cmd {};
            cmd.brightness_1 = (uint8_t) juce::jlimit (0, 100, brightness);
            cmd.time_1_ms = 0;          // hold
            cmd.flags = flags;
            link.setLed (i, cmd);
            L.mode = mode; L.brightness = brightness; L.flags = flags; L.sentMs = now;
        }
    }
}
