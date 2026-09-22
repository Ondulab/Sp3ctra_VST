#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>            // juce::ColourSelector (paper picker)
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleCatalog.h"
#include "../ui/ModuleEditorChrome.h"
#include "../ui/ModuleParamManifest.h"                // vsParam()
#include "../ui/ParamIdentity.h"                      // ParamNaming::bareName
#include "../ui/Sp3ctraControls.h"
#include "../ui/Sp3ctraGestureSlider.h"               // kLongPressMs — the one hold delay
#include "../ui/Sp3ctraGestures.h"
#include "../midi/MidiLearnAttachment.h"              // MidiLearnPopup
#include "VideoScrollMode.h"
#include "VideoScrollPreviewSource.h"
#include "VideoScrollViewportEditor.h"
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

/**
 * @file VideoScrollGridPage.h
 * @brief THE zone-3 page of VIDEO SCROLL — every patched output as one ROW,
 *        every setting as one COLUMN, each cell a drawing you seize.
 *
 * Replaces both former pages (VideoScrollPage, one output per screenful, and
 * VideoScrollAllPage, which stacked one of those per output — four outputs
 * came to ~1600 px of scrolling). The ALL tab and a CHAIN tab are now the
 * SAME page: the grid always shows every output, the tab only says which one
 * the VIEWPORT pad on top is editing.
 *
 *   ┌ VIEWPORT · CHAIN 1 ────────────────────────────────────────────────┐
 *   │  the selected output, live, with its grabbable frame + compass     │
 *   └────────────────────────────────────────────────────────────────────┘
 *    GEOMETRY              FLOW              LOOK             IMAGE
 *    Rot  Zoom  Ctr  Line  Spd Thick Comp  Atten Blur Gamma   Inv  Paper
 *   ▸CHAIN 1  ◑    ⊞    ✛    ═    »    ▬     ⋮⋮     ◺    ▨     ∫     ◉   ▢
 *    CHAIN 2  …
 *
 * Why cells and not numeric boxes (2026-09-04, after a session on the real
 * thing): the boxes were 22 px tall, needed a précise drag, and said nothing
 * without being READ. A cell is 58 px tall, shows the value as the shape the
 * renderer will produce, and answers the gesture that matches the setting —
 * a compass turns, a thickness grows up and down, a packing ruler slides
 * sideways. The number stays, small, underneath: a readout, not the control.
 *
 * Colour split — the charter (ui/Sp3ctraControls.h) with ONE addition: the
 * drawing takes the SETTING's own identity hue (kCellSpecs below) instead of
 * the single module colour, because a monochrome grid of eleven glyphs cannot
 * be scanned. What you can seize is still lime, always: hue = display,
 * Sp3ctraControls::active() = control.
 *
 * Gestures — the shared contract (Sp3ctraGestureSlider), plus what a picture
 * makes possible:
 *   • drag                the cell, along the axis its glyph draws;
 *   • ⇧ + drag            fine (kFineFactor);
 *   • double-click        back to the default (the whole cell's parameters);
 *   • long press          type the value(s) — one field per parameter;
 *   • right-click         MIDI Learn — one sub-menu per parameter of the cell;
 *   • column head drag    move THE SAME setting on every output at once;
 *   • right-click a row   copy / paste / apply this output to all the others.
 */
class VideoScrollGridPage : public juce::Component
{
public:
    //==========================================================================
    /** What a cell draws, and how it is seized. */
    enum class CellKind
    {
        Compass,     ///< Rotation — turn the knob, quarters magnetised
        Zoom,        ///< nested frames, up = bigger
        Centre,      ///< Center X·Y — one 2-D pad instead of two boxes
        Line,        ///< birth line, drawn inside the frame at this rotation
        Speed,       ///< chevrons, both sides (the renderer pushes both ways)
        Thickness,   ///< the stamped bar, grows up and down
        Pack,        ///< Compression — bipolar: packs one way, spreads the other
        Atten,       ///< the attenuation law, two handles on a curve
        Blur,        ///< the smear, grows up and down like Thickness
        Gamma,       ///< the transfer curve
        Invert,      ///< three pictograms: normal / negative / luminance
        Paper        ///< frame colour + the RGB switch
    };

    /** The axis a cell answers to — what the hand does, in one word. */
    enum class CellAxis { Angle, Horizontal, Vertical, Plane, Curve, Pick };

private:
    static constexpr int kMaxCellParams = 5;

    struct CellSpec
    {
        CellKind    kind;
        CellAxis    axis;
        const char* name;      ///< column head
        const char* group;     ///< band above it
        uint32_t    hue;       ///< the SETTING's identity colour (display only)
        /// APVTS suffixes this cell binds; [0] is the one it is named after.
        /// A null entry closes the list.
        std::array<const char*, kMaxCellParams> params;
        /// How many LEADING entries of `params` the cell actually CONTROLS.
        /// The rest are read-only companions it only draws with (the birth
        /// line needs the rotation to be drawn at the right angle, the paper
        /// swatch needs the inversion to show the frame's real colour). They
        /// belong to THEIR own cell: mapping them, resetting them on a
        /// double-click or glowing when they move all happen there, never
        /// here — a CC learnt on Rotation used to light and badge the Line
        /// cell, and a reset on Line used to snap the rotation back to 0°.
        int owned;
    };

    /** THE column table — order on screen, identity hue, and the parameters
     *  each cell owns. Adding a setting to VIDEO SCROLL means one row here. */
    static const std::array<CellSpec, 12>& cellSpecs()
    {
        static const std::array<CellSpec, 12> specs = {{
            { CellKind::Compass,   CellAxis::Angle,      "Rotation",    "GEOMETRY", 0xff00d9ff,
              { "rotation", nullptr, nullptr, nullptr, nullptr }, 1 },
            { CellKind::Zoom,      CellAxis::Vertical,   "Zoom",        "GEOMETRY", 0xff6fb7ff,
              { "zoom", nullptr, nullptr, nullptr, nullptr }, 1 },
            { CellKind::Centre,    CellAxis::Plane,      "Center X\xC2\xB7Y", "GEOMETRY", 0xffb44dff,
              { "centerX", "centerY", nullptr, nullptr, nullptr }, 2 },
            // The birth line is drawn inside the frame AT THIS OUTPUT'S
            // rotation, so it needs to read it — display only, never written.
            { CellKind::Line,      CellAxis::Vertical,   "Line",        "GEOMETRY", 0xff45ff8c,
              { "linePos", "rotation", nullptr, nullptr, nullptr }, 1 },
            { CellKind::Speed,     CellAxis::Horizontal, "Speed",       "FLOW",     0xffffb020,
              { "speed", nullptr, nullptr, nullptr, nullptr }, 1 },
            { CellKind::Thickness, CellAxis::Vertical,   "Thickness",   "FLOW",     0xffff7a3c,
              { "thickness", nullptr, nullptr, nullptr, nullptr }, 1 },
            { CellKind::Pack,      CellAxis::Horizontal, "Compression", "FLOW",     0xffff2ed0,
              { "pack", nullptr, nullptr, nullptr, nullptr }, 1 },
            { CellKind::Atten,     CellAxis::Curve,      "Attenuation", "LOOK",     0xffff5f7a,
              { "fade", "fadeMidX", "fadeMidY", "fadeMidFree", nullptr }, 4 },
            { CellKind::Blur,      CellAxis::Vertical,   "Blur",        "LOOK",     0xff9d8cff,
              { "blur", nullptr, nullptr, nullptr, nullptr }, 1 },
            { CellKind::Gamma,     CellAxis::Vertical,   "Gamma",       "LOOK",     0xffffd93c,
              { "gamma", nullptr, nullptr, nullptr, nullptr }, 1 },
            { CellKind::Invert,    CellAxis::Pick,       "Invert",      "IMAGE",    0xffe8eef6,
              { "invertMode", nullptr, nullptr, nullptr, nullptr }, 1 },
            // The swatch shows the frame colour THROUGH the display law, so it
            // reads the inversion too (white paper + Luminance = black frame).
            { CellKind::Paper,     CellAxis::Pick,       "Paper",       "IMAGE",    0xffe8eef6,
              { "bgR", "bgG", "bgB", "colorMode", "invertMode" }, 4 }
        }};
        return specs;
    }

public:
    //── Metrics ───────────────────────────────────────────────────────────────
    static constexpr int kPadH      = 300;   ///< the VIEWPORT pad (was 640, one per output)
    static constexpr int kCellH     = 58;    ///< a cell — a real target, not a 22 px box
    static constexpr int kBandH     = 17;    ///< group band ("GEOMETRY")
    static constexpr int kHeadH     = 25;    ///< column names
    static constexpr int kNameW     = 140;   ///< output name + live thumbnail
    static constexpr int kMinCellW  = 62;    ///< below this the columns wrap to a new bank
    static constexpr int kThumbW    = 58;
    static constexpr int kThumbH    = 34;
    static constexpr int kTopPad    = 4;
    static constexpr int kPadGap    = 10;

    /** How much a ⇧-drag slows the hand down — one number for every cell. */
    static constexpr float kFineFactor = 0.12f;

    //==========================================================================
    explicit VideoScrollGridPage(Sp3ctraAudioProcessor& proc)
        : processor_(proc),
          viewport_(proc.getAPVTS(), moduleColour(ModuleType::VideoScroll))
    {
        viewport_.setMidiMap(&proc.getMidiMap());
        addAndMakeVisible(viewport_);
    }

    /** Clicking an output's name selects it; the editor mirrors the tab bar. */
    std::function<void(int slot)> onSlotSelected;

    //── The page's contents ───────────────────────────────────────────────────
    /** Rebuild the rows from {slot, chainIdx} in RACK order. Cheap no-op when
     *  the topology is unchanged (a rename only refreshes the labels). */
    void refresh(const std::vector<std::pair<int, int>>& slotsChains)
    {
        const auto labels = videoScrollOutputLabels(slotsChains, processor_.chainNames());
        if (slotsChains == slots_)
        {
            if (labels != labels_) { labels_ = labels; repaint(); }
            return;
        }
        slots_  = slotsChains;
        labels_ = labels;
        rows_.clear();
        for (const auto& sc : slots_)
        {
            auto row = std::make_unique<Row>();
            row->slot = sc.first;
            for (const auto& spec : cellSpecs())
            {
                auto cell = std::make_unique<Cell>(*this, spec);
                cell->setSlot(row->slot);
                addAndMakeVisible(*cell);
                row->cells.push_back(std::move(cell));
            }
            rows_.push_back(std::move(row));
        }
        if (! hasSlot(selected_))
            selected_ = rows_.empty() ? -1 : rows_.front()->slot;
        viewport_.setSlot(selected_);
        resized();
        repaint();
    }

    /** Which output the pad edits (the ALL / CHAIN n tab). */
    void setSelectedSlot(int slot)
    {
        if (slot == selected_) return;
        selected_ = slot;
        viewport_.setSlot(slot);
        repaint();
    }
    int selectedSlot() const noexcept { return selected_; }

    void setPreviewSource(VideoScrollPreviewSource* src)
    {
        source_ = src;
        viewport_.setPreviewSource(src);
    }

    /** Editor timer (20 Hz): keep every row's solo render alive, repaint the
     *  thumbnails that changed, and let the edit glows fade. */
    void previewTick()
    {
        if (! isShowing()) return;
        viewport_.previewTick();
        if (source_ != nullptr)
        {
            for (auto& r : rows_) source_->requestOutputPreview(r->slot);
            const uint32_t fc = source_->frameCounter();
            if (fc != lastFrame_) { lastFrame_ = fc; repaintThumbs(); }
        }
        for (auto& r : rows_)
            for (auto& c : r->cells)
                if (c->lit()) c->repaint();
    }

    /** Natural height for `width` — drives the zone-3 viewport's content size. */
    int preferredHeight(int width) const
    {
        const int banks = bankCount(width);
        const int rows  = juce::jmax(1, (int) rows_.size());
        return kTopPad + kPadH + kPadGap
             + banks * (kBandH + kHeadH)
             + rows * banks * kCellH
             + ModuleChrome::kPagePad;
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        if (rows_.empty())
        {
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.5f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText("Patch a VIDEO SCROLL output into a chain",
                       getLocalBounds(), juce::Justification::centred, true);
            return;
        }

        const auto& specs = cellSpecs();
        const int banks = bankCount(getWidth());
        const int perBank = (int) ((specs.size() + (size_t) banks - 1) / (size_t) banks);
        const int cellW = cellWidth(getWidth(), perBank);
        const int y0 = kTopPad + kPadH + kPadGap;

        // ── Header: one band + one name line per bank ────────────────────────
        for (int b = 0; b < banks; ++b)
        {
            const int by = y0 + b * (kBandH + kHeadH);
            const int lo = b * perBank, hi = juce::jmin((int) specs.size(), lo + perBank);

            g.setColour(juce::Colour(0xff12121a));
            g.fillRect(0, by, getWidth(), kBandH + kHeadH);

            // Group bands, merged across consecutive columns of one group.
            g.setFont(juce::Font(juce::FontOptions(9.5f)).boldened());
            int i = lo;
            while (i < hi)
            {
                int j = i;
                while (j < hi && juce::String(specs[(size_t) j].group)
                                   == juce::String(specs[(size_t) i].group)) ++j;
                const int x = kNameW + (i - lo) * cellW;
                g.setColour(juce::Colour(specs[(size_t) i].hue).withAlpha(0.75f));
                g.drawText(specs[(size_t) i].group, x + 4, by + 3, (j - i) * cellW - 8, kBandH - 4,
                           juce::Justification::centredLeft, false);
                i = j;
            }

            // Column names — the head is itself a handle (drag = all outputs).
            g.setFont(juce::FontOptions(10.5f));
            for (int k = lo; k < hi; ++k)
            {
                const auto& sp = specs[(size_t) k];
                const int x = kNameW + (k - lo) * cellW;
                const bool hov = (headHover_ == k);
                const bool movable = sp.axis != CellAxis::Pick;
                g.setColour(hov && movable ? Sp3ctraControls::active()
                                           : juce::Colour(sp.hue).withAlpha(0.85f));
                g.drawText(juce::String::fromUTF8(sp.name), x + 2, by + kBandH,
                           cellW - 4, kHeadH - 9, juce::Justification::centred, false);
                if (hov && movable)
                {
                    g.setFont(juce::FontOptions(8.5f));
                    g.setColour(Sp3ctraControls::active());
                    g.drawText(juce::String::fromUTF8("all 4 \xE2\x87\x84"), x + 2,
                               by + kBandH + kHeadH - 11, cellW - 4, 10,
                               juce::Justification::centred, false);
                    g.setFont(juce::FontOptions(10.5f));
                }
            }
            g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
            g.fillRect(0, by + kBandH + kHeadH - 1, getWidth(), 1);
        }

        // ── Row headers: name + the output's own live thumbnail ──────────────
        const auto accent = moduleColour(ModuleType::VideoScroll);
        for (int r = 0; r < (int) rows_.size(); ++r)
        {
            const auto& row = *rows_[(size_t) r];
            const auto area = rowHeaderArea(r);
            const bool sel  = (row.slot == selected_);
            const bool hov  = (rowHover_ == r);

            g.setColour(juce::Colour(sel ? 0xff20202b : 0xff1a1a22));
            g.fillRect(area);
            if (sel)
            {
                g.setColour(accent);
                g.fillRect(area.getX(), area.getY(), 2, area.getHeight());
            }

            auto thumb = juce::Rectangle<int>(area.getX() + Sp3ctraTheme::kPad + 2,
                                              area.getCentreY() - kThumbH / 2,
                                              kThumbW, kThumbH);
            g.setColour(juce::Colours::black);
            g.fillRect(thumb);
            if (source_ != nullptr)
            {
                const auto img = source_->outputFrame(row.slot);
                if (img.isValid())
                    g.drawImage(img, thumb.toFloat(),
                                juce::RectanglePlacement::centred
                              | juce::RectanglePlacement::fillDestination);
            }
            g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
            g.drawRect(thumb, 1);

            const int tx = thumb.getRight() + Sp3ctraTheme::kGap;
            g.setFont(juce::Font(juce::FontOptions(11.5f)).boldened());
            g.setColour(sel ? accent
                            : juce::Colour(Sp3ctraTheme::kColText).withAlpha(hov ? 1.0f : 0.85f));
            g.drawText(labelFor(r), tx, area.getY() + 8, area.getRight() - tx - 4, 14,
                       juce::Justification::centredLeft, false);
            g.setFont(juce::FontOptions(9.5f));
            g.setColour(juce::Colour(Sp3ctraTheme::kColTextMuted));
            g.drawText("slot " + juce::String(row.slot), tx, area.getY() + 22,
                       area.getRight() - tx - 4, 12, juce::Justification::centredLeft, false);

            g.setColour(juce::Colour(Sp3ctraTheme::kColBorder).withAlpha(0.6f));
            g.fillRect(0, area.getBottom() - 1, getWidth(), 1);
        }
    }

    void resized() override
    {
        viewport_.setBounds(ModuleChrome::kPagePad, kTopPad,
                            juce::jmax(80, getWidth() - 2 * ModuleChrome::kPagePad), kPadH);
        if (rows_.empty()) return;

        const auto& specs = cellSpecs();
        const int banks = bankCount(getWidth());
        const int perBank = (int) ((specs.size() + (size_t) banks - 1) / (size_t) banks);
        const int cellW = cellWidth(getWidth(), perBank);

        for (int r = 0; r < (int) rows_.size(); ++r)
        {
            auto& row = *rows_[(size_t) r];
            const int top = rowTop(r, banks);
            for (int k = 0; k < (int) row.cells.size(); ++k)
            {
                const int b = k / perBank, col = k % perBank;
                row.cells[(size_t) k]->setBounds(kNameW + col * cellW,
                                                 top + b * kCellH, cellW, kCellH);
            }
        }
    }

    //── Row header interaction ────────────────────────────────────────────────
    void mouseMove(const juce::MouseEvent& e) override
    {
        const int r = rowHeaderAt(e.getPosition());
        const int h = headAt(e.getPosition());
        if (r != rowHover_ || h != headHover_)
        {
            rowHover_ = r; headHover_ = h;
            setMouseCursor(r >= 0 ? juce::MouseCursor::PointingHandCursor
                         : h >= 0 ? juce::MouseCursor::DraggingHandCursor
                                  : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (rowHover_ >= 0 || headHover_ >= 0)
        { rowHover_ = headHover_ = -1; repaint(); }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int r = rowHeaderAt(e.getPosition());
        if (r >= 0)
        {
            if (e.mods.isPopupMenu()) { showRowMenu(r); return; }
            setSelectedSlot(rows_[(size_t) r]->slot);
            if (onSlotSelected) onSlotSelected(rows_[(size_t) r]->slot);
            return;
        }
        // A column head drags THE SAME setting on every output at once.
        const int h = headAt(e.getPosition());
        if (h >= 0 && cellSpecs()[(size_t) h].axis != CellAxis::Pick)
        {
            headDrag_ = h;
            for (auto& row : rows_) row->cells[(size_t) h]->beginColumnDrag();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (headDrag_ < 0) return;
        const float fine = e.mods.isShiftDown() ? kFineFactor : 1.0f;
        for (auto& row : rows_)
            row->cells[(size_t) headDrag_]->dragColumn((float) e.getDistanceFromDragStartX(), fine);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (headDrag_ < 0) return;
        for (auto& row : rows_) row->cells[(size_t) headDrag_]->endColumnDrag();
        headDrag_ = -1;
    }

private:
    //==========================================================================
    //  ONE CELL — a drawing bound to the parameters it shows.
    //==========================================================================
    class Cell : public juce::Component,
                 public juce::SettableTooltipClient,
                 private juce::Timer
    {
    public:
        Cell(VideoScrollGridPage& ownerIn, const CellSpec& specIn)
            : owner(ownerIn), spec(specIn)
        {
            setTooltip(juce::String::fromUTF8(spec.name) + " \xE2\x80\x94 "
                       + gestureHint(spec.axis));
            // The hover state (ground, axis arrows, lit handle) has to appear
            // the moment the hand arrives, not at the next unrelated repaint.
            setRepaintsOnMouseActivity(true);
            // Nothing here consumes the wheel, so it keeps scrolling zone 3.
        }

        ~Cell() override { stopTimer(); }

        /** Bind every parameter of this cell to `slot`'s bank (< 0 unbinds). */
        void setSlot(int slotIn)
        {
            slot = slotIn;
            for (auto& b : bound) b.reset();
            n = 0;
            if (slot < 0) { repaint(); return; }
            auto& apvts = owner.processor_.getAPVTS();
            for (const char* suffix : spec.params)
            {
                if (suffix == nullptr) break;
                bound[(size_t) n].bind(apvts, vsParam(slot, suffix),
                                       [this](float) { repaint(); });
                ++n;
            }
            own = juce::jlimit(0, n, spec.owned);
            repaint();
        }

        /** Still glowing from a change (mouse, MIDI, automation, preset)? */
        bool lit() const noexcept
        {
            for (int i = 0; i < own; ++i) if (bound[(size_t) i].lit()) return true;
            return false;
        }

        //── Column drag: the page drives every row's same cell together ───────
        void beginColumnDrag()
        {
            if (slot < 0 || own == 0) return;
            colStart = normValue(0);
            bound[0].begin();
        }
        void dragColumn(float dx, float fine)
        {
            if (slot < 0 || own == 0) return;
            nudge(0, colStart, dx, fine);
        }
        void endColumnDrag() { if (slot >= 0 && own > 0) bound[0].end(); }

        //══ paint ════════════════════════════════════════════════════════════
        void paint(juce::Graphics& g) override
        {
            const auto hue = juce::Colour(spec.hue);
            const bool hovered = isMouseOverOrDragging();
            const auto look = Sp3ctraControls::stateOf(dragging, hovered, false, heat());
            // Display keeps the setting's own hue and climbs the ladder in
            // intensity; only what is BEING SEIZED goes lime.
            const auto ink  = Sp3ctraControls::inkOf(look, hue);
            const bool warm = dragging || look == Sp3ctraControls::State::Edit || heat() > 0.0f;

            if (hovered || warm)
            {
                g.setColour(juce::Colour(0xff1f1f28));
                g.fillRect(getLocalBounds());
            }
            g.setColour(juce::Colour(Sp3ctraTheme::kColBorder).withAlpha(0.45f));
            g.fillRect(getWidth() - 1, 0, 1, getHeight());

            if (slot < 0) return;

            const int w = getWidth(), h = getHeight();
            switch (spec.kind)
            {
                case CellKind::Compass:   drawCompass(g, w, h, ink.line);   break;
                case CellKind::Zoom:      drawZoom(g, w, h, ink.line);      break;
                case CellKind::Centre:    drawCentre(g, w, h, ink.line);    break;
                case CellKind::Line:      drawLine(g, w, h, ink.line);      break;
                case CellKind::Speed:     drawSpeed(g, w, h, ink.line);     break;
                case CellKind::Thickness: drawThickness(g, w, h, ink.line); break;
                case CellKind::Pack:      drawPack(g, w, h, ink.line);      break;
                case CellKind::Atten:     drawAtten(g, w, h, ink.line);     break;
                case CellKind::Blur:      drawBlur(g, w, h, ink.line);      break;
                case CellKind::Gamma:     drawGamma(g, w, h, ink.line);     break;
                case CellKind::Invert:    drawInvert(g, w, h);              break;
                case CellKind::Paper:     drawPaper(g, w, h, ink.line);     break;
            }

            // The drag affordance: which way this value moves. Cells whose
            // value hangs on named handles say "seize this dot" instead.
            if (hovered && ! handleBased())
                drawAxisArrows(g, w, h);

            // The readout — small, secondary, never the control.
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
            g.setColour(warm ? Sp3ctraControls::active() : hue.withAlpha(0.75f));
            g.drawText(readout(), 1, h - 12, w - 2, 11, juce::Justification::centred, false);

            // A mapped parameter keeps its quiet confirmation dot.
            if (mappedHere())
            {
                g.setColour(Sp3ctraControls::active().withAlpha(0.9f));
                g.fillEllipse((float) w - 7.0f, 3.0f, 3.5f, 3.5f);
            }
        }

        //══ gestures ═════════════════════════════════════════════════════════
        void mouseEnter(const juce::MouseEvent& e) override { mouseMove(e); }

        void mouseMove(const juce::MouseEvent& e) override
        {
            const int p = partAt(e.position);
            const bool changed = (p != part);
            part = p;
            setMouseCursor(cursorNow());
            if (changed) repaint();
        }

        void mouseExit(const juce::MouseEvent&) override
        {
            if (part >= 0) { part = -1; repaint(); }
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            if (slot < 0) return;
            if (e.mods.isPopupMenu()) { showLearnMenu(); return; }
            if (e.getNumberOfClicks() != 1) return;   // → mouseDoubleClick

            part = partAt(e.position);
            pressing = true;
            setMouseCursor(cursorNow());          // the click is what commits
            owner.selectRowOf(*this);

            // Pick cells act on the press; there is nothing to drag.
            if (spec.axis == CellAxis::Pick) { pick(e.position); return; }

            // A handle cell with nothing under the hand still allows the hold.
            dragging = (! handleBased()) || part >= 0;
            for (int i = 0; i < n; ++i)
            {
                startVal[(size_t) i]  = value(i);
                startNorm[(size_t) i] = normValue(i);
            }
            if (dragging) beginAll();
            startTimer(Sp3ctraGestureSlider::kLongPressMs);
            repaint();
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (slot < 0 || held) return;
            if (e.getDistanceFromDragStart() < 3) return;
            stopTimer();                      // a moving hand is not a holding hand
            if (! dragging) return;
            const float fine = e.mods.isShiftDown() ? kFineFactor : 1.0f;
            apply(e, fine);
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            stopTimer();
            if (dragging) { endAll(); dragging = false; repaint(); }
            held = false;
            pressing = false;
            part = partAt(e.position);            // the hand may have travelled
            setMouseCursor(cursorNow());          // and it opens again
        }

        /** Double-click = the whole cell back to its defaults. A pictogram
         *  cell picks on the press instead — its second click is a pick. */
        void mouseDoubleClick(const juce::MouseEvent& e) override
        {
            if (slot < 0 || e.mods.isPopupMenu()) return;
            if (spec.kind == CellKind::Invert) return;
            toDefault();
        }

    private:
        //── long press = type the cell's values (Paper: its colour picker) ───
        void timerCallback() override
        {
            stopTimer();
            if (slot < 0) return;
            held = true;
            if (dragging) { endAll(); dragging = false; }
            repaint();
            if (spec.kind == CellKind::Paper) openPaperPicker();
            else                              openEditor();
        }

        /** Every parameter the cell owns back to its declared default. */
        void toDefault()
        {
            for (int i = 0; i < own; ++i)
                bound[(size_t) i].toDefault();
        }

        //── parameter access ────────────────────────────────────────────────
        float value(int i) const noexcept
        { return (i < n) ? bound[(size_t) i].value : 0.0f; }

        const juce::NormalisableRange<float>& range(int i) const
        { return bound[(size_t) i].param->getNormalisableRange(); }

        /** Write parameter `i`, snapped to its detents, as part of a gesture
         *  (or as a complete one when no drag is open). */
        void write(int i, float v, bool inGesture)
        {
            if (i >= n || bound[(size_t) i].param == nullptr) return;
            const auto& rg = range(i);
            v = snap(i, juce::jlimit(rg.start, rg.end, v));
            if (inGesture) bound[(size_t) i].setGesture(v);
            else           bound[(size_t) i].setComplete(v);
        }

        /** Magnetism on the values that mean something — the reason a rotation
         *  can no longer land on 269.7° when 270° was meant. */
        float snap(int i, float v) const
        {
            if (i != 0) return v;
            switch (spec.kind)
            {
                case CellKind::Compass:
                    for (float d : { 0.0f, 90.0f, 180.0f, 270.0f, 360.0f })
                        if (std::abs(v - d) < 5.0f) return (d >= 360.0f) ? 0.0f : d;
                    return v;
                case CellKind::Zoom:   return std::abs(v - 1.0f) < 0.04f ? 1.0f : v;
                case CellKind::Gamma:  return std::abs(v - 1.0f) < 0.04f ? 1.0f : v;
                case CellKind::Line:
                case CellKind::Speed:
                case CellKind::Pack:   return std::abs(v) < 0.03f ? 0.0f : v;
                default:               return v;
            }
        }

        void beginAll() { for (int i = 0; i < own; ++i) bound[(size_t) i].begin(); }
        void endAll()   { for (int i = 0; i < own; ++i) bound[(size_t) i].end(); }

        float heat() const noexcept
        {
            float h = 0.0f;
            for (int i = 0; i < own; ++i) h = juce::jmax(h, bound[(size_t) i].heat());
            return h;
        }

        /** A relative drag travels in NORMALISED space, so a skewed range
         *  (Zoom, Gamma) keeps an even hand all along its course instead of
         *  crawling at one end and bolting at the other. ~170 px = full travel. */
        static constexpr float kNormPerPixel = 0.006f;

        float normValue(int i) const
        {
            if (i >= n || bound[(size_t) i].param == nullptr) return 0.0f;
            return bound[(size_t) i].param->getValue();
        }

        /** Move parameter `i` by `pixels` along its normalised course. */
        void nudge(int i, float startNorm, float pixels, float fine)
        {
            if (i >= n || bound[(size_t) i].param == nullptr) return;
            const float nrm = juce::jlimit(0.0f, 1.0f,
                                           startNorm + pixels * kNormPerPixel * fine);
            write(i, range(i).convertFrom0to1(nrm), true);
        }

        /** Cells whose value hangs on NAMED handles: only those points answer. */
        bool handleBased() const noexcept
        { return spec.axis == CellAxis::Angle || spec.axis == CellAxis::Curve; }

        /** Cells you SEIZE (a point, a line, a frame) rather than push. */
        bool grabbed() const noexcept
        { return handleBased() || spec.kind == CellKind::Line || spec.kind == CellKind::Centre; }

        /** The hand grammar (ui/Sp3ctraControls.h): OPEN at rest over anything
         *  that answers, and only at the click does it commit — CLOSED on what
         *  this cell lets you hold, INDEX on what it lets you push or pick.
         *  A handle cell with no handle under the pointer keeps the arrow. */
        juce::MouseCursor cursorNow() const
        {
            const bool live = ! handleBased() || part >= 0;
            if (! live) return juce::MouseCursor::NormalCursor;
            if (! pressing) return Sp3ctraCursors::openHand();
            return grabbed() ? Sp3ctraCursors::closedHand() : Sp3ctraCursors::finger();
        }

        static juce::String gestureHint(CellAxis a)
        {
            switch (a)
            {
                case CellAxis::Angle:      return "seize the point and turn";
                case CellAxis::Horizontal: return "drag sideways";
                case CellAxis::Vertical:   return "drag up / down";
                case CellAxis::Plane:      return "drag anywhere in the pad";
                case CellAxis::Curve:      return "seize a handle on the curve";
                case CellAxis::Pick:       return "click to choose";
            }
            return {};
        }

        //── geometry shared by the drawing and the hit test ─────────────────
        struct Dial { float cx, cy, r; };
        Dial dialOf(int w, int h) const
        {
            return { w * 0.5f, h * 0.5f - 4.0f,
                     juce::jmax(9.0f, juce::jmin((float) w, (float) h) * 0.5f - 10.0f) };
        }
        juce::Point<float> dialKnob(int w, int h) const
        {
            const auto d = dialOf(w, h);
            const float t = juce::degreesToRadians(value(0) - 90.0f);
            return { d.cx + std::cos(t) * d.r, d.cy + std::sin(t) * d.r };
        }

        /** The little output frame the Line and Centre glyphs draw in. ONE
         *  definition, so what is drawn is what the hand addresses. */
        struct FrameBox { float cx, cy, rw, rh; };
        FrameBox frameBox(int w, int h) const
        {
            const bool centre = (spec.kind == CellKind::Centre);
            return { w * 0.5f, h * 0.5f - 4.0f,
                     w * (centre ? 0.34f : 0.36f), h * 0.30f };
        }

        struct CurveBox { float x0, w, yTop, h; };
        CurveBox curveBox(int w, int h) const
        { return { w * 0.5f - w * 0.36f, w * 0.72f, h * 0.5f - 15.0f, 22.0f }; }

        /** The curve's two handles, in cell pixels. `midFree` false keeps the
         *  middle one riding the straight line (VideoScrollMode's law). */
        void curveHandles(int w, int h, juce::Point<float>& mid, juce::Point<float>& end) const
        {
            const auto b = curveBox(w, h);
            const float fade = value(0);
            const bool  free = value(3) > 0.5f;
            const float mx = free ? value(1) : 0.5f;
            const float my = free ? value(2) : 1.0f - fade * 0.5f;
            mid = { b.x0 + mx * b.w, b.yTop + b.h - my * b.h };
            end = { b.x0 + b.w,      b.yTop + b.h - (1.0f - fade) * b.h };
        }

        /** Which named handle is under the pointer (−1 = none / whole cell). */
        int partAt(juce::Point<float> p) const
        {
            const int w = getWidth(), h = getHeight();
            if (spec.axis == CellAxis::Angle)
                return p.getDistanceFrom(dialKnob(w, h)) < 13.0f ? 0 : -1;
            if (spec.axis == CellAxis::Curve)
            {
                juce::Point<float> mid, end;
                curveHandles(w, h, mid, end);
                const float dm = p.getDistanceFrom(mid), de = p.getDistanceFrom(end);
                if (juce::jmin(dm, de) > 15.0f) return -1;
                return de <= dm ? 1 : 0;         // 1 = far edge, 0 = the free middle
            }
            return 0;
        }

        //── the drag itself ─────────────────────────────────────────────────
        void apply(const juce::MouseEvent& e, float fine)
        {
            const int w = getWidth(), h = getHeight();
            const float dx = (float) e.getDistanceFromDragStartX();
            const float dy = (float) e.getDistanceFromDragStartY();

            switch (spec.axis)
            {
                case CellAxis::Angle:
                {
                    // Locked on the point, the needle follows the hand; ⇧ falls
                    // back to a relative nudge for an exact angle.
                    const auto d = dialOf(w, h);
                    const float a = std::atan2(e.position.y - d.cy, e.position.x - d.cx);
                    if (e.mods.isShiftDown())
                        write(0, startVal[0] + dx * 0.5f * fine, true);
                    else
                        write(0, std::fmod(juce::radiansToDegrees(a) + 90.0f + 360.0f, 360.0f), true);
                    break;
                }
                case CellAxis::Horizontal:
                    nudge(0, startNorm[0], dx, fine);
                    break;
                case CellAxis::Vertical:
                    // Up is always "more" — every glyph here grows upward. The
                    // birth line is the exception: it is a POSITION, so it goes
                    // exactly where the hand is.
                    if (spec.kind == CellKind::Line)
                        write(0, absoluteLine(e.position, w, h), true);
                    else
                        nudge(0, startNorm[0], -dy, fine);
                    break;
                case CellAxis::Plane:
                {
                    const auto b = frameBox(w, h);
                    write(0, juce::jlimit(-1.0f, 1.0f, (e.position.x - b.cx) / b.rw), true);
                    write(1, juce::jlimit(-1.0f, 1.0f, (e.position.y - b.cy) / b.rh), true);
                    break;
                }
                case CellAxis::Curve:
                {
                    const auto b = curveBox(w, h);
                    if (part == 0)          // the free middle point
                    {
                        if (value(3) <= 0.5f) bound[3].setGesture(1.0f);   // latch it free
                        write(1, juce::jlimit(VideoScrollLimits::kFadeMidXMin,
                                              VideoScrollLimits::kFadeMidXMax,
                                              (e.position.x - b.x0) / b.w), true);
                        write(2, juce::jlimit(0.0f, 1.0f,
                                              (b.yTop + b.h - e.position.y) / b.h), true);
                    }
                    else
                    {
                        // How much is lost at the far edge — POSITIONAL, like
                        // the middle handle: more attenuation pulls the point
                        // DOWN, so a relative "up = more" made it immovable
                        // (fade rests at 0, and both directions clamped there).
                        const float lvl = juce::jlimit(0.0f, 1.0f,
                                                       (b.yTop + b.h - e.position.y) / b.h);
                        write(0, 1.0f - lvl, true);
                    }
                    break;
                }
                case CellAxis::Pick: break;
            }
        }

        /** The birth line is positional: it goes where the hand is — measured
         *  along the SCROLL AXIS, which the glyph turns by the output's own
         *  rotation. Reading a plain screen-y (as this did) meant that at 90°
         *  or 270°, where the line is drawn upright, dragging up and down
         *  moved it not at all. */
        float absoluteLine(juce::Point<float> p, int w, int h) const
        {
            const auto b = frameBox(w, h);
            const float a = juce::degreesToRadians(value(1));
            const float sn = std::sin(a), cs = std::cos(a);
            const juce::Point<float> rel(p.x - b.cx, p.y - b.cy);
            const float along = -rel.x * sn + rel.y * cs;      // local +y, rotated
            return juce::jlimit(-1.0f, 1.0f, along / juce::jmax(1.0f, b.rh));
        }

        void pick(juce::Point<float> p)
        {
            if (spec.kind == CellKind::Invert)
            {
                const int i = pictoAt(p);
                if (i >= 0) bound[0].setComplete((float) i);
            }
            else if (spec.kind == CellKind::Paper)
            {
                if (p.x > (float) getWidth() * 0.62f)
                    bound[3].setComplete(value(3) > 0.5f ? 0.0f : 1.0f);   // RGB switch
                else
                    openPaperPicker();
            }
        }

        //══ the drawings ═════════════════════════════════════════════════════
        juce::Colour lime() const { return Sp3ctraControls::active(); }
        /** The ink of a named handle: lime the moment it can be seized. */
        juce::Colour handleInk(int which, juce::Colour base) const
        { return (part == which) ? lime() : base; }

        void drawCompass(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const auto d = dialOf(w, h);
            const float rot = value(0);
            g.setColour(ink.withMultipliedAlpha(0.30f));
            g.drawEllipse(d.cx - d.r, d.cy - d.r, d.r * 2.0f, d.r * 2.0f, 1.0f);
            for (float a : { 0.0f, 90.0f, 180.0f, 270.0f })       // the four quarters
            {
                const float t = juce::degreesToRadians(a - 90.0f);
                const bool on = std::abs(std::fmod(std::abs(rot - a), 360.0f)) < 1.0f
                             || std::abs(std::fmod(std::abs(rot - a), 360.0f) - 360.0f) < 1.0f;
                g.setColour(on ? lime() : ink.withMultipliedAlpha(0.45f));
                g.drawLine(d.cx + std::cos(t) * (d.r - 3.0f), d.cy + std::sin(t) * (d.r - 3.0f),
                           d.cx + std::cos(t) * (d.r + 3.0f), d.cy + std::sin(t) * (d.r + 3.0f),
                           on ? 1.8f : 1.0f);
            }
            const float t = juce::degreesToRadians(rot - 90.0f);
            const float hx = d.cx + std::cos(t) * d.r * 0.78f;
            const float hy = d.cy + std::sin(t) * d.r * 0.78f;
            g.setColour(ink);                                     // the needle
            g.drawLine(d.cx - std::cos(t) * d.r * 0.55f, d.cy - std::sin(t) * d.r * 0.55f,
                       hx, hy, 1.6f);
            g.drawLine(hx - std::cos(t - 0.5f) * 5.0f, hy - std::sin(t - 0.5f) * 5.0f, hx, hy, 1.4f);
            g.drawLine(hx - std::cos(t + 0.5f) * 5.0f, hy - std::sin(t + 0.5f) * 5.0f, hx, hy, 1.4f);

            const auto k = dialKnob(w, h);                        // the point you seize
            const bool live = (part == 0);
            if (live)
            {
                g.setColour(lime().withAlpha(0.22f));
                g.fillEllipse(k.x - 8.5f, k.y - 8.5f, 17.0f, 17.0f);
            }
            const float kr = live ? 4.6f : 3.8f;
            g.setColour(live ? lime() : ink);
            g.fillEllipse(k.x - kr, k.y - kr, kr * 2.0f, kr * 2.0f);
        }

        void drawZoom(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const float cx = w * 0.5f, cy = h * 0.5f - 4.0f;
            const float rw = w * 0.17f, rh = h * 0.13f;
            const float dashes[] = { 2.0f, 2.0f };                // the reference window,
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.30f));
            g.drawDashedLine({ cx - rw, cy - rh, cx + rw, cy - rh }, dashes, 2, 1.0f);
            g.drawDashedLine({ cx - rw, cy + rh, cx + rw, cy + rh }, dashes, 2, 1.0f);
            g.drawDashedLine({ cx - rw, cy - rh, cx - rw, cy + rh }, dashes, 2, 1.0f);
            g.drawDashedLine({ cx + rw, cy - rh, cx + rw, cy + rh }, dashes, 2, 1.0f);

            const float s = juce::jlimit(VideoScrollLimits::kZoomMin,
                                         VideoScrollLimits::kZoomMax, value(0));
            const float bw = juce::jmin(rw * s, w * 0.46f);       // …and the band, free to
            const float bh = juce::jmin(rh * s, h * 0.30f);       // grow well past it
            g.setColour(ink.withMultipliedAlpha(0.16f));
            g.fillRect(cx - bw, cy - bh, bw * 2.0f, bh * 2.0f);
            g.setColour(ink);
            g.drawRect(cx - bw, cy - bh, bw * 2.0f, bh * 2.0f, 1.4f);
        }

        void drawCentre(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const auto b = frameBox(w, h);
            const float cx = b.cx, cy = b.cy, rw = b.rw, rh = b.rh;
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.25f));
            g.drawRect(cx - rw, cy - rh, rw * 2.0f, rh * 2.0f, 1.0f);
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.18f));
            g.drawLine(cx - rw, cy, cx + rw, cy, 1.0f);
            g.drawLine(cx, cy - rh, cx, cy + rh, 1.0f);
            const float px = cx + juce::jlimit(-1.0f, 1.0f, value(0)) * rw;
            const float py = cy + juce::jlimit(-1.0f, 1.0f, value(1)) * rh;
            g.setColour(ink);
            g.drawLine(px - 6.0f, py, px + 6.0f, py, 1.0f);
            g.drawLine(px, py - 6.0f, px, py + 6.0f, 1.0f);
            g.fillEllipse(px - 2.6f, py - 2.6f, 5.2f, 5.2f);
        }

        void drawLine(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const auto b = frameBox(w, h);
            const float cx = b.cx, cy = b.cy, rw = b.rw, rh = b.rh;
            juce::Graphics::ScopedSaveState save(g);
            g.addTransform(juce::AffineTransform::rotation(
                juce::degreesToRadians(value(1)), cx, cy));       // the output's own rotation
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.25f));
            g.drawRect(cx - rw, cy - rh, rw * 2.0f, rh * 2.0f, 1.0f);
            const float y = cy + juce::jlimit(-1.0f, 1.0f, value(0)) * rh;
            g.setColour(ink);
            g.drawLine(cx - rw, y, cx + rw, y, 1.8f);
            g.fillEllipse(cx - 2.8f, y - 2.8f, 5.6f, 5.6f);
        }

        void drawSpeed(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const float cx = w * 0.5f, cy = h * 0.5f - 4.0f;
            const float v = value(0), m = std::abs(v);
            const float dir = v < 0.0f ? -1.0f : 1.0f;
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.25f));
            g.drawLine(cx, cy - 13.0f, cx, cy + 13.0f, 1.0f);
            // Chevrons on BOTH sides: the renderer pushes history away from the
            // birth line either way, so a single arrow would be a lie.
            for (int i = 0; i < 3; ++i)
            {
                const float a = m * 3.0f - (float) i;
                if (a <= 0.0f) break;
                g.setColour(ink.withMultipliedAlpha(juce::jlimit(0.2f, 1.0f, a)));
                for (float s : { -1.0f, 1.0f })
                {
                    const float x = cx + s * (9.0f + (float) i * 8.0f);
                    g.drawLine(x - 4.0f * s * dir, cy - 4.5f, x + 2.0f * s * dir, cy, 1.5f);
                    g.drawLine(x + 2.0f * s * dir, cy, x - 4.0f * s * dir, cy + 4.5f, 1.5f);
                }
            }
        }

        void drawThickness(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const float cx = w * 0.5f, cy = h * 0.5f - 4.0f, rw = w * 0.30f;
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.25f));
            g.drawLine(cx - rw - 5.0f, cy - 13.0f, cx - rw - 5.0f, cy + 13.0f, 1.0f);
            g.drawLine(cx + rw + 5.0f, cy - 13.0f, cx + rw + 5.0f, cy + 13.0f, 1.0f);
            const float t = 1.4f + juce::jlimit(0.0f, 1.0f, value(0)) * 20.0f;
            g.setColour(ink);
            g.fillRect(cx - rw, cy - t * 0.5f, rw * 2.0f, t);
            g.setColour(ink.withMultipliedAlpha(0.55f));          // the two growing edges
            g.drawLine(cx - rw - 3.0f, cy - t * 0.5f, cx + rw + 3.0f, cy - t * 0.5f, 1.0f);
            g.drawLine(cx - rw - 3.0f, cy + t * 0.5f, cx + rw + 3.0f, cy + t * 0.5f, 1.0f);
        }

        void drawPack(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const float x0 = w * 0.5f - w * 0.40f, rw = w * 0.80f, cy = h * 0.5f - 4.0f;
            const float v = juce::jlimit(-1.0f, 1.0f, value(0));
            const float k = std::pow(2.0f, v * 2.0f);
            const float dashes[] = { 1.0f, 3.0f };
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.22f));
            g.drawDashedLine({ x0, cy + 13.0f, x0 + rw, cy + 13.0f }, dashes, 2, 1.0f);
            // Evenly spaced on the left, packing to the right — and the other
            // way round past the neutral (decompression, 2026-09-04).
            for (int i = 0; i <= 13; ++i)
            {
                const float u = (float) i / 13.0f;
                const float x = x0 + rw * (1.0f - std::pow(1.0f - u, k));
                g.setColour(ink.withMultipliedAlpha(0.42f + 0.58f * (1.0f - u)));
                g.drawLine(x, cy - 10.0f + u * 3.0f, x, cy + 10.0f - u * 3.0f, 1.1f);
            }
            if (std::abs(v) > 0.02f)                              // which way it packs
            {
                const float s = v > 0.0f ? 1.0f : -1.0f;
                const float ax = w * 0.5f + s * 10.0f, ay = cy + 15.0f;
                g.setColour(ink.withMultipliedAlpha(0.8f));
                g.drawLine(w * 0.5f - s * 10.0f, ay, ax, ay, 1.0f);
                g.drawLine(ax - s * 3.5f, ay - 2.5f, ax, ay, 1.0f);
                g.drawLine(ax - s * 3.5f, ay + 2.5f, ax, ay, 1.0f);
            }
        }

        void drawAtten(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const auto b = curveBox(w, h);
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.22f));
            g.drawRect(b.x0, b.yTop, b.w, b.h, 1.0f);
            const float dashes[] = { 1.0f, 3.0f };                // the untouched law
            g.drawDashedLine({ b.x0, b.yTop, b.x0 + b.w, b.yTop }, dashes, 2, 1.0f);

            // Sampled from THE shared law, so the drawing cannot drift from
            // what buildWarp() actually does to the picture.
            const float fade = value(0);
            const bool  free = value(3) > 0.5f;
            juce::Path path;
            for (int i = 0; i <= 24; ++i)
            {
                const float u = (float) i / 24.0f;
                const float lv = videoScrollAttenLevel(u, fade, value(1), value(2), free);
                const float X = b.x0 + u * b.w, Y = b.yTop + b.h - lv * b.h;
                if (i == 0) path.startNewSubPath(X, Y); else path.lineTo(X, Y);
            }
            auto fill = path;
            fill.lineTo(b.x0 + b.w, b.yTop + b.h);
            fill.lineTo(b.x0, b.yTop + b.h);
            fill.closeSubPath();
            g.setColour(ink.withMultipliedAlpha(0.14f));
            g.fillPath(fill);
            g.setColour(ink);
            g.strokePath(path, juce::PathStrokeType(1.7f));

            juce::Point<float> mid, end;
            curveHandles(w, h, mid, end);
            drawKnob(g, mid, part == 0, ink, 3.0f);
            drawKnob(g, end, part == 1, ink, 3.4f);
        }

        void drawKnob(juce::Graphics& g, juce::Point<float> q, bool live,
                      juce::Colour ink, float r)
        {
            if (live)
            {
                g.setColour(lime().withAlpha(0.22f));
                g.fillEllipse(q.x - r - 4.5f, q.y - r - 4.5f, (r + 4.5f) * 2.0f, (r + 4.5f) * 2.0f);
                r += 0.6f;
            }
            g.setColour(live ? lime() : ink);
            g.fillEllipse(q.x - r, q.y - r, r * 2.0f, r * 2.0f);
        }

        void drawBlur(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const float cx = w * 0.5f, cy = h * 0.5f - 4.0f, rw = w * 0.30f;
            const float sp = 1.0f + juce::jlimit(0.0f, 1.0f, value(0)) * 13.0f;
            juce::ColourGradient grad(ink.withAlpha(0.0f), cx, cy - 4.0f - sp,
                                      ink.withAlpha(0.0f), cx, cy + 4.0f + sp, false);
            grad.addColour(0.5, ink);
            g.setGradientFill(grad);
            g.fillRect(cx - rw, cy - 4.0f - sp, rw * 2.0f, 8.0f + sp * 2.0f);
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.25f));
            g.drawLine(cx - rw - 5.0f, cy - 13.0f, cx - rw - 5.0f, cy + 13.0f, 1.0f);
            g.drawLine(cx + rw + 5.0f, cy - 13.0f, cx + rw + 5.0f, cy + 13.0f, 1.0f);
        }

        void drawGamma(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const float x0 = w * 0.5f - w * 0.28f, rw = w * 0.56f;
            const float yTop = h * 0.5f - 15.0f, rh = 22.0f;
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.22f));
            g.drawRect(x0, yTop, rw, rh, 1.0f);
            const float gm = juce::jlimit(0.01f, 10.0f, value(0));
            juce::Path path;
            for (int i = 0; i <= 24; ++i)
            {
                const float u = (float) i / 24.0f;
                const float v = std::pow(u, 1.0f / gm);
                const float X = x0 + u * rw, Y = yTop + rh - v * rh;
                if (i == 0) path.startNewSubPath(X, Y); else path.lineTo(X, Y);
            }
            g.setColour(ink);
            g.strokePath(path, juce::PathStrokeType(1.6f));
        }

        /** Three pictograms on ONE motif — a disc on its ground — so the three
         *  laws differ only by what the law actually changes. */
        juce::Rectangle<float> pictoBounds(int w, int h, int i) const
        {
            const float bw = juce::jmin(24.0f, ((float) w - 14.0f) / 3.0f);
            const float gap = ((float) w - bw * 3.0f) / 4.0f;
            return { gap + (float) i * (bw + gap), h * 0.5f - 4.0f - bw * 0.5f, bw, bw };
        }
        int pictoAt(juce::Point<float> p) const
        {
            for (int i = 0; i < 3; ++i)
                if (pictoBounds(getWidth(), getHeight(), i).expanded(3.0f).contains(p))
                    return i;
            return -1;
        }
        void drawInvert(juce::Graphics& g, int w, int h)
        {
            const int cur = juce::jlimit(0, 2, (int) std::lround(value(0)));
            const juce::Colour grounds[] = { juce::Colour(0xfff2f2ee), juce::Colour(0xff0c0c10),
                                             juce::Colour(0xff0c0c10) };
            // normal: colour on paper · negative: complementary on black ·
            // luminance: the SAME hue, its lightness turned over.
            const juce::Colour discs[]   = { juce::Colour(0xff45ff8c), juce::Colour(0xffff2ed0),
                                             juce::Colour(0xff1f7a45) };
            for (int i = 0; i < 3; ++i)
            {
                const auto r = pictoBounds(w, h, i);
                g.setColour(grounds[i]);
                g.fillRect(r);
                g.setColour(discs[i]);
                g.fillEllipse(r.getCentreX() - r.getWidth() * 0.27f,
                              r.getCentreY() - r.getWidth() * 0.27f,
                              r.getWidth() * 0.54f, r.getWidth() * 0.54f);
                const bool on = (cur == i);
                g.setColour(on ? lime()
                               : juce::Colour(Sp3ctraTheme::kColText)
                                   .withAlpha(part >= 0 ? 0.45f : 0.30f));
                g.drawRect(r.expanded(0.5f), on ? 1.8f : 1.0f);
            }
        }

        void drawPaper(juce::Graphics& g, int w, int h, juce::Colour ink)
        {
            const auto sw = swatchBounds(w, h);
            g.setColour(shownPaperColour());
            g.fillRect(sw);
            g.setColour(part >= 0 ? lime() : juce::Colour(Sp3ctraTheme::kColBorder));
            g.drawRect(sw, 1.0f);

            const bool rgb = value(3) > 0.5f;                     // the Color switch
            const auto tg = juce::Rectangle<float>((float) w * 0.66f, h * 0.5f - 13.0f, 26.0f, 18.0f);
            g.setColour(rgb ? ink : juce::Colour(Sp3ctraTheme::kColTextMuted));
            g.drawRect(tg, 1.0f);
            g.setFont(juce::FontOptions(8.5f));
            g.drawText("RGB", tg, juce::Justification::centred, false);
        }

        juce::Rectangle<float> swatchBounds(int w, int h) const
        { return { 6.0f, h * 0.5f - 13.0f, juce::jmax(20.0f, (float) w * 0.55f), 18.0f }; }

        /** The frame colour AFTER the display law — white paper under
         *  Luminance really does paint a black frame. */
        juce::Colour shownPaperColour() const
        {
            const juce::Colour c = juce::Colour::fromFloatRGBA(value(0), value(1), value(2), 1.0f);
            const int mode = juce::jlimit(0, 2, (int) std::lround(value(4)));
            if (mode == 1) return juce::Colour::fromFloatRGBA(1.0f - c.getFloatRed(),
                                                              1.0f - c.getFloatGreen(),
                                                              1.0f - c.getFloatBlue(), 1.0f);
            if (mode == 2) return c.withBrightness(1.0f - c.getBrightness());
            return c;
        }

        void openPaperPicker()
        {
            juce::Component::SafePointer<Cell> safe(this);
            auto content = std::make_unique<PaperPicker>(
                juce::Colour::fromFloatRGBA(value(0), value(1), value(2), 1.0f),
                [safe](juce::Colour c)
                {
                    if (safe == nullptr) return;
                    safe->bound[0].setComplete(c.getFloatRed());
                    safe->bound[1].setComplete(c.getFloatGreen());
                    safe->bound[2].setComplete(c.getFloatBlue());
                });
            juce::CallOutBox::launchAsynchronously(std::move(content),
                                                   getScreenBounds(), nullptr);
        }

        class PaperPicker : public juce::Component, private juce::ChangeListener
        {
        public:
            PaperPicker(juce::Colour initial, std::function<void(juce::Colour)> cb)
                : onChange(std::move(cb))
            {
                selector.setName("Paper");
                selector.setCurrentColour(initial, juce::dontSendNotification);
                selector.addChangeListener(this);
                addAndMakeVisible(selector);
                setSize(260, 300);
            }
            ~PaperPicker() override { selector.removeChangeListener(this); }
            void resized() override { selector.setBounds(getLocalBounds().reduced(6)); }
        private:
            void changeListenerCallback(juce::ChangeBroadcaster*) override
            { if (onChange) onChange(selector.getCurrentColour()); }
            juce::ColourSelector selector {
                juce::ColourSelector::showColourAtTop | juce::ColourSelector::showSliders
              | juce::ColourSelector::showColourspace };
            std::function<void(juce::Colour)> onChange;
        };

        /** The double arrow that says which way this value moves. */
        void drawAxisArrows(juce::Graphics& g, int w, int h)
        {
            g.setColour(Sp3ctraControls::active().withAlpha(0.8f));
            auto vert = [&](float x, float y, float L)
            {
                g.drawLine(x, y - L, x, y + L, 1.0f);
                g.drawLine(x - 3.0f, y - L + 4.0f, x, y - L, 1.0f);
                g.drawLine(x + 3.0f, y - L + 4.0f, x, y - L, 1.0f);
                g.drawLine(x - 3.0f, y + L - 4.0f, x, y + L, 1.0f);
                g.drawLine(x + 3.0f, y + L - 4.0f, x, y + L, 1.0f);
            };
            auto horz = [&](float x, float y, float L)
            {
                g.drawLine(x - L, y, x + L, y, 1.0f);
                g.drawLine(x - L + 4.0f, y - 3.0f, x - L, y, 1.0f);
                g.drawLine(x - L + 4.0f, y + 3.0f, x - L, y, 1.0f);
                g.drawLine(x + L - 4.0f, y - 3.0f, x + L, y, 1.0f);
                g.drawLine(x + L - 4.0f, y + 3.0f, x + L, y, 1.0f);
            };
            if (spec.axis == CellAxis::Vertical)   vert((float) w - 9.0f, h * 0.5f - 4.0f, 9.0f);
            else if (spec.axis == CellAxis::Horizontal) horz(w * 0.5f, 7.0f, 10.0f);
            else if (spec.axis == CellAxis::Plane)
            { vert((float) w - 8.0f, h * 0.5f - 4.0f, 8.0f); horz(w * 0.5f, 7.0f, 8.0f); }
        }

        //══ readout + typing ═════════════════════════════════════════════════
        juce::String readout() const
        {
            switch (spec.kind)
            {
                case CellKind::Compass:
                    return juce::String(value(0), 1) + juce::String::fromUTF8("\xC2\xB0");
                case CellKind::Zoom:      return juce::String(value(0), 2) + " x";
                case CellKind::Centre:
                    return juce::String(value(0), 2) + juce::String::fromUTF8(" \xC2\xB7 ")
                         + juce::String(value(1), 2);
                case CellKind::Pack:
                {
                    const float v = value(0);
                    if (std::abs(v) < 0.02f) return "1 : 1";
                    const float k = std::pow(2.0f, std::abs(v) * 2.0f);
                    return (v > 0.0f ? "x" : "/") + juce::String(k, 2);
                }
                case CellKind::Atten:
                {
                    const bool free = value(3) > 0.5f;
                    if (value(0) < 0.005f && ! free) return "flat";
                    return "-" + juce::String(juce::roundToInt(value(0) * 100.0f)) + "%"
                         + (free ? juce::String::fromUTF8(" \xE2\x8C\x87") : juce::String());
                }
                case CellKind::Invert:
                {
                    static const char* names[] = { "Normal", "Negative", "Luminance" };
                    return names[juce::jlimit(0, 2, (int) std::lround(value(0)))];
                }
                case CellKind::Paper:
                    return "#" + juce::Colour::fromFloatRGBA(value(0), value(1), value(2), 1.0f)
                                   .toDisplayString(false).toUpperCase();
                default: return juce::String(value(0), 2);
            }
        }

        /** The long press: one entry line per parameter the cell owns
         *  (Center X·Y types both), in the parameter's own units. */
        void openEditor()
        {
            if (own == 0) return;
            Sp3ctraGestures::BoundList l;
            for (int i = 0; i < own; ++i)
                l.push_back({ own > 1 ? juce::String(spec.params[(size_t) i])
                                      : juce::String(spec.name),
                              &bound[(size_t) i] });
            Sp3ctraGestures::openEntry(*this, getScreenBounds(), l);
        }

        //══ MIDI ═════════════════════════════════════════════════════════════
        bool mappedHere() const
        {
            if (slot < 0) return false;
            auto& mm = owner.processor_.getMidiMap();
            for (int i = 0; i < own; ++i)
                if (bound[(size_t) i].param != nullptr
                    && mm.mappingDescription(bound[(size_t) i].param->paramID).isNotEmpty())
                    return true;
            return false;
        }

        /** One sub-menu per parameter of the cell — Center X and Center Y stay
         *  separately mappable even though they share a pad. */
        void showLearnMenu()
        {
            if (slot < 0 || own == 0) return;
            auto& mm = owner.processor_.getMidiMap();
            if (own == 1)
            {
                MidiLearnPopup::show(mm, bound[0].param->paramID, this);
                return;
            }
            juce::PopupMenu menu;
            juce::StringArray ids;
            for (int i = 0; i < own; ++i)
            {
                ids.add(bound[(size_t) i].param->paramID);
                juce::PopupMenu sub;
                MidiLearnPopup::addItems(sub, mm, ids[i], MidiLearnPopup::subMenuBase(i));
                const ModuleType vs = ModuleType::VideoScroll;
                menu.addSubMenu(ParamNaming::bareName(bound[(size_t) i].param->getName(64), &vs),
                                sub);
            }
            MidiMappingEngine& engine = mm;
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this)
                                                          .withMousePosition(),
                               [&engine, ids](int choice)
                               {
                                   for (int i = 0; i < ids.size(); ++i)
                                       if (MidiLearnPopup::handle(engine, ids[i], choice,
                                                                  MidiLearnPopup::subMenuBase(i)))
                                           return;
                               });
        }

        friend class VideoScrollGridPage;

        VideoScrollGridPage& owner;
        const CellSpec&      spec;
        int   slot { -1 };
        int   n    { 0 };     ///< parameters BOUND (own + the read-only companions)
        int   own  { 0 };     ///< of those, the ones this cell controls (CellSpec::owned)
        std::array<Sp3ctraControls::Bound, kMaxCellParams> bound;
        std::array<float, kMaxCellParams> startVal { };    ///< parameter units
        std::array<float, kMaxCellParams> startNorm { };   ///< 0…1, for relative drags
        float colStart { 0.0f };                           ///< idem, column drag
        int   part     { -1 };      ///< named handle under the pointer
        bool  dragging { false };
        bool  pressing { false };   ///< a button is down ON THIS cell
        bool  held     { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Cell)
    };

    //==========================================================================
    struct Row
    {
        int slot { -1 };
        std::vector<std::unique_ptr<Cell>> cells;
    };

    //── layout helpers ────────────────────────────────────────────────────────
    /** Columns wrap into several banks rather than being crushed: a cell is a
     *  target, and a 35 px target is the box we just walked away from. */
    static int bankCount(int width)
    {
        const int usable = juce::jmax(kMinCellW, width - kNameW);
        const int perBank = juce::jmax(1, usable / kMinCellW);
        const int cols = (int) cellSpecs().size();
        return juce::jmax(1, (cols + perBank - 1) / perBank);
    }
    static int cellWidth(int width, int perBank)
    { return juce::jmax(kMinCellW, (width - kNameW) / juce::jmax(1, perBank)); }

    int rowTop(int r, int banks) const
    {
        return kTopPad + kPadH + kPadGap + banks * (kBandH + kHeadH)
             + r * banks * kCellH;
    }
    juce::Rectangle<int> rowHeaderArea(int r) const
    {
        const int banks = bankCount(getWidth());
        return { 0, rowTop(r, banks), kNameW, banks * kCellH };
    }
    int rowHeaderAt(juce::Point<int> p) const
    {
        if (p.x >= kNameW) return -1;
        for (int r = 0; r < (int) rows_.size(); ++r)
            if (rowHeaderArea(r).contains(p)) return r;
        return -1;
    }
    /** Which column head is under `p` (−1 = none) — the header block only. */
    int headAt(juce::Point<int> p) const
    {
        const int banks = bankCount(getWidth());
        const auto& specs = cellSpecs();
        const int perBank = (int) ((specs.size() + (size_t) banks - 1) / (size_t) banks);
        const int cellW = cellWidth(getWidth(), perBank);
        const int y0 = kTopPad + kPadH + kPadGap;
        if (p.x < kNameW || p.y < y0) return -1;
        for (int b = 0; b < banks; ++b)
        {
            const int by = y0 + b * (kBandH + kHeadH);
            if (p.y < by || p.y >= by + kBandH + kHeadH) continue;
            const int col = (p.x - kNameW) / juce::jmax(1, cellW);
            const int k = b * perBank + col;
            return (k < (int) specs.size() && col < perBank) ? k : -1;
        }
        return -1;
    }

    bool hasSlot(int slot) const
    {
        for (const auto& r : rows_) if (r->slot == slot) return true;
        return false;
    }
    juce::String labelFor(int r) const
    { return r < labels_.size() ? labels_[r] : ("CHAIN " + juce::String(r + 1)); }

    void repaintThumbs()
    {
        for (int r = 0; r < (int) rows_.size(); ++r)
            repaint(rowHeaderArea(r));
    }

    /** A cell was pressed: its output becomes the pad's subject. */
    void selectRowOf(const Cell& cell)
    {
        for (const auto& r : rows_)
            for (const auto& c : r->cells)
                if (c.get() == &cell)
                {
                    if (r->slot != selected_)
                    {
                        setSelectedSlot(r->slot);
                        if (onSlotSelected) onSlotSelected(r->slot);
                    }
                    return;
                }
    }

    //── copy / paste / apply to all ───────────────────────────────────────────
    /** Every value that makes an output look the way it looks. */
    static const std::vector<const char*>& copyableParams()
    {
        static const std::vector<const char*> ids = [] {
            std::vector<const char*> v;
            for (const auto& s : cellSpecs())
                for (const char* p : s.params)
                    if (p != nullptr) v.push_back(p);
            return v;
        }();
        return ids;
    }

    void showRowMenu(int r)
    {
        const int slot = rows_[(size_t) r]->slot;
        const juce::String name = labelFor(r);
        juce::PopupMenu menu;
        menu.addItem(1, "Copy " + name + " settings");
        menu.addItem(2, "Paste onto " + name, ! clipboard_.empty());
        menu.addSeparator();
        menu.addItem(3, "Apply " + name + " to every output");
        juce::Component::SafePointer<VideoScrollGridPage> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this)
                                                      .withMousePosition(),
                           [safe, slot](int choice)
                           {
                               if (safe == nullptr) return;
                               if (choice == 1) safe->copyFrom(slot);
                               else if (choice == 2) safe->pasteOnto(slot);
                               else if (choice == 3) safe->applyToAll(slot);
                           });
    }

    void copyFrom(int slot)
    {
        clipboard_.clear();
        auto& apvts = processor_.getAPVTS();
        for (const char* id : copyableParams())
            if (auto* p = apvts.getParameter(vsParam(slot, id)))
                clipboard_.push_back({ id, p->getValue() });   // normalised, range-proof
    }

    void pasteOnto(int slot)
    {
        auto& apvts = processor_.getAPVTS();
        for (const auto& kv : clipboard_)
            if (auto* p = apvts.getParameter(vsParam(slot, kv.first)))
                p->setValueNotifyingHost(kv.second);
    }

    void applyToAll(int slot)
    {
        copyFrom(slot);
        for (const auto& r : rows_)
            if (r->slot != slot) pasteOnto(r->slot);
    }

    //==========================================================================
    Sp3ctraAudioProcessor&    processor_;
    VideoScrollViewportEditor viewport_;
    VideoScrollPreviewSource* source_ { nullptr };
    uint32_t                  lastFrame_ { 0 };

    std::vector<std::pair<int, int>>  slots_;
    juce::StringArray                 labels_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::vector<std::pair<const char*, float>> clipboard_;   // normalised values

    int selected_  { -1 };
    int rowHover_  { -1 };
    int headHover_ { -1 };
    int headDrag_  { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollGridPage)
};
