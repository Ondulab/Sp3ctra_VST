/**
 * @file TimbreWaveformStrip.h
 * @brief Min/max overview of the LuxStral timbre sample with the live
 *        extraction cursor — click/drag = luxstralTimbrePos ("walk through
 *        the sample").
 *
 * The cursor shows the MODULE's actual scan position (it moves on its own
 * when the scan transport plays; the APVTS param only carries manual jumps
 * and MIDI/automation). The param listener performs the actual
 * re-extraction, coalesced on the processor's 30 ms drain.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../UITheme.h"
#include "../synthesis/luxstral/luxstral_wavetable.h"

class TimbreWaveformStrip : public juce::Component, private juce::Timer
{
public:
    explicit TimbreWaveformStrip(juce::AudioProcessorValueTreeState& s) : apvts(s)
    {
        startTimerHz(15);
    }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff141820));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(juce::Colours::darkgrey.withAlpha(0.4f));
        g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, 1.0f);

        static thread_local float mm[LUXSTRAL_WT_OVERVIEW_PAIRS * 2];
        if (! luxstral_wavetable_get_overview(mm))
        {
            g.setColour(juce::Colours::grey);
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
            g.drawText("scan strip - load a sample to walk through it",
                       getLocalBounds(), juce::Justification::centred);
            return;
        }

        const float w = (float) getWidth(), h = (float) getHeight();
        const float mid = h * 0.5f;
        g.setColour(juce::Colours::lightblue.withAlpha(0.75f));
        for (int x = 0; x < getWidth(); ++x)
        {
            const int p = juce::jlimit(0, LUXSTRAL_WT_OVERVIEW_PAIRS - 1,
                                       (int) ((float) x / w * LUXSTRAL_WT_OVERVIEW_PAIRS));
            const float mn = mm[2 * p], mx = mm[2 * p + 1];
            g.drawVerticalLine(x, mid - mx * mid * 0.92f, mid - mn * mid * 0.92f);
        }

        // Live playhead — the module's actual extraction point.
        {
            const float cx = luxstral_wavetable_get_position() * (w - 1.0f);
            g.setColour(juce::Colours::orange);
            g.fillRect(cx - 1.0f, 0.0f, 2.0f, h);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        // Right-click is the MIDI-learn gesture (MidiLearnAttachment popup) —
        // never treat it as a scrub.
        if (e.mods.isPopupMenu())
            return;
        if (auto* p = apvts.getParameter("luxstralTimbrePos"))
            p->beginChangeGesture();
        applyDrag(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            applyDrag(e);
    }
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        if (auto* p = apvts.getParameter("luxstralTimbrePos"))
            p->endChangeGesture();
    }

private:
    void applyDrag(const juce::MouseEvent& e)
    {
        if (! luxstral_wavetable_has_sample())
            return;
        const float v = juce::jlimit(0.0f, 1.0f,
                                     (float) e.x / (float) juce::jmax(1, getWidth()));
        if (auto* p = apvts.getParameter("luxstralTimbrePos"))
            p->setValueNotifyingHost(v);   // 0..1 range: normalized == plain
        repaint();
    }

    void timerCallback() override
    {
        // Follow the LIVE playhead (transport, automation, MIDI) and reflect
        // a fresh load or clear.
        const float v = luxstral_wavetable_get_position();
        const int have = luxstral_wavetable_has_sample();
        if (v != lastPos || have != lastHave)
        {
            lastPos = v;
            lastHave = have;
            repaint();
        }
    }

    juce::AudioProcessorValueTreeState& apvts;
    float lastPos = -1.0f;
    int   lastHave = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimbreWaveformStrip)
};
