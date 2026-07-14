/**
 * @file SourcesTabComponent.h
 * @brief ZONE 3 (PLAY face) — SP3CTRA source MODULE transport.
 *
 * This is the SP3CTRA (live CIS) SOURCE MODULE's own transport — play / hold /
 * stop + Fade-In — and it is a property of the MODULE, not of the chain it
 * sits in. Both source blocks (chain 1 / chain 2) drive the SAME global
 * SP3CTRA transport (imageFreezeMode) and the SAME Fade-In (imageFadeInMs), so
 * the fade on play/pause/stop is chain-independent. The chain index only tints
 * the header so you can tell which block you selected.
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

class SourcesTabComponent : public juce::Component,
                            private juce::Timer
{
public:
    explicit SourcesTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        playBtn.setIconPath(Icons::play());
        holdBtn.setIconPath(Icons::pause());
        stopBtn.setIconPath(Icons::stop());

        playBtn.onClick = [this]{ setFreezeMode(0.f);  };
        holdBtn.onClick = [this]{ setFreezeMode(0.5f); };
        stopBtn.onClick = [this]{ setFreezeMode(1.f);  };

        addAndMakeVisible(playBtn);
        addAndMakeVisible(holdBtn);
        addAndMakeVisible(stopBtn);

        // Fade-In (ms) — SP3CTRA input source (Chain 2) only. The sampler
        // (Chain 1) has no transport fade, so this row is hidden there.
        fadeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        fadeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, kTextBoxW, kCtrlH);
        fadeSlider.setTextValueSuffix(" ms");
        fadeSlider.setNumDecimalPlacesToDisplay(0);
        addChildComponent(fadeSlider);   // visibility toggled per chain in setChain()

        // ── Acquisition speed (frame-advance brake) — GLOBAL source control ──
        // "Vitesse d'acquisition": brakes how often the live CIS line advances
        // the active frame (audio + visual).  Not per-chain — bound once to the
        // global acqGate* params (sample-and-hold between gate ticks).
        auto& apvts = processor.getAPVTS();

        acqModeCombo.addItem("Off",            1);
        acqModeCombo.addItem("Internal (LFO)", 2);
        acqModeCombo.addItem("DAW Sync",       3);
        addAndMakeVisible(acqModeCombo);
        acqModeAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "acqGateMode", acqModeCombo));
        acqModeCombo.onChange = [this]{ updateAcqEnabled(); };

        acqRateSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        acqRateSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, kTextBoxW, kCtrlH);
        acqRateSlider.setTextValueSuffix(" ms");
        addAndMakeVisible(acqRateSlider);
        acqRateAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "acqGateRateMs", acqRateSlider));
        // The attachment's setRange() (interval 0.1) forces 1-decimal display;
        // override AFTER attaching so the readout stays integer ms like Fade In.
        acqRateSlider.setNumDecimalPlacesToDisplay(0);

        for (auto* s : { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" })
            acqDivCombo.addItem(s, acqDivCombo.getNumItems() + 1);
        addAndMakeVisible(acqDivCombo);
        acqDivAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "acqGateSyncDiv", acqDivCombo));

        for (auto* s : { "/32", "/16", "/8", "/4", "/2", "x1", "x2", "x4" })
            acqMultDivCombo.addItem(s, acqMultDivCombo.getNumItems() + 1);
        addAndMakeVisible(acqMultDivCombo);
        acqMultDivAttach.reset(new juce::AudioProcessorValueTreeState::ComboBoxAttachment(
            apvts, "acqGateMultDiv", acqMultDivCombo));

        updateAcqEnabled();

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

        // Right-click MIDI Learn — the transport buttons share the SP3CTRA
        // freeze-mode param (one CC spans 0=play / mid=hold / 1=stop); the
        // Fade-In and acquisition rate are global (bound once, rebound cheap).
        learnAtts_.clear();
        auto& mm = processor.getMidiMap();
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, fadeSlider, "imageFadeInMs"));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, playBtn,       freezeParamId()));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, holdBtn,       freezeParamId()));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, stopBtn,       freezeParamId()));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, acqRateSlider, "acqGateRateMs"));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, acqModeCombo,    "acqGateMode"));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, acqDivCombo,     "acqGateSyncDiv"));
        learnAtts_.push_back(std::make_unique<MidiLearnAttachment>(mm, acqMultDivCombo, "acqGateMultDiv"));

        resized();   // fade row appears/disappears → re-flow the acquisition group
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

        // All row labels share one right-justified column (Fade In + Acquisition)
        // so every control lines up on the same left edge.
        auto rowLabel = [&] (const char* t, int rowY)
        {
            g.drawText(t, formX(), rowY, labelColW() - 8, kCtrlH,
                       juce::Justification::centredRight);
        };

        // Fade In label — always shown (SP3CTRA module transport fade).
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        g.setColour(juce::Colour(0xffd2d8e8));
        rowLabel("Fade In", fadeRowY());

        // ── Acquisition speed section header ────────────────────────────────────
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
        g.setColour(juce::Colour(0xff8aa0c0));
        g.drawText("ACQUISITION SPEED", formX(), acqHeaderY(), formW(), 14,
                   juce::Justification::centredLeft);

        // ── Acquisition row labels ──────────────────────────────────────────────
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        g.setColour(juce::Colour(0xffd2d8e8));
        const char* names[] = { "Mode", "Rate", "Div", "Mult/Div" };
        for (int i = 0; i < 4; ++i)
            rowLabel(names[i], acqRowY(i));
    }

    void resized() override
    {
        const int w = getWidth();
        constexpr int btnSz = Sp3ctraTheme::kIconBtnSize;
        constexpr int gap   = Sp3ctraTheme::kGap;

        // Transport row — centred.
        const int totalW = btnSz * 3 + gap * 2;
        const int startX = w / 2 - totalW / 2;
        playBtn.setBounds(startX,                  transportY(), btnSz, btnSz);
        holdBtn.setBounds(startX + btnSz + gap,    transportY(), btnSz, btnSz);
        stopBtn.setBounds(startX + 2*(btnSz+gap),  transportY(), btnSz, btnSz);

        // Unified form grid — every control fills the same column [ctrlX, +ctrlW].
        const int cx = ctrlX();
        const int cw = ctrlW();
        fadeSlider     .setBounds(cx, fadeRowY(), cw, kCtrlH);
        acqModeCombo   .setBounds(cx, acqRowY(0), cw, kCtrlH);
        acqRateSlider  .setBounds(cx, acqRowY(1), cw, kCtrlH);
        acqDivCombo    .setBounds(cx, acqRowY(2), cw, kCtrlH);
        acqMultDivCombo.setBounds(cx, acqRowY(3), cw, kCtrlH);
    }

private:
    Sp3ctraAudioProcessor& processor;
    int activeChain_ { 1 };

    IconTextButton playBtn, holdBtn, stopBtn;
    juce::Slider   fadeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fadeAttach;
    std::vector<std::unique_ptr<MidiLearnAttachment>> learnAtts_;

    // ── Acquisition speed (global frame-advance brake) ──────────────────────────
    juce::ComboBox acqModeCombo, acqDivCombo, acqMultDivCombo;
    juce::Slider   acqRateSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> acqModeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> acqDivAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> acqMultDivAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   acqRateAttach;

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

    // ── Unified form geometry ────────────────────────────────────────────────
    // One column grid shared by the Fade In row and the whole Acquisition group,
    // so labels and controls always line up.  Wider than the old 2/5 node: the
    // form spans the panel width (minus padding), capped so it stays readable.
    static constexpr int kPad      = 18;  // outer horizontal padding
    static constexpr int kFormMaxW = 460; // cap so the form doesn't stretch absurdly
    static constexpr int kLabelColW= 92;  // right-justified label column
    static constexpr int kColGap   = 12;  // gap between label column and control
    static constexpr int kCtrlH    = 24;  // control height (taller → easier to use)
    static constexpr int kTextBoxW = 68;  // slider value box width (Fade + Rate)
    static constexpr int kRowPitch = 32;  // row-to-row spacing

    int formW()    const { return juce::jmin(getWidth() - 2 * kPad, kFormMaxW); }
    int formX()    const { return (getWidth() - formW()) / 2; }
    int labelColW()const { return kLabelColW; }
    int ctrlX()    const { return formX() + kLabelColW + kColGap; }
    int ctrlW()    const { return formW() - kLabelColW - kColGap; }

    int transportY() const { return 24; }
    int fadeRowY()   const { return transportY() + Sp3ctraTheme::kIconBtnSize + 14; }
    // The Fade In row is always present (SP3CTRA module transport fade), with
    // the Acquisition group below it.
    int acqHeaderY() const { return fadeRowY() + kRowPitch + 6; }
    int acqRowY(int i) const { return acqHeaderY() + 22 + i * kRowPitch; }

    // Grey out controls that don't apply to the current gate mode.
    void updateAcqEnabled()
    {
        int mode = 0;
        if (auto* raw = processor.getAPVTS().getRawParameterValue("acqGateMode"))
            mode = juce::roundToInt(raw->load());
        acqRateSlider  .setEnabled(mode == 1);  // Internal (LFO) only
        acqDivCombo    .setEnabled(mode == 2);  // DAW Sync only
        acqMultDivCombo.setEnabled(mode != 0);  // Internal + DAW Sync
    }

    // ── Transport button state highlighting ───────────────────────────────────
    void updateTransportButtons()
    {
        static const juce::Colour kPlay  { 0xff2a6040 };
        static const juce::Colour kHold  { 0xff6040a0 };
        static const juce::Colour kStop  { 0xff5a2020 };
        static const juce::Colour kOff   { 0xff2a2a2a };
        static const juce::Colour kFgOn  = juce::Colours::white;
        static const juce::Colour kFgOff { 0xff888888 };

        int mode = 0;
        if (auto* raw = processor.getAPVTS().getRawParameterValue(freezeParamId()))
            mode = juce::roundToInt(raw->load());

        auto style = [](IconTextButton& btn, bool active, juce::Colour col)
        {
            btn.setColour(juce::TextButton::buttonColourId,   active ? col : kOff);
            btn.setColour(juce::TextButton::textColourOffId,  active ? kFgOn : kFgOff);
        };

        style(playBtn, mode == 0, kPlay);
        style(holdBtn, mode == 1, kHold);
        style(stopBtn, mode == 2, kStop);
    }

    void timerCallback() override
    {
        updateTransportButtons();
        updateAcqEnabled();   // reflect mode changes from presets / automation
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourcesTabComponent)
};
