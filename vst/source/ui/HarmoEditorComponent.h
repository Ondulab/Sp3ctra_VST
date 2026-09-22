/**
 * @file HarmoEditorComponent.h
 * @brief Interactive scale-grid view for the LuxHarmo (SCALE) insert.
 *
 * Same visual idiom as ShapeEqComponent (X = frequency log axis, octave grid
 * + Hz labels over the instrument's default 8-octave range) but the drawn
 * object is the QUANTIZER COMB: one tooth per allowed degree of (root,
 * scale), tooth width = the Width param in semitones; the root degrees carry
 * the SELECTED-key marker (a lime Sp3ctraHandles node). Frame, caption and
 * hint are the shared ModuleEditorChrome.
 * A piano strip along the bottom gives the pitch-class orientation; clicking
 * anywhere sets the ROOT to the clicked pitch class (right-click = MIDI
 * Learn on the Root param).
 *
 * The teeth brighten while the bound pool instance is quantizing a stream
 * (harmo_active — same live overlay contract as the other FX editors).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>
#include "../UITheme.h"
#include "../midi/MidiLearnAttachment.h"
#include "../processing/lux_harmo.h"   // self-manages extern "C" linkage
#include "Sp3ctraControls.h"
#include "Sp3ctraHandles.h"
#include "ModuleEditorChrome.h"

class HarmoEditorComponent : public juce::Component,
                             private juce::Timer
{
public:
    static constexpr int kPreferredH = 120;

    HarmoEditorComponent(juce::AudioProcessorValueTreeState& apvtsIn,
                         juce::Colour accentColour)
        : apvts(apvtsIn), accent(accentColour)
    {
        setRepaintsOnMouseActivity(true);
        startTimerHz(30);
    }

    ~HarmoEditorComponent() override { stopTimer(); }

    void setMidiMap(MidiMappingEngine* m) noexcept { midiMap_ = m; }

    /** (Re)bind the view to one instance's bank and point the live overlay at
     *  that instance's pool slot. */
    void setInstance(int slot,
                     const juce::String& rootId,  const juce::String& scaleId,
                     const juce::String& widthId, const juce::String& strengthId)
    {
        slot_   = juce::jlimit(0, 7, slot);
        rootId_ = rootId;
        bind(root_,     rootId);
        bind(scale_,    scaleId);
        bind(width_,    widthId);
        bind(strength_, strengthId);
        repaint();
    }

    int preferredHeight() const noexcept { return kPreferredH; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        const auto bf = getLocalBounds().toFloat();
        ModuleChrome::drawFrame(g, bf, accent);

        const auto plot = plotArea();
        const LuxHarmoState& st = *lux_harmo_instance(slot_);
        const bool live = (st.config.enabled != 0 && st.harmo_active != 0);

        // Octave grid + Hz labels (default instrument range — informative,
        // the engine's grid is anchored on the configured axis).
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        for (int oct = 0; oct <= kOctaves; ++oct)
        {
            const float x = plot.getX()
                          + ((float) oct / (float) kOctaves) * plot.getWidth();
            g.setColour(juce::Colour(0x14ffffff));
            g.drawVerticalLine((int) x, plot.getY(), plot.getBottom());
            if (oct % 2 == 0)
            {
                const double f = kMinFreq * std::pow(2.0, (double) oct);
                const juce::String lbl = (f >= 1000.0)
                    ? juce::String(f / 1000.0, f >= 10000.0 ? 0 : 1) + "k"
                    : juce::String((int) std::lround(f));
                g.setColour(accent.withAlpha(0.4f));
                g.drawText(lbl, (int) x - 16, (int) stripArea().getBottom() + 1, 32, 10,
                           juce::Justification::centred, false);   // under the piano strip
            }
        }

        // The comb: one tooth per allowed degree over the 8-octave strip.
        const int      rootCls  = juce::jlimit(0, 11, (int) std::lround(root_.value));
        const uint16_t mask     = lux_harmo_scale_mask((int) std::lround(scale_.value));
        const float    pxPerSt  = plot.getWidth() / (float) (kOctaves * 12);
        const float    toothW   = juce::jmax(2.0f, width_.value * pxPerSt);
        const float    s        = juce::jlimit(0.0f, 1.0f, strength_.value / 100.0f);
        const float    topAlpha = (live ? 0.85f : 0.45f) * (0.35f + 0.65f * s);
        for (int n = 0; n <= kOctaves * 12; ++n)
        {
            const int cls = n % 12;                       // strip starts on a C
            if (((mask >> ((cls - rootCls + 12) % 12)) & 1u) == 0)
                continue;
            const float x = plot.getX() + (float) n * pxPerSt;
            const bool  isRoot = (cls == rootCls);
            g.setColour(accent.withAlpha(isRoot ? topAlpha : topAlpha * 0.55f));
            g.fillRect(x - toothW * 0.5f, plot.getY(), toothW, plot.getHeight());
        }

        // Piano strip (pitch-class orientation, black keys darker).
        const auto strip = stripArea();
        for (int n = 0; n < kOctaves * 12; ++n)
        {
            const int  cls   = n % 12;
            const bool black = (cls == 1 || cls == 3 || cls == 6
                             || cls == 8 || cls == 10);
            const float x0 = strip.getX() + (float) n * pxPerSt;
            g.setColour(black ? juce::Colour(0xff15151d) : juce::Colour(0xff3a3a48));
            g.fillRect(x0 + 0.5f, strip.getY(), pxPerSt - 1.0f, strip.getHeight());
            // The strip IS the root selector: the key under the pointer wears
            // the control colour, so what a click will do is visible before
            // the click (ui/Sp3ctraControls.h).
            if (cls == hoverCls_)
            {
                const auto look = Sp3ctraControls::stateOf(false, true);
                g.setColour(Sp3ctraControls::valueInk(look)
                                .withAlpha(Sp3ctraControls::fillAlpha(look)));
                g.fillRect(x0 + 0.5f, strip.getY(), pxPerSt - 1.0f, strip.getHeight());
            }
        }

        // Root markers — the SELECTED key, one per octave, at the foot of
        // the root teeth (lime handle node; painted after the strip so its
        // ring is never covered).
        if ((mask & 1u) != 0)
            for (int n = 0; n <= kOctaves * 12; ++n)
                if (n % 12 == rootCls)
                    Sp3ctraHandles::drawNode(g, { plot.getX() + (float) n * pxPerSt,
                                                  plot.getBottom() - 2.5f },
                                             Sp3ctraHandles::stateOf(false,
                                                                     rootCls == hoverCls_,
                                                                     true, root_.heat()),
                                             2.5f);

        // Caption + hint (shared chrome).
        ModuleChrome::drawCaption(g, bf, accent, "SCALE GRID");
        ModuleChrome::drawReadout(g, bf, juce::Colour(0xff55606f),
                                  "click a key to set the root", 1.0f);
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        const int cls = classAt(e.position.x);
        if (cls != hoverCls_) { hoverCls_ = cls; repaint(); }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoverCls_ >= 0) { hoverCls_ = -1; repaint(); }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        // Right-click: MIDI Learn on the Root param (the canvas IS the root
        // selector — Strength/Width learn from their sliders below).
        if (e.mods.isPopupMenu())
        {
            if (midiMap_ != nullptr && rootId_.isNotEmpty())
                MidiLearnPopup::show(*midiMap_, rootId_, this);
            return;
        }
        const int cls = classAt(e.position.x);
        if (cls >= 0) root_.setComplete((float) cls);
        repaint();
    }

private:
    // Default instrument span (C2 → ~16.7 kHz, 8 octaves) — label grid only.
    static constexpr double kMinFreq = 65.41;
    static constexpr int    kOctaves = 8;

    /** Pitch class under an x position (-1 outside the strip). */
    int classAt(float x) const noexcept
    {
        const auto plot = plotArea();
        const float pxPerSt = plot.getWidth() / (float) (kOctaves * 12);
        if (pxPerSt <= 0.0f) return -1;
        const int n = (int) std::lround((x - plot.getX()) / pxPerSt);
        if (n < 0 || n > kOctaves * 12) return -1;
        return ((n % 12) + 12) % 12;
    }

    juce::Rectangle<float> plotArea() const
    {
        return getLocalBounds().toFloat().reduced(6.0f)
                               .withTrimmedTop(12.0f).withTrimmedBottom(26.0f);
    }
    juce::Rectangle<float> stripArea() const
    {
        const auto p = plotArea();
        return { p.getX(), p.getBottom() + 1.0f, p.getWidth(), 12.0f };
    }

    //==========================================================================
    void timerCallback() override { if (isShowing()) repaint(); }

    //==========================================================================
    /** The shared parameter binding — ui/Sp3ctraControls.h. Besides the
     *  attachment and the mirrored value it carries the EDIT HEAT: any
     *  change, from this editor's drag, from the box below, from a MIDI CC
     *  or from automation, lights the handle that owns it. */
    using Bound = Sp3ctraControls::Bound;

    void bind(Bound& bnd, const juce::String& id)
    {
        bnd.bind(apvts, id, [this](float) { repaint(); });
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;
    int slot_ { 0 };   // pool slot of the bound instance (live overlay)
    juce::String rootId_;              // Root param id (learn popup)
    MidiMappingEngine* midiMap_ = nullptr;

    Bound root_, scale_, width_, strength_;
    int hoverCls_ = -1;   // pitch class under the pointer (-1 = none)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HarmoEditorComponent)
};
