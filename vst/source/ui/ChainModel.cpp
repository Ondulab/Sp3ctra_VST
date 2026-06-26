#include "ChainModel.h"

//==============================================================================
const juce::Identifier ChainModel::kChainsTag   { "CHAINS" };
const juce::Identifier ChainModel::kChainTag    { "CHAIN" };
const juce::Identifier ChainModel::kModuleTag   { "MODULE" };
const juce::Identifier ChainModel::kTypeProp    { "type" };
const juce::Identifier ChainModel::kUuidProp    { "uuid" };
const juce::Identifier ChainModel::kVersionProp { "version" };

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

bool ChainModel::canInsert(int chainIdx, ModuleType type, const juce::Uuid* movingId) const
{
    if (chainIdx < 0 || chainIdx >= numChains())
        return false;

    const auto& mods = chains[(size_t) chainIdx].modules;
    const bool wantSource = (moduleRole(type) == ModuleRole::Source);

    for (const auto& m : mods)
    {
        if (movingId != nullptr && m.id == *movingId)
            continue;  // the instance being moved doesn't block itself
        if (m.type == type)
            return false;                         // no duplicate type per chain
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

    auto& mods = chains[(size_t) chainIdx].modules;
    dropIdx = juce::jlimit(0, (int) mods.size(), dropIdx);
    mods.insert(mods.begin() + dropIdx, ModuleInstance{ type, juce::Uuid() });
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
    if (! canInsert(toChain, moved.type))
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
                const ModuleRole r = moduleRole(ch.modules[(size_t) j].type);
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
            ch.modules.push_back(ModuleInstance{
                type, muuid.isNotEmpty() ? juce::Uuid(muuid) : juce::Uuid() });
        }
        chains.push_back(std::move(ch));
    }
}

void ChainModel::validateAndRepair()
{
    for (auto& ch : chains)
    {
        std::set<ModuleType> seenTypes;
        bool                 sawSource = false;
        std::vector<ModuleInstance> kept;
        kept.reserve(ch.modules.size());

        for (auto& m : ch.modules)
        {
            if (seenTypes.count(m.type))
                continue;   // duplicate type → drop
            if (moduleRole(m.type) == ModuleRole::Source)
            {
                if (sawSource)
                    continue;   // second source → drop
                sawSource = true;
            }
            seenTypes.insert(m.type);
            kept.push_back(m);
        }
        ch.modules = std::move(kept);
    }

    if (chains.empty())
        chains.push_back(Chain{ juce::Uuid(), {} });   // always keep ≥1 chain
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
    m.chains.push_back(make({ ModuleType::Sp3ctra, ModuleType::Pitch, ModuleType::Mask,
                              ModuleType::Sampler, ModuleType::Score, ModuleType::LuxStral }));
    m.chains.push_back(make({ ModuleType::Sp3ctra, ModuleType::LuxSynth, ModuleType::LuxWave }));
    return m;
}
