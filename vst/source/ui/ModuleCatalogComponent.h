/**
 * @file ModuleCatalogComponent.h
 * @brief Far-left module catalogue (M6) — drag source for the chain rack.
 *
 * Replaces the static PaletteRailComponent stub. Lists every module from
 * moduleTable() grouped into the SRC / MIDI / UTILS / SYNTH sections; each
 * chip is draggable into the ChainRackComponent (a juce::DragAndDropTarget).
 * The drag payload is built by ModuleDrag::fromCatalogue().
 *
 * The drag works because a common ancestor (the editor) is a
 * juce::DragAndDropContainer.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../UITheme.h"
#include "ModuleCatalog.h"
#include <vector>

class ModuleCatalogComponent : public juce::Component
{
public:
    static constexpr int kRailW   = 124;
    static constexpr int kHeaderH = 16;
    static constexpr int kChipH   = 26;
    static constexpr int kChipGap = 5;
    static constexpr int kSecGap  = 10;
    static constexpr int kTopPad  = 8;
    static constexpr int kPadX    = 6;

    ModuleCatalogComponent()
    {
        for (const auto& d : moduleTable())
        {
            auto chip = std::make_unique<Chip>(d.type);
            addAndMakeVisible(chip.get());
            chips.push_back(std::move(chip));
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff14141c));

        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        for (const auto& s : sections)
        {
            g.setColour(juce::Colour(0xff66cc88));
            g.drawText(moduleCatLabel(s.cat), kPadX + 1, s.y, getWidth() - 2 * kPadX, kHeaderH,
                       juce::Justification::centredLeft, false);
        }
        // The rail's right edge / separation from the chain rack is drawn by the
        // editor (a groove), so no internal right border here.
    }

    void resized() override
    {
        sections.clear();
        const int bx = kPadX;
        const int bw = juce::jmax(40, getWidth() - 2 * kPadX);
        int y = kTopPad;

        const ModuleCat order[] = { ModuleCat::SRC, ModuleCat::MIDI,
                                    ModuleCat::UTILS, ModuleCat::SYNTH,
                                    ModuleCat::OUT };
        for (auto cat : order)
        {
            sections.push_back({ cat, y });
            y += kHeaderH + 2;

            for (auto& chip : chips)
                if (moduleCategory(chip->getType()) == cat)
                {
                    chip->setBounds(bx, y, bw, kChipH);
                    y += kChipH + kChipGap;
                }
            y += kSecGap;
        }
    }

    /** Natural content height (for hosting in a viewport if ever needed). */
    int preferredHeight() const noexcept
    {
        return kTopPad + 5 * (kHeaderH + 2 + kSecGap)
             + (int) chips.size() * (kChipH + kChipGap);
    }

private:
    //── One draggable catalogue chip ──────────────────────────────────────────
    class Chip : public juce::Component,
                 public juce::SettableTooltipClient
    {
    public:
        explicit Chip(ModuleType t)
            : type(t), name(moduleDisplayName(t)), colour(moduleColour(t))
        {
            setRepaintsOnMouseActivity(true);
            setTooltip("Drag into a chain");
        }

        ModuleType getType() const noexcept { return type; }

        void paint(juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(1.5f);
            g.setColour(isMouseOver() ? colour.withAlpha(0.18f) : colour.withAlpha(0.10f));
            g.fillRoundedRectangle(b, 4.f);
            g.setColour(colour.withAlpha(isMouseOver() ? 0.85f : 0.40f));
            g.drawRoundedRectangle(b, 4.f, 1.f);

            // grip dots (drag affordance)
            g.setColour(colour.withAlpha(0.55f));
            const float gx = b.getRight() - 9.f;
            for (int i = 0; i < 3; ++i)
            {
                const float gy = b.getCentreY() - 4.f + (float) i * 4.f;
                g.fillEllipse(gx, gy, 1.6f, 1.6f);
                g.fillEllipse(gx + 3.f, gy, 1.6f, 1.6f);
            }

            g.setColour(colour.brighter(0.35f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
            g.drawText(name, b.reduced(7.f, 0.f).withTrimmedRight(14.f).toNearestInt(),
                       juce::Justification::centredLeft, true);
        }

        void mouseDown(const juce::MouseEvent&) override { dragging = false; }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (dragging || e.getDistanceFromDragStart() < 6)
                return;
            if (auto* dnd = juce::DragAndDropContainer::findParentDragContainerFor(this))
            {
                if (! dnd->isDragAndDropActive())
                {
                    dragging = true;
                    dnd->startDragging(ModuleDrag::fromCatalogue(type), this,
                                       juce::ScaledImage(createComponentSnapshot(getLocalBounds())));
                }
            }
        }

        void mouseUp(const juce::MouseEvent&) override { dragging = false; }

    private:
        ModuleType   type;
        juce::String name;
        juce::Colour colour;
        bool         dragging { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Chip)
    };

    struct Section { ModuleCat cat; int y; };

    std::vector<std::unique_ptr<Chip>> chips;
    std::vector<Section> sections;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModuleCatalogComponent)
};
