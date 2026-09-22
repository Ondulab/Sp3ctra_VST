/**
 * @file MidiCurveEditor.h
 * @brief The MIDI CURVE window — what ONE MIDI mapping DOES with its event:
 *        a transfer law (the complete IN → OUT graph) or an ENVELOPE (the
 *        press plays the parameter over time), drawn and edited in place.
 *
 * Opened by the gear of a MIDI MAP row, or by "Edit MIDI mapping…" in any
 * control's right-click menu (MidiMappingEngine::onEditRequested). One
 * window, retargeted on every open (PluginEditor owns it).
 *
 *   ┌ CC 32  (1) CHAIN · DC BLOCK · Amount ───────────────────── [F] ┐
 *   │[LINEAR] [EXPO] [POINTS] [TABLE] ENV [GATE][1-SHOT][LOOP][RETRIG]│
 *   │ ┌ IN → OUT ───────────────────────────────────────────────────┐ │
 *   │ │ OUT MAX ┤          ╭────── curve (the whole law)            │ │
 *   │ │         │      ╭───╯   ● live dot (what the controller does)│ │
 *   │ │ OUT MIN ┤ ─────╯                                            │ │
 *   │ │         └──┬──────────────────────────────┬────── 0 … 127   │ │
 *   │ │          IN MIN                         IN MAX              │ │
 *   │ └─────────────────────────────────────────────────────────────┘ │
 *   │   IN MIN     IN MAX     OUT MIN     OUT MAX                     │
 *   │   SHAPE      HYST       SMOOTH      [RESET]                     │
 *   └─────────────────────────────────────────────────────────────────┘
 *
 * The plot is the full controller travel (x, 0 … 127) against the full
 * parameter (y, normalised): the curve shows EXACTLY what the controller
 * does — flat outside the input window, spanning OUT MIN … OUT MAX. The
 * live dot is the last (in, out) pair the engine applied.
 *
 * Grabbable (lime, Sp3ctraHandles): the IN MIN/MAX thumbs under the axis,
 * the OUT MIN/MAX thumbs left of it (drag MIN above MAX to invert, as in
 * the panel row), and the mode's own handle(s):
 *   EXPO    one node at the middle of the curve — drag up / down = exponent
 *   POINTS  click adds a breakpoint (max 16), drag moves, double-click
 *           deletes; SMOOTH blends the polyline toward a monotone cubic
 *   TABLE   drag to DRAW (one output per CC value); SMOOTH blurs it
 * HYST (any mode) opens a backlash loop: the rising branch (solid, ▲) lags
 * the falling one (dashed, ▼) by the loop width — and ±1 jitter dies.
 *
 * ENVELOPE (a trigger chip lit — MidiMappingEnvelope.h): the plot turns
 * into TIME → OUT, the classic A / D / S / R picture between OUT MIN (rest)
 * and OUT MAX (peak) — time PROPORTIONAL and auto-fitted, the sustain
 * plateau of fixed width, ONE drawing for every trigger (the plateau is
 * solid where GATE holds, dashed where ONE-SHOT skips it or LOOP repeats
 * A + D; its band says which). Under the axis each segment carries its
 * time ("A 10 ms"). Grabbable: the ATTACK corner (x = time), the DECAY
 * corner (x = time, y = SUSTAIN), the RELEASE corner — times drag like
 * knobs, relative, the plot refits under the hand — and each segment's
 * KNEE, a ring ON the curve that goes wherever it is put (grab the line
 * itself and pull: the segment bends through the pointer, monotone, no
 * overshoot). A white dot rides the curve while a press plays it. Boxes:
 * ATTACK · DECAY · SUSTAIN · RELEASE / VEL · OUT MIN · OUT MAX · RESET.
 * Clicking the lit trigger chip turns the envelope off (back to the curve);
 * clicking a curve chip does the same. RETRIG, the toggle chip after the
 * triggers, is independent of them: lit, a press while the envelope plays
 * restarts the attack from the rest (the same shape on every hit); off, it
 * resumes from the current level. Dim and inert while no trigger is lit.
 *
 * Every edit goes to MidiMappingEngine::setMappingCurve / setMappingRange
 * (baked at once, autosaved, NO broadcast — same contract as the panel
 * bars); the panel row resyncs through onEdited. The display (frame,
 * curve, captions) wears the mapping amber; the controls wear the control
 * colour, like every module editor.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/MidiMappingCurve.h"
#include "../midi/MidiMappingEngine.h"
#include "../midi/MidiMappingEnvelope.h"
#include "../session/MachinePrefs.h"
#include "MidiMapGlyphs.h"
#include "ModuleEditorChrome.h"
#include "ParamIdentity.h"
#include "Sp3ctraBarSlider.h"
#include "Sp3ctraControls.h"
#include "Sp3ctraGestures.h"
#include "Sp3ctraHandles.h"
#include <array>
#include <cmath>
#include <functional>

class MidiCurveEditorComponent : public juce::Component,
                                 private juce::Timer,
                                 private juce::ChangeListener,
                                 private Sp3ctraControls::MidiTouch::Sink
{
public:
    static constexpr int kDefaultW = 620, kDefaultH = 470;
    static constexpr int kMinW     = 600, kMinH     = 390;   // the two chip families side by side (4 + ENV + 3 + RETRIG)
    static constexpr int kPad      = 10;
    static constexpr int kHeaderH  = 26;
    static constexpr int kChipRowH = 24;
    static constexpr int kChipW    = 62;
    static constexpr int kChipGap  = 6;
    static constexpr int kRetrigGap = 12;  ///< RETRIG stands apart from the one-lit trigger family
    static constexpr int kAxisL    = 30;   ///< room left of the plot: OUT thumbs + label
    static constexpr int kAxisB    = 18;   ///< room under the plot: IN thumbs + 0…127

    /** A range / follow / curve edit landed in the engine — the panel row
     *  resyncs its bars and glyphs (no broadcast on those paths). */
    std::function<void()> onEdited;
    /** The mapping vanished (removed / re-learnt elsewhere / session
     *  restore) — the owner closes the window. Fired from the engine's
     *  ChangeBroadcaster callback: defer any deletion. */
    std::function<void()> onUnmapped;

    explicit MidiCurveEditorComponent(Sp3ctraAudioProcessor& p) : processor_(p)
    {
        setOpaque(true);
        setWantsKeyboardFocus(false);

        follow_.setTooltip("MIDI-follow: a move of this control navigates to its "
                           "module page (on by default). Switch off for controls "
                           "that move all the time.");
        follow_.onClick = [this]
        {
            engine().setMappingFollow(paramId_, follow_.getToggleState());
            if (onEdited) onEdited();
        };
        addAndMakeVisible(follow_);

        auto initBox = [this](Sp3ctraBarSlider& b, double lo, double hi, double step,
                              double def, const juce::String& tip)
        {
            b.setRange(lo, hi, step);
            b.setDoubleClickReturnValue(true, def);   // long press = default
            b.setTooltip(tip);
            addAndMakeVisible(b);
        };
        initBox(inMinBox_, 0.0, 127.0, 1.0, 0.0,
                "Input window start - the controller value the curve starts at "
                "(below it the output stays at the curve's start).");
        initBox(inMaxBox_, 0.0, 127.0, 1.0, 127.0,
                "Input window end - the controller value that reaches the curve's end.");
        initBox(outMinBox_, 0.0, 1.0, 0.001, 0.0,
                "Output minimum - where the curve's start lands. Above MAX inverts.");
        initBox(outMaxBox_, 0.0, 1.0, 0.001, 1.0,
                "Output maximum - where the curve's end lands.");
        initBox(shapeBox_, -1.0, 1.0, 0.01, 0.0,
                "EXPO exponent - x^k: above 1 a slow start, below 1 a fast one.");
        initBox(hystBox_, 0.0, (double) MidiMappingCurve::kMaxHyst, 0.005, 0.0,
                "Hysteresis - width of the backlash loop, as a fraction of the "
                "travel: the output follows a rising sweep late and a falling one "
                "early, and controller jitter within the loop is ignored.");
        initBox(smoothBox_, 0.0, 1.0, 0.01, 0.0,
                "POINTS: blend from straight segments to a monotone curve. "
                "TABLE: blur radius of the drawn table.");

        inMinBox_.textFromValueFunction = inMaxBox_.textFromValueFunction =
            [](double v) { return juce::String(juce::roundToInt(v)); };
        shapeBox_.textFromValueFunction =
            [](double v) { return "x" + juce::String(std::pow(10.0, v), 2); };
        shapeBox_.valueFromTextFunction = [](const juce::String& t)
        {
            const double k = t.retainCharacters("0123456789.-").getDoubleValue();
            return k > 0.0 ? juce::jlimit(-1.0, 1.0, std::log10(k)) : 0.0;
        };
        for (auto* b : { &hystBox_, &smoothBox_ })
        {
            b->textFromValueFunction =
                [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
            b->valueFromTextFunction =
                [](const juce::String& t)
                { return t.retainCharacters("0123456789.-").getDoubleValue() / 100.0; };
        }

        inMinBox_.onValueChange = [this]
        {
            curve_.inLo = juce::jlimit(0.0f, curve_.inHi - MidiMappingCurve::kMinInSpan,
                                       (float) (inMinBox_.getValue() / 127.0));
            commitCurve(); syncBoxes();
        };
        inMaxBox_.onValueChange = [this]
        {
            curve_.inHi = juce::jlimit(curve_.inLo + MidiMappingCurve::kMinInSpan, 1.0f,
                                       (float) (inMaxBox_.getValue() / 127.0));
            commitCurve(); syncBoxes();
        };
        outMinBox_.onValueChange = [this]
        { lo_ = (float) outMinBox_.getValue(); commitRange(); };
        outMaxBox_.onValueChange = [this]
        { hi_ = (float) outMaxBox_.getValue(); commitRange(); };
        shapeBox_.onValueChange = [this]
        { curve_.shape = (float) shapeBox_.getValue(); commitCurve(); };
        hystBox_.onValueChange = [this]
        { curve_.hyst = (float) hystBox_.getValue(); commitCurve(); };
        smoothBox_.onValueChange = [this]
        { curve_.smooth = (float) smoothBox_.getValue(); commitCurve(); };

        // ── Envelope boxes (shown while a trigger chip is lit) ──────────────
        using Env = MidiMappingEnvelope;
        initBox(attBox_, 0.0, (double) Env::kMaxAttack, 0.001, (double) Env::kDefAttack,
                "Attack - time from the rest to the peak after a press.");
        initBox(decBox_, 0.0, (double) Env::kMaxDecay, 0.001, (double) Env::kDefDecay,
                "Decay - time from the peak down to the sustain. ONE-SHOT with "
                "SUSTAIN at 100 %: a hold at the peak.");
        initBox(susBox_, 0.0, 1.0, 0.01, (double) Env::kDefSustain,
                "Sustain - the level held while the control stays pressed (GATE), "
                "the trough of a LOOP, the shoulder of a ONE-SHOT. As a fraction "
                "of the peak.");
        initBox(relBox_, 0.0, (double) Env::kMaxRelease, 0.001, (double) Env::kDefRelease,
                "Release - time back to the rest once the control is released "
                "(ONE-SHOT: right after the decay).");
        initBox(velBox_, 0.0, 1.0, 0.01, (double) Env::kDefVel,
                "Velocity - how much the press value (note velocity, CC value, the "
                "CIS hit strength) scales the peak: 0 = every press reaches the "
                "peak, 100 % = the peak is the velocity.");
        for (auto* b : { &attBox_, &decBox_ }) b->setSkewFactorFromMidPoint(0.3);
        relBox_.setSkewFactorFromMidPoint(0.5);
        for (auto* b : { &attBox_, &decBox_, &relBox_ })
        {
            b->textFromValueFunction = [](double v) { return timeText(v); };
            b->valueFromTextFunction = [](const juce::String& t) { return timeFromText(t); };
        }
        for (auto* b : { &susBox_, &velBox_ })
        {
            b->textFromValueFunction =
                [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
            b->valueFromTextFunction =
                [](const juce::String& t)
                { return t.retainCharacters("0123456789.-").getDoubleValue() / 100.0; };
        }

        attBox_.onValueChange = [this] { env_.attack  = (float) attBox_.getValue(); commitEnv(); };
        decBox_.onValueChange = [this] { env_.decay   = (float) decBox_.getValue(); commitEnv(); };
        susBox_.onValueChange = [this] { env_.sustain = (float) susBox_.getValue(); commitEnv(); };
        relBox_.onValueChange = [this] { env_.release = (float) relBox_.getValue(); commitEnv(); };
        velBox_.onValueChange = [this] { env_.vel     = (float) velBox_.getValue(); commitEnv(); };

        engine().addChangeListener(this);
        Sp3ctraControls::MidiTouch::add(this);
        startTimerHz(30);
    }

    ~MidiCurveEditorComponent() override
    {
        stopTimer();
        Sp3ctraControls::MidiTouch::remove(this);
        engine().removeChangeListener(this);
    }

    //==========================================================================
    /** Load `paramId`'s mapping from the engine (or reload it after a
     *  table mutation). */
    void setTarget(const juce::String& paramId)
    {
        paramId_ = paramId;
        param_   = processor_.getAPVTS().getParameter(paramId);
        ident_   = describeParam(processor_, paramId);
        if (! engine().getMappingCurve(paramId, curve_)) curve_ = {};
        if (! engine().getMappingEnvelope(paramId, env_)) env_ = {};
        if (! engine().getMappingRange(paramId, lo_, hi_)) { lo_ = 0.0f; hi_ = 1.0f; }
        liveStage_ = MidiMappingEnvelope::Stage::Idle; liveT_ = liveLevel_ = 0.0f;
        follow_.setToggleState(engine().mappingFollows(paramId), juce::dontSendNotification);
        liveIn_ = liveOut_ = -1.0f;
        engine().lastMappedValue(paramId, liveIn_, liveOut_);
        touchMs_ = Sp3ctraControls::kNever;
        drag_ = hover_ = {};
        installUnits();
        rebake();
        syncBoxes();
        repaint();
    }

    const juce::String&  paramId()  const noexcept { return paramId_; }
    const ParamIdentity& identity() const noexcept { return ident_; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        using namespace Sp3ctraHandles;
        const juce::Colour accent(MidiMapGlyphs::kAccent);
        g.fillAll(juce::Colour(Sp3ctraTheme::kColBg));

        // ── Header: the mapping's identity (event · chain · module · name) ──
        ParamIdentityLabel::draw(g, headerArea_, ident_, true, false);

        // ── Mode chips (the curve family) · trigger chips (the envelope) ────
        // One family lit at a time: a lit trigger means the press PLAYS the
        // parameter and the curve is out of the picture.
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        for (int i = 0; i < 4; ++i)
            drawChip(g, chipRects_[(size_t) i].toFloat(), kModeNames[i],
                     ! envView() && (int) curve_.mode == i, hoverChip_ == i);
        for (int i = 0; i < 3; ++i)
            drawChip(g, trigRects_[(size_t) i].toFloat(), kTrigNames[i],
                     (int) env_.trigger == i + 1, hoverTrig_ == i);
        // RETRIG: a toggle, not a member of the one-lit family — dim while
        // no trigger is lit (nothing to retrigger).
        {
            // (a transparency layer: drawChip sets its own colours, which
            //  would discard a plain setOpacity)
            const bool dim = ! envView();
            if (dim) g.beginTransparencyLayer(0.4f);
            drawChip(g, retrigRect_.toFloat(), "RETRIG", env_.retrig && ! dim,
                     hoverRetrig_ && ! dim);
            if (dim) g.endTransparencyLayer();
        }
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        g.setColour(accent.withAlpha(0.55f));
        g.drawText("ENV", trigRects_[0].getX() - 30, trigRects_[0].getY(), 26,
                   trigRects_[0].getHeight(), juce::Justification::centredRight, false);

        if (envView())
        {
            paintEnvelope(g, accent);
            return;
        }

        // ── Frame + captions ────────────────────────────────────────────────
        ModuleChrome::drawFrame  (g, frame_, accent);
        ModuleChrome::drawCaption(g, frame_, accent, juce::String::fromUTF8("IN \xE2\x86\x92 OUT"));
        ModuleChrome::drawReadout(g, frame_, accent, hintText());

        // ── Grid: controller quarters (x) and output thirds (y) ─────────────
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        for (int i = 0; i <= 4; ++i)
        {
            const float v = (float) i / 4.0f;
            const float x = xOf(v);
            g.setColour(accent.withAlpha(i == 0 || i == 4 ? 0.28f : 0.12f));
            g.drawVerticalLine((int) x, plot_.getY(), plot_.getBottom());
            g.setColour(accent.withAlpha(0.5f));
            g.drawText(juce::String(juce::roundToInt(v * 127.0f)),
                       (int) x - 14, (int) plot_.getBottom() + 5, 28, 10,
                       juce::Justification::centred, false);
        }
        for (int i = 0; i <= 2; ++i)
        {
            const float y = yOf((float) i / 2.0f);
            g.setColour(accent.withAlpha(i == 0 || i == 2 ? 0.28f : 0.12f));
            g.drawHorizontalLine((int) y, plot_.getX(), plot_.getRight());
        }
        g.setColour(accent.withAlpha(0.5f));
        g.drawText("OUT", (int) frame_.getX() + 6, (int) plot_.getY() - 2, kAxisL - 4, 10,
                   juce::Justification::centredLeft, false);
        g.drawText("IN", (int) plot_.getRight() - 30, (int) plot_.getBottom() + 5, 30, 10,
                   juce::Justification::centredRight, false);

        // ── The active window: IN MIN…MAX × OUT MIN…MAX ─────────────────────
        {
            const float x0 = xOf(curve_.inLo), x1 = xOf(curve_.inHi);
            const float y0 = yOf(juce::jmin(lo_, hi_)), y1 = yOf(juce::jmax(lo_, hi_));
            g.setColour(accent.withAlpha(0.07f));
            g.fillRect(juce::Rectangle<float>(x0, y1, x1 - x0, y0 - y1));
            g.setColour(accent.withAlpha(0.22f));
            g.drawVerticalLine  ((int) x0, plot_.getY(), plot_.getBottom());
            g.drawVerticalLine  ((int) x1, plot_.getY(), plot_.getBottom());
            g.drawHorizontalLine((int) y0, plot_.getX(), plot_.getRight());
            g.drawHorizontalLine((int) y1, plot_.getX(), plot_.getRight());
        }

        // ── Reference diagonal (the neutral law) ────────────────────────────
        {
            const float dash[2] = { 3.0f, 4.0f };
            g.setColour(juce::Colours::white.withAlpha(0.10f));
            g.drawDashedLine({ plot_.getX(), plot_.getBottom(), plot_.getRight(), plot_.getY() },
                             dash, 2, 1.0f);
        }

        // ── The law — one curve, or the two branches of the loop ────────────
        g.saveState();
        g.reduceClipRegion(plot_.expanded(2.0f).toNearestInt());
        if (curve_.hyst > 0.0f)
        {
            const auto up = curvePath(+1), dn = curvePath(-1);
            g.setColour(accent.withAlpha(0.95f));
            g.strokePath(up, juce::PathStrokeType(2.0f));
            juce::Path dashed;
            const float dash[2] = { 5.0f, 3.0f };
            juce::PathStrokeType(1.6f).createDashedStroke(dashed, dn, dash, 2);
            g.setColour(accent.withAlpha(0.7f));
            g.fillPath(dashed);
            drawBranchArrow(g, +1, accent);
            drawBranchArrow(g, -1, accent);
        }
        else
        {
            g.setColour(accent.withAlpha(0.95f));
            g.strokePath(curvePath(0), juce::PathStrokeType(2.0f));
        }
        g.restoreState();

        // ── Range thumbs (lime) ─────────────────────────────────────────────
        auto look = [this](Target t) { return stateOf(drag_.t == t, hover_.t == t); };
        drawThumb(g, inLoThumb (), look(Target::InLo));
        drawThumb(g, inHiThumb (), look(Target::InHi));
        drawThumb(g, outLoThumb(), look(Target::OutLo));
        drawThumb(g, outHiThumb(), look(Target::OutHi));

        // ── Mode handles ────────────────────────────────────────────────────
        if (curve_.mode == MidiMappingCurve::Mode::Expo)
            drawNode(g, shapeNode(), look(Target::Shape));
        else if (curve_.mode == MidiMappingCurve::Mode::Points)
        {
            const int n = (int) curve_.points.size();
            for (int i = 0; i < n; ++i)
                drawNode(g, fromCurve(curve_.points[(size_t) i]),
                         stateOf(drag_.t == Target::Point && drag_.index == i,
                                 hover_.t == Target::Point && hover_.index == i));
        }

        // ── Live dot: where the controller last put the parameter ───────────
        if (liveIn_ >= 0.0f)
        {
            const float heat = Sp3ctraControls::heatSince(touchMs_);
            const juce::Point<float> pt(xOf(liveIn_), yOf(liveOut_));
            g.setColour(juce::Colours::white.withAlpha(0.10f + 0.25f * heat));
            g.drawVerticalLine  ((int) pt.x, pt.y, plot_.getBottom());
            g.drawHorizontalLine((int) pt.y, plot_.getX(), pt.x);
            drawNode(g, pt, Sp3ctraControls::Look(Sp3ctraControls::State::Idle, heat), 4.0f);
            if (heat > 0.0f && drag_.t == Target::None)
                Sp3ctraHandles::drawReadout(g, inText(liveIn_) + arrow() + unitText(liveOut_),
                                            pt, plot_);
        }

        // ── Readout of the handle under the pointer / in hand ───────────────
        {
            const auto h = drag_.t != Target::None ? drag_ : hover_;
            const juce::String txt = readoutText(h);
            if (txt.isNotEmpty())
                Sp3ctraHandles::drawReadout(g, txt, anchorOf(h), plot_);
        }

        // ── Box labels + RESET chip ─────────────────────────────────────────
        ModuleChrome::drawBoxLabel(g, inMinBox_,  accent, "IN MIN");
        ModuleChrome::drawBoxLabel(g, inMaxBox_,  accent, "IN MAX");
        ModuleChrome::drawBoxLabel(g, outMinBox_, accent, "OUT MIN");
        ModuleChrome::drawBoxLabel(g, outMaxBox_, accent, "OUT MAX");
        ModuleChrome::drawBoxLabel(g, shapeBox_,  accent, "SHAPE");
        ModuleChrome::drawBoxLabel(g, hystBox_,   accent, "HYST");
        ModuleChrome::drawBoxLabel(g, smoothBox_, accent, "SMOOTH");
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        drawChip(g, resetRect_.toFloat(), "RESET", false, hoverReset_);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(kPad);

        headerArea_ = r.removeFromTop(kHeaderH);
        follow_.setBounds(headerArea_.removeFromRight(22).reduced(0, 3));
        headerArea_.removeFromRight(8);
        r.removeFromTop(4);

        auto chips = r.removeFromTop(kChipRowH);
        int cx = chips.getX();
        for (auto& c : chipRects_)
        {
            c = { cx, chips.getY() + 2, kChipW, kChipRowH - 4 };
            cx += kChipW + kChipGap;
        }
        // The trigger family + RETRIG, packed against the right edge.
        int tx = chips.getRight() - 4 * kChipW - 2 * kChipGap - kRetrigGap;
        for (auto& c : trigRects_)
        {
            c = { tx, chips.getY() + 2, kChipW, kChipRowH - 4 };
            tx += kChipW + kChipGap;
        }
        retrigRect_ = { tx - kChipGap + kRetrigGap, chips.getY() + 2, kChipW, kChipRowH - 4 };
        r.removeFromTop(6);

        auto rowB = r.removeFromBottom(ModuleChrome::kBoxRowH);
        r.removeFromBottom(ModuleChrome::kRowGap);
        auto rowA = r.removeFromBottom(ModuleChrome::kBoxRowH);
        r.removeFromBottom(ModuleChrome::kRowGap);

        frame_ = r.toFloat();
        plot_  = ModuleChrome::plotOf(frame_).withTrimmedLeft((float) kAxisL)
                                             .withTrimmedBottom((float) kAxisB);

        rowA_ = rowA; rowB_ = rowB;
        layoutBoxes();
        {
            const int bw = (rowB.getWidth() - 3 * ModuleChrome::kBoxGap) / 4;
            resetRect_ = { rowB.getX() + 3 * (bw + ModuleChrome::kBoxGap),
                           rowB.getY() + ModuleChrome::kLabelH, bw, ModuleChrome::kBoxH };
        }
    }

    /** The two box rows belong to the view: the curve's seven boxes + RESET,
     *  or the envelope's six + the OUT range (shared boxes move). */
    void layoutBoxes()
    {
        const bool ev = envView();
        if (ev)
        {
            ModuleChrome::layoutBoxRow(rowA_, { &attBox_, &decBox_, &susBox_, &relBox_ });
            ModuleChrome::layoutBoxRow(rowB_, { &velBox_, &outMinBox_, &outMaxBox_, nullptr });
        }
        else
        {
            ModuleChrome::layoutBoxRow(rowA_, { &inMinBox_, &inMaxBox_, &outMinBox_, &outMaxBox_ });
            ModuleChrome::layoutBoxRow(rowB_, { &shapeBox_, &hystBox_, &smoothBox_, nullptr });
        }
        for (auto* b : { &inMinBox_, &inMaxBox_, &shapeBox_, &hystBox_, &smoothBox_ })
            b->setVisible(! ev);
        for (auto* b : { &attBox_, &decBox_, &susBox_, &relBox_, &velBox_ })
            b->setVisible(ev);
    }

    //==========================================================================
    void mouseMove(const juce::MouseEvent& e) override
    {
        const auto h     = hitAt(e.position);
        const int  chip  = chipAt(e.getPosition());
        const int  trig  = trigAt(e.getPosition());
        const bool reset = resetRect_.contains(e.getPosition());
        const bool retrig = envView() && retrigRect_.contains(e.getPosition());
        if (h.t != hover_.t || h.index != hover_.index || chip != hoverChip_
            || trig != hoverTrig_ || reset != hoverReset_ || retrig != hoverRetrig_)
        {
            hover_ = h; hoverChip_ = chip; hoverTrig_ = trig; hoverReset_ = reset;
            hoverRetrig_ = retrig;
            repaint();
        }
        setMouseCursor(h.t == Target::Paint ? juce::MouseCursor::CrosshairCursor
                     : (h.t != Target::None || chip >= 0 || trig >= 0 || reset || retrig)
                         ? juce::MouseCursor::PointingHandCursor
                         : juce::MouseCursor::NormalCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hover_ = {}; hoverChip_ = -1; hoverTrig_ = -1; hoverReset_ = false;
        hoverRetrig_ = false;
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { showMenu(); return; }

        if (const int trig = trigAt(e.getPosition()); trig >= 0)
        {
            setTrigger((MidiMappingEnvelope::Trigger) (trig + 1));
            return;
        }
        if (retrigRect_.contains(e.getPosition()))
        {
            if (envView()) { env_.retrig = ! env_.retrig; commitEnv(); }
            return;   // inert while no trigger is lit
        }
        if (const int chip = chipAt(e.getPosition()); chip >= 0)
        {
            setMode((MidiMappingCurve::Mode) chip);
            return;
        }
        if (resetRect_.contains(e.getPosition()))
        {
            if (envView()) { env_.resetShape();   commitEnv();   }
            else           { curve_.resetShape(); commitCurve(); }
            syncBoxes();
            return;
        }

        Hit h = hitAt(e.position);
        if (isEnvTarget(h.t))
        {
            // Time handles drag like knobs: remember where the gesture
            // started — the plot refits under the hand as the times move.
            dragOrigin_ = e.position;
            dragA0_ = env_.attack; dragD0_ = env_.decay; dragR0_ = env_.release;
        }
        if (h.t == Target::None && ! envView() && plot_.contains(e.position))
        {
            if (curve_.mode == MidiMappingCurve::Mode::Points)
            {
                const int idx = addPoint(toCurve(e.position));
                if (idx >= 0) h = { Target::Point, idx };
            }
            else if (curve_.mode == MidiMappingCurve::Mode::Table)
            {
                h = { Target::Paint, -1 };
                paintIdx_ = -1;
                paintTo(toCurve(e.position));
            }
        }
        drag_ = h;
        if (boxFor(h.t) != nullptr)
            hold_.arm(e, [this] { holdToType(); });   // long press = type
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (hold_.fired()) return;            // the entry bubble owns the rest
        hold_.moved(e);
        using Mode = MidiMappingCurve::Mode;
        switch (drag_.t)
        {
            case Target::InLo:
                curve_.inLo = juce::jlimit(0.0f, curve_.inHi - MidiMappingCurve::kMinInSpan,
                                           snapCC(v01At(e.position.x)));
                commitCurve(); syncBoxes(); break;
            case Target::InHi:
                curve_.inHi = juce::jlimit(curve_.inLo + MidiMappingCurve::kMinInSpan, 1.0f,
                                           snapCC(v01At(e.position.x)));
                commitCurve(); syncBoxes(); break;
            case Target::OutLo:
                lo_ = o01At(e.position.y); commitRange(); syncBoxes(); break;
            case Target::OutHi:
                hi_ = o01At(e.position.y); commitRange(); syncBoxes(); break;
            case Target::Shape:
            {
                // The node sits at x = ½: its height is 0.5^k → k from the
                // pointer, then back to the −1 … 1 shape (k = 10^shape).
                const float ym = juce::jlimit(0.002f, 0.998f, toCurve(e.position).y);
                const float k  = std::log(ym) / std::log(0.5f);
                curve_.shape   = juce::jlimit(-1.0f, 1.0f, std::log10(k));
                commitCurve(); syncBoxes(); break;
            }
            case Target::Point:
            {
                auto& pts = curve_.points;
                const int i = drag_.index, n = (int) pts.size();
                if (i < 0 || i >= n || curve_.mode != Mode::Points) break;
                const auto c = toCurve(e.position);
                constexpr float eps = MidiMappingCurve::kMinInSpan;
                if (i > 0 && i < n - 1)
                    pts[(size_t) i].x = juce::jlimit(pts[(size_t) i - 1].x + eps,
                                                     pts[(size_t) i + 1].x - eps, c.x);
                pts[(size_t) i].y = c.y;
                commitCurve(); break;
            }
            case Target::Paint:
                if (curve_.mode == Mode::Table) paintTo(toCurve(e.position));
                break;

            // ── Envelope corners: x = the time, dragged like a knob from
            //    where the gesture started; y (D) = the sustain level ───────
            case Target::EnvA:
                env_.attack = timeDrag(dragA0_, MidiMappingEnvelope::kMaxAttack, e.position.x);
                commitEnv(); syncBoxes(); break;
            case Target::EnvD:
                env_.decay   = timeDrag(dragD0_, MidiMappingEnvelope::kMaxDecay, e.position.x);
                env_.sustain = toCurve(e.position).y;
                commitEnv(); syncBoxes(); break;
            case Target::EnvR:
                env_.release = timeDrag(dragR0_, MidiMappingEnvelope::kMaxRelease, e.position.x);
                commitEnv(); syncBoxes(); break;
            case Target::EnvKnee:
            {
                // The knee goes where the pointer is, inside its segment's
                // box: x = progress along the segment, y = travel from the
                // segment's start level to its end level.
                using Env = MidiMappingEnvelope;
                const int i = juce::jlimit(0, 2, drag_.index);
                Env::Stage st; float xa, xb;
                segOf(i, st, xa, xb);
                auto& k = env_.knee[i];
                k.x = (e.position.x - xa) / juce::jmax(1.0f, xb - xa);
                const float lv = toCurve(e.position).y;   // level 0..1 over the OUT range
                if (i == Env::SegA)
                    k.y = lv;
                else if (i == Env::SegD)
                {
                    if (1.0f - env_.sustain > 1e-4f) k.y = (1.0f - lv) / (1.0f - env_.sustain);
                }
                else if (env_.sustain > 1e-4f)
                    k.y = 1.0f - lv / env_.sustain;
                k = Env::clampKnee(k);
                commitEnv(); break;
            }
            case Target::None: break;
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        hold_.release();
        drag_ = {};
        paintIdx_ = -1;
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        const auto h = hitAt(e.position);
        switch (h.t)
        {
            case Target::Point:
            {
                auto& pts = curve_.points;
                if (h.index > 0 && h.index < (int) pts.size() - 1)   // endpoints stay
                    pts.erase(pts.begin() + h.index);
                drag_ = {};
                commitCurve(); break;
            }
            case Target::Shape: curve_.shape = 0.0f; commitCurve(); syncBoxes(); break;
            case Target::InLo:  curve_.inLo  = 0.0f; commitCurve(); syncBoxes(); break;
            case Target::InHi:  curve_.inHi  = 1.0f; commitCurve(); syncBoxes(); break;
            case Target::OutLo: lo_ = 0.0f; commitRange(); syncBoxes(); break;
            case Target::OutHi: hi_ = 1.0f; commitRange(); syncBoxes(); break;
            case Target::EnvA:  env_.attack  = MidiMappingEnvelope::kDefAttack;  commitEnv(); syncBoxes(); break;
            case Target::EnvD:  env_.decay   = MidiMappingEnvelope::kDefDecay;
                                env_.sustain = MidiMappingEnvelope::kDefSustain; commitEnv(); syncBoxes(); break;
            case Target::EnvR:  env_.release = MidiMappingEnvelope::kDefRelease; commitEnv(); syncBoxes(); break;
            case Target::EnvKnee:  env_.knee[juce::jlimit(0, 2, h.index)] = MidiMappingEnvelope::kDefKnee;
                                   commitEnv(); break;
            case Target::Paint:
            case Target::None:  break;
        }
    }

private:
    //==========================================================================
    enum class Target { None, InLo, InHi, OutLo, OutHi, Shape, Point, Paint,
                        EnvA, EnvD, EnvR, EnvKnee };
    struct Hit { Target t { Target::None }; int index { -1 }; };

    //── The UI-wide gesture pair (ui/Sp3ctraGestures.h) ─────────────────────
    // Double-click already resets every handle (above). The long press types
    // it: a canvas handle speaks through its value box below, in the box's
    // own units (CC steps, %, x2.00, seconds).
    Sp3ctraBarSlider* boxFor(Target t) noexcept
    {
        switch (t)
        {
            case Target::InLo:  return &inMinBox_;
            case Target::InHi:  return &inMaxBox_;
            case Target::OutLo: return &outMinBox_;
            case Target::OutHi: return &outMaxBox_;
            case Target::Shape: return &shapeBox_;
            case Target::EnvA:  return &attBox_;
            case Target::EnvD:  return &decBox_;
            case Target::EnvR:  return &relBox_;
            default:            return nullptr;
        }
    }

    static const char* labelOf(Target t) noexcept
    {
        switch (t)
        {
            case Target::InLo:  return "In min";
            case Target::InHi:  return "In max";
            case Target::OutLo: return "Out min";
            case Target::OutHi: return "Out max";
            case Target::Shape: return "Shape";
            case Target::EnvA:  return "Attack";
            case Target::EnvD:  return "Decay";
            case Target::EnvR:  return "Release";
            default:            return "";
        }
    }

    /** Long press on a handle: the drag ends, the entry bubble opens. */
    void holdToType()
    {
        const Target t = drag_.t;
        drag_ = {};
        repaint();
        Sp3ctraGestures::Fields f;
        if (auto* b = boxFor(t)) f.push_back(Sp3ctraGestures::fieldOf(labelOf(t), *b));
        if (t == Target::EnvD)   f.push_back(Sp3ctraGestures::fieldOf("Sustain", susBox_));
        Sp3ctraGestures::openEntry(*this, hold_.anchor(*this), std::move(f));
    }

    Sp3ctraGestures::Hold hold_;

    static constexpr const char* kModeNames[4] = { "LINEAR", "EXPO", "POINTS", "TABLE" };
    static constexpr const char* kTrigNames[3] = { "GATE", "ONE-SHOT", "LOOP" };

    static bool isEnvTarget(Target t) noexcept
    { return t == Target::EnvA || t == Target::EnvD || t == Target::EnvR || t == Target::EnvKnee; }
    bool envView() const noexcept { return ! env_.isOff(); }

    struct FollowChip : juce::Button
    {
        FollowChip() : juce::Button("follow") { setClickingTogglesState(true); }
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            MidiMapGlyphs::drawFollowChip(g, getLocalBounds().toFloat().reduced(1.0f),
                                          getToggleState(), over);
        }
    };

    MidiMappingEngine& engine() noexcept { return processor_.getMidiMap(); }

    //── Geometry ──────────────────────────────────────────────────────────────
    float xOf(float v01) const noexcept { return plot_.getX() + v01 * plot_.getWidth(); }
    float yOf(float o01) const noexcept { return plot_.getBottom() - o01 * plot_.getHeight(); }
    float v01At(float px) const noexcept
    { return juce::jlimit(0.0f, 1.0f, (px - plot_.getX()) / juce::jmax(1.0f, plot_.getWidth())); }
    float o01At(float py) const noexcept
    { return juce::jlimit(0.0f, 1.0f, (plot_.getBottom() - py) / juce::jmax(1.0f, plot_.getHeight())); }
    static float snapCC(float v01) noexcept { return std::round(v01 * 127.0f) / 127.0f; }

    /** Screen → curve space (x over the input window, y over the output range). */
    juce::Point<float> toCurve(juce::Point<float> px) const noexcept
    {
        const float x    = curve_.windowX(v01At(px.x));
        const float o    = o01At(px.y);
        const float span = hi_ - lo_;
        const float y    = std::abs(span) < 1e-6f ? 0.0f
                                                  : juce::jlimit(0.0f, 1.0f, (o - lo_) / span);
        return { x, y };
    }
    /** Curve space → screen. */
    juce::Point<float> fromCurve(juce::Point<float> c) const noexcept
    {
        return { xOf(curve_.inLo + c.x * (curve_.inHi - curve_.inLo)),
                 yOf(lo_ + c.y * (hi_ - lo_)) };
    }

    juce::Rectangle<float> inLoThumb () const noexcept { return { xOf(curve_.inLo) - 4.0f, plot_.getBottom() + 3.0f, 8.0f, 10.0f }; }
    juce::Rectangle<float> inHiThumb () const noexcept { return { xOf(curve_.inHi) - 4.0f, plot_.getBottom() + 3.0f, 8.0f, 10.0f }; }
    juce::Rectangle<float> outLoThumb() const noexcept { return { plot_.getX() - 13.0f, yOf(lo_) - 4.0f, 10.0f, 8.0f }; }
    juce::Rectangle<float> outHiThumb() const noexcept { return { plot_.getX() - 13.0f, yOf(hi_) - 4.0f, 10.0f, 8.0f }; }
    juce::Point<float>     shapeNode () const noexcept
    { return fromCurve({ 0.5f, std::pow(0.5f, curve_.exponent()) }); }

    /** The complete law at raw input v01, on hysteresis branch `dir`
     *  (+1 rising / −1 falling / 0 none): normalised parameter value. */
    float outAt(float v01, int dir) const noexcept
    {
        float x = curve_.windowX(v01);
        x = MidiMappingCurve::hystBranch(x, curve_.hyst, dir);
        return lo_ + (hi_ - lo_) * MidiMappingCurve::lutAt(lut_.data(), x);
    }

    juce::Path curvePath(int dir) const
    {
        juce::Path p;
        const int n = juce::jmax(64, (int) plot_.getWidth());
        for (int i = 0; i <= n; ++i)
        {
            const float v = (float) i / (float) n;
            const juce::Point<float> pt(xOf(v), yOf(outAt(v, dir)));
            if (i == 0) p.startNewSubPath(pt); else p.lineTo(pt);
        }
        return p;
    }

    /** ▲ on the rising branch / ▼ on the falling one, at mid-travel,
     *  pointing along the branch's direction of travel. */
    void drawBranchArrow(juce::Graphics& g, int dir, juce::Colour c) const
    {
        const float v  = 0.5f;
        const juce::Point<float> a(xOf(v - 0.02f), yOf(outAt(v - 0.02f, dir)));
        const juce::Point<float> b(xOf(v + 0.02f), yOf(outAt(v + 0.02f, dir)));
        const juce::Point<float> m(xOf(v), yOf(outAt(v, dir)));
        const float ang = std::atan2(b.y - a.y, b.x - a.x) + (dir < 0 ? juce::MathConstants<float>::pi : 0.0f);
        juce::Path tri;
        tri.addTriangle(5.0f, 0.0f, -3.0f, -4.0f, -3.0f, 4.0f);
        tri.applyTransform(juce::AffineTransform::rotation(ang).translated(m.x, m.y));
        g.setColour(c.withAlpha(dir > 0 ? 0.95f : 0.7f));
        g.fillPath(tri);
    }

    //── Hit testing ───────────────────────────────────────────────────────────
    Hit hitAt(juce::Point<float> p) const
    {
        auto nearThumb = [&](juce::Rectangle<float> r) { return r.expanded(4.0f).contains(p); };
        if (nearThumb(outLoThumb())) return { Target::OutLo, -1 };
        if (nearThumb(outHiThumb())) return { Target::OutHi, -1 };

        constexpr float grab = 9.0f;
        if (envView())
        {
            // No IN window in an envelope: the OUT thumbs, the three corners,
            // then the knees — their rings, or the body of the curve itself
            // (pulling the line moves that segment's knee).
            for (auto t : { Target::EnvA, Target::EnvD, Target::EnvR })
                if (envNode(t).getDistanceFrom(p) < grab) return { t, -1 };
            for (int i = 0; i < 3; ++i)
            {
                MidiMappingEnvelope::Stage st; float xa, xb;
                segOf(i, st, xa, xb);
                if (xb - xa >= kKneeMinW && envNode(Target::EnvKnee, i).getDistanceFrom(p) < grab)
                    return { Target::EnvKnee, i };
            }
            if (const int seg = curveBodyAt(p); seg >= 0)
                return { Target::EnvKnee, seg };
            return {};
        }
        if (nearThumb(inLoThumb ())) return { Target::InLo,  -1 };
        if (nearThumb(inHiThumb ())) return { Target::InHi,  -1 };

        if (curve_.mode == MidiMappingCurve::Mode::Expo
            && shapeNode().getDistanceFrom(p) < grab)
            return { Target::Shape, -1 };
        if (curve_.mode == MidiMappingCurve::Mode::Points)
        {
            int best = -1; float bestD = grab;
            for (int i = 0; i < (int) curve_.points.size(); ++i)
            {
                const float d = fromCurve(curve_.points[(size_t) i]).getDistanceFrom(p);
                if (d < bestD) { bestD = d; best = i; }
            }
            if (best >= 0) return { Target::Point, best };
        }
        if (curve_.mode == MidiMappingCurve::Mode::Table && plot_.contains(p))
            return { Target::Paint, -1 };
        return {};
    }

    int chipAt(juce::Point<int> p) const
    {
        for (int i = 0; i < 4; ++i)
            if (chipRects_[(size_t) i].contains(p)) return i;
        return -1;
    }
    int trigAt(juce::Point<int> p) const
    {
        for (int i = 0; i < 3; ++i)
            if (trigRects_[(size_t) i].contains(p)) return i;
        return -1;
    }

    juce::Point<float> anchorOf(const Hit& h) const
    {
        switch (h.t)
        {
            case Target::InLo:  return inLoThumb ().getCentre();
            case Target::InHi:  return inHiThumb ().getCentre();
            case Target::OutLo: return outLoThumb().getCentre();
            case Target::OutHi: return outHiThumb().getCentre();
            case Target::Shape: return shapeNode();
            case Target::Point:
                if (h.index >= 0 && h.index < (int) curve_.points.size())
                    return fromCurve(curve_.points[(size_t) h.index]);
                break;
            case Target::Paint:
                if (paintIdx_ >= 0)
                    return fromCurve({ (float) paintIdx_ / (float) (MidiMappingCurve::kTableN - 1), paintY_ });
                break;
            case Target::EnvA: case Target::EnvD: case Target::EnvR: case Target::EnvKnee:
                return envNode(h.t, h.index);
            case Target::None: break;
        }
        return plot_.getCentre();
    }

    //── Text ──────────────────────────────────────────────────────────────────
    static juce::String arrow() { return juce::String::fromUTF8(" \xE2\x86\x92 "); }
    static juce::String inText(float v01) { return juce::String(juce::roundToInt(v01 * 127.0f)); }

    /** A normalised output in the TARGET's language (the panel's idiom). */
    juce::String unitText(float norm) const
    {
        if (param_ != nullptr)
            return param_->getText(juce::jlimit(0.0f, 1.0f, norm), 24);
        return juce::String(juce::roundToInt(juce::jlimit(0.0f, 1.0f, norm) * 100.0f)) + "%";
    }

    juce::String readoutText(const Hit& h) const
    {
        switch (h.t)
        {
            case Target::InLo:  return "IN MIN " + inText(curve_.inLo);
            case Target::InHi:  return "IN MAX " + inText(curve_.inHi);
            case Target::OutLo: return "OUT MIN " + unitText(lo_);
            case Target::OutHi: return "OUT MAX " + unitText(hi_);
            case Target::Shape: return "x" + juce::String(curve_.exponent(), 2);
            case Target::Point:
                if (h.index >= 0 && h.index < (int) curve_.points.size())
                {
                    const auto& p = curve_.points[(size_t) h.index];
                    return inText(curve_.inLo + p.x * (curve_.inHi - curve_.inLo))
                         + arrow() + unitText(lo_ + p.y * (hi_ - lo_));
                }
                break;
            case Target::Paint:
                if (paintIdx_ >= 0 && drag_.t == Target::Paint)
                {
                    const float x = (float) paintIdx_ / (float) (MidiMappingCurve::kTableN - 1);
                    return inText(curve_.inLo + x * (curve_.inHi - curve_.inLo))
                         + arrow() + unitText(lo_ + paintY_ * (hi_ - lo_));
                }
                break;
            case Target::EnvA:     return "ATTACK " + timeText(env_.attack);
            case Target::EnvD:     return "DECAY " + timeText(env_.decay)
                                        + juce::String::fromUTF8("  \xC2\xB7  SUSTAIN ")
                                        + juce::String(juce::roundToInt(env_.sustain * 100.0f)) + " %";
            case Target::EnvR:     return "RELEASE " + timeText(env_.release);
            case Target::EnvKnee:
            {
                // WHEN the segment passes its knee, and at WHAT value.
                const int   i   = juce::jlimit(0, 2, h.index);
                const float sec = i == 0 ? env_.attack : i == 1 ? env_.decay : env_.release;
                return timeText(env_.knee[i].x * sec) + arrow()
                     + unitText(lo_ + (hi_ - lo_) * env_.kneeLevel(i));
            }
            case Target::None: break;
        }
        return {};
    }

    juce::String hintText() const
    {
        using Trigger = MidiMappingEnvelope::Trigger;
        if (hoverRetrig_)
            return juce::String::fromUTF8(env_.retrig
                ? "RETRIG on: every press replays the attack from the rest"
                : "RETRIG off: a press while playing resumes from the current level");
        switch (env_.trigger)
        {
            case Trigger::Gate:    return juce::String::fromUTF8("press: attack \xE2\x86\x92 decay \xE2\x86\x92 hold \xC2\xB7 release: release");
            case Trigger::OneShot: return juce::String::fromUTF8("press plays A \xE2\x86\x92 D \xE2\x86\x92 R \xC2\xB7 SUSTAIN 100 % = hold at the peak");
            case Trigger::Loop:    return juce::String::fromUTF8("A \xE2\x86\x92 D repeat while held \xC2\xB7 SUSTAIN = the trough");
            case Trigger::Off:     break;
        }
        switch (curve_.mode)
        {
            case MidiMappingCurve::Mode::Linear: return juce::String::fromUTF8("thumbs: IN window \xC2\xB7 OUT range \xC2\xB7 double-click: reset \xC2\xB7 hold: type");
            case MidiMappingCurve::Mode::Expo:   return juce::String::fromUTF8("drag the node: exponent \xC2\xB7 double-click: reset \xC2\xB7 hold: type");
            case MidiMappingCurve::Mode::Points: return juce::String::fromUTF8("click: add point \xC2\xB7 double-click: delete");
            case MidiMappingCurve::Mode::Table:  return juce::String::fromUTF8("drag to draw \xC2\xB7 SMOOTH blurs");
        }
        return {};
    }

    //── Edits ─────────────────────────────────────────────────────────────────
    void rebake() { curve_.bake(lut_); }

    void commitCurve()
    {
        rebake();
        engine().setMappingCurve(paramId_, curve_);
        repaint();
        if (onEdited) onEdited();
    }

    void commitRange()
    {
        engine().setMappingRange(paramId_, lo_, hi_);
        repaint();
        if (onEdited) onEdited();
    }

    void commitEnv()
    {
        engine().setMappingEnvelope(paramId_, env_);
        repaint();
        if (onEdited) onEdited();
    }

    /** A curve chip: the envelope goes OFF (one family lit at a time) and
     *  the curve takes the mode. */
    void setMode(MidiMappingCurve::Mode m)
    {
        const bool wasEnv = envView();
        if (wasEnv) { env_.trigger = MidiMappingEnvelope::Trigger::Off; commitEnv(); }
        if (curve_.mode == m && ! wasEnv) return;
        curve_.mode = m;
        drag_ = {};
        commitCurve();
        syncBoxes();
    }

    /** A trigger chip: arms the envelope with that trigger — clicking the
     *  lit one switches the envelope off again (back to the curve). */
    void setTrigger(MidiMappingEnvelope::Trigger t)
    {
        env_.trigger = (env_.trigger == t) ? MidiMappingEnvelope::Trigger::Off : t;
        drag_ = {};
        commitEnv();
        syncBoxes();
    }

    //── Envelope geometry ─────────────────────────────────────────────────────
    // TIME IS PROPORTIONAL — the Ableton / Serum picture: the three timed
    // segments share the plot in proportion to their real durations,
    // auto-fitted, and the sustain plateau keeps a FIXED width whatever the
    // times. The shape stays truthful (a 10 ms attack next to a 20 s
    // release IS a spike) and ONE drawing serves every trigger — the
    // plateau just says what the trigger does there. Time handles drag like
    // knobs (relative, kPxPerOctave per doubling of the time), so a segment
    // grows from 0 to 20 s in one gesture while the plot refits under the
    // hand; the sustain node is absolute in y.
    static constexpr float kHoldFrac    = 0.16f;   ///< plateau width, of the plot
    static constexpr float kMinTotal    = 0.020f;  ///< seconds — three zero segments still fit
    static constexpr float kPxPerOctave = 60.0f;   ///< horizontal drag law
    static constexpr float kTimeRef     = 0.005f;  ///< the octave base (0 s → 5 ms after one)
    static constexpr float kKneeMinW    = 12.0f;   ///< a segment narrower than this shows no knee
    static float tw(float sec) noexcept
    { return std::log2(1.0f + juce::jmax(0.0f, sec) / kTimeRef); }
    static float twInv(float w) noexcept
    { return kTimeRef * (std::pow(2.0f, juce::jmax(0.0f, w)) - 1.0f); }

    struct EnvGeo { float x0 {}, xA {}, xD {}, xH {}, xR {}; };

    EnvGeo envGeo() const noexcept
    {
        const float holdW    = plot_.getWidth() * kHoldFrac;
        const float total    = juce::jmax(kMinTotal, env_.attack + env_.decay + env_.release);
        const float pxPerSec = (plot_.getWidth() - holdW) / total;
        EnvGeo g;
        g.x0 = plot_.getX();
        g.xA = g.x0 + env_.attack  * pxPerSec;
        g.xD = g.xA + env_.decay   * pxPerSec;
        g.xH = g.xD + holdW;
        g.xR = g.xH + env_.release * pxPerSec;
        return g;
    }

    /** Envelope level (0..1) → screen y over the OUT range. */
    float yEnv(float level) const noexcept { return yOf(lo_ + (hi_ - lo_) * level); }

    /** Timed segment i (0 A, 1 D, 2 R) → its stage and x span. */
    void segOf(int i, MidiMappingEnvelope::Stage& st, float& xa, float& xb) const noexcept
    {
        using Stage = MidiMappingEnvelope::Stage;
        const auto g = envGeo();
        switch (i)
        {
            case 0:  st = Stage::Attack;  xa = g.x0; xb = g.xA; break;
            case 1:  st = Stage::Decay;   xa = g.xA; xb = g.xD; break;
            default: st = Stage::Release; xa = g.xH; xb = g.xR; break;
        }
    }

    /** The corners (A, D, R) or the knee of segment `seg` — ON the curve. */
    juce::Point<float> envNode(Target t, int seg = -1) const noexcept
    {
        const auto g = envGeo();
        switch (t)
        {
            case Target::EnvA: return { g.xA, yEnv(1.0f) };
            case Target::EnvD: return { g.xD, yEnv(env_.sustain) };
            case Target::EnvR: return { g.xR, yEnv(0.0f) };
            case Target::EnvKnee:
            {
                const int i = juce::jlimit(0, 2, seg);
                MidiMappingEnvelope::Stage st; float xa, xb;
                segOf(i, st, xa, xb);
                return { xa + env_.knee[i].x * (xb - xa), yEnv(env_.kneeLevel(i)) };
            }
            default: break;
        }
        return plot_.getCentre();
    }

    /** Append timed segment `i` to `p` — opening the sub-path unless it
     *  continues one. (Path::isEmpty() stays true after a lone moveTo, so
     *  the caller says.) */
    void segPath(juce::Path& p, int i, bool continued) const
    {
        MidiMappingEnvelope::Stage st; float xa, xb;
        segOf(i, st, xa, xb);
        const int n = juce::jmax(2, (int) ((xb - xa) / 2.0f));
        for (int k = 0; k <= n; ++k)
        {
            const float t = (float) k / (float) n;
            const juce::Point<float> pt(xa + (xb - xa) * t, yEnv(env_.levelAt(st, t)));
            if (k == 0 && ! continued) p.startNewSubPath(pt); else p.lineTo(pt);
        }
    }

    /** The whole drawn envelope — A, D, plateau, R — as ONE sub-path (the
     *  fill and the body hit-test). */
    juce::Path envPath() const
    {
        const auto g = envGeo();
        juce::Path p;
        segPath(p, 0, false);
        segPath(p, 1, true);
        p.lineTo(g.xH, yEnv(env_.sustain));
        segPath(p, 2, true);
        return p;
    }

    /** The body of the curve under the pointer → the timed segment it
     *  belongs to (0 A, 1 D, 2 R; the plateau is nobody's), or -1. */
    int curveBodyAt(juce::Point<float> p) const
    {
        if (! plot_.expanded(6.0f).contains(p)) return -1;
        juce::Point<float> on;
        envPath().getNearestPoint(p, on);
        if (on.getDistanceFrom(p) > 6.0f) return -1;
        const auto g = envGeo();
        if (on.x <= g.xA + 0.5f) return 0;
        if (on.x <= g.xD + 0.5f) return 1;
        if (on.x >= g.xH - 0.5f) return 2;
        return -1;
    }

    /** Horizontal knob-like drag of a time: octaves from where the gesture
     *  started. */
    float timeDrag(float sec0, float maxSec, float px) const noexcept
    {
        return juce::jlimit(0.0f, maxSec,
                            twInv(tw(sec0) + (px - dragOrigin_.x) / kPxPerOctave));
    }

    /** Where the running envelope is on the drawn one. */
    juce::Point<float> livePoint() const noexcept
    {
        using Stage = MidiMappingEnvelope::Stage;
        const auto g = envGeo();
        float x = g.xR;
        switch (liveStage_)
        {
            case Stage::Attack:  x = g.x0 + (g.xA - g.x0) * liveT_; break;
            case Stage::Decay:   x = g.xA + (g.xD - g.xA) * liveT_; break;
            case Stage::Sustain: x = 0.5f * (g.xD + g.xH);          break;
            case Stage::Release: x = g.xH + (g.xR - g.xH) * liveT_; break;
            case Stage::Idle:    break;
        }
        return { x, yEnv(liveLevel_) };
    }

    static juce::String timeText(double sec)
    {
        if (sec < 0.9995) return juce::String(juce::roundToInt(sec * 1000.0)) + " ms";
        if (sec < 9.995)  return juce::String(sec, 2) + " s";
        return juce::String(sec, 1) + " s";
    }
    /** "300" / "300 ms" → 0.3 ; "1.2 s" → 1.2. */
    static double timeFromText(const juce::String& t)
    {
        const double v = t.retainCharacters("0123456789.").getDoubleValue();
        const juce::String u = t.toLowerCase();
        return (u.contains("ms") || ! u.contains("s")) ? v / 1000.0 : v;
    }

    //── The envelope view ─────────────────────────────────────────────────────
    void paintEnvelope(juce::Graphics& g, juce::Colour accent)
    {
        using namespace Sp3ctraHandles;
        using Stage   = MidiMappingEnvelope::Stage;
        using Trigger = MidiMappingEnvelope::Trigger;
        const auto geo = envGeo();
        const bool curveHot = drag_.t == Target::EnvKnee
                           || (drag_.t == Target::None && hover_.t == Target::EnvKnee);

        ModuleChrome::drawFrame  (g, frame_, accent);
        ModuleChrome::drawCaption(g, frame_, accent, juce::String::fromUTF8("TIME \xE2\x86\x92 OUT"));
        ModuleChrome::drawReadout(g, frame_, accent, hintText());

        // ── Grid: output thirds (y), the segment boundaries (x) ─────────────
        for (int i = 0; i <= 2; ++i)
        {
            const float y = yOf((float) i / 2.0f);
            g.setColour(accent.withAlpha(i == 0 || i == 2 ? 0.28f : 0.12f));
            g.drawHorizontalLine((int) y, plot_.getX(), plot_.getRight());
        }
        g.setColour(accent.withAlpha(0.16f));
        for (const float x : { geo.xA, geo.xD, geo.xH })
            g.drawVerticalLine((int) x, plot_.getY(), plot_.getBottom());

        // ── Under the axis: each segment named with its time; the plateau
        //    with what the trigger does there. A band too narrow for its
        //    words stays mute — the hover readout says it. ───────────────────
        g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
        const juce::String holdWord = env_.trigger == Trigger::Gate ? "hold"
                                    : env_.trigger == Trigger::Loop ? "loop A+D" : "skip";
        struct Band { float xa, xb; juce::String text; float alpha; };
        const Band bands[4] = {
            { geo.x0, geo.xA, "A " + timeText(env_.attack),  0.55f },
            { geo.xA, geo.xD, "D " + timeText(env_.decay),   0.55f },
            { geo.xD, geo.xH, "S " + holdWord,               0.45f },
            { geo.xH, geo.xR, "R " + timeText(env_.release), 0.55f } };
        for (const auto& b : bands)
        {
            const int w    = (int) (b.xb - b.xa);
            const int need = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), b.text);
            if (w < need + 4) continue;
            g.setColour(accent.withAlpha(b.alpha));
            g.drawText(b.text, (int) b.xa, (int) plot_.getBottom() + 5, w, 10,
                       juce::Justification::centred, false);
        }
        g.setColour(accent.withAlpha(0.5f));
        g.drawText("OUT", (int) frame_.getX() + 6, (int) plot_.getY() - 2, kAxisL - 4, 10,
                   juce::Justification::centredLeft, false);

        // ── The OUT range band: rest … peak ─────────────────────────────────
        {
            const float y0 = yOf(juce::jmin(lo_, hi_)), y1 = yOf(juce::jmax(lo_, hi_));
            g.setColour(accent.withAlpha(0.07f));
            g.fillRect(juce::Rectangle<float>(plot_.getX(), y1, plot_.getWidth(), y0 - y1));
            g.setColour(accent.withAlpha(0.22f));
            g.drawHorizontalLine((int) y0, plot_.getX(), plot_.getRight());
            g.drawHorizontalLine((int) y1, plot_.getX(), plot_.getRight());
        }

        // ── The envelope: filled to the rest; the timed segments stroked;
        //    the plateau solid where GATE holds, dashed where ONE-SHOT skips
        //    it and where LOOP repeats A + D instead. ─────────────────────────
        g.saveState();
        g.reduceClipRegion(plot_.expanded(2.0f).toNearestInt());
        {
            juce::Path fill = envPath();
            fill.lineTo(geo.xR, yEnv(0.0f));
            fill.lineTo(geo.x0, yEnv(0.0f));
            fill.closeSubPath();
            g.setColour(accent.withAlpha(0.10f));
            g.fillPath(fill);

            const float w = curveHot ? 2.8f : 2.0f;
            g.setColour(accent.withAlpha(curveHot ? 1.0f : 0.95f));
            for (int i = 0; i < 3; ++i)
            {
                juce::Path sp;
                segPath(sp, i, false);
                g.strokePath(sp, juce::PathStrokeType(w));
            }
            const float yS = yEnv(env_.sustain);
            if (env_.trigger == Trigger::Gate)
                g.drawLine(geo.xD, yS, geo.xH, yS, w);
            else
            {
                const float dash[2] = { 4.0f, 4.0f };
                g.drawDashedLine({ geo.xD, yS, geo.xH, yS }, dash, 2, 1.4f);
            }
        }
        g.restoreState();

        // ── Thumbs + handles: the OUT range, each segment's knee (a ring ON
        //    the curve — the body of the curve grabs it too), then the three
        //    corners on top. ────────────────────────────────────────────────
        auto look = [this](Target t) { return stateOf(drag_.t == t, hover_.t == t); };
        drawThumb(g, outLoThumb(), look(Target::OutLo));
        drawThumb(g, outHiThumb(), look(Target::OutHi));
        for (int i = 0; i < 3; ++i)
        {
            Stage st; float xa, xb;
            segOf(i, st, xa, xb);
            if (xb - xa < kKneeMinW) continue;
            drawNode(g, envNode(Target::EnvKnee, i),
                     stateOf(drag_.t == Target::EnvKnee && drag_.index == i,
                             drag_.t == Target::None && hover_.t == Target::EnvKnee
                                                     && hover_.index == i),
                     kRingR);
        }
        drawNode(g, envNode(Target::EnvA), look(Target::EnvA));
        drawNode(g, envNode(Target::EnvD), look(Target::EnvD));
        drawNode(g, envNode(Target::EnvR), look(Target::EnvR));

        // ── Live point: a plain white dot riding the curve while a press
        //    plays it — not a handle, nothing to grab. ───────────────────────
        if (liveStage_ != Stage::Idle)
        {
            const auto pt = livePoint();
            g.setColour(juce::Colours::white.withAlpha(0.18f));
            g.drawVerticalLine((int) pt.x, plot_.getY(), plot_.getBottom());
            g.setColour(juce::Colours::white.withAlpha(0.95f));
            g.fillEllipse(pt.x - 3.5f, pt.y - 3.5f, 7.0f, 7.0f);
            g.setColour(accent);
            g.drawEllipse(pt.x - 3.5f, pt.y - 3.5f, 7.0f, 7.0f, 1.0f);
        }

        // ── Readout of the handle under the pointer / in hand ───────────────
        {
            const auto h = drag_.t != Target::None ? drag_ : hover_;
            const juce::String txt = readoutText(h);
            if (txt.isNotEmpty())
                Sp3ctraHandles::drawReadout(g, txt, anchorOf(h), plot_);
        }

        // ── Box labels ──────────────────────────────────────────────────────
        ModuleChrome::drawBoxLabel(g, attBox_,    accent, "ATTACK");
        ModuleChrome::drawBoxLabel(g, decBox_,    accent, "DECAY");
        ModuleChrome::drawBoxLabel(g, susBox_,    accent, "SUSTAIN");
        ModuleChrome::drawBoxLabel(g, relBox_,    accent, "RELEASE");
        ModuleChrome::drawBoxLabel(g, velBox_,    accent, "VEL");
        ModuleChrome::drawBoxLabel(g, outMinBox_, accent, "OUT MIN");
        ModuleChrome::drawBoxLabel(g, outMaxBox_, accent, "OUT MAX");
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontTiny)).boldened());
        drawChip(g, resetRect_.toFloat(), "RESET", false, hoverReset_);
    }

    /** Insert a breakpoint at curve-space `c`, keeping the list sorted and
     *  every x strictly between its neighbours. Returns its index, or -1. */
    int addPoint(juce::Point<float> c)
    {
        auto& pts = curve_.points;
        if ((int) pts.size() >= MidiMappingCurve::kMaxPoints) return -1;
        constexpr float eps = MidiMappingCurve::kMinInSpan;
        int i = 0;
        while (i < (int) pts.size() - 1 && pts[(size_t) i + 1].x <= c.x) ++i;
        const float xl = pts[(size_t) i].x + eps;
        const float xr = pts[(size_t) i + 1].x - eps;
        if (xr < xl) return -1;   // no room between these two
        pts.insert(pts.begin() + i + 1, { juce::jlimit(xl, xr, c.x), c.y });
        return i + 1;
    }

    /** TABLE freehand: fill every entry between the previous pen position
     *  and this one (a fast stroke must not leave gaps). */
    void paintTo(juce::Point<float> c)
    {
        auto& t = curve_.table;
        const int i1 = juce::jlimit(0, MidiMappingCurve::kTableN - 1,
                                    juce::roundToInt(c.x * (float) (MidiMappingCurve::kTableN - 1)));
        if (paintIdx_ < 0 || paintIdx_ == i1)
            t[(size_t) i1] = c.y;
        else
        {
            const int   i0 = paintIdx_;
            const float y0 = paintY_;
            const int   d  = i1 > i0 ? 1 : -1;
            for (int i = i0; i != i1 + d; i += d)
            {
                const float f = (float) (i - i0) / (float) (i1 - i0);
                t[(size_t) i] = y0 + (c.y - y0) * f;
            }
        }
        paintIdx_ = i1;
        paintY_   = c.y;
        commitCurve();
    }

    void installUnits()
    {
        for (auto* b : { &outMinBox_, &outMaxBox_ })
        {
            if (auto* p = param_)
            {
                b->textFromValueFunction = [p](double v)
                { return p->getText((float) juce::jlimit(0.0, 1.0, v), 24); };
                b->valueFromTextFunction = [p](const juce::String& t)
                { return (double) p->getValueForText(t); };
            }
            else
            {
                b->textFromValueFunction = [](double v)
                { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
                b->valueFromTextFunction = [](const juce::String& t)
                { return t.retainCharacters("0123456789.-").getDoubleValue() / 100.0; };
            }
            b->updateText();
        }
    }

    /** Boxes ← state, silently; the ones a mode does not use read disabled. */
    void syncBoxes()
    {
        using Mode = MidiMappingCurve::Mode;
        inMinBox_ .setValue(curve_.inLo * 127.0, juce::dontSendNotification);
        inMaxBox_ .setValue(curve_.inHi * 127.0, juce::dontSendNotification);
        outMinBox_.setValue(lo_,                 juce::dontSendNotification);
        outMaxBox_.setValue(hi_,                 juce::dontSendNotification);
        shapeBox_ .setValue(curve_.shape,        juce::dontSendNotification);
        hystBox_  .setValue(curve_.hyst,         juce::dontSendNotification);
        smoothBox_.setValue(curve_.smooth,       juce::dontSendNotification);
        shapeBox_ .setEnabled(curve_.mode == Mode::Expo);
        smoothBox_.setEnabled(curve_.mode == Mode::Points || curve_.mode == Mode::Table);
        attBox_.setValue(env_.attack,  juce::dontSendNotification);
        decBox_.setValue(env_.decay,   juce::dontSendNotification);
        susBox_.setValue(env_.sustain, juce::dontSendNotification);
        relBox_.setValue(env_.release, juce::dontSendNotification);
        velBox_.setValue(env_.vel,     juce::dontSendNotification);
        for (auto* b : { &inMinBox_, &inMaxBox_, &outMinBox_, &outMaxBox_,
                         &shapeBox_, &hystBox_, &smoothBox_,
                         &attBox_, &decBox_, &susBox_, &relBox_, &velBox_ })
            b->updateText();
        layoutBoxes();   // the view may have changed with the edit
    }

    void showMenu()
    {
        juce::PopupMenu m;
        if (envView())
            m.addItem(4, "Reset envelope (keep trigger)");
        else
            m.addItem(1, "Reset curve (keep window)");
        m.addItem(2, "Reset everything");
        m.addSeparator();
        m.addItem(3, "Remove MIDI mapping (" + engine().mappingDescription(paramId_) + ")");
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
            [sp = juce::Component::SafePointer<MidiCurveEditorComponent>(this)](int r)
            {
                if (sp == nullptr) return;
                switch (r)
                {
                    case 1: sp->curve_.resetShape(); sp->commitCurve(); sp->syncBoxes(); break;
                    case 2: sp->curve_ = {}; sp->env_ = {}; sp->lo_ = 0.0f; sp->hi_ = 1.0f;
                            sp->commitCurve(); sp->commitEnv(); sp->commitRange(); sp->syncBoxes(); break;
                    case 4: sp->env_.resetShape(); sp->commitEnv(); sp->syncBoxes(); break;
                    case 3: sp->engine().removeMappingFor(sp->paramId_); break;   // → onUnmapped
                    default: break;
                }
            });
    }

    //── Live feed ─────────────────────────────────────────────────────────────
    void timerCallback() override
    {
        if (paramId_.isEmpty()) return;
        bool changed = false;

        // The panel's MIN/MAX bars may have moved the range (no broadcast).
        float lo = lo_, hi = hi_;
        if (engine().getMappingRange(paramId_, lo, hi)
            && (lo != lo_ || hi != hi_)
            && drag_.t != Target::OutLo && drag_.t != Target::OutHi)
        {
            lo_ = lo; hi_ = hi;
            syncBoxes();
            changed = true;
        }
        const bool wasFollowing = follow_.getToggleState();
        if (engine().mappingFollows(paramId_) != wasFollowing)
            follow_.setToggleState(! wasFollowing, juce::dontSendNotification);

        float in = liveIn_, out = liveOut_;
        if (engine().lastMappedValue(paramId_, in, out) && (in != liveIn_ || out != liveOut_))
        {
            liveIn_ = in; liveOut_ = out;
            changed = true;
        }
        if (envView())
        {
            // The running envelope moves on its own (no MIDI touch): follow
            // it as long as it is not at rest.
            auto st = liveStage_; float t = liveT_, l = liveLevel_;
            if (engine().envelopeLive(paramId_, st, t, l)
                && (st != liveStage_ || t != liveT_ || l != liveLevel_))
            {
                liveStage_ = st; liveT_ = t; liveLevel_ = l;
                changed = true;
            }
        }
        const float heat = Sp3ctraControls::heatSince(touchMs_);
        if (changed || heat > 0.0f || lastHeat_ > 0.0f)
            repaint(frame_.toNearestInt());
        lastHeat_ = heat;
    }

    void midiTouched(const juce::String& id) override
    {
        if (id == paramId_) touchMs_ = Sp3ctraControls::nowMs();
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        // Table mutation: removed → the owner closes us; re-learnt / restored
        // → reload (event name, curve, range).
        if (paramId_.isEmpty()) return;
        MidiMappingCurve c;
        if (! engine().getMappingCurve(paramId_, c))
        {
            if (onUnmapped) onUnmapped();
            return;
        }
        setTarget(paramId_);
    }

    //==========================================================================
    Sp3ctraAudioProcessor&      processor_;
    juce::String                paramId_;
    juce::RangedAudioParameter* param_ { nullptr };
    ParamIdentity               ident_;

    MidiMappingCurve                              curve_;
    std::array<float, MidiMappingCurve::kLutN>    lut_ {};
    MidiMappingEnvelope                           env_;
    float lo_ { 0.0f }, hi_ { 1.0f };

    FollowChip       follow_;
    Sp3ctraBarSlider inMinBox_, inMaxBox_, outMinBox_, outMaxBox_;
    Sp3ctraBarSlider shapeBox_, hystBox_, smoothBox_;
    Sp3ctraBarSlider attBox_, decBox_, susBox_, relBox_, velBox_;

    juce::Rectangle<int>                 headerArea_, resetRect_, rowA_, rowB_;
    std::array<juce::Rectangle<int>, 4>  chipRects_ {};
    std::array<juce::Rectangle<int>, 3>  trigRects_ {};
    juce::Rectangle<int>                 retrigRect_;
    juce::Rectangle<float>               frame_, plot_;

    Hit   hover_, drag_;
    int   hoverChip_ { -1 }, hoverTrig_ { -1 };
    bool  hoverReset_ { false }, hoverRetrig_ { false };
    juce::Point<float> dragOrigin_;                      ///< envelope time drags are relative
    float dragA0_ { 0.0f }, dragD0_ { 0.0f }, dragR0_ { 0.0f };

    MidiMappingEnvelope::Stage liveStage_ { MidiMappingEnvelope::Stage::Idle };
    float liveT_ { 0.0f }, liveLevel_ { 0.0f };
    int   paintIdx_ { -1 };
    float paintY_ { 0.0f };

    float  liveIn_ { -1.0f }, liveOut_ { -1.0f };
    double touchMs_ { Sp3ctraControls::kNever };
    float  lastHeat_ { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiCurveEditorComponent)
};

//==============================================================================
/** The detached window hosting the editor (native title bar, resizable,
 *  bounds remembered per machine like the VIDEO MIX window). The editor
 *  owns one and retargets it on every open. */
class MidiCurveWindow : public juce::DocumentWindow
{
public:
    MidiCurveWindow(Sp3ctraAudioProcessor& p, const juce::String& paramId)
        : juce::DocumentWindow("MIDI CURVE", juce::Colour(Sp3ctraTheme::kColBg),
                               juce::DocumentWindow::closeButton
                             | juce::DocumentWindow::minimiseButton)
    {
        setUsingNativeTitleBar(true);
        auto* c = new MidiCurveEditorComponent(p);
        content_ = c;
        c->setSize(MidiCurveEditorComponent::kDefaultW, MidiCurveEditorComponent::kDefaultH);
        c->onUnmapped = [this] { if (onCloseRequested) onCloseRequested(); };
        c->onEdited   = [this] { if (onEdited) onEdited(); };
        setContentOwned(c, true);
        setResizable(true, false);
        setResizeLimits(MidiCurveEditorComponent::kMinW, MidiCurveEditorComponent::kMinH, 1600, 1200);
        centreWithSize(getWidth(), getHeight());
        if (const auto st = MachinePrefs::file().getValue("midiCurveWin.state"); st.isNotEmpty())
            restoreWindowStateFromString(st);
        setTarget(paramId);
        setVisible(true);
    }

    ~MidiCurveWindow() override
    {
        MachinePrefs::file().setValue("midiCurveWin.state", getWindowStateAsString());
    }

    void setTarget(const juce::String& paramId)
    {
        content_->setTarget(paramId);
        setName(juce::String::fromUTF8("MIDI CURVE \xC2\xB7 ") + content_->identity().text());
    }

    const juce::String& paramId() const noexcept { return content_->paramId(); }

    void closeButtonPressed() override { if (onCloseRequested) onCloseRequested(); }

    std::function<void()> onCloseRequested;   ///< close box / mapping gone — owner deletes (deferred)
    std::function<void()> onEdited;           ///< → MidiMapPanel::syncFromEngine

private:
    MidiCurveEditorComponent* content_ { nullptr };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiCurveWindow)
};
