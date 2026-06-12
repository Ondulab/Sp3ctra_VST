/**
 * @file SourcesTabComponent.h
 * @brief Tab 1 — SOURCES: dual-channel pipeline view.
 *
 * Channel model (refactor "Modulated / Live"):
 *   • RAW       — upstream UDP gate (transport + fade) feeding both channels.
 *   • MOD (A)   — modulated channel : Live ► LuxSampler ► LuxPitch ► LuxMask.
 *                 Each insert auto-bypasses when inactive (sampler not playing,
 *                 shift = 0, mask opacity = 0).  Transport = sampler transport.
 *   • LIVE (B)  — direct live frame (transport = image transport).
 *
 * Both synthesis engines (LuxStral, LuxSynth + LuxWave) independently pick
 * which channel they consume via their own APVTS source parameter; nothing
 * else needs to be configured here.
 *
 * Layout (top-to-bottom):
 *   Zone 1: RAW
 *     ↓↓
 *   Zone 2: A — Modulated (left)  |  B — Live (right)
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../IconPaths.h"
#include "../sampler/TransportBarComponent.h"   // IconTextButton
#include "PipelineNodeComponent.h"
#include "VisualizerMode.h"
#include <functional>

class SourcesTabComponent : public juce::Component,
                            private juce::Timer
{
public:
    /** Callback fired when a pipeline node is clicked. */
    std::function<void(VisualizerMode)> onNodeClicked;

    explicit SourcesTabComponent(Sp3ctraAudioProcessor& p)
        : processor(p),
          nodeRaw ("RAW",                juce::Colour(0xff68788f), VisualizerMode::RAW),
          nodeMod ("A - Modulated",      juce::Colour(0xffe0b84a), VisualizerMode::MODULATED),
          nodeLive("B - Live",           juce::Colour(0xff4ae0a0), VisualizerMode::LIVE)
    {
        auto& apvts = p.getAPVTS();

        for (auto* n : { &nodeRaw, &nodeMod, &nodeLive })
        {
            addAndMakeVisible(n);
            n->onClick = [this](VisualizerMode m)
            {
                setActiveMode(m);  // also updates eye indicator
                if (onNodeClicked) onNodeClicked(m);
            };
        }

        // ── RAW transport ─────────────────────────────────────────────────────
        rawPlayBtn.setIconPath(Icons::play());
        rawHoldBtn.setIconPath(Icons::pause());
        rawStopBtn.setIconPath(Icons::stop());

        rawPlayBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("rawFreezeMode")) param->setValueNotifyingHost(0.f); };
        rawHoldBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("rawFreezeMode")) param->setValueNotifyingHost(0.5f); };
        rawStopBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("rawFreezeMode")) param->setValueNotifyingHost(1.f); };

        addAndMakeVisible(rawPlayBtn);
        addAndMakeVisible(rawHoldBtn);
        addAndMakeVisible(rawStopBtn);

        initFadeSlider(rawFadeSlider);
        addAndMakeVisible(rawFadeSlider);
        rawFadeAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "rawFadeInMs", rawFadeSlider));

        // ── Modulated (A) transport — drives the sampler stage of the chain ──
        modPlayBtn.setIconPath(Icons::play());
        modHoldBtn.setIconPath(Icons::pause());
        modStopBtn.setIconPath(Icons::stop());

        modPlayBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("samplerFreezeMode")) param->setValueNotifyingHost(0.f); };
        modHoldBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("samplerFreezeMode")) param->setValueNotifyingHost(0.5f); };
        modStopBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("samplerFreezeMode")) param->setValueNotifyingHost(1.f); };

        addAndMakeVisible(modPlayBtn);
        addAndMakeVisible(modHoldBtn);
        addAndMakeVisible(modStopBtn);

        initFadeSlider(modFadeSlider);
        addAndMakeVisible(modFadeSlider);
        modFadeAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "samplerFadeInMs", modFadeSlider));

        // ── Live (B) transport ────────────────────────────────────────────────
        livePlayBtn.setIconPath(Icons::play());
        liveHoldBtn.setIconPath(Icons::pause());
        liveStopBtn.setIconPath(Icons::stop());

        livePlayBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("imageFreezeMode")) param->setValueNotifyingHost(0.f); };
        liveHoldBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("imageFreezeMode")) param->setValueNotifyingHost(0.5f); };
        liveStopBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("imageFreezeMode")) param->setValueNotifyingHost(1.f); };

        addAndMakeVisible(livePlayBtn);
        addAndMakeVisible(liveHoldBtn);
        addAndMakeVisible(liveStopBtn);

        initFadeSlider(liveFadeSlider);
        addAndMakeVisible(liveFadeSlider);
        liveFadeAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "imageFadeInMs", liveFadeSlider));

        // Default: MODULATED channel highlighted (most common choice).
        nodeMod.setActive(true);
        updateEyeIndicator(VisualizerMode::MODULATED);
        updateAllTransportButtons();
        startTimer(200);
    }

    ~SourcesTabComponent() override { stopTimer(); }

    void setActiveMode(VisualizerMode m)
    {
        nodeRaw .setActive(m == VisualizerMode::RAW);
        nodeMod .setActive(m == VisualizerMode::MODULATED);
        nodeLive.setActive(m == VisualizerMode::LIVE);
        updateEyeIndicator(m);
    }

    void paint(juce::Graphics& g) override
    {
        const int w = getWidth();

        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));

        // Zone 1 header — centred over full width
        g.setColour(juce::Colour(0xff68788f));
        g.drawText("UDP Input", 0, 2, w, 14, juce::Justification::centred);

        // Zone 2 headers — centred over their respective halves
        const int z2Y = zone2Y();
        g.setColour(juce::Colour(0xffe0b84a));
        g.drawText("A - Modulated  (Live > Sampler > Pitch > Mask)",
                   0, z2Y, w / 2, 14, juce::Justification::centred);
        g.setColour(juce::Colour(0xff4ae0a0));
        g.drawText("B - Live  (direct)",
                   w / 2, z2Y, w / 2, 14, juce::Justification::centred);

        // Fade In labels (Synth-page style: kFontSettings, right-justified)
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        g.setColour(juce::Colour(0xffd2d8e8));
        g.drawText("Fade In", fadeLabel1X(), fadeLabelY(0), 50, 18, juce::Justification::centredRight);
        g.drawText("Fade In", fadeLabel2X(0), fadeLabelY(1), 50, 18, juce::Justification::centredRight);
        g.drawText("Fade In", fadeLabel2X(1), fadeLabelY(1), 50, 18, juce::Justification::centredRight);
    }

    void resized() override
    {
        const int w    = getWidth();
        constexpr int btnSz = Sp3ctraTheme::kIconBtnSize;
        constexpr int gap   = Sp3ctraTheme::kGap;
        constexpr int nodeH = Sp3ctraTheme::kControlH + 6;

        // Standard node width: all nodes same size.
        const int stdNodeW = juce::jmin(w * 2 / 5, 360);

        // ── Zone 1: RAW (centred) ────────────────────────────────────────────
        const int rawX = w / 2 - stdNodeW / 2;
        nodeRaw.setBounds(rawX, 18, stdNodeW, nodeH);

        {
            const int tY = 18 + nodeH + 4;
            const int totalW = btnSz * 3 + gap * 2;
            const int startX = w / 2 - totalW / 2;
            rawPlayBtn.setBounds(startX,                  tY, btnSz, btnSz);
            rawHoldBtn.setBounds(startX + btnSz + gap,    tY, btnSz, btnSz);
            rawStopBtn.setBounds(startX + 2*(btnSz+gap),  tY, btnSz, btnSz);
        }

        {
            const int fY = fadeLabelY(0);
            const int fadeX = w / 2 - stdNodeW / 2;
            rawFadeSlider.setBounds(fadeX + 56, fY, stdNodeW - 56, 18);
        }

        // ── Zone 2: A — Modulated (left)  |  B — Live (right) ────────────────
        const int z2 = zone2Y();
        const int z2NodeY = z2 + 16;
        const int modX  = w / 4     - stdNodeW / 2;
        const int liveX = w * 3 / 4 - stdNodeW / 2;

        nodeMod .setBounds(modX,  z2NodeY, stdNodeW, nodeH);
        nodeLive.setBounds(liveX, z2NodeY, stdNodeW, nodeH);

        auto placeTransport2 = [&](int centreX,
                                   IconTextButton& play, IconTextButton& hold, IconTextButton& stop)
        {
            const int tY = z2NodeY + nodeH + 4;
            const int totalW = btnSz * 3 + gap * 2;
            const int startX = centreX - totalW / 2;
            play.setBounds(startX,                  tY, btnSz, btnSz);
            hold.setBounds(startX + btnSz + gap,    tY, btnSz, btnSz);
            stop.setBounds(startX + 2*(btnSz+gap),  tY, btnSz, btnSz);
        };
        placeTransport2(w / 4,     modPlayBtn,  modHoldBtn,  modStopBtn);
        placeTransport2(w * 3 / 4, livePlayBtn, liveHoldBtn, liveStopBtn);

        {
            const int fY = fadeLabelY(1);
            modFadeSlider .setBounds(modX  + 56, fY, stdNodeW - 56, 18);
            liveFadeSlider.setBounds(liveX + 56, fY, stdNodeW - 56, 18);
        }
    }

private:
    Sp3ctraAudioProcessor& processor;

    // ── Nodes ─────────────────────────────────────────────────────────────────
    PipelineNodeComponent nodeRaw, nodeMod, nodeLive;

    // ── RAW transport ─────────────────────────────────────────────────────────
    IconTextButton rawPlayBtn, rawHoldBtn, rawStopBtn;
    juce::Slider rawFadeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> rawFadeAttach;

    // ── Modulated (A) transport — bound to samplerFreezeMode ──────────────────
    IconTextButton modPlayBtn, modHoldBtn, modStopBtn;
    juce::Slider modFadeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> modFadeAttach;

    // ── Live (B) transport — bound to imageFreezeMode ─────────────────────────
    IconTextButton livePlayBtn, liveHoldBtn, liveStopBtn;
    juce::Slider liveFadeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> liveFadeAttach;

    // ── Layout helpers ────────────────────────────────────────────────────────
    // 2-zone vertical layout (RAW on top, two-column A/B below).
    int zoneH()  const { return getHeight() / 2; }
    int zone2Y() const { return zoneH(); }

    int fadeLabel1X() const { return getWidth() / 2 - stdNodeW() / 2; }
    int fadeLabel2X(int col) const
    {
        const int nw = stdNodeW();
        return col == 0 ? (getWidth() / 4 - nw / 2)
                        : (getWidth() * 3 / 4 - nw / 2);
    }
    int stdNodeW() const { return juce::jmin(getWidth() * 2 / 5, 360); }

    int fadeLabelY(int zone) const
    {
        constexpr int nodeH = Sp3ctraTheme::kControlH + 6;
        constexpr int btnSz = Sp3ctraTheme::kIconBtnSize;
        if (zone == 0)
            return 18 + nodeH + 4 + btnSz + 4;
        return zone2Y() + 16 + nodeH + 4 + btnSz + 4;
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    static void initFadeSlider(juce::Slider& s)
    {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 16);
        s.setTextValueSuffix(" ms");
        s.setNumDecimalPlacesToDisplay(0);
    }

    void updateEyeIndicator(VisualizerMode active)
    {
        nodeRaw .setShowEye(active == VisualizerMode::RAW);
        nodeMod .setShowEye(active == VisualizerMode::MODULATED);
        nodeLive.setShowEye(active == VisualizerMode::LIVE);
    }

    // ── Transport button state highlighting ───────────────────────────────────
    static void styleTransportGroup(IconTextButton& play, IconTextButton& hold,
                                    IconTextButton& stop, int mode)
    {
        static const juce::Colour kPlay  { 0xff2a6040 };
        static const juce::Colour kHold  { 0xff6040a0 };
        static const juce::Colour kStop  { 0xff5a2020 };
        static const juce::Colour kOff   { 0xff2a2a2a };
        static const juce::Colour kFgOn  = juce::Colours::white;
        static const juce::Colour kFgOff { 0xff888888 };

        auto style = [](IconTextButton& btn, bool active, juce::Colour col)
        {
            btn.setColour(juce::TextButton::buttonColourId,   active ? col : kOff);
            btn.setColour(juce::TextButton::textColourOffId,  active ? kFgOn : kFgOff);
        };

        style(play, mode == 0, kPlay);
        style(hold, mode == 1, kHold);
        style(stop, mode == 2, kStop);
    }

    void updateAllTransportButtons()
    {
        auto& apvts = processor.getAPVTS();
        auto readMode = [&](const char* paramId) -> int
        {
            if (auto* raw = apvts.getRawParameterValue(paramId))
                return juce::roundToInt(raw->load());
            return 0;
        };

        styleTransportGroup(rawPlayBtn,  rawHoldBtn,  rawStopBtn,  readMode("rawFreezeMode"));
        styleTransportGroup(modPlayBtn,  modHoldBtn,  modStopBtn,  readMode("samplerFreezeMode"));
        styleTransportGroup(livePlayBtn, liveHoldBtn, liveStopBtn, readMode("imageFreezeMode"));
    }

    void timerCallback() override
    {
        updateAllTransportButtons();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourcesTabComponent)
};
