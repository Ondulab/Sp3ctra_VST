/**
 * @file ChainRackComponent.h
 * @brief ZONE 2 — vertical chain rack (M4 four-zone shell).
 *
 * Shows the two image chains as stacked block lists:
 *
 *   CHAIN 1                      CHAIN 2
 *     SOURCE CIS                   SOURCE CIS
 *     PITCH  ⇅  MASK  (order =     ♪ LUXSYNTH
 *       "chainInsertOrder")        ♪ LUXWAVE
 *     SAMPLER
 *     SCORE
 *     ♪ LUXSTRAL
 *
 * Each block carries an identity colour, a 3-state LED
 * (● active / ◐ enabled-but-idle / ○ disabled, refreshed at 10 Hz) and is
 * clickable: clicking fires onBlockSelected so the editor can drive the
 * single selection model (zone 1 view + zone 3 editor).
 *
 * The topology is FIXED for now — M6 makes it editable (palette drag & drop).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include <functional>
#include <vector>

//==============================================================================
/** Identifies one block of the (fixed) two-chain topology. */
enum class ChainBlockId
{
    Chain1Source = 0,   ///< CHAIN 1 — SOURCE CIS
    Pitch,              ///< CHAIN 1 — PITCH insert
    Mask,               ///< CHAIN 1 — MASK insert
    Sampler,            ///< CHAIN 1 — SAMPLER
    Score,              ///< CHAIN 1 — SCORE (playable spectrogram)
    LuxStral,           ///< CHAIN 1 — ♪ LUXSTRAL engine
    Chain2Source,       ///< CHAIN 2 — SOURCE CIS
    LuxSynth,           ///< CHAIN 2 — ♪ LUXSYNTH engine
    LuxWave             ///< CHAIN 2 — ♪ LUXWAVE engine
};

//==============================================================================
class ChainRackComponent : public juce::Component,
                           private juce::Timer,
                           private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit ChainRackComponent(Sp3ctraAudioProcessor& p);
    ~ChainRackComponent() override;

    /** Fired when the user clicks a block (selection is owned by the editor). */
    std::function<void(ChainBlockId)> onBlockSelected;

    /** Identity colour of a block — single source of truth for the rack,
     *  the zone-3 PLAY/SETUP switcher and the SETUP-face headers (M5). */
    static juce::Colour blockColour(ChainBlockId id) noexcept;

    /** APVTS enable/device-on parameter that powers a block on/off — shared by
     *  the rack's clickable LED and the zone-3 header power toggle. Returns an
     *  empty string for blocks that have no enable switch (SOURCE CIS, SCORE). */
    static juce::String enableParamId(ChainBlockId id) noexcept;

    /** Updates the highlighted block (called back by the editor). */
    void setSelectedBlock(ChainBlockId id);

    /** Natural content height — the editor's viewport sizes us with this. */
    int preferredHeight() const noexcept;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    //==========================================================================
    enum class LedState { Off, Idle, Active };

    class BlockComponent : public juce::Component,
                           public juce::SettableTooltipClient
    {
    public:
        BlockComponent(ChainBlockId idIn, const juce::String& nameIn, juce::Colour colourIn)
            : id(idIn), name(nameIn), colour(colourIn)
        {
            setRepaintsOnMouseActivity(true);
        }

        std::function<void(ChainBlockId)> onClick;       ///< click the body → select
        std::function<void()>             onToggleEnable; ///< click the LED  → power on/off

        /** APVTS enable param for this block ("" = no power switch). When set,
         *  the LED dot becomes a clickable power button. */
        juce::String enableParam;

        ChainBlockId getId() const noexcept { return id; }

        void setLed(LedState s)       { if (led != s)      { led = s;        repaint(); } }
        void setSelected(bool sel)    { if (selected != sel){ selected = sel; repaint(); } }

        void paint(juce::Graphics& g) override;

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (! e.mouseWasClicked())
                return;
            if (enableParam.isNotEmpty() && dotBounds().contains(e.position) && onToggleEnable)
                onToggleEnable();
            else if (onClick)
                onClick(id);
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            const bool over = enableParam.isNotEmpty() && dotBounds().contains(e.position);
            if (over != overDot)
            {
                overDot = over;
                setMouseCursor(over ? juce::MouseCursor::PointingHandCursor
                                    : juce::MouseCursor::NormalCursor);
                repaint();
            }
        }

        void mouseExit(const juce::MouseEvent&) override
        {
            if (overDot)
            {
                overDot = false;
                setMouseCursor(juce::MouseCursor::NormalCursor);
                repaint();
            }
        }

    private:
        /** Hit/draw rect of the LED dot (generous square for easy clicking). */
        juce::Rectangle<float> dotBounds() const
        {
            const auto b = getLocalBounds().toFloat().reduced(2.f);
            const float r = 9.f;
            return { b.getRight() - 11.f - r, b.getCentreY() - r, 2 * r, 2 * r };
        }

        ChainBlockId id;
        juce::String name;
        juce::Colour colour;
        LedState     led      { LedState::Off };
        bool         selected { false };
        bool         overDot  { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BlockComponent)
    };

    /** Small ⇅ button drawn with paths (no font dependency). */
    class SwapOrderButton : public juce::Button
    {
    public:
        SwapOrderButton() : juce::Button("swapInsertOrder") {}
        void paintButton(juce::Graphics& g, bool isMouseOver, bool isButtonDown) override;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SwapOrderButton)
    };

    //==========================================================================
    void timerCallback() override;                                    // 10 Hz LED refresh
    void parameterChanged(const juce::String& paramID, float) override;

    void toggleInsertOrder();
    void toggleEnable(const juce::String& paramId);
    bool isMaskFirst() const;
    void updateLeds();

    /** Blocks of each chain in current display order (Pitch/Mask may swap). */
    std::vector<BlockComponent*> chain1Order();
    std::vector<BlockComponent*> chain2Order();
    std::vector<BlockComponent*> toolsOrder();

    //==========================================================================
    Sp3ctraAudioProcessor& processor;

    BlockComponent srcABlock, pitchBlock, maskBlock, samplerBlock, stralBlock;
    BlockComponent srcBBlock, synthBlock, waveBlock;
    BlockComponent scoreBlock;
    SwapOrderButton swapBtn;

    // Source-activity tracking (UDP feed advancing → LED active)
    juce::uint64 lastLinesSeen { 0 };
    LedState     sourceLed     { LedState::Idle };

    // Vertical positions of the group headers (set in resized, used in paint)
    int header1Y { 0 };
    int header2Y { 0 };
    int header3Y { -1 };  // TOOLS (-1 = hidden / empty section)

    // ── Geometry ──────────────────────────────────────────────────────────────
    static constexpr int kTopPad   = 6;
    static constexpr int kPadX     = 8;
    static constexpr int kHeaderH  = 18;
    static constexpr int kBlockH   = 32;
    static constexpr int kBlockGap = 12;   // connector arrow lives here
    static constexpr int kSwapGap  = 22;   // wider gap between PITCH and MASK (⇅ button)
    static constexpr int kChainGap = 18;
    static constexpr int kBottomPad= 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainRackComponent)
};
