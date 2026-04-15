/**
 * @file SourcesTabComponent.h
 * @brief Tab 1 — SOURCES: vertical pipeline flow RAW → S/L → M.
 *
 * Layout (top-to-bottom):
 *   Zone 1: RAW (upstream gate) — transport + fade
 *     ↓↓  arrows
 *   Zone 2: S (Sampler) + L (Live) side by side — transport + fade each
 *     ↓↓  arrows
 *   Zone 3: M (Mix) — opacity sliders + darken-blend node
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
          nodeRaw  ("RAW",     juce::Colour(0xff68788f), VisualizerMode::RAW),
          nodeSamp ("S",       juce::Colour(0xffe0b84a), VisualizerMode::SAMPLER),
          nodeLive ("L",       juce::Colour(0xff4ae0a0), VisualizerMode::LIVE),
          nodeMix  ("M",       juce::Colour(0xffa87ae0), VisualizerMode::MIX)
    {
        auto& apvts = p.getAPVTS();

        for (auto* n : { &nodeRaw, &nodeSamp, &nodeLive, &nodeMix })
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

        // ── Sampler transport ─────────────────────────────────────────────────
        smpPlayBtn.setIconPath(Icons::play());
        smpHoldBtn.setIconPath(Icons::pause());
        smpStopBtn.setIconPath(Icons::stop());

        smpPlayBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("samplerFreezeMode")) param->setValueNotifyingHost(0.f); };
        smpHoldBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("samplerFreezeMode")) param->setValueNotifyingHost(0.5f); };
        smpStopBtn.onClick  = [&apvts]{ if (auto* param = apvts.getParameter("samplerFreezeMode")) param->setValueNotifyingHost(1.f); };

        addAndMakeVisible(smpPlayBtn);
        addAndMakeVisible(smpHoldBtn);
        addAndMakeVisible(smpStopBtn);

        initFadeSlider(smpFadeSlider);
        addAndMakeVisible(smpFadeSlider);
        smpFadeAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "samplerFadeInMs", smpFadeSlider));

        // ── Live transport ────────────────────────────────────────────────────
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

        // ── Mix balance crossfader ────────────────────────────────────────────
        // 0.0 = full Sampler, 0.5 = equal (center), 1.0 = full Live.
        // Magnetic centre snap at 0.5 ±0.03.
        addAndMakeVisible(mixBalanceSlider);
        mixBalanceSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        mixBalanceSlider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
        mixBalanceSlider.onValueChange = [this]
        {
            // Magnetic centre snap: if within ±0.03 of 0.5, snap to 0.5
            double v = mixBalanceSlider.getValue();
            if (std::abs(v - 0.5) < 0.03 && !mixBalanceSlider.isMouseButtonDown())
                mixBalanceSlider.setValue(0.5, juce::dontSendNotification);
        };
        mixBalanceAttach.reset(new juce::AudioProcessorValueTreeState::SliderAttachment(
            apvts, "imageMixBalance", mixBalanceSlider));

        // Default: MIX active
        nodeMix.setActive(true);
        updateEyeIndicator(VisualizerMode::MIX);
        updateAllTransportButtons();
        startTimer(200);
    }

    ~SourcesTabComponent() override { stopTimer(); }

    void setActiveMode(VisualizerMode m)
    {
        nodeRaw .setActive(m == VisualizerMode::RAW);
        nodeSamp.setActive(m == VisualizerMode::SAMPLER);
        nodeLive.setActive(m == VisualizerMode::LIVE);
        nodeMix .setActive(m == VisualizerMode::MIX);
        updateEyeIndicator(m);
    }

    void paint(juce::Graphics& g) override
    {
        const int w = getWidth();

        // ── Zone labels ──────────────────────────────────────────────────────
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));

        // Zone 1 header — centred over full width
        g.setColour(juce::Colour(0xff68788f));
        g.drawText("UDP Input", 0, 2, w, 14, juce::Justification::centred);

        // Zone 2 headers — centred over their respective halves
        const int z2Y = zone2Y();
        g.setColour(juce::Colour(0xffe0b84a));
        g.drawText("S - Sampler", 0, z2Y, w / 2, 14, juce::Justification::centred);
        g.setColour(juce::Colour(0xff4ae0a0));
        g.drawText("L - Live", w / 2, z2Y, w / 2, 14, juce::Justification::centred);

        // Zone 3 header
        const int z3Y = zone3Y();
        g.setColour(juce::Colour(0xffa87ae0));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
        g.drawText("M - Mix (darken-blend)", w / 4, z3Y, w / 2, 14,
                   juce::Justification::centred);

        // Mix crossfader labels: S ◄──────── ► L
        g.setColour(juce::Colour(0xffd2d8e8));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
        const int mixSlY = z3Y + 18;
        const int nw = stdNodeW();
        const int crossX = w / 2 - nw / 2;
        g.drawText("S",  crossX,          mixSlY, 20, 18, juce::Justification::centredLeft);
        g.drawText("L",  crossX + nw - 20, mixSlY, 20, 18, juce::Justification::centredRight);

        // Fade In labels (Synth-page style: kFontSettings, right-justified)
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        g.setColour(juce::Colour(0xffd2d8e8));
        // Zone 1
        g.drawText("Fade In", fadeLabel1X(), fadeLabelY(0), 50, 18, juce::Justification::centredRight);
        // Zone 2 — S
        g.drawText("Fade In", fadeLabel2X(0), fadeLabelY(1), 50, 18, juce::Justification::centredRight);
        // Zone 2 — L
        g.drawText("Fade In", fadeLabel2X(1), fadeLabelY(1), 50, 18, juce::Justification::centredRight);

    }

    void resized() override
    {
        const int w    = getWidth();
        constexpr int btnSz = Sp3ctraTheme::kIconBtnSize;
        constexpr int gap   = Sp3ctraTheme::kGap;
        constexpr int nodeH = Sp3ctraTheme::kControlH + 6;

        // ── Standard node width: all nodes same size ─────────────────────────
        const int stdNodeW = juce::jmin(w * 2 / 5, 360);

        // ── Zone 1: RAW (centred) ────────────────────────────────────────────
        const int rawX = w / 2 - stdNodeW / 2;
        nodeRaw.setBounds(rawX, 18, stdNodeW, nodeH);

        // Transport centred below node
        {
            const int tY = 18 + nodeH + 4;
            const int totalW = btnSz * 3 + gap * 2;
            const int startX = w / 2 - totalW / 2;
            rawPlayBtn.setBounds(startX,                  tY, btnSz, btnSz);
            rawHoldBtn.setBounds(startX + btnSz + gap,    tY, btnSz, btnSz);
            rawStopBtn.setBounds(startX + 2*(btnSz+gap),  tY, btnSz, btnSz);
        }

        // Fade slider
        {
            const int fY = fadeLabelY(0);
            const int fadeX = w / 2 - stdNodeW / 2;
            rawFadeSlider.setBounds(fadeX + 56, fY, stdNodeW - 56, 18);
        }

        // ── Zone 2: S (left) + L (right), each same stdNodeW ─────────────────
        const int z2 = zone2Y();
        const int z2NodeY = z2 + 16;
        // Left column centred in left half
        const int smpX = w / 4 - stdNodeW / 2;
        // Right column centred in right half
        const int liveX = w * 3 / 4 - stdNodeW / 2;

        nodeSamp.setBounds(smpX,  z2NodeY, stdNodeW, nodeH);
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
        placeTransport2(w / 4,     smpPlayBtn,  smpHoldBtn,  smpStopBtn);
        placeTransport2(w * 3 / 4, livePlayBtn, liveHoldBtn, liveStopBtn);

        // Fade sliders zone 2
        {
            const int fY = fadeLabelY(1);
            smpFadeSlider .setBounds(smpX  + 56, fY, stdNodeW - 56, 18);
            liveFadeSlider.setBounds(liveX + 56, fY, stdNodeW - 56, 18);
        }

        // ── Zone 3: Mix crossfader + node ────────────────────────────────────
        const int z3 = zone3Y();
        const int mixSlY = z3 + 18;
        const int crossX = w / 2 - stdNodeW / 2 + 20;  // after "S" label
        const int crossW = stdNodeW - 40;               // between labels
        mixBalanceSlider.setBounds(crossX, mixSlY, crossW, 20);

        nodeMix.setBounds(w / 2 - stdNodeW / 2, mixSlY + 26, stdNodeW, nodeH);
    }

private:
    Sp3ctraAudioProcessor& processor;

    // ── Nodes ─────────────────────────────────────────────────────────────────
    PipelineNodeComponent nodeRaw, nodeSamp, nodeLive, nodeMix;

    // ── RAW transport ─────────────────────────────────────────────────────────
    IconTextButton rawPlayBtn, rawHoldBtn, rawStopBtn;
    juce::Slider rawFadeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> rawFadeAttach;

    // ── Sampler transport ─────────────────────────────────────────────────────
    IconTextButton smpPlayBtn, smpHoldBtn, smpStopBtn;
    juce::Slider smpFadeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> smpFadeAttach;

    // ── Live transport ────────────────────────────────────────────────────────
    IconTextButton livePlayBtn, liveHoldBtn, liveStopBtn;
    juce::Slider liveFadeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> liveFadeAttach;

    // ── Mix balance crossfader ────────────────────────────────────────────────
    juce::Slider mixBalanceSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixBalanceAttach;

    // ── Layout helpers ────────────────────────────────────────────────────────
    // 3-zone vertical layout: each zone ~1/3 of height
    int zoneH()  const { return getHeight() / 3; }
    int zone2Y() const { return zoneH(); }
    int zone3Y() const { return zoneH() * 2; }


    // Fade label positions — aligned with standard node left edge
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
        nodeSamp.setShowEye(active == VisualizerMode::SAMPLER);
        nodeLive.setShowEye(active == VisualizerMode::LIVE);
        nodeMix .setShowEye(active == VisualizerMode::MIX);
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
        styleTransportGroup(smpPlayBtn,  smpHoldBtn,  smpStopBtn,  readMode("samplerFreezeMode"));
        styleTransportGroup(livePlayBtn, liveHoldBtn, liveStopBtn, readMode("imageFreezeMode"));
    }

    void timerCallback() override
    {
        updateAllTransportButtons();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourcesTabComponent)
};
