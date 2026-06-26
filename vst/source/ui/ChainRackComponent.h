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
 * The model's only Phase-1 audio effect is projecting module presence onto the
 * existing APVTS enable params + chainInsertOrder (see applyModelToParams).
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
    Chain2Source, LuxSynth, LuxWave
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
        LedState     led      { LedState::Off };
        bool         selected { false };
        bool         overDot  { false };
        bool         overClose{ false };
        bool         dragging { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BlockComponent)
    };

    //── Layout bookkeeping (built in resized, used for paint + hit-testing) ───
    struct Slot  { int chainIdx; int moduleIdx; juce::Rectangle<int> bounds; };
    struct Band  { int chainIdx; int headerY; int topY; int bottomY; bool empty; };
    struct DropTarget { int chainIdx; int index; bool valid; bool newChain; };

    //==========================================================================
    void timerCallback() override;        // 10 Hz LED refresh

    void rebuild();                       // (re)create block components from model
    void mutateAndRefresh(bool notifySelection); // after a model change: rebuild + bridge + persist + relayout
    void scheduleRefresh(bool notifySelection);  // defer mutateAndRefresh to the next tick (lifetime-safe)

    void loadModelFromState();            // read apvts.state child "CHAINS"
    void persistModel();                  // write it back
    void applyModelToParams(const std::set<ModuleType>& prevActive); // enable params + order bridge

    void toggleEnable(const juce::String& paramId);
    void removeInstance(const juce::Uuid& id);

    void updateLeds();
    LedState ledFor(ModuleType type) const;

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

    ChainModel model;
    std::vector<std::unique_ptr<BlockComponent>> blocks;   // one per ModuleInstance

    std::set<ModuleType> activeTypes;     // last-known presence set (param-bridge diff)
    juce::Uuid           selectedId;      // currently highlighted instance

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
