/**
 * @file ScoreTransportBar.h
 * @brief The audition transport every generator page shares: PLAY/STOP,
 *        PAUSE (hold the sounding column), LOOP, INVERSE and the SPEED knob.
 *
 * One widget, one layout, one set of glyphs. The page keeps the behaviour —
 * what PLAY loads, how PAUSE holds — and wires it through onPlay / onPause;
 * the loop / inverse / speed widgets are APVTS-bound by ScoreTransportBinding
 * (rebind() takes them from the accessors below). The accent colour is the
 * page's module colour, so the bar reads as the page's own.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "../IconPaths.h"
#include "../UITheme.h"
#include "../ui/Sp3ctraGestureSlider.h"

class ScoreTransportBar : public juce::Component
{
public:
    /** Height of the bar: the speed knob's drawing (42) plus its value box. */
    static constexpr int kHeight = 56;

    /** @param accent     the page's module colour
     *  @param withPause  false hides the PAUSE button (a page that has no
     *                    held-column gesture) */
    explicit ScoreTransportBar(juce::Colour accent, bool withPause = true)
        : accent_(accent), withPause_(withPause),
          play_(accent), pause_(accent),
          loop_(accent, IconToggle::Glyph::Loop),
          reverse_(accent, IconToggle::Glyph::Inverse)
    {
        play_.onClick  = [this] { if (onPlay)  onPlay();  };
        pause_.onClick = [this] { if (onPause) onPause(); };
        addAndMakeVisible(play_);
        addChildComponent(pause_);
        pause_.setVisible(withPause_);
        addAndMakeVisible(loop_);
        addAndMakeVisible(reverse_);

        speedLabel_.setText("Speed", juce::dontSendNotification);
        speedLabel_.setJustificationType(juce::Justification::centredRight);
        speedLabel_.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
        addAndMakeVisible(speedLabel_);

        speed_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        speed_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 52, 22);
        speed_.setColour(juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
        speed_.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        speed_.setColour(juce::Slider::textBoxTextColourId,       juce::Colour(0xffa0c4e8));
        speed_.setRange(0.1, 6.0, 0.01);
        speed_.setTextValueSuffix("x");
        speed_.setSkewFactorFromMidPoint(1.0);
        speed_.setDoubleClickReturnValue(true, 1.0);   // long-press reset target
        addAndMakeVisible(speed_);

        setTooltips("Play / stop through the score player",
                    "Freeze the current instant: the held column keeps "
                    "sounding; click/drag the preview to move it. Works "
                    "while playing or from stop.");
        loop_.setTooltip("Loop playback");
        reverse_.setTooltip("Reverse (play backward)");
    }

    std::function<void()> onPlay;
    std::function<void()> onPause;

    void setPlaying(bool p)        { play_.setPlaying(p); }
    void setPaused(bool p)         { pause_.setPaused(p); }
    void setPauseEnabled(bool on)  { pause_.setEnabled(on); }
    void setTooltips(const juce::String& play, const juce::String& pause)
    {
        play_.setTooltip(play);
        pause_.setTooltip(pause);
    }

    // The widgets ScoreTransportBinding attaches / MIDI-learns.
    juce::Button& playButton()    { return play_; }
    juce::Button& loopButton()    { return loop_; }
    juce::Button& reverseButton() { return reverse_; }
    juce::Slider& speedSlider()   { return speed_; }

    void resized() override
    {
        const int gap = 6, ch = Sp3ctraTheme::kControlH;
        const int knobW = 56, knobDrwH = 42;
        const int blockH = kHeight;
        const int btn = 40;
        const int icon = 34;   // loop / inverse pictograms

        int x = 0;
        play_.setBounds(x, (blockH - btn) / 2, btn, btn);  x += btn + gap;
        if (withPause_)
        {
            pause_.setBounds(x, (blockH - btn) / 2, btn, btn);
            x += btn + gap;
        }
        loop_.setBounds   (x, (blockH - icon) / 2, icon, icon);  x += icon + 4;
        reverse_.setBounds(x, (blockH - icon) / 2, icon, icon);  x += icon + gap;

        const int knobX = getWidth() - knobW;
        speed_.setBounds(knobX, 0, knobW, blockH);
        speedLabel_.setBounds(x, (knobDrwH - ch) / 2, juce::jmax(0, knobX - gap - x), ch);
    }

private:
    //==========================================================================
    /** Square play/stop button: a play triangle, or a stop square in the
     *  page's accent while playing. */
    class PlayButton : public juce::Button
    {
    public:
        explicit PlayButton(juce::Colour accent) : juce::Button("scorePlayStop"), accent_(accent) {}

        void setPlaying(bool p)
        {
            if (p == playing_) return;
            playing_ = p;
            repaint();
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const juce::Colour bg(0xff222230);
            g.setColour(down ? bg.brighter(0.30f) : over ? bg.brighter(0.12f) : bg);
            g.fillRoundedRectangle(b, 3.f);
            g.setColour(juce::Colour(0xff33373f));
            g.drawRoundedRectangle(b, 3.f, 1.f);

            const auto inner = b.reduced(b.getHeight() * 0.30f);
            if (! isEnabled())
                Icons::fillPath(g, Icons::play(), inner, juce::Colour(0xff555a62));
            else if (playing_)
                Icons::fillPath(g, Icons::stop(), inner, accent_);
            else
                Icons::fillPath(g, Icons::play(), inner, juce::Colour(0xff66cc88));
        }

    private:
        juce::Colour accent_;
        bool playing_ = false;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayButton)
    };

    /** Square pause toggle: freeze the transport on the CURRENT column — the
     *  player keeps re-injecting that instant every tick (sustained sound)
     *  and the head can be dragged in the preview while frozen. */
    class PauseButton : public juce::Button
    {
    public:
        explicit PauseButton(juce::Colour accent) : juce::Button("scorePause"), accent_(accent) {}

        void setPaused(bool p)
        {
            if (p == paused_) return;
            paused_ = p;
            repaint();
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const bool on = paused_ && isEnabled();

            const juce::Colour bg = on ? accent_.withAlpha(0.22f) : juce::Colour(0xff222230);
            g.setColour(down ? bg.brighter(0.30f) : over ? bg.brighter(0.12f) : bg);
            g.fillRoundedRectangle(b, 3.f);
            g.setColour(on ? accent_.withAlpha(0.9f) : juce::Colour(0xff33373f));
            g.drawRoundedRectangle(b, 3.f, 1.f);

            const auto inner = b.reduced(b.getHeight() * 0.32f);
            const juce::Colour fg = on ? accent_
                                       : juce::Colour(isEnabled() ? 0xff9aa6ba : 0xff555a62);
            const float bw = inner.getWidth() * 0.30f;
            g.setColour(fg);
            g.fillRoundedRectangle(inner.getX(), inner.getY(), bw, inner.getHeight(), 1.5f);
            g.fillRoundedRectangle(inner.getRight() - bw, inner.getY(), bw, inner.getHeight(), 1.5f);
        }

    private:
        juce::Colour accent_;
        bool paused_ = false;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PauseButton)
    };

    /** Compact pictogram toggle for the loop controls — the same glyph as the
     *  SAMPLER loop-mode pictograms (LoopModeButton), so the transport reads
     *  identically everywhere; only the accent differs.
     *   • Loop    → a "racetrack" loop with a left-pointing arrow (repeat).
     *   • Inverse → the same loop, mirrored (right-pointing arrow) = play backward.
     *  JUCE toggle — APVTS-bound (scoreLoop / scoreReverse). */
    class IconToggle : public juce::Button
    {
    public:
        enum class Glyph { Loop, Inverse };

        IconToggle(juce::Colour accent, Glyph g)
            : juce::Button("scoreLoopToggle"), accent_(accent), glyph_(g)
        {
            setClickingTogglesState(true);
        }

        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(1.f);
            const bool on = getToggleState() && isEnabled();

            const juce::Colour bg = on ? accent_.withAlpha(0.22f) : juce::Colour(0xff222230);
            g.setColour(down ? bg.brighter(0.30f) : over ? bg.brighter(0.12f) : bg);
            g.fillRoundedRectangle(b, 3.f);
            g.setColour(on ? accent_.withAlpha(0.9f) : juce::Colour(0xff33373f));
            g.drawRoundedRectangle(b, 3.f, 1.f);

            const auto inner = b.reduced(b.getHeight() * 0.22f);
            const juce::Colour fg = on ? accent_
                                       : juce::Colour(isEnabled() ? 0xff9aa6ba : 0xff555a62);
            drawLoopGlyph(g, inner, fg, glyph_ == Glyph::Inverse);
        }

    private:
        /** Stadium (racetrack) loop, open at the top, with an arrow capping the
         *  gap — reads as "repeat". The RING is centred (the arrow head overshoots
         *  its top, so it is EXCLUDED from the centring measurement). @p reversed
         *  mirrors it horizontally (arrow points right) = play backward. */
        static void drawLoopGlyph(juce::Graphics& g, juce::Rectangle<float> r,
                                  juce::Colour col, bool reversed)
        {
            const float h  = r.getHeight();
            const float th = juce::jmax(2.0f, h * 0.12f);   // stroke thickness

            // Ring built symmetric about r's centre → centred by construction.
            const float ringH  = h * 0.64f;
            const float L = r.getX() + th * 0.6f;
            const float R = r.getRight() - th * 0.6f;
            const float T = r.getCentreY() - ringH * 0.5f;
            const float B = r.getCentreY() + ringH * 0.5f;
            const float radius = (B - T) * 0.5f;
            const float midY   = (T + B) * 0.5f;
            const float topLx  = L + radius;                // top straight: left end
            const float topRx  = R - radius;                // top straight: right end
            const float gx0    = juce::jmap(0.34f, topLx, topRx); // gap (arrow) start
            const float gx1    = juce::jmap(0.66f, topLx, topRx); // gap (arrow) end

            juce::Path loop;
            loop.startNewSubPath(gx1, T);
            loop.lineTo(topRx, T);
            loop.addCentredArc(topRx, midY, radius, radius, 0.0f,
                               0.0f, juce::MathConstants<float>::pi, false);
            loop.lineTo(topLx, B);
            loop.addCentredArc(topLx, midY, radius, radius, 0.0f,
                               juce::MathConstants<float>::pi,
                               juce::MathConstants<float>::twoPi, false);
            loop.lineTo(gx0, T);

            const float aH     = radius * 0.85f;
            const float aTipX  = gx0 - th * 0.25f;
            const float aBackX = gx1 + th * 0.25f;
            juce::Path arrow;
            arrow.addTriangle(aTipX, T, aBackX, T - aH, aBackX, T + aH);

            const auto ringBounds = loop.getBounds().expanded(th * 0.5f);
            const auto offset = r.getCentre() - ringBounds.getCentre();
            const auto move = juce::AffineTransform::translation(offset.x, offset.y);
            loop.applyTransform(move);
            arrow.applyTransform(move);

            if (reversed)
            {
                const auto flip = juce::AffineTransform::scale(-1.0f, 1.0f)
                                      .translated(r.getCentreX() * 2.0f, 0.0f);
                loop.applyTransform(flip);
                arrow.applyTransform(flip);
            }

            g.setColour(col);
            g.strokePath(loop, juce::PathStrokeType(th, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
            g.fillPath(arrow);
        }

        juce::Colour accent_;
        Glyph glyph_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IconToggle)
    };

    juce::Colour accent_;
    bool         withPause_;
    PlayButton   play_;
    PauseButton  pause_;
    IconToggle   loop_, reverse_;
    Sp3ctraGestureSlider speed_;
    juce::Label  speedLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ScoreTransportBar)
};
