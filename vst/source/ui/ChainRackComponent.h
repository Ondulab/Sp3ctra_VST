/**
 * @file ChainRackComponent.h
 * @brief ZONE 2 — editable vertical chain rack (M6 drag & drop).
 *
 * Renders N chains from a ChainModel as stacked block lists. Modules are
 * dragged in from the left ModuleCatalogComponent and reordered / moved /
 * removed by dragging existing blocks. Placement rules (one source per chain,
 * no duplicate type per chain, order matters) live in ChainModel::canInsert.
 *
 * Each block carries an identity colour, a 3-state LED (● active / ◐ idle /
 * ○ off, refreshed at 10 Hz) and is clickable: clicking fires onBlockSelected
 * so the editor drives the single selection model (zone 1 view + zone 3 editor).
 *
 * Selection is tracked internally per instance (Uuid) so duplicate types across
 * chains highlight correctly; the editor-facing API stays ChainBlockId-keyed.
 *
 * The model is owned by the processor (Phase 2): every mutation here goes
 * through processor.onChainModelEdited(), which derives the per-chain routing,
 * publishes the RT ChainPlan, bridges the enable params and persists.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "ModuleCatalog.h"
#include "ChainModel.h"
#include <functional>
#include <memory>
#include <set>
#include <vector>

//==============================================================================
/** Editor-facing selection key (compatibility shim over ModuleType + chain).
 *  Chain1Source / Chain2Source distinguish the source's chain so the editor's
 *  zone-1 view (Modulated vs Live) and SOURCES transport keep working. */
enum class ChainBlockId
{
    Chain1Source = 0, Pitch, Mask, Sampler, Score, LuxStral,
    Chain2Source, LuxSynth, LuxWave, Sequencer, VideoScroll
};

/** Maps a selection key to its module type (sources → Sp3ctra). */
ModuleType chainBlockToModuleType(ChainBlockId id) noexcept;

//==============================================================================
class ChainRackComponent : public juce::Component,
                           public juce::DragAndDropTarget,
                           private juce::Timer
{
public:
    explicit ChainRackComponent(Sp3ctraAudioProcessor& p);
    ~ChainRackComponent() override;

    /** Fired when the user clicks a block (selection is owned by the editor). */
    std::function<void(ChainBlockId)> onBlockSelected;

    /** Fired (BEFORE onBlockSelected) when the selected block is a VideoScroll
     *  output, carrying its per-instance slot (0..7) so the editor can bind the
     *  contextual panel / mixer voice to videoScroll{slot}_*. */
    std::function<void(int)> onVideoBlockSelected;

    /** Fired (BEFORE onBlockSelected) when the selected block is a SAMPLER,
     *  carrying its engine index (slot: 0 = A, 1 = B) so the editor can bind the
     *  sampler page + setup panel to the right engine. */
    std::function<void(int)> onSamplerBlockSelected;

    /** Fired after a model mutation so the editor can re-run layoutZones()
     *  (the rack's preferred height changed). Persistence + the audio-param
     *  bridge are handled internally. */
    std::function<void()> onModelChanged;

    /** Identity colour of a block — shared with the zone-3 switcher + headers. */
    static juce::Colour blockColour(ChainBlockId id) noexcept;

    /** APVTS enable/device-on parameter that powers a block on/off ("" = none). */
    static juce::String enableParamId(ChainBlockId id) noexcept;

    /** Updates the highlighted block (called back by the editor). */
    void setSelectedBlock(ChainBlockId id);

    /** Locks the rack (performance mode): hides every delete affordance — the
     *  per-module × and the per-chain × — while keeping drag-to-reorder (within
     *  a chain, across chains, into a new chain) and "+ CHAIN" fully usable.
     *  Driven by the editor when the module catalogue rail is collapsed. */
    void setLocked(bool shouldLock);
    bool isLocked() const noexcept { return locked; }

    /** Natural content height — the editor's viewport sizes us with this. */
    int preferredHeight() const noexcept;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseUp(const juce::MouseEvent& e) override;   // header × + "+ CHAIN" row

    //── juce::DragAndDropTarget ───────────────────────────────────────────────
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragEnter(const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped  (const SourceDetails&) override;

private:
    //==========================================================================
    enum class LedState { Off, Idle, Active };

    //── One block = one ModuleInstance ────────────────────────────────────────
    class BlockComponent : public juce::Component,
                           public juce::SettableTooltipClient
    {
    public:
        BlockComponent(ModuleType t, juce::Uuid uidIn)
            : type(t), uid(uidIn), name(moduleDisplayName(t)),
              colour(moduleColour(t)), enableParam(moduleEnableParam(t))
        {
            setRepaintsOnMouseActivity(true);
        }

        std::function<void(juce::Uuid)> onClick;        ///< body → select
        std::function<void()>           onToggleEnable; ///< LED → power on/off
        std::function<void(juce::Uuid)> onRemove;       ///< × → remove from chain

        ModuleType   getType()        const noexcept { return type; }
        juce::Uuid   getUuid()        const noexcept { return uid; }
        juce::String getEnableParam() const noexcept { return enableParam; }

        void setLed(LedState s)    { if (led != s)       { led = s;        repaint(); } }
        void setSelected(bool sel) { if (selected != sel) { selected = sel; repaint(); } }

        /** Append an engine letter (e.g. "A"/"B") to the shown name — used to tell
         *  the two LuxStral engines apart in the rack. */
        void setEngineSuffix(const juce::String& letter)
        {
            name = moduleDisplayName(type) + " " + letter;
            repaint();
        }

        /** Override the APVTS enable param this block's LED toggles — e.g. the 2nd
         *  LuxStral engine (B) drives "luxstralBEnabled", independent of A. */
        void setEnableParamOverride(const juce::String& id) { enableParam = id; }

        /** When false the × remove glyph is hidden + its click is ignored
         *  (the rack is locked). Reorder dragging is unaffected. */
        void setRemovable(bool r)
        {
            if (removable != r) { removable = r; if (! r) overClose = false; repaint(); }
        }

        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp  (const juce::MouseEvent& e) override;
        void mouseMove(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent&) override;

    private:
        juce::Rectangle<float> dotBounds()   const;  ///< LED hit/draw rect
        juce::Rectangle<float> closeBounds() const;  ///< × hit/draw rect (hover)

        ModuleType   type;
        juce::Uuid   uid;
        juce::String name;
        juce::Colour colour;
        juce::String enableParam;
        LedState     led       { LedState::Off };
        bool         selected  { false };
        bool         removable { true };
        bool         overDot   { false };
        bool         overClose { false };
        bool         dragging  { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BlockComponent)
    };

    //── Layout bookkeeping (built in resized, used for paint + hit-testing) ───
    struct Slot  { int chainIdx; int moduleIdx; juce::Rectangle<int> bounds; };
    struct Band  { int chainIdx; int headerY; int topY; int bottomY; bool empty; };
    struct DropTarget { int chainIdx; int index; bool valid; bool newChain; };

    //==========================================================================
    void timerCallback() override;        // 10 Hz LED refresh

    void rebuild();                       // (re)create block components from model
    void mutateAndRefresh(bool notifySelection); // after a model change: processor bridge + rebuild + relayout
    void scheduleRefresh(bool notifySelection);  // defer mutateAndRefresh to the next tick (lifetime-safe)

    void toggleEnable(const juce::String& paramId);
    void removeInstance(const juce::Uuid& id);

    void updateLeds();
    // engineSlot: per-instance engine index (LuxStral/Sampler A=0/B=1); -1 = n/a.
    LedState ledFor(ModuleType type, int chainIdx, int engineSlot = -1) const;

    void selectInstance(const juce::Uuid& id, bool notify);
    juce::Uuid   firstInstanceId() const;
    ChainBlockId instanceToBlockId(ModuleType type, int chainIdx) const noexcept;

    DropTarget computeDrop(juce::Point<int> localPos, ModuleType type,
                           const juce::Uuid* movingId) const;
    void updateDropFromDetails(const SourceDetails&);

    //── Geometry helpers ──────────────────────────────────────────────────────
    juce::Rectangle<int> addChainRowBounds() const { return addRowRect; }

    //==========================================================================
    Sp3ctraAudioProcessor& processor;

    ChainModel& model;   // owned by the processor (M6 Phase 2); the rack edits it in place
    std::vector<std::unique_ptr<BlockComponent>> blocks;   // one per ModuleInstance

    juce::Uuid           selectedId;      // currently highlighted instance
    bool                 locked { false };// performance mode: delete affordances off

    // Source-activity tracking (UDP feed advancing → source LED active)
    juce::uint64 lastLinesSeen { 0 };
    LedState     sourceLed     { LedState::Idle };

    // Layout map + drag indicator state
    std::vector<Slot>    slots;
    std::vector<Band>    bands;
    juce::Rectangle<int> addRowRect;
    bool        dragActive { false };
    DropTarget  dropTarget { -1, 0, false, false };

    // ── Geometry ──────────────────────────────────────────────────────────────
    static constexpr int kTopPad    = 6;
    static constexpr int kPadX      = 8;
    static constexpr int kHeaderH   = 18;
    static constexpr int kBlockH    = 32;
    static constexpr int kBlockGap  = 12;   // connector arrow lives here
    static constexpr int kChainGap  = 18;
    static constexpr int kEmptyH    = 30;   // empty-chain drop zone height
    static constexpr int kAddRowH   = 26;   // "+ CHAIN" row
    static constexpr int kBottomPad = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainRackComponent)
};
