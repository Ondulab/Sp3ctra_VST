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
                                   const juce::String& paramId)
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
    s.paramId = paramId;
    // Publish exactly ONE active field last (release) — the audio thread reads
    // both and acts on whichever is live.
    if (param != nullptr) s.param  .store(param,   std::memory_order_release);
    else                  s.vtarget.store(vtarget, std::memory_order_release);

    notifyChanged();
    return true;
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
    return (type == 1 ? juce::MidiMessage::getMidiNoteName(number, true, true, 3)
                      : "CC " + juce::String(number))
         + juce::String::fromUTF8(" \xC2\xB7 ch ") + juce::String(channel);
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
            addMapping((int) map.getProperty("type", 0),
                       (int) map.getProperty("ch",   0),
                       (int) map.getProperty("num",  0),
                       map.getProperty("param", "").toString());
            // Unknown param ids (renamed/removed banks) are silently dropped
            // by addMapping — the rest of the table still restores.
        }

    notifyChanged();
}
