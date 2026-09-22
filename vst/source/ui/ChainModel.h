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
    int         slot{-1}; ///< per-instance index, keyed per POOL FAMILY (each
                          ///  family numbers independently, 0..7): VideoScroll
                          ///  bank, MidiTap bank, sampler engine, engine-send
                          ///  bank, media source or score player.
                          ///  -1 for non-slotted types (see hasSlot()).

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

    /** Which pole of the image is the BACKGROUND (the silence) for every
     *  module of this chain — the C-side convention shared by all inserts
     *  (LUX_*_BG_*): 0 = Black, 1 = White, 2 = Auto (each module's own
     *  learn-then-lock detector). Chain-owned since schema 4 — the former
     *  per-module BackgroundMode params are gone; applyConfigurationToCore
     *  projects this value onto every member module's config. */
    int backgroundMode { 1 };   // White — paper is the typical Sp3ctra stream

    /** User label shown in the rack header NEXT TO the chain number (the
     *  number stays the identity — colours, delete confirms, zone-3 tabs).
     *  Empty = the default "CHAIN" caption. */
    juce::String name;

    /** Rack fold state: true = the chain card is collapsed to its header
     *  (blocks hidden, no drop target). Pure UI — routing, the RT plan and
     *  the enable bridge ignore it; persisted so the rack reopens as left. */
    bool collapsed { false };
};

/** Chain::backgroundMode poles (C-side LUX_*_BG_* convention). */
enum ChainBackground { kChainBgBlack = 0, kChainBgWhite = 1, kChainBgAuto = 2 };

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

    //── MidiTap per-instance slot pool — INDEPENDENT of the VideoScroll pool ───
    // A MIDI TAP is a probe like VideoScroll (pass-through, may repeat in a
    // chain), but it owns its OWN 8-slot pool: `slot` indexes both its RT
    // capture ring (midi_tap_instance) and its APVTS bank (midiTap{slot}_*).
    // Sharing the VideoScroll pool would mean one slot addressing two unrelated
    // RT states AND one budget counter for two module types (a 9th module of
    // EITHER type would then be dropped on load).
    static constexpr int kMaxMidiTaps = 8;   // MUST equal CHAIN_MAX_CHAINS
    static bool isMidiTap(ModuleType t) noexcept { return t == ModuleType::MidiTap; }
    /** Lowest free MidiTap slot 0..kMaxMidiTaps-1 across ALL chains, or -1 if full. */
    int firstFreeMidiTapSlot(const juce::Uuid* movingId = nullptr) const;
    /** Count of MidiTap instances across the whole model (optionally excluding one). */
    int midiTapCount(const juce::Uuid* exclude = nullptr) const;

    /** Types EXEMPT from the per-chain "never two instances of a type" rule —
     *  they are bounded by a model-wide slot pool instead. THE list: canInsert()
     *  and validateAndRepair() both go through it, so adding the next
     *  multi-instance type updates them at once (the kScoreFamily lesson). */
    static bool mayRepeatInChain(ModuleType t) noexcept
        { return isSlottedType(t) || isMidiTap(t) || isSamplerEngine(t); }

    //── Sampler engine slot pool (A=0, B=1) — INDEPENDENT of the VideoScroll pool ─
    // A Sampler instance's `slot` is its engine index: first placed = A (0),
    // second = B (1). Up to 2 may coexist (even in the same chain).
    static constexpr int kMaxSamplerEngines = 8;   // P6 — sampler engines ×8
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
    static bool isEngineSend(ModuleType t) noexcept
        { return t == ModuleType::LuxStral || t == ModuleType::LuxSynth
              || t == ModuleType::LuxWave  || t == ModuleType::LuxGrain; }
    /** Lowest free send slot of `type` (0..kMaxEngineSends-1), or -1 if full.
     *  Each send type owns an independent pool. */
    int firstFreeEngineSendSlot(ModuleType type,
                                const juce::Uuid* movingId = nullptr) const;

    //── Media source slot pools (IMAGE / VIDEO / CAMERA) — P5-M1 ───────────────
    // Each media type owns an independent 8-slot pool: an instance's `slot` is
    // its future engine/bank index. Multi-chain (one instance per chain via the
    // ≤1-source rule). M1 NOTE: the runtime still reads KIND-wide state — every
    // instance of a kind shows the same media until P5-M2.
    static constexpr int kMaxMediaSlots = 8;
    static bool isMediaSource(ModuleType t) noexcept
        { return t == ModuleType::Image || t == ModuleType::Video
              || t == ModuleType::Camera; }
    int firstFreeMediaSlot(ModuleType type,
                           const juce::Uuid* movingId = nullptr) const;

    //── Score-player slot pool — P5-M1 ──────────────────────────────────────────
    // ONE pool SHARED by the whole score family (kScoreFamily: SCORE / TIMBRE /
    // MIDI SCORE / VOICE): an instance's `slot` is its ScoreSlotPool lecteur
    // index (8 lecteurs indépendants) and, since P7, also the index of its OWN
    // transport bank (scoreXportParam) and of its generator page document —
    // two SCOREs living in two chains share nothing.
    static constexpr int kMaxScorePlayers = 8;
    int firstFreeScorePlayerSlot(const juce::Uuid* movingId = nullptr) const;

    /** Types that carry a per-instance `slot` (VideoScroll bank, MidiTap bank,
     *  sampler engine, engine send, media source or score player).
     *  CRITICAL: toValueTree() only persists `slot` when this is true — a type
     *  missing here re-heals to a different slot on every reload, silently
     *  swapping settings AND host automation lanes between instances. */
    static bool hasSlot(ModuleType t) noexcept
        { return isSlottedType(t) || isMidiTap(t) || isSamplerEngine(t)
              || isEngineSend(t)  || isMediaSource(t) || isScoreFamily(t); }

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


    //── Persistence ───────────────────────────────────────────────────────────
    juce::ValueTree toValueTree() const;
    void            fromValueTree(const juce::ValueTree&);

    /** Drops unknown/duplicate modules, enforces ≤1 source per chain, removes
     *  empty residue but always keeps ≥1 chain. Safe to call after load. */
    void validateAndRepair();

    /** Schema-4 migration — normalize a legacy per-module BackgroundMode raw
     *  value (a VALUES/MEMORY attribute) onto the C-side convention
     *  (0 = Black, 1 = White, 2 = Auto), undoing the three legacy choice
     *  orders. Returns -1 when this tree/type carries no background. Shared
     *  by fromValueTree() and the .sp3chain preset loader. */
    static int legacyBackgroundOf(ModuleType t, const juce::ValueTree& values);

    /** In-place migration of one module's VALUES / MEMORY tree written by an
     *  older build: keys that did not exist yet are seeded from their legacy
     *  source so the module renders as it did. Called on every tree read from
     *  a session (fromValueTree) or a .sp3chain preset (loadChainPreset). */
    static void migrateModuleValues(ModuleType t, juce::ValueTree& values);

    /** Fresh-session topology — used on a fresh session / failed load:
     *  two empty chains, the rack is built by the user from the catalogue. */
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
    static const juce::Identifier kBackgroundProp; // "background" (schema 4 — chain-owned pole)
    // Additive CHAIN attributes (no schema bump — absent = default):
    static const juce::Identifier kNameProp;       // "name" (user chain label)
    static const juce::Identifier kCollapsedProp;  // "collapsed" (rack fold state)

    /** CHAINS schema version written by toValueTree(). Migrations gate on the
     *  version read back from a loaded tree:
     *   1 — pre-SEQUENCER-module era (a missing Sequencer meant "old save")
     *   2 — Sequencer was a chain block; a missing Sequencer meant the user
     *       deleted it and it must NOT be re-injected on load.
     *   3 — each MODULE may carry a VALUES child (its settings at rest) —
     *       the chain owns its modules' settings; projected onto the runtime
     *       banks at load.
     *   4 — the CHAIN node carries "background" (the chain-owned pole). The
     *       former per-module BackgroundMode params are gone; a CHAIN without
     *       the attribute is migrated by fromValueTree() from the first
     *       member VALUES carrying one (per-family choice-order normalized).
     *   5 — the EQ banks (EQUALIZER + CENTROID/LEVELS output EQ) are typed
     *       handles (Sh{0..3}Type/Freq/Gain/Width — shape_eq.h). Legacy
     *       Band0..8 + NumPoints VALUES attributes no longer match any
     *       manifest suffix and are silently skipped on projection: old
     *       curves reload FLAT, by design (no spline→handle fitting).
     *  The SEQUENCER rack module was retired (the sequencer is internal to
     *  each sampler engine): "Sequencer" MODULE entries in old trees no longer
     *  resolve to a type and are silently dropped by fromValueTree(). */
    static constexpr int kSchemaVersion = 5;
};
