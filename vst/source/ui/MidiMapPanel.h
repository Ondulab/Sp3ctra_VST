/**
 * @file MidiMapPanel.h
 * @brief Right-band MIDI MAP section — every MIDI mapping of the session as an
 *        editable list (the Ableton "key/MIDI assignments" browser, Sp3ctra-
 *        styled).
 *
 * One row per mapping slot of MidiMappingEngine — the NAME comes first, the
 * controls take what is left:
 *
 *   CC 21  (4) VIDEO · LUXSTRAL · Volume        (min) (max)  ⚙ F ✕
 *   ▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░░░░
 *
 * The title is the parameter's IDENTITY LABEL (ui/ParamIdentity.h): the MIDI
 * event in white, the owning chain as the rack's numbered pastille + name in
 * the chain colour, the module in its colour, then the BARE parameter name
 * (no "DC2" bank tag — the chain and the module already locate it). The MIDI
 * channel lives in the tooltip. The event is NEVER traded away for room
 * (ParamIdentityLabel locks it): a row that cannot say "CC 21" is useless.
 * Everything to its right is fixed-width and small, so the designation gets
 * the whole elastic part of the row instead of splitting it with two boxes.
 *
 * Under the name runs the VU — the mapping READ at a glance: the amber fill
 * is where the parameter stands right now (its APVTS value, or the last
 * normalised output the mapping applied for a virtual target), the pale
 * ground between the two ticks is the MIN..MAX window the controller sweeps.
 * MIN/MAX themselves are two mini rotaries (no number boxes, no column
 * captions: the VU already shows them, and the value only has to be read
 * while it is being set — it then takes over the right of the title line).
 * They are stored normalised in the engine and spoken in the parameter's own
 * units via RangedAudioParameter::getText. Turning MIN above MAX inverts the
 * control.
 *
 * Click on the title area navigates to the owning module page. ⚙ opens the
 * mapping's transfer-law window (ui/MidiCurveEditor.h: input window,
 * EXPO / POINTS / TABLE shape, hysteresis — or an ENVELOPE the press plays,
 * GATE / ONE-SHOT / LOOP, midi/MidiMappingEnvelope.h) and lights amber once
 * the law is not neutral or an envelope is armed — HOLDING it wipes both
 * back to neutral instead, the same hold-to-default gesture every value
 * control answers, and the gear BLINKING three times is the receipt.
 * A row driven by an LFO (midi/LfoBank.h) says "LFO 3" where a controller row
 * says "CC 21", in the modulation hue, its two MIN/MAX knobs ARE the reach of
 * the modulation (and follow on their own when the window slides under a hand
 * on the destination — MidiMappingEngine::tick), and it gains ONE control the
 * others do not need: the phase chip (0° / 90° / 180° / 270°), the offset of THIS
 * destination along the shared cycle. It is what turns one shape into a
 * complex motion — four destinations in quadrature off a single sine. Clicking
 * such a row also opens its LFO in the bank above (onLfoSelected).
 *
 * F is the per-mapping MIDI-follow opt-out (lit = a controller move
 * navigates to the module, the default; off for controls that move all the
 * time). Clicking the row ALWAYS navigates, whatever F says. ✕ removes the
 * mapping (same as the control's right-click menu).
 *
 * The section is collapsible to its header band (chevron), mirroring the
 * MidiMixPanel collapse API (setCollapsed / isCollapsed / onCollapseToggled)
 * so the editor persists it the same way. Folded, the band displays the
 * mapping a controller is playing right now (MidiTouch bus), except for
 * mappings whose F follow is off. Rows live in a viewport: past
 * kMaxVisibleRows the list scrolls instead of eating the video preview.
 *
 * Refresh contract: the engine broadcasts a ChangeMessage on every mapping
 * TABLE mutation (learn / remove / restore) → rebuildRows(). Range edits do
 * NOT broadcast (they would rebuild the knob being turned). The curve window
 * is the other writer of lo/hi + follow: it calls syncFromEngine() after
 * each of its edits so the knobs and glyphs follow without a rebuild.
 * The VUs are the only thing that moves on its own: one panel timer polls
 * the rows at kLiveHz and each repaints its 4-px strip only when it moved.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiMappingEngine.h"
#include "MidiMapGlyphs.h"
#include "ModuleCatalog.h"
#include "ParamIdentity.h"
#include "Sp3ctraControls.h"
#include "Sp3ctraGestureSlider.h"
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

class MidiMapPanel : public juce::Component,
                     private juce::ChangeListener,
                     private juce::Timer,
                     private Sp3ctraControls::MidiTouch::Sink
{
public:
    static constexpr int kHeaderH        = 24;
    static constexpr int kRowH           = 30;  // identity line + VU strip
    static constexpr int kRowGap         = 3;
    static constexpr int kPad            = 6;
    static constexpr int kMaxVisibleRows = 8;   // taller lists scroll
    static constexpr int kKnobD          = 26;  // the MIN / MAX mini rotaries
    static constexpr int kVuH            = 4;   // the live-value strip
    static constexpr int kLiveHz         = 25;  // VU / collapsed-readout poll

    /** Fired after the collapse state changed (editor relayouts + persists). */
    std::function<void(bool)> onCollapseToggled;
    /** Fired when a row's title line is clicked — the editor navigates to the
     *  module page that owns the mapped parameter (same path as MIDI-follow,
     *  NOT gated by the row's F flag). */
    std::function<void(const juce::String&)> onMappingSelected;
    /** Fired when an LFO-driven row is clicked — the editor opens that LFO
     *  in the bank section above. */
    std::function<void(int)> onLfoSelected;
    /** Fired by a row's ⚙ — the editor opens the MIDI CURVE window on it. */
    std::function<void(const juce::String&)> onEditRequested;
    /** Fired after a row's ⚙ was HELD — that mapping's transfer law went back
     *  to neutral. The editor reloads the MIDI CURVE window when it happens
     *  to show that mapping: setMappingCurve does not broadcast, so an open
     *  window would keep drawing the law that was just wiped. */
    std::function<void(const juce::String&)> onCurveReset;

    explicit MidiMapPanel(Sp3ctraAudioProcessor& p) : processor_(p)
    {
        chevron_.collapsed = &collapsed_;
        chevron_.onClick = [this] { setCollapsed(! collapsed_, true); };
        chevron_.setTooltip("Collapse / expand the MIDI mapping list");
        addAndMakeVisible(chevron_);

        viewport_.setViewedComponent(&rowsHolder_, false);
        viewport_.setScrollBarsShown(true, false);
        viewport_.setScrollBarThickness(8);
        addAndMakeVisible(viewport_);

        processor_.getMidiMap().addChangeListener(this);
        Sp3ctraControls::MidiTouch::add(this);   // collapsed "now playing" readout
        rebuildRows();
        startTimerHz(kLiveHz);   // row VUs + the collapsed readout's fade
    }

    ~MidiMapPanel() override
    {
        Sp3ctraControls::MidiTouch::remove(this);
        processor_.getMidiMap().removeChangeListener(this);
    }

    /** Height for the CURRENT row count — the editor uses it to split zone 4. */
    int preferredHeight() const noexcept
    {
        if (collapsed_) return kHeaderH;
        const int n = juce::jmax(1, (int) rows_.size());   // 1 = empty hint line
        const int shown = juce::jmin(n, kMaxVisibleRows);
        return kHeaderH + kPad + shown * (kRowH + kRowGap) + kPad - kRowGap;
    }

    void setCollapsed(bool shouldCollapse, bool notify)
    {
        if (collapsed_ == shouldCollapse) { resized(); return; }
        collapsed_ = shouldCollapse;
        resized();
        repaint();
        if (notify && onCollapseToggled) onCollapseToggled(collapsed_);
    }
    bool isCollapsed() const noexcept { return collapsed_; }

    /** Scroll to a mapping's row and flash it. Deferred when the row does not
     *  exist yet: the engine broadcasts its rebuild asynchronously, so a row
     *  created a microsecond ago is still to come. */
    void revealRow(const juce::String& paramId)
    {
        pendingReveal_ = paramId;
        applyPendingReveal();
    }

    /** Rows ← engine, WITHOUT a rebuild: the curve window edited a range,
     *  a follow flag or the law of some mapping through the no-broadcast
     *  paths. Cheap at these row counts. */
    void syncFromEngine()
    {
        for (auto& row : rows_) row->syncFromEngine();
    }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0c0c10));

        g.setColour(kAccent);
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("MIDI MAP", 8, 0, getWidth() - 16, kHeaderH,
                   juce::Justification::centredLeft, false);

        // Collapsed band: the mapping a controller is playing RIGHT NOW takes
        // the readout spot (fading with the touch heat, skipped for mappings
        // whose F follow is off); the dimmed count keeps it otherwise.
        const float ph = collapsed_ ? Sp3ctraControls::heatSince(playingMs_) : 0.0f;
        if (ph > 0.0f && playing_.paramId.isNotEmpty())
        {
            const int rx = 70, rw = chevron_.getX() - 6 - rx;
            // "CC 32  (4) VIDEO · DC BLOCK · Amount" — the rows' identity
            // label, right-aligned, fading with the touch heat.
            ParamIdentityLabel::draw(g, { rx, 0, rw, kHeaderH - 5 }, playing_,
                                     true, true, 0.35f + 0.55f * ph);

            // Live value bar under the text — WHERE the controller just put
            // the parameter (normalised), in the bar language of the UI.
            // Virtual targets (no APVTS param) keep the text alone.
            if (auto* p = processor_.getAPVTS().getParameter(playingParam_);
                p != nullptr && rw > 20)
            {
                const juce::Rectangle<float> bar((float) rx, (float) kHeaderH - 5.0f,
                                                 (float) rw, 3.0f);
                g.setColour(juce::Colour(0xff181820).withAlpha(ph));
                g.fillRect(bar);
                g.setColour(kAccent.withAlpha(0.30f + 0.60f * ph));
                g.fillRect(bar.withWidth(bar.getWidth()
                               * juce::jlimit(0.0f, 1.0f, p->getValue())));
                g.setColour(kAccent.withAlpha(0.35f * ph));
                g.drawRect(bar, 1.0f);
            }
        }
        else if (const int n = (int) rows_.size(); n > 0)
        {
            g.setColour(kAccent.withAlpha(0.55f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText(juce::String(n), 0, 0, chevron_.getX() - 6, kHeaderH,
                       juce::Justification::centredRight, false);
        }

        if (collapsed_) return;

        if (rows_.empty())
        {
            g.setColour(juce::Colour(0xff9aa6ba));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText(juce::String::fromUTF8("right-click a control \xE2\x86\x92 MIDI Learn"),
                       viewport_.getBounds(), juce::Justification::centredLeft, false);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        // Chevron at the panel's TRUE right edge — aligned with the VIDEO MIX
        // header buttons whatever the zone width.
        const int btn = kHeaderH - 6;
        chevron_.setBounds(getWidth() - btn - 6, (kHeaderH - btn) / 2, btn, btn);

        // Same cap as the other zone-4 strips: rows stop stretching with a
        // very wide zone.
        r.setWidth(juce::jmin(r.getWidth(), Sp3ctraTheme::kMaxContentW));
        r.removeFromTop(kHeaderH);

        viewport_.setVisible(! collapsed_);
        if (collapsed_)
            return;

        r.removeFromTop(kPad);
        r.reduce(kPad, 0);
        r.removeFromBottom(kPad);
        viewport_.setBounds(r);

        // Reserve the scrollbar as soon as the content overflows what the
        // editor actually granted us (its clamp can be tighter than
        // kMaxVisibleRows).
        const int contentH = juce::jmax(0,
            (int) rows_.size() * (kRowH + kRowGap) - kRowGap);
        const int rowW = juce::jmax(60, r.getWidth()
                             - (contentH > r.getHeight()
                                    ? viewport_.getScrollBarThickness() : 0));
        rowsHolder_.setSize(rowW, contentH);
        int y = 0;
        for (auto& row : rows_)
        {
            row->setBounds(0, y, rowW, kRowH);
            y += kRowH + kRowGap;
        }
    }

private:
    static const juce::Colour kAccent;

    //── Header chevron — ▾ expanded / ▸ collapsed ────────────────────────────
    struct Chevron : juce::Button
    {
        Chevron() : juce::Button("collapse") {}
        bool* collapsed = nullptr;
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            const auto b = getLocalBounds().toFloat().reduced(4.0f);
            g.setColour(kAccent.withAlpha(over ? 0.95f : 0.6f));
            juce::Path p;
            if (collapsed != nullptr && *collapsed)
                p.addTriangle(b.getX(), b.getY(), b.getX(), b.getBottom(),
                              b.getRight(), b.getCentreY());          // ▸
            else
                p.addTriangle(b.getX(), b.getY(), b.getRight(), b.getY(),
                              b.getCentreX(), b.getBottom());         // ▾
            g.fillPath(p);
        }
    };

    //── ✕ — remove this mapping (same action as the control's right-click) ───
    struct RemoveButton : juce::Button
    {
        RemoveButton() : juce::Button("remove") {}
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            const auto b = getLocalBounds().toFloat().reduced(4.5f);
            g.setColour(over ? juce::Colour(0xffff5b50)
                             : juce::Colours::white.withAlpha(0.35f));
            juce::Path p;
            p.startNewSubPath(b.getTopLeft());     p.lineTo(b.getBottomRight());
            p.startNewSubPath(b.getTopRight());    p.lineTo(b.getBottomLeft());
            g.strokePath(p, juce::PathStrokeType(1.4f));
        }
    };

    //── F — per-mapping MIDI-follow toggle (lit = navigate on move) ──────────
    struct FollowButton : juce::Button
    {
        FollowButton() : juce::Button("follow") { setClickingTogglesState(true); }
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            MidiMapGlyphs::drawFollowChip(g, getLocalBounds().toFloat().reduced(1.5f),
                                          getToggleState(), over);
        }
    };

    //── ⚙ — open the transfer-law window; amber once the law is not neutral ──
    // CLICK opens it, HOLD wipes it. The gear is not a value but the door to
    // one — its click already IS the "type it" gesture — so it keeps the one
    // hold-to-reset left in the UI (values answer double-click = default,
    // hold = type: ui/Sp3ctraGestures.h), with the same delay as everywhere
    // (Sp3ctraGestureSlider::kLongPressMs), on the one glyph that stands for
    // the whole advanced law. The gear
    // keeps its size and its look under the finger — no ring, no shrink —
    // and the RECEIPT comes when the hold fires: the gear BLINKS three times
    // (lime, the gesture colour, against near-invisible), then rests dim — it
    // is lit exactly while the law is not neutral. A hold that had nothing
    // to wipe gives no receipt (onLongPress answers false).
    struct GearButton : juce::Button,
                        private juce::Timer
    {
        GearButton() : juce::Button("curve") {}
        bool lit = false;
        /** Hold → the law back to neutral. Answer true when something was
         *  actually wiped: that is what earns the blink. */
        std::function<bool()> onLongPress;

        static constexpr int    kBlinkCount = 3;
        static constexpr double kBlinkMs    = 110.0;   ///< one half-period (on, then off)
        static constexpr double kBlinkTotal = kBlinkCount * 2.0 * kBlinkMs;

        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            const auto  b = getLocalBounds().toFloat();
            const float R = b.getWidth() * 0.5f - 2.5f;
            juce::Colour c = lit ? kAccent.withAlpha(over ? 1.0f : 0.9f)
                                 : juce::Colours::white.withAlpha(over ? 0.7f : 0.35f);
            if (blinkMs_ > 0.0)
                c = blinkOn() ? juce::Colour(Sp3ctraTheme::kColHandle)
                              : juce::Colours::white.withAlpha(0.10f);
            MidiMapGlyphs::drawGear(g, b.getCentre(), R, c);
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            held_    = false;
            pressMs_ = 0.0;
            juce::Button::mouseDown(e);
            if (! e.mods.isPopupMenu() && onLongPress != nullptr)
            {
                pressMs_ = juce::Time::getMillisecondCounterHiRes();
                startTimerHz(30);   // watches the hold (and runs a blink)
            }
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            pressMs_ = 0.0;
            if (blinkMs_ <= 0.0) stopTimer();   // a running receipt finishes
            if (held_)
            {
                // The hold already acted: this release must NOT also open the
                // window. Clearing the down/over state is what makes
                // juce::Button skip its click callback (it reads them before
                // updating), and it leaves the button in a sane state.
                held_ = false;
                juce::Button::mouseExit(e);
            }
            juce::Button::mouseUp(e);
            repaint();
        }

    private:
        bool blinkOn() const noexcept
        {
            const double t = juce::Time::getMillisecondCounterHiRes() - blinkMs_;
            return t < kBlinkTotal && ((int) (t / kBlinkMs)) % 2 == 0;
        }

        void timerCallback() override
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            bool keep = false;

            if (blinkMs_ > 0.0)   // the receipt in progress
            {
                if (now - blinkMs_ >= kBlinkTotal) blinkMs_ = 0.0;
                else                               keep = true;
                repaint();
            }

            if (pressMs_ > 0.0)   // a hold building
            {
                if (! isDown() || ! isEnabled())
                    pressMs_ = 0.0;   // pointer wandered off — not a hold
                else if (now - pressMs_ >= (double) Sp3ctraGestureSlider::kLongPressMs)
                {
                    pressMs_ = 0.0;
                    held_    = true;
                    if (onLongPress && onLongPress())
                    {
                        blinkMs_ = now;   // three flashes, then rest
                        keep = true;
                        repaint();
                    }
                }
                else
                    keep = true;
            }

            if (! keep) stopTimer();
        }

        bool   held_    { false };   ///< the hold fired: swallow the release
        double pressMs_ { 0.0 };     ///< press time while a hold may be building
        double blinkMs_ { 0.0 };     ///< start of the receipt (0 = none)
    };

    //── MIN / MAX — a mini rotary, no text box ───────────────────────────────
    // The knobs keep the UI-wide CONTROL colour (acid lime): the amber of
    // this panel is the mapping's IDENTITY (title, VU, F, ⚙), and what one
    // touches is lime everywhere. The gesture contract comes for free from
    // Sp3ctraGestureSlider (drag, double-click = default, long press = type
    // the value in a bubble, in the parameter's own units).
    struct RangeKnob : Sp3ctraGestureSlider
    {
        RangeKnob()
        {
            setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            setScrollWheelEnabled(false);   // the zone-4 column scrolls instead
            setRange(0.0, 1.0, 0.001);
        }
        /** True while the pointer is actually turning it — the row then puts
         *  the value in words at the end of the title line. */
        bool isTurning() const { return isMouseButtonDown(true); }

        // The LookAndFeel reads the live pointer state at paint time, and a
        // plain juce::Slider only repaints on a value change: without these
        // the knob would never light up under the pointer (same fix as
        // Sp3ctraBarSlider).
        void mouseEnter(const juce::MouseEvent& e) override
        { Sp3ctraGestureSlider::mouseEnter(e); repaint(); }
        void mouseExit(const juce::MouseEvent& e) override
        { Sp3ctraGestureSlider::mouseExit(e); repaint(); }
    };

    //── PHASE — an LFO destination's offset along the shared cycle ───────────
    // Only LFO rows carry it. A click steps a quarter turn: the four values
    // that matter (a quadrature set) in one gesture, no drag to aim at. The
    // chip speaks in degrees because that is how a phase relationship reads.
    struct PhaseChip : juce::Button
    {
        PhaseChip() : juce::Button("phase") {}
        int deg = 0;
        void paintButton(juce::Graphics& g, bool over, bool down) override
        {
            const auto b = getLocalBounds().toFloat().reduced(0.5f);
            const juce::Colour acc = juce::Colour(Sp3ctraTheme::kColHandle);
            g.setColour(juce::Colour(Sp3ctraTheme::kColBarBg));
            g.fillRoundedRectangle(b, 2.0f);
            g.setColour(acc.withAlpha(down ? 0.40f : over ? 0.30f : deg != 0 ? 0.22f : 0.10f));
            g.fillRoundedRectangle(b, 2.0f);
            g.setColour(acc.withAlpha(over ? 0.70f : deg != 0 ? 0.45f : 0.25f));
            g.drawRoundedRectangle(b, 2.0f, 1.0f);
            g.setColour(juce::Colour(0xffdfe6f0).withAlpha(deg != 0 ? 1.0f : 0.6f));
            g.setFont(juce::FontOptions(9.5f));
            g.drawText(juce::String(deg) + juce::String::fromUTF8("\xC2\xB0"),
                       getLocalBounds(), juce::Justification::centred, false);
        }
    };

    //── One mapping row: identity + VU over the range knobs and the glyphs ───
    class MapRow : public juce::Component,
                   public juce::SettableTooltipClient
    {
    public:
        MapRow(MidiMapPanel& owner, const MidiMappingEngine::MappingInfo& info)
            : owner_(owner), paramId_(info.paramId)
        {
            // One identity for the row — event · chain · module · parameter
            // (ui/ParamIdentity.h); the MIDI channel only shows in the tooltip.
            ident_ = describeParam(owner_.processor_, paramId_);
            setTooltip(ident_.text() + "\n" + paramId_
                       + "\nClick to open the module page");

            auto initKnob = [&](RangeKnob& k, float v, double dflt)
            {
                // No APVTS attachment behind these: the double-click return
                // value IS the double-click default, so it must be set by hand.
                k.setDoubleClickReturnValue(true, dflt);
                k.setValue(v, juce::dontSendNotification);
                installText(k);
                k.onValueChange = [this]
                {
                    owner_.processor_.getMidiMap().setMappingRange(
                        paramId_, (float) minKnob_.getValue(),
                                  (float) maxKnob_.getValue());
                    refreshRangeTooltips();
                    repaint(vuArea_);   // the window ticks follow the knob
                };
                addAndMakeVisible(k);
            };
            initKnob(minKnob_, info.lo, 0.0);
            initKnob(maxKnob_, info.hi, 1.0);
            refreshRangeTooltips();

            follow_.setToggleState(info.follow, juce::dontSendNotification);
            follow_.setTooltip("MIDI-follow: moving this control navigates to "
                               "its module page (on by default). Switch off for "
                               "controls that move all the time - clicking the "
                               "row still navigates.");
            follow_.onClick = [this]
            {
                owner_.processor_.getMidiMap().setMappingFollow(
                    paramId_, follow_.getToggleState());
            };
            addAndMakeVisible(follow_);

            gear_.lit = info.curved || info.enveloped;
            gear_.setTooltip("Click: edit the mapping - response curve (EXPO / POINTS / "
                             "TABLE), input window, hysteresis, or an ENVELOPE the "
                             "press plays (GATE / ONE-SHOT / LOOP)\n"
                             "Hold: back to a plain linear mapping");
            gear_.onClick = [this]
            { if (owner_.onEditRequested) owner_.onEditRequested(paramId_); };
            gear_.onLongPress = [this]() -> bool
            {
                if (! gear_.lit)
                    return false;   // already neutral: nothing to wipe, no receipt
                // Everything the gear stands for goes at once - the law (mode,
                // input window, shape, hysteresis, points, table) AND the
                // envelope: the glyph is lit exactly while either is armed,
                // so a hold that left any of it behind would leave it lit and
                // lie. MIN/MAX are NOT touched: the two knobs hold-reset on
                // their own.
                owner_.processor_.getMidiMap().setMappingCurve(paramId_, {});
                owner_.processor_.getMidiMap().setMappingEnvelope(paramId_, {});
                syncFromEngine();
                if (owner_.onCurveReset) owner_.onCurveReset(paramId_);
                return true;   // acted → the gear blinks its receipt
            };
            addAndMakeVisible(gear_);

            // Phase — LFO rows only (a controller has no cycle to offset).
            isLfo_ = (info.type == MidiMappingEngine::kTypeLfo);
            lfoIndex_ = info.number;
            // On an LFO row the phase chip TAKES THE PLACE of F: MIDI-follow
            // is fed by processMidi, which an LFO never goes through, so the
            // toggle is inert there — and the designation must not lose a
            // single pixel to a control that does nothing.
            follow_.setVisible(! isLfo_);
            if (isLfo_)
            {
                phase_.deg = juce::roundToInt(info.phase * 360.0f) % 360;
                phase_.setTooltip("Phase of THIS destination along the LFO's "
                                  "cycle. Four destinations at 0 / 90 / 180 / "
                                  "270 turn one shape into a circular motion.\n"
                                  "Click: quarter turn.");
                phase_.onClick = [this]
                {
                    phase_.deg = (phase_.deg + 90) % 360;
                    owner_.processor_.getMidiMap().setMappingPhase(
                        paramId_, (float) phase_.deg / 360.0f);
                    phase_.repaint();
                };
                addAndMakeVisible(phase_);
            }

            remove_.setTooltip("Remove this MIDI mapping");
            remove_.onClick = [this]
            { owner_.processor_.getMidiMap().removeMappingFor(paramId_); };
            addAndMakeVisible(remove_);
        }

        const juce::String& paramId() const noexcept { return paramId_; }

        /** Knobs / F / ⚙ ← engine (the curve window edited them, no broadcast). */
        void syncFromEngine()
        {
            auto& eng = owner_.processor_.getMidiMap();
            float lo = 0.0f, hi = 1.0f;
            if (eng.getMappingRange(paramId_, lo, hi))
            {
                if (! minKnob_.isTurning()) minKnob_.setValue(lo, juce::dontSendNotification);
                if (! maxKnob_.isTurning()) maxKnob_.setValue(hi, juce::dontSendNotification);
                refreshRangeTooltips();
                repaint(vuArea_);
            }
            follow_.setToggleState(eng.mappingFollows(paramId_), juce::dontSendNotification);
            MidiMappingCurve    c;
            MidiMappingEnvelope e;
            const bool lit = (eng.getMappingCurve(paramId_, c) && ! c.isNeutral())
                          || (eng.getMappingEnvelope(paramId_, e) && ! e.isOff());
            if (lit != gear_.lit) { gear_.lit = lit; gear_.repaint(); }
        }

        /** "Here is your new modulation" — the row lights in the modulation
         *  hue for the usual glow, long enough to find the two knobs. */
        void flash()
        {
            flashMs_ = Sp3ctraControls::nowMs();
            repaint();
        }

        void paint(juce::Graphics& g) override
        {
            g.setColour(juce::Colour(0xff14141c));
            g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
            if (const float h = Sp3ctraControls::heatSince(flashMs_); h > 0.0f)
            {
                g.setColour(juce::Colour(Sp3ctraTheme::kColMod).withAlpha(0.30f * h));
                g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);
            }

            ParamIdentityLabel::draw(g, titleArea_, ident_, true, false);
            paintRangeReadout(g);
            paintVu(g);
        }

        /** While a knob is being turned its value takes the end of the title
         *  line: the knobs carry no text box, and setting a bound blind is
         *  not an option. Transient — nothing is written there at rest. */
        void paintRangeReadout(juce::Graphics& g)
        {
            if (turning_ == 0) return;
            auto& k = turning_ == 1 ? minKnob_ : maxKnob_;
            const juce::String txt = juce::String(turning_ == 1 ? "MIN " : "MAX ")
                                   + k.getTextFromValue(k.getValue());
            const juce::Font f(juce::FontOptions(10.5f));
            const int w = (int) std::ceil(juce::GlyphArrangement::getStringWidth(f, txt));
            auto box = titleArea_;
            box = box.removeFromRight(juce::jmin(box.getWidth(), w + 10));

            g.setColour(juce::Colour(0xff14141c).withAlpha(0.92f));
            g.fillRoundedRectangle(box.toFloat(), 3.0f);
            g.setFont(f);
            g.setColour(kAccent);
            g.drawText(txt, box, juce::Justification::centredRight, false);
        }

        /** The VU: pale ground = the MIN..MAX window the controller sweeps,
         *  amber fill = where the parameter stands right now, two ticks at
         *  the bounds. Nothing to fill yet (a virtual target no controller
         *  has touched) leaves the window alone. */
        void paintVu(juce::Graphics& g) const
        {
            const auto vu = vuArea_.toFloat();
            if (vu.isEmpty()) return;

            const float lo = (float) juce::jmin(minKnob_.getValue(), maxKnob_.getValue());
            const float hi = (float) juce::jmax(minKnob_.getValue(), maxKnob_.getValue());
            auto atX = [&vu](float t)
            { return vu.getX() + vu.getWidth() * juce::jlimit(0.0f, 1.0f, t); };

            g.setColour(juce::Colour(0xff181820));
            g.fillRect(vu);
            g.setColour(kAccent.withAlpha(0.16f));
            g.fillRect(juce::Rectangle<float>(atX(lo), vu.getY(),
                                              juce::jmax(1.0f, atX(hi) - atX(lo)),
                                              vu.getHeight()));
            if (live_ >= 0.0f)
            {
                g.setColour(kAccent.withAlpha(0.80f));
                g.fillRect(vu.withWidth(juce::jmax(1.0f, vu.getWidth() * live_)));
            }
            g.setColour(kAccent.withAlpha(0.70f));
            g.fillRect(juce::Rectangle<float>(atX(lo), vu.getY() - 1.0f, 1.0f,
                                              vu.getHeight() + 2.0f));
            g.fillRect(juce::Rectangle<float>(atX(hi) - 1.0f, vu.getY() - 1.0f, 1.0f,
                                              vu.getHeight() + 2.0f));
        }

        /** Panel timer (kLiveHz): the VU value and the turning readout are
         *  the only things that move between rebuilds — each repaints its
         *  own strip, never the row. */
        void tickLive()
        {
            // The window can slide on its own: an LFO whose destination was
            // aimed by hand carries its MIN/MAX along (engine tick). The two
            // knobs must say the truth without a rebuild — except the one
            // under the pointer.
            auto& eng = owner_.processor_.getMidiMap();
            float elo = 0.0f, ehi = 1.0f;
            if (eng.getMappingRange(paramId_, elo, ehi))
            {
                bool moved = false;
                if (! minKnob_.isTurning()
                    && std::abs((float) minKnob_.getValue() - elo) > 1.0e-4f)
                { minKnob_.setValue(elo, juce::dontSendNotification); moved = true; }
                if (! maxKnob_.isTurning()
                    && std::abs((float) maxKnob_.getValue() - ehi) > 1.0e-4f)
                { maxKnob_.setValue(ehi, juce::dontSendNotification); moved = true; }
                if (moved) { refreshRangeTooltips(); repaint(vuArea_); }
            }

            const float v = liveValue();
            if (std::abs(v - live_) > 0.002f) { live_ = v; repaint(vuArea_); }

            const int t = minKnob_.isTurning() ? 1 : (maxKnob_.isTurning() ? 2 : 0);
            if (t != 0 || t != turning_) { turning_ = t; repaint(titleArea_); }

            if (Sp3ctraControls::heatSince(flashMs_) > 0.0f) repaint();
        }

        void resized() override
        {
            //   EVENT · CHAIN · MODULE · NAME     (min) (max)  ⚙ F ✕
            //   ▓▓▓▓▓▓▓░░░░░░░░
            // The right block is FIXED — the designation keeps every pixel
            // the glyphs and the two knobs don't need, whatever the band's
            // width (before, two elastic bars ate up to 240 px of it).
            auto b = getLocalBounds().reduced(kPad, 2);

            const int gh = juce::jmin(18, b.getHeight());
            auto glyph = [&b, gh](juce::Component& c, int w)
            { c.setBounds(b.removeFromRight(w).withSizeKeepingCentre(w, gh)); };

            glyph(remove_, 16);  b.removeFromRight(2);
            if (isLfo_) glyph(phase_,  22);   // the chip stands where F stands
            else        glyph(follow_, 18);
            b.removeFromRight(3);
            glyph(gear_,   18);  b.removeFromRight(6);

            const int kd = juce::jmin(kKnobD, b.getHeight());
            maxKnob_.setBounds(b.removeFromRight(kd).withSizeKeepingCentre(kd, kd));
            b.removeFromRight(2);
            minKnob_.setBounds(b.removeFromRight(kd).withSizeKeepingCentre(kd, kd));
            b.removeFromRight(8);

            vuArea_ = b.removeFromBottom(kVuH);
            b.removeFromBottom(2);
            titleArea_ = b;
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            // Title-area click (name or VU) → navigate to the owning module
            // page; the knobs and glyphs consume their own clicks first.
            if (e.mods.isPopupMenu()
                || e.getPosition().x > (titleArea_.isEmpty()
                                            ? getWidth() : titleArea_.getRight()))
                return;
            // An LFO row leads BOTH ways: to the destination's module page,
            // and to the LFO itself in the bank above.
            if (isLfo_ && owner_.onLfoSelected) owner_.onLfoSelected(lfoIndex_);
            if (owner_.onMappingSelected) owner_.onMappingSelected(paramId_);
        }

    private:
        /** Value overlays speak the TARGET's language: normalised 0..1 in the
         *  engine, rendered through the parameter's own text conversion
         *  ("-16 dB", "1.29 ms"…); virtual targets fall back to percent.
         *  Feeds the turning readout, the double-click bubble and the
         *  tooltips — the knobs themselves show no number. */
        void installText(juce::Slider& k)
        {
            if (auto* p = owner_.processor_.getAPVTS().getParameter(paramId_))
                k.textFromValueFunction = [p](double v)
                { return p->getText((float) juce::jlimit(0.0, 1.0, v), 24); };
            else
                k.textFromValueFunction = [](double v)
                { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
            k.updateText();
        }

        /** The knobs carry no caption: hovering one must therefore say both
         *  what it is AND where it stands. Refreshed on every edit. */
        void refreshRangeTooltips()
        {
            minKnob_.setTooltip(
                "MIN " + minKnob_.getTextFromValue(minKnob_.getValue())
                + " - where the controller's 0 lands.\n"
                  "Turn above MAX to invert the control.\n"
                  "Double-click: back to the bottom. Hold: type it.");
            maxKnob_.setTooltip(
                "MAX " + maxKnob_.getTextFromValue(maxKnob_.getValue())
                + " - where the controller's 127 lands.\n"
                  "Double-click: back to the top. Hold: type it.");
        }

        /** Where the parameter stands NOW, normalised — the VU fill. APVTS
         *  targets answer straight (every source counts, not just MIDI);
         *  virtual ones only know the last output the mapping applied.
         *  -1 = nothing to show yet. */
        float liveValue() const
        {
            if (auto* p = owner_.processor_.getAPVTS().getParameter(paramId_))
                return juce::jlimit(0.0f, 1.0f, p->getValue());
            float in01 = 0.0f, out01 = 0.0f;
            if (owner_.processor_.getMidiMap().lastMappedValue(paramId_, in01, out01))
                return juce::jlimit(0.0f, 1.0f, out01);
            return -1.0f;
        }

        MidiMapPanel&    owner_;
        juce::String     paramId_;
        ParamIdentity    ident_;
        RangeKnob        minKnob_, maxKnob_;
        GearButton       gear_;
        FollowButton     follow_;
        RemoveButton     remove_;
        PhaseChip        phase_;
        bool             isLfo_    { false };
        int              lfoIndex_ { 0 };
        juce::Rectangle<int> titleArea_, vuArea_;
        double flashMs_ { Sp3ctraControls::kNever };   ///< reveal glow
        float live_    { -1.0f };   ///< last VU value painted
        int   turning_ { 0 };       ///< 0 none, 1 MIN, 2 MAX (readout gate)
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MapRow)
    };

    //==========================================================================
    void applyPendingReveal()
    {
        if (pendingReveal_.isEmpty()) return;
        for (int i = 0; i < (int) rows_.size(); ++i)
        {
            if (rows_[(size_t) i]->paramId() != pendingReveal_) continue;
            if (collapsed_) setCollapsed(false, true);
            viewport_.setViewPosition(0, juce::jmax(0,
                i * (kRowH + kRowGap) - juce::jmax(0, viewport_.getHeight() - kRowH) / 2));
            rows_[(size_t) i]->flash();
            pendingReveal_.clear();
            return;
        }
    }

    void rebuildRows()
    {
        rows_.clear();
        for (const auto& m : processor_.getMidiMap().allMappings())
        {
            auto row = std::make_unique<MapRow>(*this, m);
            rowsHolder_.addAndMakeVisible(row.get());
            rows_.push_back(std::move(row));
        }
        resized();
        repaint();
        if (onContentChanged) onContentChanged();
        applyPendingReveal();   // a modulation posted just before this rebuild
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        // Table mutations only (learn / remove / restore) — range edits don't
        // broadcast. Rebuild is cheap at these row counts.
        rebuildRows();
    }

    //── Collapsed "now playing" readout ──────────────────────────────────────
    // The editor's MIDI-follow drain notes every mapped move on the MidiTouch
    // bus; while the list is folded the header shows WHAT is being played —
    // except for mappings whose F follow was switched off (they asked not to
    // announce themselves).
    void midiTouched(const juce::String& paramId) override
    {
        if (! collapsed_) return;
        if (! processor_.getMidiMap().mappingFollows(paramId)) return;

        playing_      = describeParam(processor_, paramId);
        playingParam_ = paramId;   // paint reads its live value for the bar
        playingMs_    = Sp3ctraControls::nowMs();
        repaint(0, 0, getWidth(), kHeaderH);
    }

    /** The panel's single heartbeat: folded it fades the "now playing"
     *  readout, unfolded it drives the row VUs. Off screen (the zone-4
     *  column collapsed, another band showing) it costs nothing. */
    void timerCallback() override
    {
        if (! isShowing()) return;

        if (collapsed_)
        {
            // One repaint PAST the last live frame, so the readout actually
            // clears instead of freezing on its last drawn state.
            const bool live = Sp3ctraControls::heatSince(playingMs_) > 0.0f;
            if (live || headerWasLive_) repaint(0, 0, getWidth(), kHeaderH);
            headerWasLive_ = live;
            return;
        }
        for (auto& row : rows_) row->tickLive();
    }

public:
    /** Fired after the row list rebuilt (count → height changed): the editor
     *  relayouts zone 4 so the section grows/shrinks with its content. */
    std::function<void()> onContentChanged;

private:
    Sp3ctraAudioProcessor& processor_;
    juce::String    pendingReveal_;   ///< a row to show as soon as it exists
    Chevron         chevron_;
    juce::Viewport  viewport_;
    juce::Component rowsHolder_;
    std::vector<std::unique_ptr<MapRow>> rows_;
    bool collapsed_ { false };

    // Collapsed "now playing" readout (see midiTouched).
    ParamIdentity playing_;
    juce::String  playingParam_;
    double       playingMs_ { Sp3ctraControls::kNever };
    bool         headerWasLive_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiMapPanel)
};

/** The mapped-badge amber (MidiLearnAttachment's dot) — the one colour that
 *  already MEANS "this control has a MIDI mapping" everywhere in the UI. */
inline const juce::Colour MidiMapPanel::kAccent { 0xffe0a24a };
