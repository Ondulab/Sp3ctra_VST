#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../ui/ModuleCatalog.h"   // moduleColour()
#include "VideoScrollPage.h"
#include "VideoScrollMode.h"       // videoScrollOutputLabels()
#include "../ui/ScrollWheelGuard.h"
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

/**
 * @brief Zone-3 "ALL" tab of the VIDEO SCROLL module — every patched output's
 *        page at once.
 *
 * Reached from the VIDEO MIX banner (zone 4) or the ALL segment of the chain
 * tabs. One VideoScrollPage per output (rack order, labels shared with the
 * VIDEO MIX strip through videoScrollOutputLabels), each under a "CHAIN n"
 * header that jumps to that output's own tab. Two columns when the page is
 * wide enough for them, one otherwise. docs/PLAN_VIDEO_SCROLL_CHAIN_PAGES_ZOOM.md (D2).
 */
class VideoScrollAllPage : public juce::Component
{
public:
    explicit VideoScrollAllPage(Sp3ctraAudioProcessor& proc) : processor_(proc)
    {
        setRepaintsOnMouseActivity(true);
    }

    /** Header click → the editor opens that output's chain tab. */
    std::function<void(int slot)> onChainClicked;

    /** Rebuild the sections from {slot, chainIdx} in RACK order
     *  (Sp3ctraAudioProcessor::activeVideoSlots()). Cheap no-op when unchanged. */
    void refresh(const std::vector<std::pair<int, int>>& slotsChains)
    {
        if (slotsChains == slots_)
            return;
        slots_ = slotsChains;
        sections_.clear();
        const auto labels = videoScrollOutputLabels(slots_);
        for (int i = 0; i < (int) slots_.size(); ++i)
        {
            auto s   = std::make_unique<Section>();
            s->slot  = slots_[(size_t) i].first;
            s->label = labels[i];
            s->page  = std::make_unique<VideoScrollPage>(processor_);
            s->page->setPreviewSource(source_);
            s->page->setSlot(s->slot);
            addAndMakeVisible(*s->page);
            sections_.push_back(std::move(s));
        }
        // Built AFTER the editor's one-shot guard walk: the wheel must keep
        // scrolling this (tall) page, never nudge a slider under the pointer.
        Sp3ctraUI::disableSliderScrollWheel(*this);
        hover_ = -1;
        resized();
        repaint();
    }

    /** The VIEWPORT pads' live thumbnail / view aspect (the zone-4 mixer) —
     *  forwarded to every section, current and future. */
    void setPreviewSource(VideoScrollPreviewSource* src)
    {
        source_ = src;
        for (auto& s : sections_) s->page->setPreviewSource(src);
    }

    /** Editor timer (20 Hz): refresh the visible pads' thumbnails. */
    void previewTick()
    {
        if (! isShowing()) return;
        for (auto& s : sections_) s->page->previewTick();
    }

    /** Natural height for `width` (drives the zone-3 viewport's content size). */
    int preferredHeight(int width) const
    {
        const int n = (int) sections_.size();
        if (n == 0) return 120;
        const int cols = columnsFor(width);
        const int rows = (n + cols - 1) / cols;
        return kTop + rows * (kHeaderH + VideoScrollPage::kPreferredH + kGapV);
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        if (sections_.empty())
        {
            g.setColour(juce::Colour(Sp3ctraTheme::kColText).withAlpha(0.5f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText("Patch a VIDEO SCROLL output into a chain",
                       getLocalBounds(), juce::Justification::centred, true);
            return;
        }

        const auto accent = moduleColour(ModuleType::VideoScroll);
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontSubTab)).boldened());
        for (int i = 0; i < (int) sections_.size(); ++i)
        {
            const auto& s = *sections_[(size_t) i];
            const bool hov = (i == hover_);
            // Header band: label in the module colour, hairline under it; the
            // hovered header brightens and underlines (it is a link to the tab).
            g.setColour(juce::Colour(0xff14141c));
            g.fillRect(s.header);
            g.setColour(hov ? accent.brighter(0.5f) : accent);
            const auto textR = s.header.reduced(Sp3ctraTheme::kHPad, 0);
            g.drawText(s.label, textR, juce::Justification::centredLeft, false);
            if (hov)
            {
                const int tw = (int) std::ceil(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), s.label));
                g.fillRect(textR.getX(), s.header.getBottom() - 5, tw, 1);
            }
            g.setColour(juce::Colour(Sp3ctraTheme::kColBorder));
            g.fillRect(s.header.getX(), s.header.getBottom() - 1, s.header.getWidth(), 1);
        }
    }

    void resized() override
    {
        const int W    = getWidth();
        const int cols = columnsFor(W);
        const int colW = cols == 2 ? (W - kGapH) / 2 : W;
        for (int i = 0; i < (int) sections_.size(); ++i)
        {
            auto& s = *sections_[(size_t) i];
            const int col = i % cols, row = i / cols;
            const int x = col * (colW + kGapH);
            const int y = kTop + row * (kHeaderH + VideoScrollPage::kPreferredH + kGapV);
            s.header = { x, y, colW, kHeaderH };
            s.page->setBounds(x, y + kHeaderH, colW, VideoScrollPage::kPreferredH);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const int h = headerAt(e.getPosition());
        setMouseCursor(h >= 0 ? juce::MouseCursor::PointingHandCursor
                              : juce::MouseCursor::NormalCursor);
        if (h != hover_) { hover_ = h; repaint(); }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hover_ != -1) { hover_ = -1; repaint(); }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (! e.mouseWasClicked() || e.mods.isPopupMenu()) return;
        const int h = headerAt(e.getPosition());
        if (h >= 0 && onChainClicked)
            onChainClicked(sections_[(size_t) h]->slot);
    }

private:
    struct Section
    {
        int slot { -1 };
        juce::String label;
        std::unique_ptr<VideoScrollPage> page;
        juce::Rectangle<int> header;
    };

    static int columnsFor(int width) noexcept
    {
        return width >= 2 * Sp3ctraTheme::kMaxContentW + 16 ? 2 : 1;
    }

    int headerAt(juce::Point<int> p) const noexcept
    {
        for (int i = 0; i < (int) sections_.size(); ++i)
            if (sections_[(size_t) i]->header.contains(p))
                return i;
        return -1;
    }

    static constexpr int kTop     = 4;
    static constexpr int kHeaderH = 26;
    static constexpr int kGapV    = 10;
    static constexpr int kGapH    = 16;

    Sp3ctraAudioProcessor& processor_;
    VideoScrollPreviewSource* source_ { nullptr };
    std::vector<std::pair<int, int>>      slots_;
    std::vector<std::unique_ptr<Section>> sections_;
    int hover_ { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoScrollAllPage)
};
