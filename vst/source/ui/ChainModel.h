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
 * Since Phase 2 the model is owned by the processor (Sp3ctraAudioProcessor::
 * getChainModel). Edits are pushed through onChainModelEdited(), which derives
 * the per-synth routing + RT ChainPlan, projects presence onto the APVTS
 * enable params and persists the topology.
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
    juce::Uuid  id;       ///< stable identity (survives reordering)
    int         slot{-1}; ///< per-instance index: VideoScroll bank (0..7),
                          ///  sampler engine (0=A, 1=B) OR engine-send bank
                          ///  (0..7); -1 for non-slotted types.

    /** J2 — the module's settings AT REST (chain-owned): a "VALUES" tree whose
     *  properties are the manifest suffixes → raw param values. Written by
     *  snapshotBankValuesIntoModel() at save time, projected back onto the
     *  runtime banks at load (projectChainValuesToBanks()). Non-structural:
     *  invalid when never snapshotted (pre-v3 sessions). NOTE — ValueTree
     *  copies share the underlying node; deep-copy (createCopy) when
     *  duplicating an instance. */
    juce::ValueTree values;
};

struct Chain
{
    juce::Uuid                  id;
    std::vector<ModuleInstance> modules;   ///< order is significant

    /** J3 — the chain REMEMBERS the settings of modules that left it, per
     *  type (VALUES-shaped trees: manifest suffixes → raw values, Enabled
     *  excluded). A re-added module of that type inherits them (chain
     *  inheritance); the memory dies with the chain. Serialized as MEMORY
     *  children of the CHAIN node (subsumes the legacy INSERT_MEMORY blob). */
    std::map<ModuleType, juce::ValueTree> typeMemory;
};

//==============================================================================
class ChainModel
{
public:
    std::vector<Chain> chains;

    /** Hard cap on the number of chains. Every RT consumer (Pitch/Mask state
     *  pools, chain masks, the ChainPlan) sizes its per-chain storage with
     *  CHAIN_MAX_CHAINS (chain_plan.h); a 9th chain would silently share pool
     *  state with chain 8. Enforced by addChain() and validateAndRepair(). */
    static constexpr int kMaxChains = 8;   // MUST equal CHAIN_MAX_CHAINS

    //── Queries ───────────────────────────────────────────────────────────────
    bool chainHasRole(int chainIdx, ModuleRole role) const;
    bool chainHasType(int chainIdx, ModuleType type) const;

    /** True if `type` may be inserted into chain `chainIdx`. When `movingId` is
     *  non-null, that instance is excluded from the duplicate/role counts (used
     *  for in-rack reordering / cross-chain moves of an existing block). */
    bool canInsert(int chainIdx, ModuleType type, const juce::Uuid* movingId = nullptr) const;

    /** True if `type` could be inserted into a freshly created empty chain —
     *  i.e. it passes the GLOBAL limits only (singleton util/media types,
     *  VideoScroll/Sampler/engine-send slot pools). Used by the rack to
     *  validate a drop on the "+ CHAIN" row BEFORE the chain is actually
     *  created, so an invalid drop never leaves a phantom empty chain behind. */
    bool canInsertIntoNewChain(ModuleType type, const juce::Uuid* movingId = nullptr) const;

    //── VideoScroll per-instance slot pool ─────────────────────────────────────
    static constexpr int kMaxVideoSlots = 8;   // MUST equal CHAIN_MAX_CHAINS
    static bool isSlottedType(ModuleType t) noexcept { return t == ModuleType::VideoScroll; }
    /** Lowest free slot 0..kMaxVideoSlots-1 across ALL chains, or -1 if full.
     *  `movingId` (if set) is excluded so a moved instance keeps its slot. */
    int firstFreeVideoSlot(const juce::Uuid* movingId = nullptr) const;
    /** Count of slotted instances across the whole model (optionally excluding one). */
    int videoSlotCount(const juce::Uuid* exclude = nullptr) const;

    //── Sampler engine slot pool (A=0, B=1) — INDEPENDENT of the VideoScroll pool ─
    // A Sampler instance's `slot` is its engine index: first placed = A (0),
    // second = B (1). Up to 2 may coexist (even in the same chain).
    static constexpr int kMaxSamplerEngines = 2;
    static bool isSamplerEngine(ModuleType t) noexcept { return t == ModuleType::Sampler; }
    /** Lowest free sampler-engine slot 0..kMaxSamplerEngines-1, or -1 if full. */
    int firstFreeSamplerSlot(const juce::Uuid* movingId = nullptr) const;

    //── Engine SEND slot pools (LuxStral / LuxSynth / LuxWave) ──────────────────
    // Synth-split M6: an OUT instance is a "→ ENGINE" SEND toward its single
    // global engine; its `slot` is its conditioning-bank index
    // ({luxstral,luxsynth,luxwave}Out{slot}_*). One send per type per chain
    // (per-chain duplicate rule, D5), up to kMaxChains sends PER TYPE
    // model-wide — the audio-thread mixers blend every active send into the
    // engine feed.
    static constexpr int kMaxEngineSends = kMaxChains;       // 8 sends per type
    static constexpr int kMaxLuxStralEngines = kMaxEngineSends; // legacy alias
    static bool isEngineSend(ModuleType t) noexcept
        { return t == ModuleType::LuxStral || t == ModuleType::LuxSynth
              || t == ModuleType::LuxWave; }
    static bool isLuxStralEngine(ModuleType t) noexcept { return t == ModuleType::LuxStral; }
    /** Lowest free send slot of `type` (0..kMaxEngineSends-1), or -1 if full.
     *  Each send type owns an independent pool. */
    int firstFreeEngineSendSlot(ModuleType type,
                                const juce::Uuid* movingId = nullptr) const;

    /** Types that carry a per-instance `slot` (VideoScroll bank, sampler engine
     *  OR engine send). */
    static bool hasSlot(ModuleType t) noexcept
        { return isSlottedType(t) || isSamplerEngine(t) || isEngineSend(t); }

    //── Mutations (return false when the rule check fails) ─────────────────────
    bool insert(int chainIdx, ModuleType type, int dropIdx);
    bool moveWithin(int chainIdx, int from, int to);
    bool moveAcross(int fromChain, int from, int toChain, int dropIdx);
    bool remove(int chainIdx, int idx);

    bool canAddChain() const noexcept { return numChains() < kMaxChains; }
    int  addChain();                 ///< appends an empty chain, returns its index (-1 when at kMaxChains)
    bool removeChain(int chainIdx);  ///< refuses to delete the last chain

    /** J3 — insert a full copy of chain `chainIdx` right after it: fresh
     *  UUIDs everywhere, deep-copied VALUES + type memory. validateAndRepair()
     *  then drops whatever cannot be duplicated (singletons, exhausted pools)
     *  and heals the slots. Returns the new chain's index, or -1 (cap/range).
     *  Caller (processor) projects the copied VALUES onto the fresh banks. */
    int duplicateChain(int chainIdx);

    //── Lookups ───────────────────────────────────────────────────────────────
    int  numChains() const noexcept { return (int) chains.size(); }
    const ModuleInstance* find(const juce::Uuid& id, int& outChain, int& outIdx) const;

    //── Phase-1 audio bridge helpers ──────────────────────────────────────────
    /** Every ModuleType present in at least one chain. */
    void deriveActiveTypes(std::set<ModuleType>& out) const;
    /** Global Pitch/Mask relation for the binary chainInsertOrder param: true if
     *  the first chain containing both has Mask before Pitch. */
    bool isMaskBeforePitch() const;


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
    static const juce::Identifier kSlotProp;    // "slot" (VideoScroll bank index)
    static const juce::Identifier kValuesTag;   // "VALUES" (J2 — chain-owned settings)
    static const juce::Identifier kMemoryTag;   // "MEMORY" (J3 — chain type memory)

    /** CHAINS schema version written by toValueTree(). Migrations gate on the
     *  version read back from a loaded tree:
     *   1 — pre-SEQUENCER-module era (a missing Sequencer means "old save")
     *   2 — Sequencer is a chain block; a missing Sequencer means the user
     *       deleted it and it must NOT be re-injected on load.
     *   3 — each MODULE may carry a VALUES child (its settings at rest) —
     *       the chain owns its modules' settings; projected onto the runtime
     *       banks at load. */
    static constexpr int kSchemaVersion = 3;
};
