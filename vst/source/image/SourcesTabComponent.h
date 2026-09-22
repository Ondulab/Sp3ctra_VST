/**
 * @file SourcesTabComponent.h
 * @brief ZONE 3 (PLAY face) — SP3CTRA source MODULE transport.
 *
 * This is the SP3CTRA (live CIS) SOURCE MODULE's own transport — a PLAY/STOP
 * toggle + PAUSE + Fade-In — and it is a property of the MODULE, not of the
 * chain it sits in. Both source blocks (chain 1 / chain 2) drive the SAME
 * global SP3CTRA transport (imageFreezeMode) and the SAME Fade-In
 * (imageFadeInMs), so the fade on play/pause/stop is chain-independent. The
 * chain index only tints the header so you can tell which block you selected.
 * The rack block's LED drives the same play/stop switch.
 *
 * The RAW upstream UDP gate is the instrument's own signal and is not surfaced
 * here. The sampler has its own transport elsewhere (samplerFreezeMode) — this
 * SOURCE view never touches it.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../IconPaths.h"
#include "../midi/MidiLearnAttachment.h"
#include "../sampler/TransportBarComponent.h"   // IconTextButton
#include "VisualizerMode.h"
#include "../ui/Sp3ctraBarSlider.h"

class SourcesTabComponent : public juce::Component,
                            private juce::Timer
{
public:
    explicit SourcesTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        // The module power lives where every block has it: at the right of the
        // PLAY|SETUP face row (PluginEditor's modulePowerButton, hand-wired to
        // imageFreezeMode). This page only carries FREEZE: hold the current
        // image (mode 1), inert while the module is off (holding black IS stop).
        freezeBtn.setClickingTogglesState(true);
        freezeBtn.setTooltip("Freeze: hold the current image (module stays on)");
        freezeBtn.onClick = [this]
        {
            const int m = currentMode();
            if (freezeBtn.getToggleState()) { if (m == 0) setFreezeMode(0.5f); }
            else                            { if (m == 1) setFreezeMode(0.f);  }
        };

        addAndMakeVisible(freezeBtn);

        // Fade-In (ms) — SP3CTRA input source (Chain 2) only. The sampler
        // (Chain 1) has no transport fade, so this row is hidden there.
        fadeSlider.setTextValueSuffix(" ms");
        fadeSlider.setNumDecimalPlacesToDisplay(0);
        addChildComponent(fadeSlider);   // visibility toggled per chain in setChain()

        setChain(1);   // default; the editor re-sets this on block selection
        startTimer(200);
    }

    ~SourcesTabComponent() override { stopTimer(); }

    /** Select which SP3CTRA source block (chain 1 / 2) is shown. Both drive the
     *  SAME global SP3CTRA transport + Fade-In; the chain only tints the header. */
    void setChain(int chain)
    {
        activeChain_ = (chain == 2) ? 2 : 1;

        auto& apvts = processor.getAPVTS();

        // Fade-In belongs to the SP3CTRA source MODULE (imageFadeInMs), so it is
        // bound and shown for every source block — the fade on play/pause/stop
        // is a module feature, independent of the chain.
        fadeAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "imageFadeInMs", fadeSlider));
        fadeSlider.setVisible(true);

        // Right-click MIDI Learn. FREEZE learns the VIRTUAL 2-state target
        // (kImgFreezeMidiId), NOT imageFreezeMode directly: a CC on the raw
        // 3-step param sweeps play↔stop, i.e. it would drive the module's
        // power instead of freezing. The Fade-In is global (bound once,
        // rebound cheap).
        learnAtts_.clear();
        auto& mm = processor.getMidiMap();
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, fadeSlider, "imageFadeInMs"));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, freezeBtn,
                                 Sp3ctraAudioProcessor::kImgFreezeMidiId));

        resized();
        updateTransportButtons();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const int w = getWidth();

        // SP3CTRA source header — centred; the chain colour only tells you which
        // source block you selected (the transport itself is the same module).
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
        g.setColour(activeChain_ == 1 ? juce::Colour(0xffe0b84a)
                                      : juce::Colour(0xff4ae0a0));
        g.drawText("SP3CTRA",
                   0, 4, w, 14, juce::Justification::centred);

        // Row labels share one right-justified column so every control lines
        // up on the same left edge.
        auto rowLabel = [&] (const char* t, int rowY)
        {
            g.drawText(t, formX(), rowY, labelColW() - 8, kCtrlH,
                       juce::Justification::centredRight);
        };

        // Fade In label — always shown (SP3CTRA module transport fade).
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        g.setColour(juce::Colour(0xffd2d8e8));
        rowLabel("Fade In", fadeRowY());
    }

    void resized() override
    {
        const int w = getWidth();
        constexpr int btnSz = Sp3ctraTheme::kIconBtnSize;
        constexpr int gap   = Sp3ctraTheme::kGap;

        // Transport row — centred FREEZE toggle (the power switch sits in the
        // face row like every other module).
        juce::ignoreUnused(gap);
        constexpr int freezeW = 76;
        freezeBtn.setBounds(w / 2 - freezeW / 2, transportY(), freezeW, btnSz);

        // Unified form grid — every control fills the same column [ctrlX, +ctrlW].
        const int cx = ctrlX();
        const int cw = ctrlW();
        fadeSlider     .setBounds(cx, fadeRowY(), cw, kCtrlH);
    }

private:
    Sp3ctraAudioProcessor& processor;
    int activeChain_ { 1 };

    juce::TextButton  freezeBtn { "FREEZE" };
    Sp3ctraBarSlider fadeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fadeAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    // ── SP3CTRA source module transport param (global, chain-independent) ───────
    const char* freezeParamId() const noexcept
    {
        return "imageFreezeMode";
    }

    void setFreezeMode(float v)
    {
        if (auto* param = processor.getAPVTS().getParameter(freezeParamId()))
            param->setValueNotifyingHost(v);
    }

    /** Current transport state: 0 = play / 1 = hold / 2 = stop. */
    int currentMode() const noexcept
    {
        if (auto* raw = processor.getAPVTS().getRawParameterValue(freezeParamId()))
            return juce::roundToInt(raw->load());
        return 0;
    }

    // ── Unified form geometry ────────────────────────────────────────────────
    // One column grid so labels and controls always line up.  Wider than the
    // old 2/5 node: the form spans the panel width (minus padding), capped so
    // it stays readable.
    static constexpr int kPad      = 18;  // outer horizontal padding
    static constexpr int kFormMaxW = 460; // cap so the form doesn't stretch absurdly
    static constexpr int kLabelColW= 92;  // right-justified label column
    static constexpr int kColGap   = 12;  // gap between label column and control
    static constexpr int kCtrlH    = 24;  // control height (taller → easier to use)

    int formW()    const { return juce::jmin(getWidth() - 2 * kPad, kFormMaxW); }
    int formX()    const { return (getWidth() - formW()) / 2; }
    int labelColW()const { return kLabelColW; }
    int ctrlX()    const { return formX() + kLabelColW + kColGap; }
    int ctrlW()    const { return formW() - kLabelColW - kColGap; }

    int transportY() const { return 24; }
    int fadeRowY()   const { return transportY() + Sp3ctraTheme::kIconBtnSize + 14; }

    // ── Transport button state (200 ms timer + setChain) ──────────────────────
    void updateTransportButtons()
    {
        static const juce::Colour kHold  { 0xff6040a0 };
        static const juce::Colour kOff   { 0xff2a2a2a };

        const int mode = currentMode();
        freezeBtn.setToggleState(mode == 1, juce::dontSendNotification);
        freezeBtn.setColour(juce::TextButton::buttonColourId,   kOff);
        freezeBtn.setColour(juce::TextButton::buttonOnColourId, kHold);
        freezeBtn.setColour(juce::TextButton::textColourOffId,  juce::Colour(0xff888888));
        freezeBtn.setColour(juce::TextButton::textColourOnId,   juce::Colours::white);
    }

    void timerCallback() override
    {
        updateTransportButtons();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourcesTabComponent)
};
