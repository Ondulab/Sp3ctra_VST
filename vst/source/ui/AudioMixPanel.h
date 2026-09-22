/**
 * @file AudioMixPanel.h
 * @brief AUDIO MIX — bottom half of ZONE 4 (synth-split P2b).
 *
 * The global engines + MASTER as a vertical mixer, one strip each:
 *
 *   engine name
 *   PAN · M · S                        ← the ENGINE row, top of the strip
 *   ┌ main fader ┐ ┌send┐ ┌send┐ …
 *   │ ticks│bar│VU│ │name│ │name│      ← one THIN column per "→ ENGINE"
 *   │            │ │bar│VU │bar│VU        send placed in a chain, named
 *   │            │ │ M  │ │ M  │         after that chain: level · mute ·
 *   │   value    │ │ S  │ │ S  │         solo, VU glued to the bar
 *
 *   • M (engine)  — the engine's ENABLE param shown INVERTED (lit = off):
 *     muting an engine switches it OFF — anti-click fade, then zero CPU —
 *     the very truth the former power LED carried (rack, zone-3 header MUTE
 *     and any MIDI mapping on the enable keep agreeing). An engine that is
 *     off never solos.
 *   • S (engine)  — solo: every other FED engine is silenced (processBlock,
 *     ramped).
 *   • PAN         — stereo balance, rotary with its arc from centre; no
 *     value box (double-click types "L37" / "C" / "R100", hold recentres).
 *   • main fader  — the engine's volume (dB display, linear gain law) with
 *     the audio VU (post-volume peak, processBlock) glued to its bar.
 *   • send column — the send's pre-engine mix weight (…Out{slot}_volume,
 *     the `intensity` the staging mixers blend with); M = the send's power
 *     (…Out{slot}_enabled inverted — the rack LED's truth), S = solo among
 *     THIS engine's sends. Its VU is a LIGHT meter: RMS of the line the
 *     send last staged × its effective weight (Processor::sendMeter) — what
 *     that chain brings to the engine's feed. The column wears its CHAIN's
 *     identity colour (ChainIdentity — the rack pastille's amber/green/grey
 *     cycle), not the engine's: inside one strip the columns are told apart
 *     by the chain that feeds them, and its rotated name gets first call on
 *     the column height (the light VU, glued to the fader, takes what is
 *     left) — a clipped chain name is a send you cannot identify.
 *
 * Only engines with at least one send placed are shown (send-less engine =
 * strip hidden entirely; the audio side skips its render too: zero-CPU
 * contract). Columns follow the chain model live (4 Hz poll). This panel is
 * the ONLY place the per-send level / M / S live: the engine PLAY pages used
 * to repeat them as rows and no longer do (the rack tile LED still carries
 * the send's power). Clicking a strip body opens the ENGINE
 * page in ZONE 3 (editor callback). In MINI mode (ZONE 4 collapsed to its
 * 24 px band) only a vertical MASTER fader + its VU remain visible.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "ChainIdentity.h"
#include "ModuleCatalog.h"
#include "ModuleParamManifest.h"
#include "Sp3ctraBarSlider.h"
#include "Sp3ctraControls.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

class AudioMixPanel : public juce::Component,
                      private juce::Timer
{
public:
    static constexpr int kPreferredH = 236;

    /** Fired when a strip body is clicked — the editor shows the engine page. */
    std::function<void(ModuleType)> onEngineSelected;

    explicit AudioMixPanel(Sp3ctraAudioProcessor& p)
        : processor(p)
    {
        static const struct { ModuleType t; const char* volumeId;
                              const char* panId; const char* soloId; } kEngines[] = {
            { ModuleType::LuxStral, "luxstralVolume", "luxstralPan", "luxstralSolo" },
            { ModuleType::LuxSynth, "luxsynthVolume", "luxsynthPan", "luxsynthSolo" },
            { ModuleType::LuxWave,  "luxwaveVolume",  "luxwavePan",  "luxwaveSolo"  },
            { ModuleType::LuxGrain, "luxgrainVolume", "luxgrainPan", "luxgrainSolo" },
        };
        auto& apvts = p.getAPVTS();
        auto& mm    = p.getMidiMap();
        for (size_t i = 0; i < kNumEngines; ++i)
        {
            auto& s = strips[i];
            s.type   = kEngines[i].t;
            s.colour = moduleColour(kEngines[i].t);
            s.name   = moduleDisplayName(kEngines[i].t)
                           .removeCharacters(juce::String::fromUTF8("\xE2\x86\x92")).trim();

            // MUTE — the engine's enable shown INVERTED (lit = off). One
            // param, one truth with the rack and the zone-3 header MUTE; a
            // MIDI mapping learned here speaks "power" (ON = audible).
            const juce::String enableId = moduleEnableParam(kEngines[i].t);
            s.mute = std::make_unique<MiniToggle>("M", juce::Colour(kMuteOn), kEngineBtnFont);
            s.mute->setTooltip("Mute this engine (switches it off; its solo is ignored while muted)");
            addAndMakeVisible(*s.mute);
            bindInverted(apvts, enableId, *s.mute, s.muteAttach);
            s.muteLearn = std::make_unique<MidiLearnAttachment>(mm, *s.mute, enableId);

            // SOLO — mutes every other fed engine (processBlock, ramped).
            s.solo = std::make_unique<MiniToggle>("S", juce::Colour(kSoloOn), kEngineBtnFont);
            s.solo->setTooltip("Solo this engine (mutes the other engines)");
            addAndMakeVisible(*s.solo);
            s.soloAttach = std::make_unique<BtnAttach>(apvts, kEngines[i].soloId, *s.solo);
            s.soloLearn  = std::make_unique<MidiLearnAttachment>(mm, *s.solo, kEngines[i].soloId);

            // PAN — stereo balance as a rotary (arc grows from 12 o'clock), no
            // value box: the gesture contract (Sp3ctraGestureSlider) types the
            // value in a bubble on double-click ("L37" / "C" / "R100").
            s.pan = std::make_unique<Sp3ctraGestureSlider>();
            s.pan->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.pan->setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            s.pan->getProperties().set("rotaryFromCentre", true);
            s.pan->setScrollWheelEnabled(false);
            s.pan->setTooltip("Stereo balance (double-click: centre, hold: type)");
            addAndMakeVisible(*s.pan);
            s.panAttach = std::make_unique<SldAttach>(apvts, kEngines[i].panId, *s.pan);
            s.pan->setDoubleClickReturnValue(true, 0.0);
            setPanDisplay(*s.pan);
            s.panLearn = std::make_unique<MidiLearnAttachment>(mm, *s.pan, kEngines[i].panId);

            s.fader = std::make_unique<Sp3ctraGestureSlider>();
            initFader(*s.fader, true, s.colour, kMainBarW);
            s.faderAttach = std::make_unique<SldAttach>(apvts, kEngines[i].volumeId, *s.fader);
            setDbDisplay(*s.fader);
            s.learn = std::make_unique<MidiLearnAttachment>(mm, *s.fader, kEngines[i].volumeId);
        }

        // MASTER — output gain after all engines. No engine row, no page.
        auto& m = strips[kMasterIdx];
        m.type   = ModuleType::Sp3ctra;   // sentinel — never clickable
        m.colour = juce::Colour(0xffc9d4e0);
        m.name   = "MASTER";
        m.fader  = std::make_unique<Sp3ctraGestureSlider>();
        initFader(*m.fader, true, m.colour, kMainBarW);
        m.faderAttach = std::make_unique<SldAttach>(apvts, "masterVolume", *m.fader);
        setDbDisplay(*m.fader);
        m.learn = std::make_unique<MidiLearnAttachment>(mm, *m.fader, "masterVolume");

        // MINI (collapsed band): a bare vertical MASTER fader + VU strip.
        initFader(miniMaster, false, m.colour, 12);
        miniMaster.setTooltip("Master volume");
        addChildComponent(miniMaster);
        miniMasterAttach = std::make_unique<SldAttach>(apvts, "masterVolume", miniMaster);
        miniMasterLearn  = std::make_unique<MidiLearnAttachment>(mm, miniMaster, "masterVolume");

        refreshSends();     // build the send columns before the first paint
        startTimerHz(30);   // VU refresh (+ send list poll every ~8 ticks)
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
            drawVu(g, miniVuArea, strips[kMasterIdx].disp, strips[kMasterIdx].colour);
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

            // Strip body
            auto b = s.area.toFloat().reduced(1.f);
            g.setColour(s.colour.withAlpha(0.06f));
            g.fillRoundedRectangle(b, 4.f);
            g.setColour(s.selected ? s.colour.withAlpha(0.95f)
                                   : s.colour.withAlpha(0.28f));
            g.drawRoundedRectangle(b, 4.f, s.selected ? 1.5f : 1.f);

            // Name
            g.setColour(s.colour.brighter(0.35f));
            g.setFont(juce::Font(juce::FontOptions(9.5f)).boldened());
            g.drawText(s.name, s.nameArea, juce::Justification::centred, true);

            // Main VU (audio), glued to the fader bar
            drawVu(g, s.vuArea, s.disp, s.colour);

            // Send columns: the CHAIN's colour throughout — quiet backdrop,
            // vertical chain name, light VU.
            for (const auto& d : s.sends)
            {
                g.setColour(d->colour.withAlpha(0.07f));
                g.fillRoundedRectangle(d->area.toFloat(), 3.f);
                drawVerticalText(g, d->name, d->nameArea,
                                 d->colour.brighter(0.1f), kSendNameFont);
                drawVu(g, d->vuArea, d->disp, d->colour);

            }
        }
    }

    /** The VIDEO MIX -> AUDIO ceiling, drawn OVER the faders.
     *
     *  It has to be here and not in paint(): a JUCE child paints AFTER its
     *  parent, and the send fader fills its bar opaquely — a marker drawn in
     *  paint() disappears under it, with only the pixels that overhang the
     *  bar left showing (that was the first version, and it read as two
     *  green specks stuck to the bottom of the bar).
     *
     *  What it says: the bar is still the level the user set — the fader is
     *  never written — and the part the toile takes away is switched off
     *  (darkened) above a line in the VIDEO SCROLL colour. Bar full + line
     *  at the bottom = "you asked for 100%, the picture allows none". */
    void paintOverChildren(juce::Graphics& g) override
    {
        if (mini_) return;
        const auto tint = moduleColour(ModuleType::VideoScroll);
        for (size_t i = 0; i < kNumEngines; ++i)
            for (const auto& d : strips[i].sends)
            {
                if (d->videoW >= 0.995f || d->barArea.isEmpty() || d->fader == nullptr)
                    continue;
                const auto  bar = d->barArea.reduced(1, 1);
                const float v   = juce::jlimit(0.0f, 1.0f, (float) d->fader->getValue())
                                * d->videoW;
                const int   yy  = juce::jlimit(bar.getY(), bar.getBottom(),
                                      bar.getBottom()
                                        - juce::roundToInt(v * (float) bar.getHeight()));
                // What the toile takes away: switched off, not tinted — a
                // wash in the video colour would read as another value.
                g.setColour(juce::Colours::black.withAlpha(0.62f));
                g.fillRect(bar.getX(), bar.getY(), bar.getWidth(), yy - bar.getY());
                g.setColour(tint.withAlpha(0.95f));
                g.fillRect(bar.getX(), juce::jmin(yy, bar.getBottom() - 2),
                           bar.getWidth(), 2);
            }
    }

    void resized() override
    {
        if (mini_)
        {
            for (auto& s : strips)
                setStripVisible(s, false);
            miniMaster.setVisible(true);

            // Fader capped to a strip-like height and anchored at the bottom
            // of the band (where AUDIO MIX lives expanded) — a full-window
            // track reads as broken.
            const int w   = getWidth();
            const int vuW = 6;
            const int fh  = juce::jlimit(10, kMiniFaderMaxH, getHeight() - 8);
            const int fy  = getHeight() - 4 - fh;
            miniVuArea = { w - vuW - 1, fy + kBarIn, vuW, fh - 2 * kBarIn };
            miniMaster.setBounds(0, fy, juce::jmax(10, w - vuW - 2), fh);
            return;
        }

        miniMaster.setVisible(false);

        const int pad = 4;
        const int gap = 4;
        int nShown = 0, totalSends = 0;
        for (auto& s : strips)
            if (isShown(s)) { ++nShown; totalSends += (int) s.sends.size(); }

        // Natural widths first; when the panel is narrower, tighten the send
        // pitch, then the main column, down to their floors (past that the
        // rightmost strips clip — the zone-4 splitter is the fix).
        const int availW = getWidth() - 2 * pad - juce::jmax(0, nShown - 1) * gap;
        int sendPitch = kSendPitch, mainW = kMainW;
        auto totalFor = [&](int sp, int mw)
        {
            int t = 0;
            for (auto& s : strips)
                if (isShown(s)) t += stripWidthFor(s, sp, mw);
            return t;
        };
        if (totalSends > 0 && totalFor(sendPitch, mainW) > availW)
            sendPitch = juce::jmax(kSendPitchMin,
                                   sendPitch - (totalFor(sendPitch, mainW) - availW
                                                + totalSends - 1) / totalSends);
        if (nShown > 0 && totalFor(sendPitch, mainW) > availW)
            mainW = juce::jmax(kMainWMin,
                               mainW - (totalFor(sendPitch, mainW) - availW
                                        + nShown - 1) / nShown);
        const int sendW   = sendPitch - kSendGap;
        const int sendBar = sendW >= kSendW ? kSendBarW : juce::jmax(4, sendW - kSendVuW - 2);

        int x = pad;
        const int y = kHeaderH;
        const int h = getHeight() - y - pad;

        for (auto& s : strips)
        {
            if (! isShown(s))
            {
                s.area = s.nameArea = s.vuArea = {};
                setStripVisible(s, false);
                continue;
            }
            setStripVisible(s, true);

            const int sw = stripWidthFor(s, sendPitch, mainW);
            s.area = { x, y, sw, h };
            const int cx = x + kInset;
            const int cw = sw - 2 * kInset;
            int cy = y + 3;

            s.nameArea = { cx, cy, cw, 12 };
            cy += 13;

            // ENGINE row: PAN rotary, then M / S beside it. MASTER (no
            // controls) leaves the row empty so every fader starts level.
            if (s.pan != nullptr)
            {
                s.pan->setBounds(cx, cy, kPanD, kPanD);
                const int bw = juce::jlimit(10, kEngineBtnW, (cw - kPanD - 2 * 3) / 2);
                const int by = cy + (kPanD - kEngineBtnH) / 2;
                s.mute->setBounds(cx + kPanD + 3, by, bw, kEngineBtnH);
                s.solo->setBounds(cx + kPanD + 3 + bw + 3, by, bw, kEngineBtnH);
            }
            cy += kPanD + 3;

            // MAIN column: fader (ticks left of its bar, value box below), the
            // audio VU glued to the right of the bar — the LookAndFeel centres
            // the bar in the slider width, so the VU sits inside the slider's
            // bounds (its transparent right half) and shows through. The bar
            // itself is kBarIn shorter than the slider at both ends (JUCE's
            // thumb indent): the VU spans exactly the bar, not the slider.
            const int fh = juce::jmax(40, y + h - cy - 3);
            s.fader->setBounds(cx, cy, mainW, fh);
            const int barX = cx + (mainW - kMainBarW) / 2;
            s.vuArea = { barX + kMainBarW + 1, cy + kBarIn, kMainVuW,
                         fh - kValH - 2 * kBarIn };

            // SEND columns: vertical name · thin fader + glued light VU · M · S
            int sx = cx + mainW + kColGap;
            for (auto& d : s.sends)
            {
                // Name · fader+VU · M · S. The rotated name is served first
                // (its target height, down to its floor on a short panel);
                // the fader — and the light VU glued to it — take the rest.
                const int soloY = cy + fh - kSendBtnH;
                const int muteY = soloY - kSendBtnH - 2;
                const int nameH = juce::jlimit(kSendNameMinH, kSendNameH,
                                               muteY - 3 - cy - kSendFaderMinH);
                d->area     = { sx, cy, sendW, fh };
                d->nameArea = { sx, cy, sendW, nameH };
                const int fy    = cy + nameH + 2;
                const int sfh   = juce::jmax(20, muteY - 3 - fy);
                d->fader->getProperties().set("faderBarW", sendBar);
                d->fader->setBounds(sx, fy, sendW, sfh);
                // barArea = the bar the LookAndFeel really draws (kBarIn
                // inside the slider top/bottom). The VIDEO ceiling overlay
                // darkens barArea's interior: sized from the slider instead,
                // it painted over the bar's top outline and left the column
                // open at the top. The light VU spans the same extent.
                const int sbx = sx + (sendW - sendBar) / 2;
                d->barArea = { sbx, fy + kBarIn, sendBar, sfh - 2 * kBarIn };
                d->vuArea = { sbx + sendBar + 1, fy + kBarIn, kSendVuW, sfh - 2 * kBarIn };
                d->mute->setBounds(sx, muteY, sendW, kSendBtnH);
                d->solo->setBounds(sx, soloY, sendW, kSendBtnH);
                sx += sendPitch;
            }

            x += sw + gap;
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
    static constexpr int   kHeaderH       = 24;
    static constexpr int   kMiniFaderMaxH = 180;   // mini MASTER fader cap
    static constexpr int   kInset         = 3;     // strip frame → content
    static constexpr int   kBarIn         = Sp3ctraTheme::kVFaderInset;   // slider → its bar (top/bottom)
    static constexpr int   kMainW         = 52;    // fader column: tick labels · bar · VU
    static constexpr int   kMainWMin      = 44;
    static constexpr int   kMainBarW      = 10;
    static constexpr int   kMainVuW       = 5;
    static constexpr int   kValW          = 40;    // fader value box
    static constexpr int   kValH          = 15;
    static constexpr int   kPanD          = 26;    // PAN rotary diameter = engine row height
    static constexpr int   kEngineBtnW    = 18;
    static constexpr int   kEngineBtnH    = 14;
    static constexpr float kEngineBtnFont = 9.5f;
    static constexpr int   kColGap        = 4;     // main column → first send column
    static constexpr int   kSendW         = 17;    // send column = bar + glued VU
    static constexpr int   kSendBarW      = 7;
    static constexpr int   kSendVuW       = 4;
    static constexpr int   kSendGap       = 3;
    static constexpr int   kSendPitch     = kSendW + kSendGap;
    static constexpr int   kSendPitchMin  = 14;
    static constexpr int   kSendNameH     = 46;    // vertical chain name (target)
    static constexpr int   kSendNameMinH  = 24;    //   …its floor on a short panel
    static constexpr int   kSendFaderMinH = 56;    // fader + glued VU keep at least this
    static constexpr float kSendNameFont  = 8.5f;
    static constexpr int   kSendBtnH      = 11;
    static constexpr float kSendBtnFont   = 8.0f;
    static constexpr juce::uint32 kMuteOn = 0xffe05548;
    static constexpr juce::uint32 kSoloOn = 0xffe8b64a;

    //── Compact lettered toggle (M / S) — a TextButton's text inset would
    //   squash a single letter at these widths, so it paints itself: state
    //   colour when lit, quiet chrome otherwise, outline on the interaction
    //   ladder (pointer or MIDI heat → the control colour). ─────────────────
    struct MiniToggle : juce::Button
    {
        MiniToggle(const juce::String& letter, juce::Colour on, float fontPx)
            : juce::Button(letter), letter_(letter), on_(on), fontPx_(fontPx)
        {
            setClickingTogglesState(true);
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto r  = getLocalBounds().toFloat().reduced(0.5f);
            const bool on = getToggleState();
            g.setColour(on ? on_.withMultipliedBrightness(down ? 0.75f : 1.0f)
                           : juce::Colour(0xff20202a).withMultipliedBrightness(
                                 down ? 0.8f : over ? 1.3f : 1.0f));
            g.fillRoundedRectangle(r, 2.5f);

            const float lit = juce::jmax(over ? 1.0f : 0.0f, Sp3ctraControls::heatOf(*this));
            g.setColour(juce::Colour(Sp3ctraTheme::kColBorder)
                            .interpolatedWith(Sp3ctraControls::active(), lit));
            g.drawRoundedRectangle(r, 2.5f, 1.0f);

            g.setColour(juce::Colours::white.withAlpha(on ? 1.0f : 0.72f));
            g.setFont(juce::Font(juce::FontOptions(fontPx_)).boldened());
            g.drawText(letter_, getLocalBounds(), juce::Justification::centred, false);
        }

        juce::String letter_;
        juce::Colour on_;
        float        fontPx_;
    };

    using SldAttach = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BtnAttach = juce::AudioProcessorValueTreeState::ButtonAttachment;

    /** One "→ ENGINE" send of an engine: a thin column of the strip. */
    struct SendStrip
    {
        int          chainIdx { 0 }, slot { 0 };
        juce::String name;
        juce::Colour colour;          // the chain's identity (ChainIdentity)
        float        disp { 0.0f };   // displayed light level [0..1]
        float        videoW { 1.0f };  // VIDEO MIX -> AUDIO mask, last drawn
        juce::Rectangle<int> area, nameArea, vuArea, barArea;
        std::unique_ptr<juce::Slider> fader;
        std::unique_ptr<MiniToggle>   mute, solo;
        std::unique_ptr<SldAttach>                volAttach;
        std::unique_ptr<juce::ParameterAttachment> muteAttach;   // inverted view
        std::unique_ptr<BtnAttach>                soloAttach;
        std::unique_ptr<MidiLearnAttachment>      volLearn, muteLearn, soloLearn;
    };

    struct Strip
    {
        ModuleType   type { ModuleType::Sp3ctra };
        juce::Colour colour;
        juce::String name;
        bool         selected { false };
        float        disp     { 0.0f };   // displayed VU level [0..1]
        juce::Rectangle<int> area, nameArea, vuArea;

        std::unique_ptr<MiniToggle>   mute, solo;   // engines only
        std::unique_ptr<juce::Slider> pan;          // engines only
        std::unique_ptr<juce::Slider> fader;
        std::unique_ptr<SldAttach>                 faderAttach, panAttach;
        std::unique_ptr<juce::ParameterAttachment> muteAttach;   // enable, inverted
        std::unique_ptr<BtnAttach>                 soloAttach;
        std::unique_ptr<MidiLearnAttachment>       learn, panLearn, muteLearn, soloLearn;
        std::vector<std::unique_ptr<SendStrip>>    sends;
    };

    bool isShown(const Strip& s) const noexcept
    {
        return &s == &strips[kMasterIdx] || ! s.sends.empty();
    }

    int stripWidthFor(const Strip& s, int sendPitch, int mainW) const noexcept
    {
        const int n = (int) s.sends.size();
        return 2 * kInset + mainW + (n > 0 ? kColGap + n * sendPitch - kSendGap : 0);
    }

    static void setStripVisible(Strip& s, bool v)
    {
        if (s.fader) s.fader->setVisible(v);
        if (s.pan)   s.pan  ->setVisible(v);
        if (s.mute)  s.mute ->setVisible(v);
        if (s.solo)  s.solo ->setVisible(v);
        for (auto& d : s.sends)
        {
            d->fader->setVisible(v);
            d->mute ->setVisible(v);
            d->solo ->setVisible(v);
        }
    }

    void initFader(juce::Slider& s, bool withTextBox, juce::Colour accent, int barW)
    {
        s.setSliderStyle(juce::Slider::LinearVertical);
        // Sp3ctraBarSlider::setAccent's colour scheme — the LookAndFeel's
        // LinearVertical branch reads these ids to draw the thin DC-block bar
        // in the strip's own colour ("faderBarW" = the bar's width).
        s.setColour(juce::Slider::trackColourId,             accent.withAlpha(0.22f));
        s.setColour(juce::Slider::backgroundColourId,        juce::Colour(0xff181820));
        s.setColour(juce::Slider::textBoxTextColourId,       juce::Colours::white.withAlpha(0.92f));
        s.setColour(juce::Slider::textBoxOutlineColourId,    accent.withAlpha(0.3f));
        s.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff181820));
        if (withTextBox)
            s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, kValW, kValH);
        else
            s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);   // bubble editor
        // Graduations beside the track (Sp3ctraLookAndFeel LinearVertical
        // branch): every 10%, majors at 0/50/100 with their dB value. Only on
        // the main faders — the send columns and the mini MASTER are too
        // narrow, the labels would clip.
        if (withTextBox)
            s.getProperties().set("faderTicks", true);
        s.getProperties().set("faderBarW", barW);
        addAndMakeVisible(s);
    }

    /** Bind a toggle to a bool param shown INVERTED (lit = param off). The
     *  button owns the gesture (setValueAsCompleteGesture), the attachment
     *  the feedback — exactly the engine pages' send-MUTE contract. */
    static void bindInverted(juce::AudioProcessorValueTreeState& apvts,
                             const juce::String& paramId, juce::Button& btn,
                             std::unique_ptr<juce::ParameterAttachment>& out)
    {
        auto* param = apvts.getParameter(paramId);
        if (param == nullptr) { jassertfalse; return; }
        out = std::make_unique<juce::ParameterAttachment>(
            *param,
            [&btn](float v) { btn.setToggleState(v < 0.5f, juce::dontSendNotification); },
            apvts.undoManager);
        out->sendInitialUpdate();
        auto* att = out.get();
        btn.onClick = [&btn, att]
        { att->setValueAsCompleteGesture(btn.getToggleState() ? 0.0f : 1.0f); };
    }

    juce::String outParam(ModuleType t, int slot, const char* suffix) const
    {
        switch (t)
        {
            case ModuleType::LuxSynth: return lxOutParam(slot, suffix);
            case ModuleType::LuxWave:  return lwOutParam(slot, suffix);
            case ModuleType::LuxGrain: return lgOutParam(slot, suffix);
            default:                   return lsOutParam(slot, suffix);
        }
    }

    std::unique_ptr<SendStrip> buildSend(const Strip& s, int chainIdx, int slot,
                                         const juce::String& name)
    {
        auto& apvts = processor.getAPVTS();
        auto& mm    = processor.getMidiMap();
        auto d = std::make_unique<SendStrip>();
        d->chainIdx = chainIdx;
        d->slot     = slot;
        d->name     = name;
        d->colour   = ChainIdentity::colour(chainIdx);   // rack pastille cycle
        const auto id = [&](const char* sfx) { return outParam(s.type, slot, sfx); };

        // Level — the send's pre-engine mix weight. No value box (the bubble
        // editor types it), no ticks: the column is the bar and its VU.
        d->fader = std::make_unique<Sp3ctraGestureSlider>();
        initFader(*d->fader, false, d->colour, kSendBarW);
        d->fader->setTooltip("Send level of " + name + " into the " + s.name + " mix");
        d->volAttach = std::make_unique<SldAttach>(apvts, id("volume"), *d->fader);
        d->volLearn  = std::make_unique<MidiLearnAttachment>(mm, *d->fader, id("volume"));

        // MUTE — the send's power param shown INVERTED (lit = silent): the
        // rack tile LED and any existing MIDI mapping keep working.
        d->mute = std::make_unique<MiniToggle>("M", juce::Colour(kMuteOn), kSendBtnFont);
        d->mute->setTooltip("Mute this chain's send (its power; wins over solo)");
        addAndMakeVisible(*d->mute);
        bindInverted(apvts, id("enabled"), *d->mute, d->muteAttach);
        d->muteLearn = std::make_unique<MidiLearnAttachment>(mm, *d->mute, id("enabled"));

        // SOLO — silences the OTHER sends of this engine (mix weight zeroed).
        d->solo = std::make_unique<MiniToggle>("S", juce::Colour(kSoloOn), kSendBtnFont);
        d->solo->setTooltip("Solo this chain's send (mutes the engine's other chains)");
        addAndMakeVisible(*d->solo);
        d->soloAttach = std::make_unique<BtnAttach>(apvts, id("solo"), *d->solo);
        d->soloLearn  = std::make_unique<MidiLearnAttachment>(mm, *d->solo, id("solo"));
        return d;
    }

    /** Rebuild the send columns of every engine whose placed sends / chain
     *  names changed (chain model scan). Returns true when any strip moved. */
    bool refreshSends()
    {
        struct Sig { int chainIdx, slot; juce::String name; };
        bool changed = false;
        const auto& model = processor.getChainModel();
        for (size_t e = 0; e < kNumEngines; ++e)
        {
            auto& s = strips[e];
            std::vector<Sig> sig;
            for (int c = 0; c < model.numChains(); ++c)
                for (const auto& m : model.chains[(size_t) c].modules)
                    if (m.type == s.type && m.slot >= 0 && m.slot < 8)   // …Out{0..7} banks
                        sig.push_back({ c, m.slot, processor.chainDisplayName(c) });

            const bool same = sig.size() == s.sends.size()
                && std::equal(sig.begin(), sig.end(), s.sends.begin(),
                              [](const Sig& a, const std::unique_ptr<SendStrip>& b)
                              { return a.chainIdx == b->chainIdx && a.slot == b->slot
                                    && a.name == b->name; });
            if (same)
                continue;
            s.sends.clear();
            for (const auto& g : sig)
                s.sends.push_back(buildSend(s, g.chainIdx, g.slot, g.name));
            changed = true;
        }
        return changed;
    }

    /** PAN value speaks balance: "L37" / "C" / "R100" (bubble editor). Must
     *  run AFTER the SliderAttachment (same contract as setDbDisplay). */
    static void setPanDisplay(juce::Slider& s)
    {
        s.textFromValueFunction = [](double v)
        {
            const int p = juce::roundToInt(std::abs(v) * 100.0);
            if (p < 1) return juce::String("C");
            return (v < 0.0 ? juce::String("L") : juce::String("R"))
                 + juce::String(p);
        };
        s.valueFromTextFunction = [](const juce::String& t)
        {
            const auto up = t.trim().toUpperCase();
            if (up.startsWith("L")) return -up.substring(1).getDoubleValue() / 100.0;
            if (up.startsWith("R")) return  up.substring(1).getDoubleValue() / 100.0;
            if (up == "C")          return 0.0;
            return juce::jlimit(-1.0, 1.0, up.getDoubleValue() / 100.0);
        };
        s.updateText();
    }

    /** Value box speaks dB — the mixing convention: "0" at unity gain, "-6"
     *  at half, "-inf" at zero. One decimal, trailing ".0" trimmed (fits the
     *  tick labels' narrow gutter). Typing a dB value (with or without "dB")
     *  still works. The fader LAW stays linear in gain; only the display
     *  converts. Must run AFTER the SliderAttachment (which installs the
     *  parameter's own text conversion). */
    static void setDbDisplay(juce::Slider& s)
    {
        s.textFromValueFunction = [](double v)
        {
            if (v <= 1.0e-4) return juce::String("-inf");   // below -80 dB
            return juce::String(20.0 * std::log10(v), 1)
                       .trimCharactersAtEnd("0").trimCharactersAtEnd(".");
        };
        s.valueFromTextFunction = [](const juce::String& t)
        {
            const auto txt = t.trim().toLowerCase();
            if (txt.contains("inf")) return 0.0;
            const double db = txt.upToFirstOccurrenceOf("db", false, true)
                                 .trim().getDoubleValue();
            return juce::jlimit(0.0, 1.0, std::pow(10.0, db / 20.0));
        };
        s.updateText();
    }

    /** Text rotated -90° (reads bottom → top) filling `area` — the mixer
     *  idiom for a name on a column too narrow to hold it upright. */
    static void drawVerticalText(juce::Graphics& g, const juce::String& text,
                                 juce::Rectangle<int> area, juce::Colour c, float fontPx)
    {
        if (area.isEmpty()) return;
        juce::Graphics::ScopedSaveState ss(g);
        const auto ctr = area.getCentre().toFloat();
        g.addTransform(juce::AffineTransform::rotation(
            -juce::MathConstants<float>::halfPi, ctr.x, ctr.y));
        g.setColour(c);
        g.setFont(juce::Font(juce::FontOptions(fontPx)).boldened());
        g.drawText(text,
                   juce::Rectangle<float>((float) area.getHeight(), (float) area.getWidth())
                       .withCentre(ctr).toNearestInt(),
                   juce::Justification::centred, true);
    }

    /** Peak bar with a soft perceptual curve; red cap above 1.0 (clip). */
    static void drawVu(juce::Graphics& g, juce::Rectangle<int> r,
                       float level, juce::Colour accent)
    {
        if (r.isEmpty()) return;
        g.setColour(juce::Colour(0xff16161e));
        g.fillRect(r);
        g.setColour(juce::Colour(0xff2a2a34));
        g.drawRect(r, 1);

        if (level <= 0.001f) return;
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
        auto follow = [&dirty](float& disp, float target)
        {
            const float next = juce::jmax(target, disp * 0.88f);
            if (std::abs(next - disp) > 0.002f) { disp = next; dirty = true; }
        };
        for (size_t i = 0; i < strips.size(); ++i)
        {
            follow(strips[i].disp, lv[i]);
            if (i < kNumEngines)
                for (auto& d : strips[i].sends)
                {
                    follow(d->disp, processor.sendMeter((int) i, d->chainIdx, d->slot));
                    // The toile's mask on this chain (1 while the link is off
                    // or drives the picture instead) — its ceiling line.
                    const float vw = processor.chainVideoWeight(d->chainIdx);
                    if (std::abs(vw - d->videoW) > 0.002f) { d->videoW = vw; dirty = true; }
                }
        }

        // Send list poll (cheap; every ~8th tick ≈ 4 Hz). A change adds or
        // removes columns/strips → relayout, not just repaint.
        if (++tick_ % 8 == 0 && refreshSends())
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

    static constexpr size_t kNumEngines = 4;   // index = Processor::sendMeter engine
    static constexpr size_t kMasterIdx  = kNumEngines;
    std::array<Strip, kNumEngines + 1> strips;   // engines + MASTER

    Sp3ctraGestureSlider miniMaster;
    juce::Rectangle<int> miniVuArea;
    std::unique_ptr<SldAttach> miniMasterAttach;
    std::unique_ptr<MidiLearnAttachment> miniMasterLearn;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioMixPanel)
};
