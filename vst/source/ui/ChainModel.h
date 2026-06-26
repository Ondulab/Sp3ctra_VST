/**
 * @file ChainModel.h
 * @brief Editable, persisted model of the chain rack (M6).
 *
 * Replaces the former FIXED two-chain topology (9 hardcoded BlockComponents).
 * A ChainModel is N chains; each chain is an ordered list of module instances.
 * Order matters (Pitch→Mask ≠ Mask→Pitch). Placement rules (canInsert):
 *   • at most one Source-role module per chain (a source is optional),
 *   • never two instances of the same ModuleType in the same chain.
 *
 * In Phase 1 the editor owns the model; its only audio effect is to project
 * module presence onto the existing APVTS enable params + chainInsertOrder
 * (see Sp3ctraAudioProcessorEditor::applyModelToParams). The richer per-chain
 * routing is Phase 2 — the data model is already shaped for it.
 *
 * Persistence: toValueTree()/fromValueTree() ride on apvts.state, so the model
 * round-trips through the processor's existing getState/setState path.
 */
#pragma once

#include "ModuleCatalog.h"
#include <juce_data_structures/juce_data_structures.h>
#include <vector>
#include <set>

//==============================================================================
struct ModuleInstance
{
    ModuleType  type;
    juce::Uuid  id;   ///< stable identity (survives reordering; Phase 2: per-instance params)
};

struct Chain
{
    juce::Uuid                  id;
    std::vector<ModuleInstance> modules;   ///< order is significant
};

//==============================================================================
class ChainModel
{
public:
    std::vector<Chain> chains;

    //── Queries ───────────────────────────────────────────────────────────────
    bool chainHasRole(int chainIdx, ModuleRole role) const;
    bool chainHasType(int chainIdx, ModuleType type) const;

    /** True if `type` may be inserted into chain `chainIdx`. When `movingId` is
     *  non-null, that instance is excluded from the duplicate/role counts (used
     *  for in-rack reordering / cross-chain moves of an existing block). */
    bool canInsert(int chainIdx, ModuleType type, const juce::Uuid* movingId = nullptr) const;

    //── Mutations (return false when the rule check fails) ─────────────────────
    bool insert(int chainIdx, ModuleType type, int dropIdx);
    bool moveWithin(int chainIdx, int from, int to);
    bool moveAcross(int fromChain, int from, int toChain, int dropIdx);
    bool remove(int chainIdx, int idx);

    int  addChain();                 ///< appends an empty chain, returns its index
    bool removeChain(int chainIdx);  ///< refuses to delete the last chain

    //── Lookups ───────────────────────────────────────────────────────────────
    int  numChains() const noexcept { return (int) chains.size(); }
    const ModuleInstance* find(const juce::Uuid& id, int& outChain, int& outIdx) const;

    //── Phase-1 audio bridge helpers ──────────────────────────────────────────
    /** Every ModuleType present in at least one chain. */
    void deriveActiveTypes(std::set<ModuleType>& out) const;
    /** Global Pitch/Mask relation for the binary chainInsertOrder param: true if
     *  the first chain containing both has Mask before Pitch. */
    bool isMaskBeforePitch() const;

    /** Phase-2 source routing: the channel a synth should read given its chain
     *  placement — 0 = MODULATED (an image Processor/Util sits upstream of it in
     *  its chain), 1 = LIVE (raw source upstream, or none). Returns `fallback`
     *  when the synth isn't placed in any chain. */
    int sourceChannelForSynth(ModuleType synthType, int fallback) const;

    //── Persistence ───────────────────────────────────────────────────────────
    juce::ValueTree toValueTree() const;
    void            fromValueTree(const juce::ValueTree&);

    /** Drops unknown/duplicate modules, enforces ≤1 source per chain, removes
     *  empty residue but always keeps ≥1 chain. Safe to call after load. */
    void validateAndRepair();

    /** The legacy fixed topology — used on a fresh session / failed load:
     *  Chain 1: SP3CTRA→PITCH→MASK→SAMPLER→SCORE→LUXSTRAL
     *  Chain 2: SP3CTRA→LUXSYNTH→LUXWAVE */
    static ChainModel makeDefault();

    //── ValueTree identifiers (shared with persistence) ───────────────────────
    static const juce::Identifier kChainsTag;   // "CHAINS"
    static const juce::Identifier kChainTag;    // "CHAIN"
    static const juce::Identifier kModuleTag;   // "MODULE"
    static const juce::Identifier kTypeProp;    // "type"
    static const juce::Identifier kUuidProp;    // "uuid"
    static const juce::Identifier kVersionProp; // "version"
};
