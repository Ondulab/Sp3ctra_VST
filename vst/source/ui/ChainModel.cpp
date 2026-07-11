#include "ChainModel.h"
#include "../processing/chain_plan.h"   // CHAIN_MAX_CHAINS / CHAIN_PLAN_MAX_INSERTS

static_assert(ChainModel::kMaxVideoSlots == CHAIN_MAX_CHAINS,
              "ChainModel::kMaxVideoSlots must equal CHAIN_MAX_CHAINS (chain_plan.h)");
static_assert(ChainModel::kMaxChains == CHAIN_MAX_CHAINS,
              "ChainModel::kMaxChains must equal CHAIN_MAX_CHAINS (chain_plan.h) — "
              "every per-chain RT pool is sized with it.");
static_assert(CHAIN_PLAN_MAX_INSERTS >= 16,
              "The insert list holds Pitch + Mask + Reverb + Echo + EQ + up to 8 "
              "VideoScroll probes AND up to 2 Sampler position markers AND 1 Score "
              "position marker in a single chain = 16 entries, so "
              "CHAIN_PLAN_MAX_INSERTS must be >= 16 or deriveAndPublishChainPlan "
              "silently drops probes/markers (a dropped marker misroutes every "
              "probe/FX placed after it).");

//==============================================================================
const juce::Identifier ChainModel::kChainsTag   { "CHAINS" };
const juce::Identifier ChainModel::kChainTag    { "CHAIN" };
const juce::Identifier ChainModel::kModuleTag   { "MODULE" };
const juce::Identifier ChainModel::kTypeProp    { "type" };
const juce::Identifier ChainModel::kUuidProp    { "uuid" };
const juce::Identifier ChainModel::kVersionProp { "version" };
const juce::Identifier ChainModel::kSlotProp    { "slot" };
const juce::Identifier ChainModel::kValuesTag   { "VALUES" };

//==============================================================================
// Queries
//==============================================================================
bool ChainModel::chainHasRole(int chainIdx, ModuleRole role) const
{
    if (chainIdx < 0 || chainIdx >= numChains())
        return false;
    for (const auto& m : chains[(size_t) chainIdx].modules)
        if (moduleRole(m.type) == role)
            return true;
    return false;
}

bool ChainModel::chainHasType(int chainIdx, ModuleType type) const
{
    if (chainIdx < 0 || chainIdx >= numChains())
        return false;
    for (const auto& m : chains[(size_t) chainIdx].modules)
        if (m.type == type)
            return true;
    return false;
}

int ChainModel::videoSlotCount(const juce::Uuid* exclude) const
{
    int n = 0;
    for (const auto& ch : chains)
        for (const auto& m : ch.modules)
        {
            if (! isSlottedType(m.type)) continue;
            if (exclude != nullptr && m.id == *exclude) continue;
            ++n;
        }
    return n;
}

int ChainModel::firstFreeVideoSlot(const juce::Uuid* movingId) const
{
    bool used[kMaxVideoSlots] = { false };
    for (const auto& ch : chains)
        for (const auto& m : ch.modules)
        {
            if (! isSlottedType(m.type)) continue;
            if (movingId != nullptr && m.id == *movingId) continue;
            if (m.slot >= 0 && m.slot < kMaxVideoSlots) used[m.slot] = true;
        }
    for (int s = 0; s < kMaxVideoSlots; ++s)
        if (! used[s]) return s;
    return -1;
}

int ChainModel::firstFreeSamplerSlot(const juce::Uuid* movingId) const
{
    bool used[kMaxSamplerEngines] = { false };
    for (const auto& ch : chains)
        for (const auto& m : ch.modules)
        {
            if (! isSamplerEngine(m.type)) continue;
            if (movingId != nullptr && m.id == *movingId) continue;
            if (m.slot >= 0 && m.slot < kMaxSamplerEngines) used[m.slot] = true;
        }
    for (int s = 0; s < kMaxSamplerEngines; ++s)
        if (! used[s]) return s;
    return -1;
}

int ChainModel::firstFreeEngineSendSlot(ModuleType type,
                                        const juce::Uuid* movingId) const
{
    bool used[kMaxEngineSends] = { false };
    for (const auto& ch : chains)
        for (const auto& m : ch.modules)
        {
            if (m.type != type) continue;
            if (movingId != nullptr && m.id == *movingId) continue;
            if (m.slot >= 0 && m.slot < kMaxEngineSends) used[m.slot] = true;
        }
    for (int s = 0; s < kMaxEngineSends; ++s)
        if (! used[s]) return s;
    return -1;
}

// Global (model-wide) placement limits — the per-chain rules live in canInsert.
bool ChainModel::canInsertIntoNewChain(ModuleType type, const juce::Uuid* movingId) const
{
    // VideoScroll is a multi-per-chain slotted type: it's bounded by the 8-slot
    // pool, not the per-chain duplicate rule.
    if (isSlottedType(type) && firstFreeVideoSlot(movingId) < 0)
        return false;   // pool full (8 used)

    // Sampler is multi-instance too (engines A/B), bounded by its own 2-slot pool.
    if (isSamplerEngine(type) && firstFreeSamplerSlot(movingId) < 0)
        return false;   // both sampler engines (A + B) already placed

    // Engine sends (→ LUXSTRAL / → LUXSYNTH / → LUXWAVE, M6) are bounded by
    // their per-type 8-slot pools. They stay subject to the per-chain
    // duplicate rule (1 per type per chain, D5), so N sends of one type
    // necessarily live in N different chains.
    if (isEngineSend(type) && firstFreeEngineSendSlot(type, movingId) < 0)
        return false;   // this type's 8 send slots are all placed

    // Engine-backed UTIL modules (Score/Sequencer/Timbre) are singletons: at
    // most one across the whole model. Pitch/Mask (processors), the SP3CTRA
    // source, the Sampler and the engine sends (their own pools above) stay
    // multi-instance.
    // M9: the internal media sources (IMAGE/VIDEO/CAMERA) are engine singletons
    // too — each engine holds ONE media/transport, so at most one instance each.
    const ModuleRole role = moduleRole(type);
    const bool mediaSource = (type == ModuleType::Image
                           || type == ModuleType::Video
                           || type == ModuleType::Camera);
    if (mediaSource
        || ((role == ModuleRole::Synth || role == ModuleRole::Util)
            && ! isSamplerEngine(type) && ! isEngineSend(type)))
    {
        for (const auto& ch : chains)
            for (const auto& m : ch.modules)
            {
                if (movingId != nullptr && m.id == *movingId)
                    continue;
                if (m.type == type)
                    return false;                 // already placed in some chain
            }
    }
    return true;
}

bool ChainModel::canInsert(int chainIdx, ModuleType type, const juce::Uuid* movingId) const
{
    if (chainIdx < 0 || chainIdx >= numChains())
        return false;

    if (! canInsertIntoNewChain(type, movingId))   // global limits first
        return false;

    const bool wantSource = (moduleRole(type) == ModuleRole::Source);

    const auto& mods = chains[(size_t) chainIdx].modules;
    for (const auto& m : mods)
    {
        if (movingId != nullptr && m.id == *movingId)
            continue;  // the instance being moved doesn't block itself
        if (m.type == type && ! isSlottedType(type) && ! isSamplerEngine(type))
            return false;             // duplicate type per chain (VideoScroll/Sampler may repeat)
        if (wantSource && moduleRole(m.type) == ModuleRole::Source)
            return false;                         // at most one source per chain
    }
    return true;
}

//==============================================================================
// Mutations
//==============================================================================
bool ChainModel::insert(int chainIdx, ModuleType type, int dropIdx)
{
    if (! canInsert(chainIdx, type))
        return false;

    const int slot = isSlottedType(type)   ? firstFreeVideoSlot()
                   : isSamplerEngine(type) ? firstFreeSamplerSlot()
                   : isEngineSend(type)    ? firstFreeEngineSendSlot(type)
                   : -1;
    jassert(! hasSlot(type) || slot >= 0);   // canInsert already gated pool-full
    auto& mods = chains[(size_t) chainIdx].modules;
    dropIdx = juce::jlimit(0, (int) mods.size(), dropIdx);
    mods.insert(mods.begin() + dropIdx, ModuleInstance{ type, juce::Uuid(), slot });
    return true;
}

bool ChainModel::moveWithin(int chainIdx, int from, int to)
{
    if (chainIdx < 0 || chainIdx >= numChains())
        return false;
    auto& mods = chains[(size_t) chainIdx].modules;
    const int n = (int) mods.size();
    if (from < 0 || from >= n)
        return false;
    to = juce::jlimit(0, n - 1, to);
    if (from == to)
        return true;

    ModuleInstance moved = mods[(size_t) from];
    mods.erase(mods.begin() + from);
    mods.insert(mods.begin() + to, moved);
    return true;
}

bool ChainModel::moveAcross(int fromChain, int from, int toChain, int dropIdx)
{
    if (fromChain < 0 || fromChain >= numChains()
        || toChain < 0 || toChain >= numChains())
        return false;
    if (fromChain == toChain)
        return moveWithin(fromChain, from, dropIdx);

    auto& src = chains[(size_t) fromChain].modules;
    if (from < 0 || from >= (int) src.size())
        return false;

    const ModuleInstance moved = src[(size_t) from];
    if (! canInsert(toChain, moved.type, &moved.id))   // moved keeps its slot
        return false;

    src.erase(src.begin() + from);
    auto& dst = chains[(size_t) toChain].modules;
    dropIdx = juce::jlimit(0, (int) dst.size(), dropIdx);
    dst.insert(dst.begin() + dropIdx, moved);
    return true;
}

bool ChainModel::remove(int chainIdx, int idx)
{
    if (chainIdx < 0 || chainIdx >= numChains())
        return false;
    auto& mods = chains[(size_t) chainIdx].modules;
    if (idx < 0 || idx >= (int) mods.size())
        return false;
    mods.erase(mods.begin() + idx);
    return true;
}

int ChainModel::addChain()
{
    if (! canAddChain())
        return -1;   // at kMaxChains — a 9th chain would have no RT pool slot
    chains.push_back(Chain{ juce::Uuid(), {} });
    return (int) chains.size() - 1;
}

bool ChainModel::removeChain(int chainIdx)
{
    if (chainIdx < 0 || chainIdx >= numChains())
        return false;
    if (numChains() <= 1)
        return false;   // always keep at least one chain
    chains.erase(chains.begin() + chainIdx);
    return true;
}

//==============================================================================
// Lookups
//==============================================================================
const ModuleInstance* ChainModel::find(const juce::Uuid& id, int& outChain, int& outIdx) const
{
    for (int c = 0; c < numChains(); ++c)
    {
        const auto& mods = chains[(size_t) c].modules;
        for (int i = 0; i < (int) mods.size(); ++i)
            if (mods[(size_t) i].id == id)
            {
                outChain = c;
                outIdx   = i;
                return &mods[(size_t) i];
            }
    }
    outChain = outIdx = -1;
    return nullptr;
}

//==============================================================================
// Phase-1 audio bridge helpers
//==============================================================================
void ChainModel::deriveActiveTypes(std::set<ModuleType>& out) const
{
    out.clear();
    for (const auto& ch : chains)
        for (const auto& m : ch.modules)
            out.insert(m.type);
}

bool ChainModel::isMaskBeforePitch() const
{
    for (const auto& ch : chains)
    {
        int pitchAt = -1, maskAt = -1;
        for (int i = 0; i < (int) ch.modules.size(); ++i)
        {
            if (ch.modules[(size_t) i].type == ModuleType::Pitch) pitchAt = i;
            if (ch.modules[(size_t) i].type == ModuleType::Mask)  maskAt  = i;
        }
        if (pitchAt >= 0 && maskAt >= 0)
            return maskAt < pitchAt;   // first chain holding both decides
    }
    return false;   // default: Pitch first (legacy)
}

/* (M8: sourceChannelForSynth removed — the ChainPlan recipes are the single
 * routing authority.) */

//==============================================================================
// Persistence
//==============================================================================
juce::ValueTree ChainModel::toValueTree() const
{
    juce::ValueTree root(kChainsTag);
    root.setProperty(kVersionProp, kSchemaVersion, nullptr);

    for (const auto& ch : chains)
    {
        juce::ValueTree ct(kChainTag);
        ct.setProperty(kUuidProp, ch.id.toString(), nullptr);
        for (const auto& m : ch.modules)
        {
            juce::ValueTree mt(kModuleTag);
            mt.setProperty(kTypeProp, juce::String(moduleTypeId(m.type)), nullptr);
            mt.setProperty(kUuidProp, m.id.toString(), nullptr);
            if (hasSlot(m.type) && m.slot >= 0)
                mt.setProperty(kSlotProp, m.slot, nullptr);
            if (m.values.isValid())
                mt.appendChild(m.values.createCopy(), nullptr);   // J2 — settings at rest
            ct.appendChild(mt, nullptr);
        }
        root.appendChild(ct, nullptr);
    }
    return root;
}

void ChainModel::fromValueTree(const juce::ValueTree& root)
{
    chains.clear();
    if (! root.hasType(kChainsTag))
        return;

    for (const auto& ct : root)
    {
        if (! ct.hasType(kChainTag))
            continue;

        Chain ch;
        const juce::String cuuid = ct.getProperty(kUuidProp).toString();
        ch.id = cuuid.isNotEmpty() ? juce::Uuid(cuuid) : juce::Uuid();

        for (const auto& mt : ct)
        {
            if (! mt.hasType(kModuleTag))
                continue;
            ModuleType type;
            if (! moduleTypeFromId(mt.getProperty(kTypeProp).toString(), type))
                continue;   // unknown type (newer session) → drop, don't crash
            const juce::String muuid = mt.getProperty(kUuidProp).toString();
            const int slot = hasSlot(type)
                               ? (int) mt.getProperty(kSlotProp, juce::var(-1)) : -1;
            ModuleInstance mi{
                type, muuid.isNotEmpty() ? juce::Uuid(muuid) : juce::Uuid(), slot };
            const auto values = mt.getChildWithName(kValuesTag);
            if (values.isValid())
                mi.values = values.createCopy();   // J2 — settings at rest
            ch.modules.push_back(std::move(mi));
        }
        chains.push_back(std::move(ch));
    }
}

void ChainModel::validateAndRepair()
{
    // Hard cap FIRST: every RT pool (Pitch/Mask instances, chain masks, plan
    // slots) is sized for kMaxChains; extra chains from a corrupt/hand-edited
    // session are dropped — same philosophy as dropping unknown module types.
    if (numChains() > kMaxChains)
        chains.resize((size_t) kMaxChains);

    // UUIDs are identity: chain ids key the stable Pitch/Mask pool-slot binding,
    // module ids key selection & drag moves. A duplicate (hand-edited session)
    // would silently alias two entities — regenerate the later one.
    {
        std::set<juce::Uuid> seenIds;
        for (auto& ch : chains)
        {
            if (! seenIds.insert(ch.id).second)
                ch.id = juce::Uuid();
            for (auto& m : ch.modules)
                if (! seenIds.insert(m.id).second)
                    m.id = juce::Uuid();
        }
    }

    std::set<ModuleType> seenSingletons;   // synth/util types already placed (global)
    int videoBudget    = kMaxVideoSlots;    // at most 8 slotted instances model-wide
    int samplerBudget  = kMaxSamplerEngines;// at most 2 sampler engines (A/B) model-wide
    // M6 — engine sends: up to 8 per TYPE model-wide (independent pools).
    auto sendIdx = [](ModuleType t) noexcept {
        return t == ModuleType::LuxStral ? 0 : t == ModuleType::LuxSynth ? 1 : 2;
    };
    int sendBudget[3] = { kMaxEngineSends, kMaxEngineSends, kMaxEngineSends };

    for (auto& ch : chains)
    {
        std::set<ModuleType> seenTypes;
        bool                 sawSource = false;
        std::vector<ModuleInstance> kept;
        kept.reserve(ch.modules.size());

        for (auto& m : ch.modules)
        {
            if (isSlottedType(m.type))   // exempt from the per-chain duplicate rule
            {
                if (videoBudget <= 0)
                    continue;            // 9th+ slotted instance across model → drop
                --videoBudget;
                kept.push_back(m);
                continue;
            }
            if (isSamplerEngine(m.type)) // up to 2 engines (A/B); may repeat in a chain
            {
                if (samplerBudget <= 0)
                    continue;            // 3rd+ sampler across model → drop
                --samplerBudget;
                kept.push_back(m);
                continue;
            }
            if (isEngineSend(m.type)) // up to 8 sends per type; max 1 PER chain (D5)
            {
                if (sendBudget[sendIdx(m.type)] <= 0)
                    continue;            // 9th+ send of this type across model → drop
                if (seenTypes.count(m.type))
                    continue;            // duplicate in this chain → drop (1 per chain)
                --sendBudget[sendIdx(m.type)];
                seenTypes.insert(m.type);
                kept.push_back(m);
                continue;
            }
            if (seenTypes.count(m.type))
                continue;   // duplicate type → drop
            const ModuleRole role = moduleRole(m.type);
            if (role == ModuleRole::Source)
            {
                if (sawSource)
                    continue;   // second source → drop
                sawSource = true;
            }
            if (role == ModuleRole::Synth || role == ModuleRole::Util)
            {
                if (seenSingletons.count(m.type))
                    continue;   // engine-backed type already placed elsewhere → drop
                seenSingletons.insert(m.type);
            }
            seenTypes.insert(m.type);
            kept.push_back(m);
        }
        ch.modules = std::move(kept);
    }

    if (chains.empty())
        chains.push_back(Chain{ juce::Uuid(), {} });   // always keep ≥1 chain

    // Heal slots: PRESERVE valid unique slots (automation-lane stability); only
    // reassign -1 / out-of-range / colliding ones. Two INDEPENDENT pools:
    // VideoScroll (kMaxVideoSlots) and Sampler engines (kMaxSamplerEngines).
    // Non-slotted types forced to -1.
    bool usedVid[kMaxVideoSlots]     = { false };
    bool usedSmp[kMaxSamplerEngines] = { false };
    bool usedSend[3][kMaxEngineSends] = {{ false }};
    for (auto& ch : chains)
        for (auto& m : ch.modules)
        {
            if (isSlottedType(m.type))
            {
                if (m.slot >= 0 && m.slot < kMaxVideoSlots && ! usedVid[m.slot])
                    usedVid[m.slot] = true;
                else
                    m.slot = -1;
            }
            else if (isSamplerEngine(m.type))
            {
                if (m.slot >= 0 && m.slot < kMaxSamplerEngines && ! usedSmp[m.slot])
                    usedSmp[m.slot] = true;
                else
                    m.slot = -1;
            }
            else if (isEngineSend(m.type))
            {
                bool* used = usedSend[sendIdx(m.type)];
                if (m.slot >= 0 && m.slot < kMaxEngineSends && ! used[m.slot])
                    used[m.slot] = true;
                else
                    m.slot = -1;
            }
            else
                m.slot = -1;
        }
    for (auto& ch : chains)
        for (auto& m : ch.modules)
        {
            if (isSlottedType(m.type) && m.slot < 0)
            {
                for (int s = 0; s < kMaxVideoSlots; ++s)
                    if (! usedVid[s]) { m.slot = s; usedVid[s] = true; break; }
                jassert(m.slot >= 0);   // guaranteed by the videoBudget cap above
            }
            else if (isSamplerEngine(m.type) && m.slot < 0)
            {
                for (int s = 0; s < kMaxSamplerEngines; ++s)
                    if (! usedSmp[s]) { m.slot = s; usedSmp[s] = true; break; }
                jassert(m.slot >= 0);   // guaranteed by the samplerBudget cap above
            }
            else if (isEngineSend(m.type) && m.slot < 0)
            {
                bool* used = usedSend[sendIdx(m.type)];
                for (int s = 0; s < kMaxEngineSends; ++s)
                    if (! used[s]) { m.slot = s; used[s] = true; break; }
                jassert(m.slot >= 0);   // guaranteed by the sendBudget cap above
            }
        }
}

//==============================================================================
// Default topology (== the legacy fixed rack)
//==============================================================================
ChainModel ChainModel::makeDefault()
{
    auto make = [](std::initializer_list<ModuleType> types)
    {
        Chain ch;
        ch.id = juce::Uuid();
        for (auto t : types)
            ch.modules.push_back(ModuleInstance{ t, juce::Uuid() });
        return ch;
    };

    ChainModel m;
    // VideoScroll sits right BEFORE the synth (LuxStral) so deriveAndPublishChainPlan's
    // upstream-only fill() captures it. Placing it after the synth would never capture.
    m.chains.push_back(make({ ModuleType::Sp3ctra, ModuleType::Pitch, ModuleType::Mask,
                              ModuleType::Sampler, ModuleType::Score, ModuleType::Sequencer,
                              ModuleType::VideoScroll, ModuleType::LuxStral }));
    m.chains.push_back(make({ ModuleType::Sp3ctra, ModuleType::LuxSynth, ModuleType::LuxWave }));
    m.validateAndRepair();   // assigns slot 0 to the lone VideoScroll, normalises the rest
    return m;
}
