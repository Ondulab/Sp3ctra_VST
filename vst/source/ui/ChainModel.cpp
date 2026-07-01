#include "ChainModel.h"
#include "../processing/chain_plan.h"   // CHAIN_MAX_CHAINS / CHAIN_PLAN_MAX_INSERTS

static_assert(ChainModel::kMaxVideoSlots == CHAIN_MAX_CHAINS,
              "ChainModel::kMaxVideoSlots must equal CHAIN_MAX_CHAINS (chain_plan.h)");
static_assert(CHAIN_PLAN_MAX_INSERTS >= 10,
              "VideoScroll probes share insert slots with Pitch/Mask; a single chain "
              "may hold Pitch+Mask + up to 8 probes, so CHAIN_PLAN_MAX_INSERTS must be "
              ">= 10 or deriveAndPublishChainPlan silently drops probes.");

//==============================================================================
const juce::Identifier ChainModel::kChainsTag   { "CHAINS" };
const juce::Identifier ChainModel::kChainTag    { "CHAIN" };
const juce::Identifier ChainModel::kModuleTag   { "MODULE" };
const juce::Identifier ChainModel::kTypeProp    { "type" };
const juce::Identifier ChainModel::kUuidProp    { "uuid" };
const juce::Identifier ChainModel::kVersionProp { "version" };
const juce::Identifier ChainModel::kSlotProp    { "slot" };

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

int ChainModel::firstFreeLuxStralSlot(const juce::Uuid* movingId) const
{
    bool used[kMaxLuxStralEngines] = { false };
    for (const auto& ch : chains)
        for (const auto& m : ch.modules)
        {
            if (! isLuxStralEngine(m.type)) continue;
            if (movingId != nullptr && m.id == *movingId) continue;
            if (m.slot >= 0 && m.slot < kMaxLuxStralEngines) used[m.slot] = true;
        }
    for (int s = 0; s < kMaxLuxStralEngines; ++s)
        if (! used[s]) return s;
    return -1;
}

bool ChainModel::canInsert(int chainIdx, ModuleType type, const juce::Uuid* movingId) const
{
    if (chainIdx < 0 || chainIdx >= numChains())
        return false;

    const ModuleRole role = moduleRole(type);
    const bool wantSource = (role == ModuleRole::Source);

    // VideoScroll is a multi-per-chain slotted type: it's bounded by the 8-slot
    // pool, not the per-chain duplicate rule below.
    if (isSlottedType(type) && firstFreeVideoSlot(movingId) < 0)
        return false;   // pool full (8 used)

    // Sampler is multi-instance too (engines A/B), bounded by its own 2-slot pool.
    if (isSamplerEngine(type) && firstFreeSamplerSlot(movingId) < 0)
        return false;   // both sampler engines (A + B) already placed

    // LuxStral is dual-engine (A/B), bounded by its own 2-slot pool. Unlike the
    // Sampler it stays subject to the per-chain duplicate rule below (1 per chain),
    // so the two engines necessarily live in different chains.
    if (isLuxStralEngine(type) && firstFreeLuxStralSlot(movingId) < 0)
        return false;   // both LuxStral engines (A + B) already placed

    // Engine-backed modules (synths + Score/Sequencer) are singletons: at most one
    // across the whole model. Pitch/Mask (processors), sources, the Sampler and
    // LuxStral (their own pools above) stay multi-instance.
    if ((role == ModuleRole::Synth || role == ModuleRole::Util)
        && ! isSamplerEngine(type) && ! isLuxStralEngine(type))
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

    const int slot = isSlottedType(type)    ? firstFreeVideoSlot()
                   : isSamplerEngine(type)  ? firstFreeSamplerSlot()
                   : isLuxStralEngine(type) ? firstFreeLuxStralSlot()
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

int ChainModel::sourceChannelForSynth(ModuleType synthType, int fallback) const
{
    for (const auto& ch : chains)
    {
        for (int i = 0; i < (int) ch.modules.size(); ++i)
        {
            if (ch.modules[(size_t) i].type != synthType)
                continue;

            // Found the synth — does any image processor / utility sit upstream?
            for (int j = 0; j < i; ++j)
            {
                const ModuleType ut = ch.modules[(size_t) j].type;
                if (isSlottedType(ut))
                    continue;   // VideoScroll is a PASSIVE output tap — pass-through,
                                // it must NOT flip the synth to the MODULATED channel.
                const ModuleRole r = moduleRole(ut);
                if (r == ModuleRole::Processor || r == ModuleRole::Util)
                    return 0;   // MODULATED — processed signal upstream
            }
            return 1;           // LIVE — only a raw source (or nothing) upstream
        }
    }
    return fallback;            // synth not placed anywhere
}

//==============================================================================
// Persistence
//==============================================================================
juce::ValueTree ChainModel::toValueTree() const
{
    juce::ValueTree root(kChainsTag);
    root.setProperty(kVersionProp, 1, nullptr);

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
            ch.modules.push_back(ModuleInstance{
                type, muuid.isNotEmpty() ? juce::Uuid(muuid) : juce::Uuid(), slot });
        }
        chains.push_back(std::move(ch));
    }
}

void ChainModel::validateAndRepair()
{
    std::set<ModuleType> seenSingletons;   // synth/util types already placed (global)
    int videoBudget    = kMaxVideoSlots;    // at most 8 slotted instances model-wide
    int samplerBudget  = kMaxSamplerEngines;// at most 2 sampler engines (A/B) model-wide
    int luxstralBudget = kMaxLuxStralEngines;// at most 2 LuxStral engines (A/B) model-wide

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
            if (isLuxStralEngine(m.type)) // up to 2 engines (A/B); but max 1 PER chain
            {
                if (luxstralBudget <= 0)
                    continue;            // 3rd+ LuxStral across model → drop
                if (seenTypes.count(m.type))
                    continue;            // duplicate in this chain → drop (1 per chain)
                --luxstralBudget;
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
    bool usedLux[kMaxLuxStralEngines] = { false };
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
            else if (isLuxStralEngine(m.type))
            {
                if (m.slot >= 0 && m.slot < kMaxLuxStralEngines && ! usedLux[m.slot])
                    usedLux[m.slot] = true;
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
            else if (isLuxStralEngine(m.type) && m.slot < 0)
            {
                for (int s = 0; s < kMaxLuxStralEngines; ++s)
                    if (! usedLux[s]) { m.slot = s; usedLux[s] = true; break; }
                jassert(m.slot >= 0);   // guaranteed by the luxstralBudget cap above
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
