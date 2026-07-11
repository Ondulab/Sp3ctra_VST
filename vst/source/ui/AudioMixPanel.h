/**
 * @file AudioMixPanel.h
 * @brief AUDIO MIX — bottom half of ZONE 4 (synth-split P2b).
 *
 * The three global engines + MASTER as a vertical mixer: one strip each with
 *
 *   ● power LED · engine name · sends count
 *   VU meter (post-volume peak, fed by processBlock) + vertical fader
 *
 * Clicking a strip body opens the ENGINE page in ZONE 3 (editor callback);
 * a strip with zero sends renders dimmed. In MINI mode (ZONE 4 collapsed to
 * its 24 px band) only a vertical MASTER fader + its VU remain visible.
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
        };
        for (int i = 0; i < 3; ++i)
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
            s.learn = std::make_unique<MidiLearnAttachment>(p.getMidiMap(),
                *s.fader, kEngines[i].volumeId);
        }

        // MASTER — output gain after all engines. No LED, no engine page.
        auto& m = strips[3];
        m.type   = ModuleType::Sp3ctra;   // sentinel — never clickable
        m.colour = juce::Colour(0xffc9d4e0);
        m.name   = "MASTER";
        m.fader  = std::make_unique<juce::Slider>();
        initFader(*m.fader, true);
        m.faderAttach = std::make_unique<SldAttach>(processor.getAPVTS(),
                                                    "masterVolume", *m.fader);
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
            s.selected = selected && s.type == t && &s != &strips[3];
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
            drawVu(g, miniVuArea, strips[3].disp, strips[3].colour, true);
            return;
        }

        // Header badge
        g.setColour(juce::Colour(0xff5a9de0));
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("AUDIO MIX", 8, 0, getWidth() - 16, kHeaderH,
                   juce::Justification::centredLeft, false);

        for (auto& s : strips)
        {
            const bool isMaster = (&s == &strips[3]);
            const bool fed      = isMaster || s.sendCount > 0;
            const float alpha   = fed ? 1.0f : 0.45f;

            // Strip body
            auto b = s.area.toFloat().reduced(1.f);
            g.setColour(s.colour.withAlpha(0.06f * alpha));
            g.fillRoundedRectangle(b, 4.f);
            g.setColour(s.selected ? s.colour.withAlpha(0.95f)
                                   : s.colour.withAlpha(0.28f * alpha));
            g.drawRoundedRectangle(b, 4.f, s.selected ? 1.5f : 1.f);

            // Name (tiny, centred under the LED row)
            g.setColour(s.colour.brighter(0.35f).withAlpha(alpha));
            g.setFont(juce::Font(juce::FontOptions(9.5f)).boldened());
            g.drawText(s.name, s.nameArea, juce::Justification::centred, true);

            // Sends count under the name ("2 SEND" / "NO SEND"); master skips it.
            if (! isMaster)
            {
                g.setFont(juce::FontOptions(8.5f));
                g.setColour(fed ? s.colour.withAlpha(0.75f) : juce::Colour(0xff5a5a66));
                g.drawText(fed ? juce::String(s.sendCount) + " SEND" : juce::String("NO SEND"),
                           s.badgeArea, juce::Justification::centred, false);
            }

            // VU meter
            drawVu(g, s.vuArea, s.disp, s.colour, fed);
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

        const int pad  = 4;
        const int gap  = 4;
        const int stripW = juce::jmax(44, (getWidth() - 2 * pad - 3 * gap) / 4);
        int x = pad;
        int y = kHeaderH;
        const int h = getHeight() - y - pad;

        for (auto& s : strips)
        {
            s.area = { x, y, stripW, h };

            const int cx  = x + 3;
            const int cw  = stripW - 6;
            int cy = y + 4;

            const bool isMaster = (&s == &strips[3]);
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
        for (size_t i = 0; i < 3; ++i)   // master strip is not clickable
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
        const float lv[4] = { processor.meterLuxStral(), processor.meterLuxSynth(),
                              processor.meterLuxWave(),  processor.meterMaster() };
        bool dirty = false;
        for (size_t i = 0; i < strips.size(); ++i)
        {
            auto& s = strips[i];
            const float next = juce::jmax(lv[i], s.disp * 0.88f);
            if (std::abs(next - s.disp) > 0.002f) { s.disp = next; dirty = true; }
        }

        // Sends recount (cheap; every ~8th tick ≈ 4 Hz)
        if (++tick_ % 8 == 0)
        {
            const auto& model = processor.getChainModel();
            for (size_t i = 0; i < 3; ++i)
            {
                int n = 0;
                for (const auto& chain : model.chains)
                    for (const auto& mi : chain.modules)
                        if (mi.type == strips[i].type)
                            ++n;
                if (n != strips[i].sendCount) { strips[i].sendCount = n; dirty = true; }
            }
        }

        if (dirty)
            repaint(mini_ ? miniVuArea
                          : juce::Rectangle<int>(0, kHeaderH, getWidth(), getHeight() - kHeaderH));
    }

    Sp3ctraAudioProcessor& processor;
    bool mini_ { false };
    int  tick_ { 0 };

    std::array<Strip, 4> strips;   // LuxStral / LuxSynth / LuxWave / MASTER

    juce::Slider miniMaster;
    juce::Rectangle<int> miniVuArea;
    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SldAttach> miniMasterAttach;
    std::unique_ptr<MidiLearnAttachment> miniMasterLearn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioMixPanel)
};
