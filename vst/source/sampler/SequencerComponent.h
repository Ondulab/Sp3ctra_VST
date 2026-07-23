#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../framesequencer/FrameSequencer.h"
#include "../luxsampler/LuxSampler.h"
#include <functional>
#include <cmath>

class Sp3ctraAudioProcessor;

/**
 * @brief Step sequencer grid — square cells, ONLY the active steps are shown
 *        (SeqNumSteps tiles, default 8): 1 row up to 8 steps, 2 rows of 8
 *        beyond, 16 displayable max.
 *
 * Bound to ONE sampler engine (the sequencer is internal to its sampler):
 * steps cycle through THIS engine's non-empty banks only.
 *
 * Each cell displays a mini spectral thumbnail of its assigned bank's
 * ACTIVE zone (content between start and end bounds), so the user can
 * identify which sample is where at a glance.
 *
 * Interaction model:
 *   Left click   → next assignable value (+1)
 *   Right click  → previous assignable value (−1)
 *   Drag up/down → increment / decrement continuously
 *
 * Assignable values cycle EMPTY → every non-empty bank of the bound
 * engine → LIVE; empty banks are skipped, content is the only filter
 * (see cycleStep in the .cpp).
 */
class SequencerComponent : public juce::Component,
                           private juce::Timer
{
public:
    explicit SequencerComponent(Sp3ctraAudioProcessor& proc);
    ~SequencerComponent() override;

    /** Bind the grid to sampler engine @p i (its own sequencer + banks). */
    void setSamplerIndex(int i);

    void paint  (juce::Graphics& g) override;
    void resized() override;

    // Display constants
    static constexpr int kDisplayCols  = 8;
    static constexpr int kDisplayRows  = 2;
    static constexpr int kDisplaySteps = kDisplayCols * kDisplayRows; // 16

private:
    // ── StepCell ─────────────────────────────────────────────────────────────
    struct StepCell final : public juce::TextButton
    {
        /** Called with delta = +1 or -1. */
        std::function<void(int delta)> onStep;

        /** Bank slot index assigned to this step (>=0), or STEP_EMPTY/LIVE. */
        int          bankSlot    = FrameSequencer::STEP_EMPTY;
        LuxSampler* luxSampler = nullptr;

        // ── Rendering ────────────────────────────────────────────────────────
        void paintButton(juce::Graphics& g, bool isHighlighted, bool isDown) override
        {
            // Background fill — no border/outline
            auto bg = findColour(juce::TextButton::buttonColourId);
            if (isHighlighted) bg = bg.brighter(0.08f);
            if (isDown)        bg = bg.darker(0.12f);
            g.setColour(bg);
            g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

            // Mini spectral thumbnail of the bank's active zone [start, end]
            if (bankSlot >= 0
                && luxSampler != nullptr
                && luxSampler->slotHasContent(bankSlot))
            {
                paintSlotThumbnail(g);
            }

            // Bank label — top-left corner, small font
            const auto lbl = getButtonText();
            if (lbl.isNotEmpty())
            {
                g.setColour(findColour(juce::TextButton::textColourOffId));
                g.setFont(juce::FontOptions(10.0f));
                g.drawText(lbl,
                           juce::Rectangle<int>(4, 3, getWidth() - 8, 14),
                           juce::Justification::centredLeft, false);
            }
        }

    private:
        /** Draw the spectral waveform of the bank's ACTIVE zone [start, end].
         *
         *  Matches SlotTimelineComponent rendering: bass bars down / treble bars
         *  up from the vertical centre, same γ=0.4 compression, single colour.
         *
         *  Temporal resolution improvement: the number of samples requested is
         *  scaled up so the active zone always has ~tw samples — preventing the
         *  "blocky" look when the zone is a small fraction of the full slot.
         */
        void paintSlotThumbnail(juce::Graphics& g)
        {
            if (!luxSampler) return;
            const int w = getWidth();
            const int h = getHeight();
            constexpr int margin = 3;
            const int tx = margin;
            const int ty = margin;      // waveform uses full height (label overlaid)
            const int tw = w - 2 * margin;
            const int th = h - 2 * margin;
            if (tw <= 0 || th <= 0) return;

            // Active zone
            const float sf       = luxSampler->getSlotStartFrac(bankSlot);
            const float ef       = luxSampler->getSlotEndFrac(bankSlot);
            const float zoneFrac = juce::jmax(0.01f, ef - sf);

            // Request enough samples so the active zone has ~tw bins
            // (same approach as SlotTimelineComponent: N ≥ tw / zoneFrac, capped)
            constexpr int kMaxCols = 512;
            const int kCols = juce::jmin(kMaxCols,
                                          juce::jmax(tw,
                                                      (int)((float)tw / zoneFrac)));
            float bass  [kMaxCols] {};
            float treble[kMaxCols] {};
            luxSampler->sampleSpectralForTimeline(bankSlot, bass, treble, kCols);

            // Map active zone to sample indices
            const int startI  = juce::jlimit(0, kCols - 1, (int)(sf * (float)kCols));
            const int endI    = juce::jlimit(startI + 1, kCols, (int)(ef * (float)kCols));
            const int zoneLen = juce::jmax(1, endI - startI);

            // Centre y and half-amplitude (same ratio as SlotTimelineComponent: cy=h/2)
            const float cy    = (float)(ty + th / 2);
            const float halfH = (float)(th / 2);

            // ── Build bass (below cy) and treble (above cy) paths ─────────────
            juce::Path bassPath, treblePath;
            bassPath  .startNewSubPath((float)tx, cy);
            treblePath.startNewSubPath((float)tx, cy);

            for (int col = 0; col < tw; ++col)
            {
                // Box-average over the bin range for this pixel (anti-alias)
                const int s0 = startI + col       * zoneLen / tw;
                const int s1 = startI + (col + 1) * zoneLen / tw;
                float bSum = 0.f, tSum = 0.f;
                int   cnt  = 0;
                for (int s = s0; s <= s1 && s < endI; ++s)
                {
                    bSum += bass  [s];
                    tSum += treble[s];
                    ++cnt;
                }
                if (cnt == 0)
                {
                    const int sc = juce::jlimit(0, kCols - 1, s0);
                    bSum = bass[sc]; tSum = treble[sc]; cnt = 1;
                }
                // γ=0.4 — identical to SlotTimelineComponent
                const float bv = std::pow(bSum / (float)cnt, 0.4f);
                const float tv = std::pow(tSum / (float)cnt, 0.4f);
                const float x  = (float)(tx + col) + 0.5f;
                bassPath  .lineTo(x, cy + bv * halfH);
                treblePath.lineTo(x, cy - tv * halfH);
            }

            bassPath  .lineTo((float)(tx + tw), cy);
            treblePath.lineTo((float)(tx + tw), cy);
            bassPath  .closeSubPath();
            treblePath.closeSubPath();

            // ── Single-colour fill (no bass/treble colour split) ──────────────
            const juce::Colour fillCol = juce::Colour(0xff1e4055).withAlpha(0.85f);
            const juce::Colour edgeCol = juce::Colour(0xff66bbcc).withAlpha(0.65f);
            g.setColour(fillCol);
            g.fillPath(bassPath);
            g.fillPath(treblePath);

            // Stroke only the outer waveform edge — NOT the horizontal closing
            // segment at cy (which would produce the unwanted centre line).
            // Open paths = same points as fill paths but without closeSubPath().
            juce::Path bassEdge, trebleEdge;
            bassEdge  .startNewSubPath((float)tx, cy);
            trebleEdge.startNewSubPath((float)tx, cy);
            for (int col = 0; col < tw; ++col)
            {
                const int s0 = startI + col       * zoneLen / tw;
                const int s1 = startI + (col + 1) * zoneLen / tw;
                float bS = 0.f, tS = 0.f;
                int   c  = 0;
                for (int s = s0; s <= s1 && s < endI; ++s)
                    { bS += bass[s]; tS += treble[s]; ++c; }
                if (c == 0)
                    { const int sc = juce::jlimit(0, kCols - 1, s0);
                      bS = bass[sc]; tS = treble[sc]; c = 1; }
                const float bv = std::pow(bS / (float)c, 0.4f);
                const float tv = std::pow(tS / (float)c, 0.4f);
                const float x  = (float)(tx + col) + 0.5f;
                bassEdge  .lineTo(x, cy + bv * halfH);
                trebleEdge.lineTo(x, cy - tv * halfH);
            }
            // End at (tx+tw, cy) without closing — no horizontal line drawn
            bassEdge  .lineTo((float)(tx + tw), cy);
            trebleEdge.lineTo((float)(tx + tw), cy);

            const juce::PathStrokeType thin(0.8f,
                juce::PathStrokeType::curved,
                juce::PathStrokeType::rounded);
            g.setColour(edgeCol);
            g.strokePath(bassEdge,   thin);
            g.strokePath(trebleEdge, thin);
        }

        // ── Interaction ───────────────────────────────────────────────────────
        void mouseDown(const juce::MouseEvent& e) override
        {
            dragStartY_   = e.position.y;
            dragAccSteps_ = 0;
            isDrag_       = false;
            if (e.mods.isRightButtonDown())
            {
                if (onStep) onStep(-1);
                return;
            }
            juce::TextButton::mouseDown(e);
        }

        void mouseDrag(const juce::MouseEvent& e) override
        {
            if (e.mods.isRightButtonDown()) return;
            constexpr float kPxPerStep = 8.0f;
            const int steps = static_cast<int>(
                (dragStartY_ - e.position.y) / kPxPerStep);
            if (steps != dragAccSteps_)
            {
                const int diff = steps - dragAccSteps_;
                dragAccSteps_  = steps;
                isDrag_        = true;
                if (onStep) onStep(diff);
            }
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            if (e.mods.isRightButtonDown()) return;
            if (!isDrag_)
                if (onStep) onStep(+1);
            isDrag_       = false;
            dragAccSteps_ = 0;
            juce::TextButton::mouseUp(e);
        }

        float dragStartY_   = 0.f;
        int   dragAccSteps_ = 0;
        bool  isDrag_       = false;
    };

    // ── Helpers ──────────────────────────────────────────────────────────────
    void timerCallback() override;
    void updateButton(int i);

    Sp3ctraAudioProcessor& processor;
    int samplerIndex_ = 0;   // engine whose sequencer/banks this grid edits
    StepCell stepBtns[FrameSequencer::MAX_STEPS];
    int cachedNumSteps = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SequencerComponent)
};
