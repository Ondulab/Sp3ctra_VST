/**
 * @file WaveformSelectorComponent.h
 * @brief Waveform strip with a draggable export-region window (SCORE PLAY page).
 *
 * Shows the loaded WAV as a juce::AudioThumbnail. A highlighted window — whose
 * WIDTH equals the seconds of audio that fill one page at the current writing
 * speed + page format — can be dragged along the file to choose WHERE the score
 * is extracted from. Emits the new start offset (seconds) via onStartChange.
 *
 * Frame + readout = the shared ModuleEditorChrome; the grab triangle and the
 * free-mode edge grips are controls, painted lime by Sp3ctraHandles (the
 * accent stays on the window outline and its highlight).
 */
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <functional>
#include "../UITheme.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/Sp3ctraHandles.h"
#include "../ui/Sp3ctraGestures.h"

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

    /** Free-selection mode only: called with (start, length) in seconds when
     *  either edge is resized or the region is moved. */
    std::function<void(double, double)> onRegionChange;

    void setFile(const juce::File& f)
    {
        startSec = 0.0;
        // useFileTimeInHashGeneration=true: takes are re-recorded/re-synthesized
        // to the SAME path, and the thumbnail cache is keyed by this hash — a
        // path-only hash would keep showing the previous take's waveform.
        if (f.existsAsFile()) thumb.setSource(new juce::FileInputSource(f, true));
        else                  thumb.setSource(nullptr);
        clampStart();
        repaint();
    }

    /** Window length (seconds) that fills one page; 0 ⇒ whole file. In free
     *  mode this is the CURRENT selection length (edited by the edge drags). */
    void setWindowSeconds(double w) { windowSec = w; clampStart(); repaint(); }

    /** SCORE "Selection" sheet: the region edges become draggable — the left
     *  edge moves the start (right edge anchored), the right edge the length;
     *  dragging inside still moves the whole region. Fixed mode (A4/A3 pages,
     *  page-width window) is the historical behaviour. */
    void setFreeSelection(bool on)
    {
        if (freeSelection == on)
            return;
        freeSelection = on;
        clampStart();
        repaint();
    }

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
        const auto bf = getLocalBounds().toFloat();
        ModuleChrome::drawFrame(g, bf, accent);

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

        // Window outline (display) + lime grab triangle (control — white
        // rim while the window is being dragged).
        g.setColour(accent.withAlpha(0.95f));
        g.drawRect(sel, 1.5f);
        juce::Path tri;
        tri.addTriangle(selX, (float) area.getY(),
                        selX + 11.0f, (float) area.getY(),
                        selX, (float) area.getY() + 11.0f);
        const bool down = isMouseButtonDown();
        g.setColour(Sp3ctraHandles::colour());
        g.fillPath(tri);
        if (down && dragMode == DragMode::move)
        {
            g.setColour(Sp3ctraHandles::hot());
            g.strokePath(tri, juce::PathStrokeType(1.2f));
        }

        // Free mode: edge grips (lime thumbs) advertise that the region is
        // resizable — hover / drag states per edge.
        if (freeSelection)
        {
            const float gy = sel.getCentreY();
            Sp3ctraHandles::drawThumb(g, { sel.getX() - 2.0f, gy - 9.0f, 4.0f, 18.0f },
                Sp3ctraHandles::stateOf(down && dragMode == DragMode::resizeL, hotEdge == 1));
            Sp3ctraHandles::drawThumb(g, { sel.getRight() - 2.0f, gy - 9.0f, 4.0f, 18.0f },
                Sp3ctraHandles::stateOf(down && dragMode == DragMode::resizeR, hotEdge == 2));
        }

        // Readout: window length @ start (shared frame readout).
        ModuleChrome::drawReadout(g, bf, accent,
                                  juce::String(win, 1) + "s  @ "
                                      + juce::String(startSec, 1) + "s",
                                  0.85f);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.getNumberOfClicks() != 1) return;   // 2nd click → mouseDoubleClick
        const double total = totalSeconds();
        if (total <= 0.0) return;
        auto area = getLocalBounds().reduced(3);
        const double win = windowLen(total);
        const float selX = (float) area.getX() + (float) (startSec / total) * area.getWidth();
        const float selW = (float) (win / total) * area.getWidth();

        dragMode = DragMode::move;
        if (freeSelection && selW > 24.0f)
        {
            // Edge grabs win over the body (resize the selection).
            if (std::abs(e.position.x - selX) <= kEdgePx)
            {
                dragMode  = DragMode::resizeL;
                anchorEnd = startSec + win;
            }
            else if (std::abs(e.position.x - (selX + selW)) <= kEdgePx)
                dragMode = DragMode::resizeR;
        }

        // Click outside the window → jump-centre it on the cursor first.
        if (dragMode == DragMode::move
            && (e.position.x < selX || e.position.x > selX + selW))
        {
            const double frac = (e.position.x - area.getX()) / juce::jmax(1, area.getWidth());
            startSec = frac * total - win * 0.5;
            clampStart();
        }
        dragStartSec = startSec;
        dragLenSec   = windowLen(total);
        dragStartX   = e.position.x;
        hold_.arm(e, [this] { holdToType(); });   // long press = type
        notify();
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (hold_.fired()) return;            // the entry bubble owns the rest
        hold_.moved(e);
        const double total = totalSeconds();
        if (total <= 0.0) return;
        auto area = getLocalBounds().reduced(3);
        const double secPerPx = total / juce::jmax(1, area.getWidth());
        const double sec = (e.position.x - area.getX()) * secPerPx;

        switch (dragMode)
        {
            case DragMode::resizeL:
            {
                // Left edge: the start moves, the RIGHT edge stays anchored.
                startSec  = juce::jlimit(0.0, anchorEnd - kMinSelSec, sec);
                windowSec = anchorEnd - startSec;
                break;
            }
            case DragMode::resizeR:
                windowSec = juce::jlimit(kMinSelSec, total - startSec,
                                         sec - startSec);
                break;
            case DragMode::move:
            default:
                startSec = dragStartSec + (e.position.x - dragStartX) * secPerPx;
                if (freeSelection)   // keep the grabbed length while moving
                    startSec = juce::jlimit(0.0, juce::jmax(0.0, total - dragLenSec),
                                            startSec);
                else
                    clampStart();
                break;
        }
        notify();
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override
    {
        hold_.release();
        dragMode = DragMode::move;
        repaint();
    }

    //── The UI-wide gesture pair (ui/Sp3ctraGestures.h) ─────────────────────
    /** Double-click = the window back to its default: from the start, and
     *  (free selection) to the end of the file. */
    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || totalSeconds() <= 0.0) return;
        startSec = 0.0;
        if (freeSelection) windowSec = 0.0;
        clampStart();
        notify();
        repaint();
    }

    /** Long press = type the start (and, free selection, the length), in
     *  seconds. */
    void holdToType()
    {
        dragMode = DragMode::move;
        repaint();
        juce::Component::SafePointer<WaveformSelectorComponent> safe(this);
        Sp3ctraGestures::Fields f;
        f.push_back(Sp3ctraGestures::fieldOf("Start (s)", juce::String(startSec, 2),
            [safe](const juce::String& t)
            {
                if (safe == nullptr) return;
                safe->startSec = juce::jmax(0.0, t.getDoubleValue());
                safe->clampStart(); safe->notify(); safe->repaint();
            }));
        if (freeSelection)
            f.push_back(Sp3ctraGestures::fieldOf("Length (s)",
                juce::String(windowLen(totalSeconds()), 2),
                [safe](const juce::String& t)
                {
                    if (safe == nullptr) return;
                    const double total = safe->totalSeconds();
                    safe->windowSec = juce::jlimit(kMinSelSec, juce::jmax(kMinSelSec, total - safe->startSec),
                                                   t.getDoubleValue());
                    safe->notify(); safe->repaint();
                }));
        Sp3ctraGestures::openEntry(*this, hold_.anchor(*this), std::move(f));
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        // Resize cursor + hot grip over the edges in free mode.
        auto cursor = juce::MouseCursor::NormalCursor;
        int  edge   = 0;
        const double total = totalSeconds();
        if (freeSelection && total > 0.0)
        {
            auto area = getLocalBounds().reduced(3);
            const double win = windowLen(total);
            const float selX = (float) area.getX()
                             + (float) (startSec / total) * area.getWidth();
            const float selW = (float) (win / total) * area.getWidth();
            if (selW > 24.0f)
            {
                if      (std::abs(e.position.x - selX) <= kEdgePx)          edge = 1;
                else if (std::abs(e.position.x - (selX + selW)) <= kEdgePx) edge = 2;
            }
            if (edge != 0)
                cursor = juce::MouseCursor::LeftRightResizeCursor;
        }
        setMouseCursor(cursor);
        if (edge != hotEdge) { hotEdge = edge; repaint(); }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hotEdge != 0) { hotEdge = 0; repaint(); }
    }

private:
    static constexpr float  kEdgePx    = 7.0f;   // edge grab half-width
    static constexpr double kMinSelSec = 0.1;    // shortest free selection

    enum class DragMode { move, resizeL, resizeR };
    Sp3ctraGestures::Hold hold_;

    double totalSeconds() const { return thumb.getTotalLength(); }
    double windowLen(double total) const
    {
        if (freeSelection)
        {
            // Selection: its own length, 0 ⇒ to the end of the file.
            const double remain = juce::jmax(0.0, total - startSec);
            return (windowSec > 0.0) ? juce::jmin(windowSec, remain) : remain;
        }
        return (windowSec > 0.0 && windowSec < total) ? windowSec : total;
    }
    void clampStart()
    {
        const double total = totalSeconds();
        if (freeSelection)
        {
            startSec = juce::jlimit(0.0, juce::jmax(0.0, total - kMinSelSec),
                                    startSec);
            return;
        }
        const double win = windowLen(total);
        startSec = juce::jlimit(0.0, juce::jmax(0.0, total - win), startSec);
    }
    void notify()
    {
        if (onStartChange)
            onStartChange(startSec);
        if (freeSelection && onRegionChange)
            onRegionChange(startSec, windowLen(totalSeconds()));
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

    juce::Colour accent;
    juce::AudioFormatManager  formatManager;
    juce::AudioThumbnailCache thumbCache;
    juce::AudioThumbnail      thumb;
    double windowSec = 0.0;     // 0 ⇒ whole file (free mode: selection length)
    double startSec  = 0.0;
    double playheadSec = -1.0;  // <0 ⇒ hidden
    double dragStartSec = 0.0;
    double dragLenSec   = 0.0;  // length held while moving a free selection
    double anchorEnd    = 0.0;  // fixed right edge during a left-edge resize
    float  dragStartX   = 0.0f;
    bool   freeSelection = false;
    int    hotEdge = 0;         // grip under the cursor: 1 = left, 2 = right
    DragMode dragMode = DragMode::move;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformSelectorComponent)
};
