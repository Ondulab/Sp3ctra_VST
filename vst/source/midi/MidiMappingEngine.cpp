#include "MidiMappingEngine.h"

//==============================================================================
// Message thread — mapping edits
//==============================================================================

int MidiMappingEngine::slotIndexFor(const juce::String& paramId) const
{
    // A slot is occupied iff its paramId mirror is non-empty (holds for both
    // APVTS and virtual targets); freed slots clear it.
    for (int i = 0; i < kMaxMappings; ++i)
        if (! slots_[i].paramId.isEmpty() && slots_[i].paramId == paramId)
            return i;
    return -1;
}

bool MidiMappingEngine::addMapping(int type, int channel, int number,
                                   const juce::String& paramId,
                                   float lo, float hi, bool follow,
                                   const MidiMappingCurve* curve,
                                   const MidiMappingEnvelope* env,
                                   float phase)
{
    // Resolve the target: an APVTS parameter, or (failing that) a virtual
    // sampler target via the sink. Unknown ids (stale session entries) drop.
    auto* param = apvts.getParameter(paramId);
    int   vtarget = -1;
    if (param == nullptr)
    {
        if (sink_ != nullptr)
            vtarget = sink_->virtualResolve(paramId);
        if (vtarget < 0)
            return false;
    }

    // Replace an existing mapping of the same parameter in place: deactivate
    // first so the audio thread never matches a half-updated slot.
    int idx = slotIndexFor(paramId);
    if (idx < 0)
        for (int i = 0; i < kMaxMappings && idx < 0; ++i)
            if (slots_[i].paramId.isEmpty())   // free slot (either kind)
                idx = i;
    if (idx < 0)
        return false;   // table full

    auto& s = slots_[idx];
    s.param  .store(nullptr, std::memory_order_release);   // deactivate both
    s.vtarget.store(-1,      std::memory_order_release);
    s.type   .store(type,    std::memory_order_relaxed);
    s.channel.store(channel, std::memory_order_relaxed);
    s.number .store(number,  std::memory_order_relaxed);
    s.lo     .store(juce::jlimit(0.0f, 1.0f, lo), std::memory_order_relaxed);
    s.hi     .store(juce::jlimit(0.0f, 1.0f, hi), std::memory_order_relaxed);
    s.follow .store(follow, std::memory_order_relaxed);
    s.phase  .store(juce::jlimit(0.0f, 1.0f, phase), std::memory_order_relaxed);
    s.curve = (curve != nullptr) ? *curve : MidiMappingCurve{};
    bakeCurve(s);   // window / hysteresis / LUT ready before the slot goes live
    // Envelope: description mirrored, runner loaded and at rest — the slot
    // is inactive here (both targets null), so the audio thread is not in
    // its runner.
    s.envelope = (env != nullptr) ? *env : MidiMappingEnvelope{};
    s.env.reset();
    s.env.load(s.envelope);
    s.lastIn .store(-1.0f, std::memory_order_relaxed);
    s.lastOut.store(-1.0f, std::memory_order_relaxed);
    s.paramId = paramId;
    // Publish exactly ONE active field last (release) — the audio thread reads
    // both and acts on whichever is live.
    if (param != nullptr) s.param  .store(param,   std::memory_order_release);
    else                  s.vtarget.store(vtarget, std::memory_order_release);

    notifyChanged();
    return true;
}

bool MidiMappingEngine::addLfoMapping(int lfoIndex, const juce::String& paramId,
                                      float halfSpan)
{
    if (lfoIndex == kNewLfo)
    {
        if (lfos_ == nullptr) return false;
        lfoIndex = lfos_->lfoAdd();
        if (lfoIndex < 0) return false;   // sixteen in use already
    }
    // Where the parameter stands right now — the window is centred there so
    // arming a modulation never makes the destination jump.
    float centre = 0.5f;
    if (auto* p = apvts.getParameter(paramId))
        centre = p->getValue();
    else if (sink_ != nullptr)
    {
        const int vt = sink_->virtualResolve(paramId);
        if (vt < 0) return false;
        centre = sink_->virtualRead(vt);
    }
    const float h  = juce::jlimit(0.0f, 0.5f, halfSpan);
    const float c  = juce::jlimit(h, 1.0f - h, centre);
    // follow = false: an LFO moves without anyone touching it, and a row that
    // navigates on every move would drag the editor around forever.
    if (! addMapping(kTypeLfo, 0, lfoIndex, paramId, c - h, c + h, false))
        return false;
    if (onLfoMappingAdded) onLfoMappingAdded(paramId);
    return true;
}

bool MidiMappingEngine::assignEvent(int type, int channel, int number,
                                    const juce::String& paramId)
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return addMapping(type, channel, number, paramId);

    // Already mapped: carry the work the user put into this destination over
    // to the new controller (range, law, envelope). Copies first — addMapping
    // rewrites the very slot they live in.
    const auto& s = slots_[idx];
    const bool wasLfo = s.type.load(std::memory_order_relaxed) == kTypeLfo;
    const MidiMappingCurve    curve = s.curve;
    const MidiMappingEnvelope env   = s.envelope;
    const float lo     = wasLfo ? 0.0f : s.lo    .load(std::memory_order_relaxed);
    const float hi     = wasLfo ? 1.0f : s.hi    .load(std::memory_order_relaxed);
    const bool  follow = wasLfo ? true : s.follow.load(std::memory_order_relaxed);
    return addMapping(type, channel, number, paramId, lo, hi, follow, &curve, &env);
}

void MidiMappingEngine::removeMappingFor(const juce::String& paramId)
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return;
    slots_[idx].param  .store(nullptr, std::memory_order_release);
    slots_[idx].vtarget.store(-1,      std::memory_order_release);
    slots_[idx].paramId.clear();
    notifyChanged();
}

void MidiMappingEngine::clearAll()
{
    // Same slot-reset as the restore preamble (restoreFromValueTree): release
    // every mapping, then notify once so badges refresh and the session saves.
    for (auto& s : slots_)
    {
        s.param  .store(nullptr, std::memory_order_release);
        s.vtarget.store(-1,      std::memory_order_release);
        s.paramId.clear();
    }
    notifyChanged();
}

bool MidiMappingEngine::getMappingFor(const juce::String& paramId,
                                      int& type, int& channel, int& number) const
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return false;
    type    = slots_[idx].type   .load(std::memory_order_relaxed);
    channel = slots_[idx].channel.load(std::memory_order_relaxed);
    number  = slots_[idx].number .load(std::memory_order_relaxed);
    return true;
}

juce::String MidiMappingEngine::mappingDescription(const juce::String& paramId) const
{
    int type = 0, channel = 0, number = 0;
    if (! getMappingFor(paramId, type, channel, number))
        return {};
    if (type == kTypeLfo)
        return "LFO " + juce::String(number + 1);
    return (type == kTypeNote ? juce::MidiMessage::getMidiNoteName(number, true, true, 3)
                              : "CC " + juce::String(number))
         + juce::String::fromUTF8(" \xC2\xB7 ch ") + juce::String(channel);
}

std::vector<MidiMappingEngine::MappingInfo> MidiMappingEngine::allMappings() const
{
    std::vector<MappingInfo> out;
    for (const auto& s : slots_)
    {
        if (s.paramId.isEmpty())
            continue;
        MappingInfo m;
        m.paramId = s.paramId;
        m.type    = s.type   .load(std::memory_order_relaxed);
        m.channel = s.channel.load(std::memory_order_relaxed);
        m.number  = s.number .load(std::memory_order_relaxed);
        m.lo      = s.lo     .load(std::memory_order_relaxed);
        m.hi      = s.hi     .load(std::memory_order_relaxed);
        m.phase   = s.phase  .load(std::memory_order_relaxed);
        m.follow  = s.follow .load(std::memory_order_relaxed);
        m.curved  = ! s.curve.isNeutral();
        m.enveloped = ! s.envelope.isOff();
        out.push_back(std::move(m));
    }
    return out;
}

bool MidiMappingEngine::getMappingRange(const juce::String& paramId,
                                        float& lo, float& hi) const
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return false;
    lo = slots_[idx].lo.load(std::memory_order_relaxed);
    hi = slots_[idx].hi.load(std::memory_order_relaxed);
    return true;
}

void MidiMappingEngine::setMappingRange(const juce::String& paramId,
                                        float lo, float hi)
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return;
    slots_[idx].lo.store(juce::jlimit(0.0f, 1.0f, lo), std::memory_order_relaxed);
    slots_[idx].hi.store(juce::jlimit(0.0f, 1.0f, hi), std::memory_order_relaxed);
    // Persist (session autosave) but NO ChangeMessage — see the header doc:
    // a broadcast would rebuild the very MIN/MAX bar being dragged.
    if (onMappingsEdited) onMappingsEdited();
}

float MidiMappingEngine::getMappingPhase(const juce::String& paramId) const
{
    const int idx = slotIndexFor(paramId);
    return idx < 0 ? 0.0f : slots_[idx].phase.load(std::memory_order_relaxed);
}

void MidiMappingEngine::setMappingPhase(const juce::String& paramId, float phase01)
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return;
    slots_[idx].phase.store(juce::jlimit(0.0f, 1.0f, phase01), std::memory_order_relaxed);
    if (onMappingsEdited) onMappingsEdited();   // no broadcast — see the header
}

bool MidiMappingEngine::mappingFollows(const juce::String& paramId) const
{
    const int idx = slotIndexFor(paramId);
    return idx < 0 || slots_[idx].follow.load(std::memory_order_relaxed);
}

void MidiMappingEngine::setMappingFollow(const juce::String& paramId,
                                         bool shouldFollow)
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return;
    slots_[idx].follow.store(shouldFollow, std::memory_order_relaxed);
    if (onMappingsEdited) onMappingsEdited();   // no broadcast — see the header
}

//==============================================================================
// Message thread — transfer law (MIDI CURVE window)
//==============================================================================

bool MidiMappingEngine::getMappingCurve(const juce::String& paramId,
                                        MidiMappingCurve& out) const
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return false;
    out = slots_[idx].curve;
    return true;
}

void MidiMappingEngine::setMappingCurve(const juce::String& paramId,
                                        const MidiMappingCurve& curve)
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return;
    slots_[idx].curve = curve;
    bakeCurve(slots_[idx]);
    if (onMappingsEdited) onMappingsEdited();   // no broadcast — see the header
}

void MidiMappingEngine::bakeCurve(Slot& s)
{
    const auto& c = s.curve;
    const float wl = juce::jlimit(0.0f, 1.0f, c.inLo);
    const float wh = juce::jlimit(wl, 1.0f, c.inHi);
    s.inLo.store(wl, std::memory_order_relaxed);
    s.inHi.store(wh, std::memory_order_relaxed);
    s.hyst.store(juce::jlimit(0.0f, MidiMappingCurve::kMaxHyst, c.hyst),
                 std::memory_order_relaxed);

    if (c.mode == MidiMappingCurve::Mode::Linear)
    {
        s.lutActive.store(-1, std::memory_order_release);   // identity: no LUT read
        return;
    }
    // Bake into the buffer the audio thread is NOT reading, then flip.
    const int cur = s.lutActive.load(std::memory_order_relaxed);
    const int nxt = (cur == 0) ? 1 : 0;
    std::array<float, MidiMappingCurve::kLutN> tmp;
    c.bake(tmp);
    for (int i = 0; i < MidiMappingCurve::kLutN; ++i)
        s.lut[nxt][i].store(tmp[(size_t) i], std::memory_order_relaxed);
    s.lutActive.store(nxt, std::memory_order_release);
}

bool MidiMappingEngine::lastMappedValue(const juce::String& paramId,
                                        float& in01, float& out01) const
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return false;
    in01  = slots_[idx].lastIn .load(std::memory_order_relaxed);
    out01 = slots_[idx].lastOut.load(std::memory_order_relaxed);
    return in01 >= 0.0f;
}

//==============================================================================
// Message thread — envelope (MIDI CURVE window)
//==============================================================================

bool MidiMappingEngine::getMappingEnvelope(const juce::String& paramId,
                                           MidiMappingEnvelope& out) const
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return false;
    out = slots_[idx].envelope;
    return true;
}

void MidiMappingEngine::setMappingEnvelope(const juce::String& paramId,
                                           const MidiMappingEnvelope& env)
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0)
        return;
    slots_[idx].envelope = env;
    slots_[idx].env.load(env);   // atomics only — the runner may be mid-flight
    if (onMappingsEdited) onMappingsEdited();   // no broadcast — see the header
}

bool MidiMappingEngine::envelopeLive(const juce::String& paramId,
                                     MidiMappingEnvelope::Stage& stage,
                                     float& t01, float& level01) const
{
    const int idx = slotIndexFor(paramId);
    if (idx < 0 || slots_[idx].envelope.isOff())
        return false;
    const auto& r = slots_[idx].env;
    stage   = (MidiMappingEnvelope::Stage) r.stageOut.load(std::memory_order_relaxed);
    t01     = r.tnOut   .load(std::memory_order_relaxed);
    level01 = r.levelOut.load(std::memory_order_relaxed);
    return true;
}

//==============================================================================
// Message thread — MIDI learn completion (timer)
//==============================================================================

void MidiMappingEngine::timerCallback()
{
    // Learn cancelled elsewhere (or never armed) with no capture pending.
    if (! isLearning())
    {
        stopTimer();
        return;
    }

    const int result = learnResult_.load(std::memory_order_acquire);
    if (result == kNoResult)
        return;   // still waiting for the first Note/CC

    const int type    = (result >> 16) & 0xFF;
    const int channel = (result >> 8)  & 0xFF;
    const int number  =  result        & 0x7F;

    addMapping(type, channel, number, learnParamId_);   // notifies on success

    learnResult_.store(kNoResult, std::memory_order_relaxed);
    learnParamId_.clear();
    stopTimer();
}

//==============================================================================
// Message thread — persistence
//==============================================================================

juce::ValueTree MidiMappingEngine::toValueTree() const
{
    juce::ValueTree root("MIDI_MAPPINGS");
    for (const auto& s : slots_)
    {
        if (s.paramId.isEmpty())   // free slot (APVTS or virtual)
            continue;
        juce::ValueTree map("MAP");
        map.setProperty("type",  s.type   .load(std::memory_order_relaxed), nullptr);
        map.setProperty("ch",    s.channel.load(std::memory_order_relaxed), nullptr);
        map.setProperty("num",   s.number .load(std::memory_order_relaxed), nullptr);
        map.setProperty("param", s.paramId, nullptr);
        // MIN/MAX range + follow — only when off-default, so pre-range
        // sessions round-trip byte-identical.
        const float lo = s.lo.load(std::memory_order_relaxed);
        const float hi = s.hi.load(std::memory_order_relaxed);
        if (lo != 0.0f) map.setProperty("lo", lo, nullptr);
        if (hi != 1.0f) map.setProperty("hi", hi, nullptr);
        if (const float ph = s.phase.load(std::memory_order_relaxed); ph != 0.0f)
            map.setProperty("ph", ph, nullptr);
        if (! s.follow.load(std::memory_order_relaxed))
            map.setProperty("follow", false, nullptr);
        s.curve.writeTo(map);      // transfer law — off-default attributes only
        s.envelope.writeTo(map);   // envelope — only while armed
        root.appendChild(map, nullptr);
    }
    return root;
}

void MidiMappingEngine::restoreFromValueTree(const juce::ValueTree& tree)
{
    // Clear the current table first — the restored session is authoritative.
    for (auto& s : slots_)
    {
        s.param  .store(nullptr, std::memory_order_release);
        s.vtarget.store(-1,      std::memory_order_release);
        s.paramId.clear();
    }

    if (tree.isValid() && tree.hasType("MIDI_MAPPINGS"))
        for (const auto& map : tree)
        {
            if (! map.hasType("MAP"))
                continue;
            const MidiMappingCurve    curve = MidiMappingCurve::readFrom(map);
            const MidiMappingEnvelope env   = MidiMappingEnvelope::readFrom(map);
            addMapping((int) map.getProperty("type", 0),
                       (int) map.getProperty("ch",   0),
                       (int) map.getProperty("num",  0),
                       map.getProperty("param", "").toString(),
                       (float) (double) map.getProperty("lo", 0.0),
                       (float) (double) map.getProperty("hi", 1.0),
                       (bool) map.getProperty("follow", true),
                       &curve, &env,
                       (float) (double) map.getProperty("ph", 0.0));
            // Unknown param ids (renamed/removed banks) are silently dropped
            // by addMapping — the rest of the table still restores.
        }

    notifyChanged();
}
