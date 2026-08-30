/**
 * @file DeviceFeedback.cpp
 * @brief VST → CIS feedback (LEDs + OLED overlay) — see header.
 */
#include "DeviceFeedback.h"
#include "../PluginProcessor.h"
#include "../communication/link/Sp3ctraLink.h"
#include "../midi/HidMidiMapper.h"
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

    bool ok = id.startsWith ("image") || id.startsWith ("acqGate") || id.startsWith ("rawFreeze");
    if (! ok)
    {
        const auto t = proc_.navTargetForParam (id);
        if (t.valid)
            ok = (t.type == ModuleType::Sp3ctra) || proc_.instanceChainHostsSp3ctra (t.instanceId);
    }
    eligCache_[index] = { ok, now };
    return ok;
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

    if (mode != lastOverlayMode_) { eligCache_.clear(); lastOverlayMode_ = mode; }

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
                recent_.push_back ({ i, t });
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
        auto* p = params[recent_[k].index];
        if (p == nullptr) continue;
        Sp3ctraLink::OverlayItem it;
        it.label = p->getName (recent_.size() == 1 ? 12 : 14);
        it.value = p->getCurrentValueAsText().substring (0, 10);
        it.highlight = (k == 0);
        if (dynamic_cast<juce::AudioParameterBool*> (p) != nullptr)
            it.norm = -1.0f;                                  // no bar for switches
        else
            it.norm = p->getValue();
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto& r = rp->getNormalisableRange();
            it.bipolar = r.start < 0.0f && r.end > 0.0f;
        }
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
