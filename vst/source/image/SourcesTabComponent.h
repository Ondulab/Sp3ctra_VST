/**
 * @file SourcesTabComponent.h
 * @brief ZONE 3 (PLAY face) — SOURCE CIS transport, contextual to its chain.
 *
 * Each SOURCE CIS block is bound to the chain it sits on.  Selecting it shows
 * ONLY that chain's transport (play / hold / stop + fade-in):
 *   • Chain 1  → sampler transport  (samplerFreezeMode / samplerFadeInMs)
 *   • Chain 2  → live frame transport (imageFreezeMode  / imageFadeInMs)
 *
 * The RAW upstream UDP gate is the instrument's own signal and is no longer
 * surfaced here — chains are migrating toward modular slots, so the source
 * view only exposes the transport of the chain it is dropped into.
 *
 * The active chain is set by the editor via setChain() on block selection.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../IconPaths.h"
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

        fadeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        fadeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 16);
        fadeSlider.setTextValueSuffix(" ms");
        fadeSlider.setNumDecimalPlacesToDisplay(0);
        addAndMakeVisible(fadeSlider);

        setChain(1);   // default; the editor re-sets this on block selection
        startTimer(200);
    }

    ~SourcesTabComponent() override { stopTimer(); }

    /** Bind the transport to chain 1 (Modulated) or chain 2 (Live). */
    void setChain(int chain)
    {
        activeChain_ = (chain == 2) ? 2 : 1;

        auto& apvts = processor.getAPVTS();
        fadeAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, fadeParamId(), fadeSlider));

        updateTransportButtons();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const int w = getWidth();

        // Chain header — centred, identity colour matching the rack.
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSmall)).boldened());
        g.setColour(activeChain_ == 1 ? juce::Colour(0xffe0b84a)
                                      : juce::Colour(0xff4ae0a0));
        g.drawText(activeChain_ == 1 ? "CHAIN 1" : "CHAIN 2",
                   0, 4, w, 14, juce::Justification::centred);

        // Fade In label (Synth-page style: kFontSettings, right-justified).
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        g.setColour(juce::Colour(0xffd2d8e8));
        g.drawText("Fade In", fadeX(), fadeY(), 50, 18, juce::Justification::centredRight);
    }

    void resized() override
    {
        const int w = getWidth();
        constexpr int btnSz = Sp3ctraTheme::kIconBtnSize;
        constexpr int gap   = Sp3ctraTheme::kGap;

        // Transport row — centred.
        const int tY = 24;
        const int totalW = btnSz * 3 + gap * 2;
        const int startX = w / 2 - totalW / 2;
        playBtn.setBounds(startX,                  tY, btnSz, btnSz);
        holdBtn.setBounds(startX + btnSz + gap,    tY, btnSz, btnSz);
        stopBtn.setBounds(startX + 2*(btnSz+gap),  tY, btnSz, btnSz);

        // Fade slider — under the transport, label to its left.
        fadeSlider.setBounds(fadeX() + 56, fadeY(), stdNodeW() - 56, 18);
    }

private:
    Sp3ctraAudioProcessor& processor;
    int activeChain_ { 1 };

    IconTextButton playBtn, holdBtn, stopBtn;
    juce::Slider   fadeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fadeAttach;

    // ── Per-chain APVTS parameter ids ───────────────────────────────────────────
    const char* freezeParamId() const noexcept
    {
        return activeChain_ == 1 ? "samplerFreezeMode" : "imageFreezeMode";
    }
    const char* fadeParamId() const noexcept
    {
        return activeChain_ == 1 ? "samplerFadeInMs" : "imageFadeInMs";
    }

    void setFreezeMode(float v)
    {
        if (auto* param = processor.getAPVTS().getParameter(freezeParamId()))
            param->setValueNotifyingHost(v);
    }

    // ── Layout helpers ──────────────────────────────────────────────────────────
    int stdNodeW() const { return juce::jmin(getWidth() * 2 / 5, 360); }
    int fadeX()    const { return getWidth() / 2 - stdNodeW() / 2; }
    int fadeY()    const
    {
        constexpr int btnSz = Sp3ctraTheme::kIconBtnSize;
        return 24 + btnSz + 8;
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
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourcesTabComponent)
};
