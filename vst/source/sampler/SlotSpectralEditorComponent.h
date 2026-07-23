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
 *   • Fade curves — drawn full-height ON the image, edited with two handles
 *     each (no widget row, no top strip):
 *       – END handle (top, coloured): drag horizontally = fade length;
 *       – MID handle (white, on the curve): drag vertically = shape — below
 *         the straight line → EXP, above → LOG, near it → LIN (power derived
 *         so the curve passes through the mouse); with an S curve the drag
 *         adjusts its power instead. Right-click a handle → LIN/EXP/LOG/S
 *         menu; double-click the MID handle → reset to LIN.
 *     Fires onFadeChanged so the owner's info labels (under the view) follow.
 *   • Rotation arrows ↺ ↻ — translucent overlay centred at the TOP OF THE
 *     IMAGE, shown only for image-loaded banks: re-render the content from
 *     the source picture ±90° (lossless, duration kept). Fires
 *     onContentRotated for the owner.
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

    /** Fired after a successful ↺/↻ rotation — the owner refreshes the views
     *  that mirror the slot content (sliders, loop buttons…). */
    std::function<void()> onContentRotated;

    /** Fired on every fade edit (length, shape, type) — the owner keeps its
     *  fade info labels (under the view) in sync. */
    std::function<void()> onFadeChanged;

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

    enum class Mode { None, Start, End, Attack, Decay,
                      AttackShape, DecayShape };   // Shape = mid-curve handle
    Mode mode_ = Mode::None;
    int  fadeHover_ = 0; // 0=none, 1=in end, 2=out end, 3=in mid, 4=out mid

    // Fade handle geometry (full-height overlay). in=true → fade-in.
    // Both return (-1,-1) when unavailable (no content / zero span for mid).
    juce::Point<float> fadeEndPoint(bool in) const;
    juce::Point<float> fadeMidPoint(bool in) const;
    void showFadeTypeMenu(bool in);

    // Rotation arrows (top-centre of the fade strip) — visible only when the
    // bank was loaded from an image (visibility driven by the timer).
    juce::TextButton rotCcwBtn_, rotCwBtn_;

    static constexpr int kTopStrip = 0;  // fades live ON the image now
    static constexpr int kSnap     = 12; // px tolerance for time bars
    static constexpr int kHandleR  = 4;  // fade handle radius (matches EQ nodes)
    static constexpr int kGrabR    = 9;  // fade handle grab radius (px)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SlotSpectralEditorComponent)
};
