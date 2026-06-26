/**
 * @file WaveformSelectorComponent.h
 * @brief Waveform strip with a draggable export-region window (SCORE PLAY page).
 *
 * Shows the loaded WAV as a juce::AudioThumbnail. A highlighted window — whose
 * WIDTH equals the seconds of audio that fill one page at the current writing
 * speed + page format — can be dragged along the file to choose WHERE the score
 * is extracted from. Emits the new start offset (seconds) via onStartChange.
 */
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../UITheme.h"

class WaveformSelectorComponent : public juce::Component,
                                  private juce::ChangeListener
{
public:
    explicit WaveformSelectorComponent(juce::Colour accentColour)
        : accent(accentColour), thumbCache(1), thumb(512, formatManager, thumbCache)
    {
        formatManager.registerBasicFormats();
        thumb.addChangeListener(this);
    }
    ~WaveformSelectorComponent() override { thumb.removeChangeListener(this); }

    /** Called with the new start offset (seconds) when the window is moved. */
    std::function<void(double)> onStartChange;

    void setFile(const juce::File& f)
    {
        startSec = 0.0;
        if (f.existsAsFile()) thumb.setSource(new juce::FileInputSource(f));
        else                  thumb.setSource(nullptr);
        clampStart();
        repaint();
    }

    /** Window length (seconds) that fills one page; 0 ⇒ whole file. */
    void setWindowSeconds(double w) { windowSec = w; clampStart(); repaint(); }

    double getStartSeconds() const noexcept { return startSec; }
    void   setStartSeconds(double s) { startSec = s; clampStart(); repaint(); }

    /** Absolute playback position (seconds) to draw, or <0 to hide. */
    void setPlayhead(double absSec)
    {
        if (playheadSec != absSec) { playheadSec = absSec; repaint(); }
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        auto bf = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff15151c));
        g.fillRoundedRectangle(bf.reduced(0.5f), 4.0f);
        g.setColour(accent.withAlpha(0.18f));
        g.drawRoundedRectangle(bf.reduced(0.5f), 4.0f, 1.0f);

        const double total = totalSeconds();
        if (total <= 0.0)
        {
            g.setColour(juce::Colour(0xff55606f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
            g.drawText("Load a WAV to choose the export region",
                       getLocalBounds(), juce::Justification::centred);
            return;
        }

        auto area = getLocalBounds().reduced(3);
        const double win = windowLen(total);
        const float  selX = (float) area.getX() + (float) (startSec / total) * area.getWidth();
        const float  selW = (float) (win / total) * area.getWidth();
        juce::Rectangle<float> sel(selX, (float) area.getY(), selW, (float) area.getHeight());

        // Dim the unselected parts, highlight the window.
        g.setColour(juce::Colours::white.withAlpha(0.30f));
        thumb.drawChannels(g, area, 0.0, total, 1.0f);

        g.setColour(accent.withAlpha(0.22f));
        g.fillRect(sel);
        {
            juce::Graphics::ScopedSaveState ss(g);
            g.reduceClipRegion(sel.getSmallestIntegerContainer());
            g.setColour(juce::Colours::white.withAlpha(0.92f));
            thumb.drawChannels(g, area, 0.0, total, 1.0f);
        }

        // Playback head (source-audio preview).
        if (playheadSec >= 0.0 && playheadSec <= total)
        {
            const float px = (float) area.getX() + (float) (playheadSec / total) * area.getWidth();
            g.setColour(juce::Colours::white.withAlpha(0.9f));
            g.fillRect(px - 0.75f, (float) area.getY(), 1.5f, (float) area.getHeight());
        }

        // Window outline + grab handle.
        g.setColour(accent.withAlpha(0.95f));
        g.drawRect(sel, 1.5f);
        juce::Path tri;
        tri.addTriangle(selX, (float) area.getY(),
                        selX + 11.0f, (float) area.getY(),
                        selX, (float) area.getY() + 11.0f);
        g.fillPath(tri);

        // Readout: window length @ start.
        g.setColour(accent.withAlpha(0.85f));
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontTiny));
        g.drawText(juce::String(win, 1) + "s  @ " + juce::String(startSec, 1) + "s",
                   area.getRight() - 150, area.getY() + 1, 148, 11,
                   juce::Justification::right, false);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const double total = totalSeconds();
        if (total <= 0.0) return;
        auto area = getLocalBounds().reduced(3);
        const double win = windowLen(total);
        const float selX = (float) area.getX() + (float) (startSec / total) * area.getWidth();
        const float selW = (float) (win / total) * area.getWidth();
        // Click outside the window → jump-centre it on the cursor first.
        if (e.position.x < selX || e.position.x > selX + selW)
        {
            const double frac = (e.position.x - area.getX()) / juce::jmax(1, area.getWidth());
            startSec = frac * total - win * 0.5;
            clampStart();
        }
        dragStartSec = startSec;
        dragStartX   = e.position.x;
        notify();
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        const double total = totalSeconds();
        if (total <= 0.0) return;
        auto area = getLocalBounds().reduced(3);
        const double secPerPx = total / juce::jmax(1, area.getWidth());
        startSec = dragStartSec + (e.position.x - dragStartX) * secPerPx;
        clampStart();
        notify();
        repaint();
    }

private:
    double totalSeconds() const { return thumb.getTotalLength(); }
    double windowLen(double total) const
    {
        return (windowSec > 0.0 && windowSec < total) ? windowSec : total;
    }
    void clampStart()
    {
        const double total = totalSeconds();
        const double win = windowLen(total);
        startSec = juce::jlimit(0.0, juce::jmax(0.0, total - win), startSec);
    }
    void notify() { if (onStartChange) onStartChange(startSec); }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

    juce::Colour accent;
    juce::AudioFormatManager  formatManager;
    juce::AudioThumbnailCache thumbCache;
    juce::AudioThumbnail      thumb;
    double windowSec = 0.0;     // 0 ⇒ whole file
    double startSec  = 0.0;
    double playheadSec = -1.0;  // <0 ⇒ hidden
    double dragStartSec = 0.0;
    float  dragStartX   = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformSelectorComponent)
};
