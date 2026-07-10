#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_graphics/juce_graphics.h>
#include "../luxsampler/LuxSampler.h"

class Sp3ctraAudioProcessor;

/**
 * @brief Image + time-bounds + fade editor for one LuxSampler slot.
 *
 * Shows the AUTHENTIC recorded image (X = time left→right, Y = frequency with
 * bass at the bottom / treble at the top) with the playback edits overlaid and
 * — crucially — APPLIED to the displayed pixels so the user previews the result:
 *
 *   • Start / End vertical bars  — drag to set the play region [start, end]
 *     (grab anywhere BELOW the top fade strip). The region outside is dimmed.
 *   • Fade handles (Reaper-style) — live in the thin strip along the TOP edge:
 *       top-left  = fade-in  (attack) length,
 *       top-right = fade-out (release) length.
 *     Drag horizontally to set the length; the fade curve is drawn in the strip
 *     and the image is whitened accordingly (visual fade-in / fade-out).
 *   • Playhead — vertical line while PLAYING.
 *
 * The per-fade curve TYPE and POWER, and the EQ, are edited elsewhere
 * (SlotEditorComponent) — this view reads them to render the preview. The time /
 * fade handle drags write LuxSampler atomics directly (Non-RT). markDirty()
 * rebuilds the previewed image (call it on record stop / crop / load / EQ or fade
 * parameter change).
 */
class SlotSpectralEditorComponent : public juce::Component,
                                    private juce::Timer
{
public:
    static constexpr int kPreferredH = 180;

    explicit SlotSpectralEditorComponent(Sp3ctraAudioProcessor& proc);
    ~SlotSpectralEditorComponent() override;

    void setSelectedSlot(int idx);
    void setSamplerIndex(int i);
    void markDirty() noexcept { imageDirty_ = true; repaint(); }

    void paint    (juce::Graphics& g)         override;
    void resized  ()                          override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp  (const juce::MouseEvent& e) override;

private:
    void timerCallback() override;
    void rebuildImage();                       // fetch + apply fade/EQ preview

    juce::Rectangle<float> plotArea() const;   // whole drawable area
    juce::Rectangle<float> imageArea() const;  // plot minus the top fade strip
    float xToFrac(float x) const;
    float fracToX(float f) const;

    void drawFades(juce::Graphics& g, juce::Rectangle<float> img);
    void drawTimeBars(juce::Graphics& g, juce::Rectangle<float> img);

    Sp3ctraAudioProcessor& processor;
    int selectedSlot_ = 0;
    int samplerIndex_ = 0;

    juce::Image image_;          // preview (fade + EQ applied)
    bool        imageDirty_ = true;
    int         builtW_ = 0, builtH_ = 0;

    enum class Mode { None, Start, End, Attack, Decay };
    Mode mode_ = Mode::None;
    int  fadeHover_ = 0; // 0 = none, 1 = fade-in handle, 2 = fade-out handle

    static constexpr int kTopStrip = 18; // reserved for fade handles (no overlap)
    static constexpr int kSnap     = 12; // px tolerance for time bars
    static constexpr int kHandleR  = 4;  // fade handle radius (matches EQ nodes)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotSpectralEditorComponent)
};
