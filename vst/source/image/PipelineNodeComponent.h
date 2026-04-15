/**
 * @file PipelineNodeComponent.h
 * @brief Clickable pipeline output node — reusable building block.
 *
 * Renders a coloured rounded rectangle with a label. Clicking the node
 * fires a callback that the parent tab uses to switch the visualizer mode.
 * The active node is highlighted with a glow border + brighter background.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "VisualizerMode.h"
#include "../UITheme.h"
#include "../IconPaths.h"
#include <functional>

class PipelineNodeComponent : public juce::Component
{
public:
    PipelineNodeComponent(const juce::String& label,
                          juce::Colour         colour,
                          VisualizerMode       mode)
        : nodeLabel(label), nodeColour(colour), vizMode(mode)
    {
        setRepaintsOnMouseActivity(true);
    }

    /** Show/hide the eye indicator (active visualizer source). */
    void setShowEye(bool show)
    {
        if (showEye != show) { showEye = show; repaint(); }
    }
    bool isShowingEye() const noexcept { return showEye; }

    void setActive(bool a)
    {
        if (active != a) { active = a; repaint(); }
    }

    bool isActive() const noexcept { return active; }
    VisualizerMode getMode() const noexcept { return vizMode; }

    /** Called when the user clicks this node. */
    std::function<void(VisualizerMode)> onClick;

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced(1.f);

        // Background
        juce::Colour bg = active
            ? nodeColour.withAlpha(0.25f)
            : (isMouseOver() ? nodeColour.withAlpha(0.10f)
                             : juce::Colour(0xff1a1f2a));
        g.setColour(bg);
        g.fillRoundedRectangle(b, 5.f);

        // Border (glow when active)
        juce::Colour border = active
            ? nodeColour.withAlpha(0.9f)
            : nodeColour.withAlpha(0.35f);
        g.setColour(border);
        g.drawRoundedRectangle(b, 5.f, active ? 2.f : 1.f);

        const float cy = b.getCentreY();

        // Eye indicator (left of label) — always visible, bright green when active source
        {
            constexpr float eyeW = 18.f;
            constexpr float eyeH = 10.f;
            const float eyeX = b.getX() + 8.f;
            const float eyeY = cy - eyeH * 0.5f;

            // Colour: bright green if this is the viewed source, dim gray otherwise
            const juce::Colour eyeCol = showEye
                ? juce::Colour(0xff4ae080)   // active source — green
                : juce::Colour(0xff3a3f4a);  // inactive — dim gray

            // Outer eye shape (almond) — stroked outline
            juce::Path eyeShape;
            eyeShape.startNewSubPath(eyeX, cy);
            eyeShape.cubicTo(eyeX + eyeW * 0.22f, eyeY,
                             eyeX + eyeW * 0.78f, eyeY,
                             eyeX + eyeW, cy);
            eyeShape.cubicTo(eyeX + eyeW * 0.78f, eyeY + eyeH,
                             eyeX + eyeW * 0.22f, eyeY + eyeH,
                             eyeX, cy);
            eyeShape.closeSubPath();

            g.setColour(eyeCol);
            g.strokePath(eyeShape, juce::PathStrokeType(1.5f));

            // Pupil — filled circle in centre
            constexpr float pupilR = 2.5f;
            g.fillEllipse(eyeX + eyeW * 0.5f - pupilR,
                          cy - pupilR,
                          pupilR * 2.f, pupilR * 2.f);
        }

        // Label
        g.setColour(active ? juce::Colours::white : nodeColour.brighter(0.3f));
        g.setFont(juce::Font(Sp3ctraTheme::kFontBadge));
        g.drawText(nodeLabel, b, juce::Justification::centred);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mouseWasClicked() && onClick)
            onClick(vizMode);
    }

private:
    juce::String   nodeLabel;
    juce::Colour   nodeColour;
    VisualizerMode vizMode;
    bool           active        = false;
    bool           showEye       = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PipelineNodeComponent)
};
