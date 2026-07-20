/**
 * @file AudioMixPanel.h
 * @brief AUDIO MIX — bottom half of ZONE 4 (synth-split P2b).
 *
 * The global engines + MASTER as a vertical mixer: one strip each with
 *
 *   ● power LED · engine name · sends count
 *   VU meter (post-volume peak, fed by processBlock) + vertical fader
 *
 * Only engines with at least one OUT send placed in a chain are shown — a
 * send-less engine's strip is hidden entirely (it is also skipped by the
 * audio side: zero-CPU contract). Faders display 0..100 (percent), not the
 * raw 0..1 parameter value. Clicking a strip body opens the ENGINE page in
 * ZONE 3 (editor callback). In MINI mode (ZONE 4 collapsed to its 24 px
 * band) only a vertical MASTER fader + its VU remain visible.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "ModuleCatalog.h"
#include <array>
#include <functional>
#include <memory>

class AudioMixPanel : public juce::Component,
                      private juce::Timer
{
public:
    static constexpr int kPreferredH = 230;

    /** Fired when a strip body is clicked — the editor shows the engine page. */
    std::function<void(ModuleType)> onEngineSelected;

    explicit AudioMixPanel(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        static const struct { ModuleType t; const char* volumeId; } kEngines[] = {
            { ModuleType::LuxStral, "luxstralVolume" },
            { ModuleType::LuxSynth, "luxsynthVolume" },
            { ModuleType::LuxWave,  "luxwaveVolume"  },
            { ModuleType::LuxGrain, "luxgrainVolume" },
        };
        for (int i = 0; i < kNumEngines; ++i)
        {
            auto& s = strips[(size_t) i];
            s.type   = kEngines[i].t;
            s.colour = moduleColour(kEngines[i].t);
            s.name   = moduleDisplayName(kEngines[i].t)
                           .removeCharacters(juce::String::fromUTF8("\xE2\x86\x92")).trim();

            s.led = std::make_unique<LedButton>();
            s.led->colour = s.colour;
            s.led->setClickingTogglesState(true);
            addAndMakeVisible(s.led.get());
            s.ledAttach = std::make_unique<BtnAttach>(p.getAPVTS(),
                moduleEnableParam(kEngines[i].t), *s.led);

            s.fader = std::make_unique<juce::Slider>();
            initFader(*s.fader, true);
            s.faderAttach = std::make_unique<SldAttach>(p.getAPVTS(),
                kEngines[i].volumeId, *s.fader);
            setPercentDisplay(*s.fader);
            s.learn = std::make_unique<MidiLearnAttachment>(p.getMidiMap(),
                *s.fader, kEngines[i].volumeId);
        }

        // MASTER — output gain after all engines. No LED, no engine page.
        auto& m = strips[kMasterIdx];
        m.type   = ModuleType::Sp3ctra;   // sentinel — never clickable
        m.colour = juce::Colour(0xffc9d4e0);
        m.name   = "MASTER";
        m.fader  = std::make_unique<juce::Slider>();
        initFader(*m.fader, true);
        m.faderAttach = std::make_unique<SldAttach>(processor.getAPVTS(),
                                                    "masterVolume", *m.fader);
        setPercentDisplay(*m.fader);
        m.learn = std::make_unique<MidiLearnAttachment>(processor.getMidiMap(),
                                                        *m.fader, "masterVolume");

        // MINI (collapsed band): a bare vertical MASTER fader + VU strip.
        initFader(miniMaster, false);
        miniMaster.setTooltip("Master volume");
        addChildComponent(miniMaster);
        miniMasterAttach = std::make_unique<SldAttach>(processor.getAPVTS(),
                                                       "masterVolume", miniMaster);
        miniMasterLearn = std::make_unique<MidiLearnAttachment>(processor.getMidiMap(),
                                                                miniMaster, "masterVolume");

        recountSends();     // hide send-less strips from the first paint
        startTimerHz(30);   // VU refresh (+ sends recount every ~8 ticks)
    }

    /** MINI mode — hosted in the collapsed ZONE-4 band (master fader only). */
    void setMini(bool mini)
    {
        if (mini_ == mini) return;
        mini_ = mini;
        resized();
        repaint();
    }

    /** Highlight the strip whose engine page is open in ZONE 3 (or none). */
    void setSelectedEngine(ModuleType t, bool selected)
    {
        for (auto& s : strips)
            s.selected = selected && s.type == t && &s != &strips[kMasterIdx];
        repaint();
    }

    void clearSelection() { setSelectedEngine(ModuleType::Sp3ctra, false); }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0c0c10));

        if (mini_)
        {
            // Mini VU beside the master fader.
            drawVu(g, miniVuArea, strips[kMasterIdx].disp,
                   strips[kMasterIdx].colour, true);
            return;
        }

        // Header badge
        g.setColour(juce::Colour(0xff5a9de0));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("AUDIO MIX", 8, 0, getWidth() - 16, kHeaderH,
                   juce::Justification::centredLeft, false);

        for (auto& s : strips)
        {
            if (s.area.isEmpty())
                continue;   // send-less engine — strip hidden entirely
            const bool isMaster = (&s == &strips[kMasterIdx]);

            // Strip body
            auto b = s.area.toFloat().reduced(1.f);
            g.setColour(s.colour.withAlpha(0.06f));
            g.fillRoundedRectangle(b, 4.f);
            g.setColour(s.selected ? s.colour.withAlpha(0.95f)
                                   : s.colour.withAlpha(0.28f));
            g.drawRoundedRectangle(b, 4.f, s.selected ? 1.5f : 1.f);

            // Name (tiny, centred under the LED row)
            g.setColour(s.colour.brighter(0.35f));
            g.setFont(juce::Font(juce::FontOptions(9.5f)).boldened());
            g.drawText(s.name, s.nameArea, juce::Justification::centred, true);

            // Sends count under the name ("2 SEND"); master skips it.
            if (! isMaster)
            {
                g.setFont(juce::FontOptions(8.5f));
                g.setColour(s.colour.withAlpha(0.75f));
                g.drawText(juce::String(s.sendCount) + " SEND",
                           s.badgeArea, juce::Justification::centred, false);
            }

            // VU meter
            drawVu(g, s.vuArea, s.disp, s.colour, true);
        }
    }

    void resized() override
    {
        if (mini_)
        {
            for (auto& s : strips)
                if (s.fader) s.fader->setVisible(false);
            for (auto& s : strips)
                if (s.led) s.led->setVisible(false);
            miniMaster.setVisible(true);

            // Fader capped to a strip-like height and anchored at the bottom
            // of the band (where AUDIO MIX lives expanded) — a full-window
            // track reads as broken.
            const int w  = getWidth();
            const int vuW = 6;
            const int fh = juce::jlimit(10, kMiniFaderMaxH, getHeight() - 8);
            const int fy = getHeight() - 4 - fh;
            miniVuArea = { w - vuW - 1, fy, vuW, fh };
            miniMaster.setBounds(0, fy, juce::jmax(10, w - vuW - 2), fh);
            return;
        }

        miniMaster.setVisible(false);

        // Only strips with a send (plus MASTER) get laid out — hidden strips
        // keep an empty area (skipped by paint/mouseUp) and no visible child.
        auto isShown = [this](const Strip& s)
        {
            return &s == &strips[kMasterIdx] || s.sendCount > 0;
        };

        const int pad  = 4;
        const int gap  = 4;
        int nStrips = 0;
        for (auto& s : strips)
            if (isShown(s)) ++nStrips;
        const int stripW = juce::jmax(40, (getWidth() - 2 * pad
                                           - (nStrips - 1) * gap) / nStrips);
        int x = pad;
        int y = kHeaderH;
        const int h = getHeight() - y - pad;

        for (auto& s : strips)
        {
            if (! isShown(s))
            {
                s.area = s.nameArea = s.badgeArea = s.vuArea = {};
                if (s.led)   s.led->setVisible(false);
                if (s.fader) s.fader->setVisible(false);
                continue;
            }
            s.area = { x, y, stripW, h };

            const int cx  = x + 3;
            const int cw  = stripW - 6;
            int cy = y + 4;

            const bool isMaster = (&s == &strips[kMasterIdx]);
            if (s.led)
            {
                s.led->setVisible(true);
                s.led->setBounds(x + stripW / 2 - 6, cy, 12, 12);
            }
            cy += isMaster ? 0 : 14;
            s.nameArea  = { cx, cy, cw, 12 };
            cy += 12;
            s.badgeArea = { cx, cy, cw, 10 };
            cy += 12;

            // Fader (value box below, full width so it never ellipsizes) with
            // the VU bar along the strip's right edge. The vertical track
            // centres itself inside the slider width.
            const int fh  = juce::jmax(40, y + h - cy - 4);
            const int vuW = 8;
            const int fw  = juce::jmax(24, cw - vuW - 3);
            if (s.fader)
            {
                s.fader->setVisible(true);
                s.fader->setTextBoxStyle(juce::Slider::TextBoxBelow, false, fw, 16);
                s.fader->setBounds(cx, cy, fw, fh);
            }
            s.vuArea = { cx + fw + 3, cy + 2, vuW, fh - 16 - 6 };

            x += stripW + gap;
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (mini_) return;
        for (size_t i = 0; i < kNumEngines; ++i)   // master strip is not clickable
            if (strips[i].area.contains(e.getPosition()))
            {
                if (onEngineSelected) onEngineSelected(strips[i].type);
                return;
            }
    }

private:
    static constexpr int kHeaderH = 24;
    static constexpr int kMiniFaderMaxH = 180;   // mini MASTER fader cap

    //── Power LED — same visual language as the rack block dot ───────────────
    struct LedButton : juce::ToggleButton
    {
        juce::Colour colour { juce::Colours::white };
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            auto r = getLocalBounds().toFloat().reduced(1.5f);
            g.setColour(getToggleState() ? colour : colour.withAlpha(0.18f));
            g.fillEllipse(r);
            g.setColour(colour.withAlpha(over ? 0.9f : 0.5f));
            g.drawEllipse(r, 1.f);
        }
    };

    struct Strip
    {
        ModuleType   type { ModuleType::Sp3ctra };
        juce::Colour colour;
        juce::String name;
        bool         selected  { false };
        int          sendCount { 0 };
        float        disp      { 0.0f };   // displayed VU level [0..1]
        juce::Rectangle<int> area, nameArea, badgeArea, vuArea;

        std::unique_ptr<LedButton>    led;
        std::unique_ptr<juce::Slider> fader;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> faderAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> ledAttach;
        std::unique_ptr<MidiLearnAttachment> learn;
    };

    void initFader(juce::Slider& s, bool withTextBox)
    {
        s.setSliderStyle(juce::Slider::LinearVertical);
        if (withTextBox)
            s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 40, 15);
        else
            s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        addAndMakeVisible(s);
    }

    /** Value box shows 0..100 (percent), not the raw 0..1 param value. Must
     *  run AFTER the SliderAttachment (which installs the parameter's own
     *  text conversion). */
    static void setPercentDisplay(juce::Slider& s)
    {
        s.textFromValueFunction = [](double v)
        { return juce::String(juce::roundToInt(v * 100.0)); };
        s.valueFromTextFunction = [](const juce::String& t)
        { return juce::jlimit(0.0, 1.0, t.getDoubleValue() / 100.0); };
        s.updateText();
    }

    /** Pull per-engine OUT send counts from the processor (derived on every
     *  chain-model change). Returns true when any count changed. */
    bool recountSends()
    {
        const int n[kNumEngines] = { processor.sendsLuxStral(),
                                     processor.sendsLuxSynth(),
                                     processor.sendsLuxWave(),
                                     processor.sendsLuxGrain() };
        bool changed = false;
        for (size_t i = 0; i < kNumEngines; ++i)
            if (n[i] != strips[i].sendCount)
            {
                strips[i].sendCount = n[i];
                changed = true;
            }
        return changed;
    }

    /** Peak bar with a soft perceptual curve; red cap above 1.0 (clip). */
    static void drawVu(juce::Graphics& g, juce::Rectangle<int> r,
                       float level, juce::Colour accent, bool lit)
    {
        if (r.isEmpty()) return;
        g.setColour(juce::Colour(0xff16161e));
        g.fillRect(r);
        g.setColour(juce::Colour(0xff2a2a34));
        g.drawRect(r, 1);

        if (! lit || level <= 0.001f) return;
        const float shaped = juce::jlimit(0.0f, 1.0f, std::pow(juce::jmin(level, 1.0f), 0.5f));
        const int   hh     = juce::roundToInt(shaped * (float) (r.getHeight() - 2));
        auto bar = r.reduced(1).removeFromBottom(hh);
        g.setColour(accent.withAlpha(0.85f));
        g.fillRect(bar);
        if (level > 1.0f)   // clip indicator
        {
            g.setColour(juce::Colours::red);
            g.fillRect(r.reduced(1).removeFromTop(3));
        }
    }

    void timerCallback() override
    {
        // VU: read processor peaks, keep our own display release for smoothness.
        const float lv[5] = { processor.meterLuxStral(), processor.meterLuxSynth(),
                              processor.meterLuxWave(),  processor.meterLuxGrain(),
                              processor.meterMaster() };
        bool dirty = false;
        for (size_t i = 0; i < strips.size(); ++i)
        {
            auto& s = strips[i];
            const float next = juce::jmax(lv[i], s.disp * 0.88f);
            if (std::abs(next - s.disp) > 0.002f) { s.disp = next; dirty = true; }
        }

        // Sends recount (cheap; every ~8th tick ≈ 4 Hz). A change shows/hides
        // strips → relayout, not just repaint.
        if (++tick_ % 8 == 0 && recountSends())
        {
            resized();
            dirty = true;
        }

        if (dirty)
            repaint(mini_ ? miniVuArea
                          : juce::Rectangle<int>(0, kHeaderH, getWidth(), getHeight() - kHeaderH));
    }

    Sp3ctraAudioProcessor& processor;
    bool mini_ { false };
    int  tick_ { 0 };

    static constexpr size_t kNumEngines = 4;
    static constexpr size_t kMasterIdx  = kNumEngines;
    std::array<Strip, kNumEngines + 1> strips;   // engines + MASTER

    juce::Slider miniMaster;
    juce::Rectangle<int> miniVuArea;
    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SldAttach> miniMasterAttach;
    std::unique_ptr<MidiLearnAttachment> miniMasterLearn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioMixPanel)
};
