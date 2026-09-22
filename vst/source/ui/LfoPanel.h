/**
 * @file LfoPanel.h
 * @brief Right-band LFO section — the eight shapes of the modulation bank,
 *        live, and the settings of the one that is selected.
 *
 *   ┌ LFO ─────────────────────────────── 3 ▾┐
 *   │ ①  ∿∿∿∿∿∿∿∿∿      0.25 Hz  FREE   ⟳ ⏻ │   one row per LFO
 *   │ ②  ⊓⊔⊓⊔⊓⊔⊓⊔        1/4    SYNC   ⟳ ⏻ │
 *   │ ③  ▂▅▁▇▃▆▁▄       500 li  SCAN   ⟳ ⏻ │
 *   ├─────────────────────────────────────────┤
 *   │ ∿ △ ◹ ◺ ⊓ ⋮ ≈                          │   the SELECTED LFO's settings
 *   │ FREE     SYNC      SCAN                 │
 *   │ (◯)RATE  (◯)SKEW   (◯)STEPS  (◯)TEMPO   │   TEMPO: SYNC only — a knob in
 *   │  0.250 Hz    50 %      OFF     120 BPM  │   the standalone, the DAW's
 *   │                                         │   tempo read-only under a host
 *   │ 2 destinations · clic droit ▸ Modulate  │
 *   └─────────────────────────────────────────┘
 *
 * The row's scope is not a decoration: it is LfoBank::shapeAt drawn over one
 * cycle with the running dot on it — the same function the audio thread reads,
 * so what the row shows IS what the destinations get (the shared-law rule the
 * VIDEO SCROLL indicators already follow). Collapsed, the band keeps drawing
 * the scopes of the running LFOs: the movement stays visible without unfolding.
 *
 * Every setting here is an ordinary mappable target ("lfo:2:rate" — see
 * LfoMidiTargets): each control carries a MidiLearnAttachment, so a knob can
 * ride an LFO's rate and a CIS HIT can retrigger it, through the very same
 * right-click menu as any other control. Which also means the menu's
 * "Modulate ▸ LFO n" works HERE: an LFO modulating another LFO costs nothing.
 *
 * Only the LFOs the session USES have a row. A bank grows by needing a
 * modulation ("Modulate ▸ New LFO…" on any control, or the header's +) and
 * shrinks from a row's right-click menu — which also drops the mappings that
 * named it, since a row that drives nothing is a lie. Slot numbers are
 * stable across removals (LfoBank doc): the list may show ①③④, and the next
 * add brings ② back.
 *
 * The section mirrors the collapse API of MidiMixPanel / MidiMapPanel
 * (setCollapsed / isCollapsed / onCollapseToggled) so PluginEditor splits
 * zone 4 and persists it exactly the same way.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../midi/LfoBank.h"
#include "../midi/MidiLearnAttachment.h"
#include "ChainIdentity.h"
#include "Sp3ctraBarSlider.h"
#include "Sp3ctraControls.h"
#include <functional>
#include <memory>
#include <vector>

class LfoPanel : public juce::Component,
                 private juce::Timer
{
public:
    static constexpr int kHeaderH        = 24;
    static constexpr int kRowH           = 26;
    static constexpr int kRowGap         = 2;
    static constexpr int kPad            = 6;
    static constexpr int kMaxVisibleRows = 4;    ///< taller lists scroll
    static constexpr int kShapeH         = 26;   ///< the shape list (7 cells)
    static constexpr int kClockH         = 22;   ///< FREE | SYNC | SCAN
    static constexpr int kKnobD          = 30;   ///< rotary diameter
    static constexpr int kKnobH          = 34;   ///< the rotary row
    static constexpr int kGapY           = 5;
    static constexpr int kCaptionH       = 13;
    static constexpr int kEditH          = kPad + kShapeH + kGapY + kClockH + kGapY
                                         + kKnobH + 2 + kCaptionH;
    static constexpr int kLiveHz         = 25;

    /** Fired after the collapse state changed (editor relayouts + persists). */
    std::function<void(bool)> onCollapseToggled;
    /** Fired when the panel's preferred height changed (selection). */
    std::function<void()>     onContentChanged;

    explicit LfoPanel(Sp3ctraAudioProcessor& p) : processor_(p)
    {
        chevron_.collapsed = &collapsed_;
        chevron_.onClick = [this] { setCollapsed(! collapsed_, true); };
        chevron_.setTooltip("Collapse / expand the LFO bank");
        addAndMakeVisible(chevron_);

        viewport_.setViewedComponent(&rowsHolder_, false);
        viewport_.setScrollBarsShown(true, false);
        viewport_.setScrollBarThickness(8);
        addAndMakeVisible(viewport_);

        add_.setTooltip("Add an LFO (the lowest free slot). A control's right-click "
                        "menu does it too, already bound: Modulate > New LFO");
        add_.onClick = [this]
        {
            const int i = processor_.getLfoBank().lfoAdd();
            if (i >= 0) { syncActive(); selectLfo(i); }
        };
        addAndMakeVisible(add_);

        edit_ = std::make_unique<Edit>(*this);
        addAndMakeVisible(edit_.get());

        selected_ = -1;
        syncActive();
        startTimerHz(kLiveHz);
    }

    ~LfoPanel() override { stopTimer(); }

    /** Height for the current state — the editor uses it to split zone 4. */
    int preferredHeight() const noexcept
    {
        if (collapsed_) return kHeaderH;
        const int n     = (int) rows_.size();
        const int shown = juce::jmax(1, juce::jmin(n, kMaxVisibleRows));   // 1 = the empty hint
        return kHeaderH + kPad + shown * (kRowH + kRowGap) - kRowGap + kPad
             + (n > 0 ? kEditH + kPad : 0);
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

    /** Open LFO `i`'s settings (a MIDI MAP row's source token clicks here). */
    void selectLfo(int i)
    {
        syncActive();   // a slot brought into use a moment ago has its row now
        if (! processor_.getLfoBank().lfoExists(i)) return;
        if (i == selected_) { flash(i); return; }
        selected_ = i;
        edit_->bind(selected_);
        edit_->setVisible(! collapsed_);
        flash(i);
        repaintRows();
        if (onContentChanged) onContentChanged();   // the edit block may have appeared
    }
    int selectedLfo() const noexcept { return selected_; }

    //==========================================================================
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0c0c10));

        g.setColour(kAccent);
        g.setFont(juce::Font(juce::FontOptions(Sp3ctraTheme::kFontBadge)).boldened());
        g.drawText("LFO", 8, 0, getWidth() - 16, kHeaderH,
                   juce::Justification::centredLeft, false);

        if (const int n = (int) rows_.size(); n > 0)
        {
            g.setColour(kAccent.withAlpha(0.55f));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText(juce::String(n), 0, 0, add_.getX() - 6, kHeaderH,
                       juce::Justification::centredRight, false);
        }

        if (! collapsed_)
        {
            if (rows_.empty())
            {
                g.setColour(juce::Colour(0xff9aa6ba));
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
                g.drawText(juce::String::fromUTF8("no LFO \xE2\x80\x94 + or right-click a control \xE2\x96\xB8 Modulate"),
                           viewport_.getBounds(), juce::Justification::centredLeft, false);
            }
            return;
        }

        // Folded: the running shapes keep moving in the header band — one
        // strip each, so the bank is never invisible while it plays.
        const int x0 = 34, x1 = juce::jmax(x0, add_.getX() - 22);
        const int n  = runningCount();
        if (n <= 0 || x1 - x0 < 20) return;
        const int w = (x1 - x0) / n - 3;
        int x = x0;
        auto& bank = processor_.getLfoBank();
        for (int i = 0; i < LfoBank::kNumLfos; ++i)
        {
            if (! bank.lfoRunning(i)) continue;
            drawScope(g, bank, i, juce::Rectangle<float>((float) x, 4.0f,
                                                         (float) w, (float) kHeaderH - 8.0f),
                      0.75f);
            x += w + 3;
        }
    }

    void resized() override
    {
        const int btn = kHeaderH - 6;
        chevron_.setBounds(getWidth() - btn - 6, (kHeaderH - btn) / 2, btn, btn);
        add_    .setBounds(chevron_.getX() - btn - 2, (kHeaderH - btn) / 2, btn, btn);
        add_.setVisible(processor_.getLfoBank().activeCount() < LfoBank::kNumLfos);

        auto r = getLocalBounds();
        r.setWidth(juce::jmin(r.getWidth(), Sp3ctraTheme::kMaxContentW));
        r.removeFromTop(kHeaderH);

        const bool hasEdit = ! collapsed_ && selected_ >= 0 && ! rows_.empty();
        viewport_.setVisible(! collapsed_);
        edit_->setVisible(hasEdit);
        if (collapsed_)
            return;

        r.reduce(kPad, 0);
        r.removeFromTop(kPad);
        r.removeFromBottom(kPad);

        if (hasEdit)
        {
            edit_->setBounds(r.removeFromBottom(kEditH));
            r.removeFromBottom(kPad);
        }
        viewport_.setBounds(r);

        const int contentH = juce::jmax(0, (int) rows_.size() * (kRowH + kRowGap) - kRowGap);
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

    int runningCount() const noexcept
    {
        int n = 0;
        for (int i = 0; i < LfoBank::kNumLfos; ++i)
            if (processor_.getLfoBank().lfoRunning(i)) ++n;
        return n;
    }

    //==========================================================================
    /** The shape over ONE cycle plus the running dot — LfoBank::shapeAt, the
     *  same function the audio thread reads. */
    static void drawScope(juce::Graphics& g, const LfoBank& bank, int i,
                          juce::Rectangle<float> b, float alpha)
    {
        const auto  c   = bank.get(i);
        const float ph  = bank.phaseOf(i);
        const juce::Colour col = juce::Colour(Sp3ctraTheme::kColMod)
                                     .withMultipliedAlpha(c.run ? alpha : alpha * 0.4f);

        g.setColour(juce::Colour(Sp3ctraTheme::kColBarBg));
        g.fillRoundedRectangle(b, 2.0f);

        const auto p = b.reduced(2.0f);
        const int  N = juce::jmax(8, (int) p.getWidth());
        juce::Path path;
        for (int k = 0; k <= N; ++k)
        {
            const float t = (float) k / (float) N;
            const float v = LfoBank::shapeAt(c.shape, t, c.skew, c.steps, i);
            const float x = p.getX() + p.getWidth() * t;
            const float y = p.getBottom() - p.getHeight() * v;
            k ? path.lineTo(x, y) : path.startNewSubPath(x, y);
        }
        g.setColour(col);
        g.strokePath(path, juce::PathStrokeType(1.2f));

        // The dot: where the cycle stands right now.
        const float dx = p.getX() + p.getWidth() * ph;
        const float dy = p.getBottom() - p.getHeight()
                       * LfoBank::shapeAt(c.shape, ph, c.skew, c.steps, i);
        g.setColour(juce::Colours::white.withAlpha(c.run ? 0.9f * alpha : 0.3f * alpha));
        g.fillEllipse(dx - 2.0f, dy - 2.0f, 4.0f, 4.0f);
    }

    /** "0.250 Hz" / "1/4" / "500 li" — what the row's readout says. */
    static juce::String rateText(const LfoBank::Config& c)
    {
        switch (c.clock)
        {
            case LfoBank::Sync: return LfoBank::divName(c.div);
            case LfoBank::Scan: return juce::String(c.lines) + " li";
            default:            return c.rate < 1.0f ? juce::String(c.rate, 3) + " Hz"
                                                     : juce::String(c.rate, 2) + " Hz";
        }
    }

    //==========================================================================
    //── Header chevron — ▾ expanded / ▸ collapsed ────────────────────────────
    struct Chevron : juce::Button
    {
        Chevron() : juce::Button("collapse") {}
        bool* collapsed = nullptr;
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            const auto b = getLocalBounds().toFloat().reduced(4.0f);
            g.setColour(juce::Colours::white.withAlpha(over ? 0.85f : 0.5f));
            juce::Path p;
            if (collapsed != nullptr && *collapsed)
            {
                p.startNewSubPath(b.getX() + 1.0f, b.getY());
                p.lineTo(b.getRight() - 1.0f, b.getCentreY());
                p.lineTo(b.getX() + 1.0f, b.getBottom());
            }
            else
            {
                p.startNewSubPath(b.getX(), b.getY() + 1.0f);
                p.lineTo(b.getCentreX(), b.getBottom() - 1.0f);
                p.lineTo(b.getRight(), b.getY() + 1.0f);
            }
            g.fillPath(p);
        }
    };

    //── + — bring the lowest free slot into use ──────────────────────────────
    struct AddButton : juce::Button   // a Button already is a tooltip client
    {
        AddButton() : juce::Button("add") {}
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            const auto b = getLocalBounds().toFloat().reduced(4.5f);
            g.setColour(juce::Colour(Sp3ctraTheme::kColHandle).withAlpha(over ? 1.0f : 0.7f));
            g.drawLine(b.getX(), b.getCentreY(), b.getRight(), b.getCentreY(), 1.6f);
            g.drawLine(b.getCentreX(), b.getY(), b.getCentreX(), b.getBottom(), 1.6f);
        }
    };

    //── ⏻ / ⟳ — run and retrigger, the two gestures a row needs ─────────────
    struct IconButton : juce::Button
    {
        enum Kind { Power, Retrig };
        IconButton(Kind k) : juce::Button("icon"), kind(k)
        { if (k == Power) setClickingTogglesState(true); }
        Kind kind;
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            const auto  b = getLocalBounds().toFloat().reduced(4.0f);
            const bool  on = kind == Power ? getToggleState() : false;
            const juce::Colour c = on ? juce::Colour(Sp3ctraTheme::kColHandle)
                                      : juce::Colours::white.withAlpha(over ? 0.75f : 0.35f);
            g.setColour(c);
            if (kind == Power)
            {
                juce::Path p;
                p.addCentredArc(b.getCentreX(), b.getCentreY(),
                                b.getWidth() * 0.42f, b.getHeight() * 0.42f,
                                0.0f, 0.6f, juce::MathConstants<float>::twoPi - 0.6f, true);
                g.strokePath(p, juce::PathStrokeType(1.4f));
                g.drawLine(b.getCentreX(), b.getY(), b.getCentreX(), b.getCentreY(), 1.4f);
            }
            else
            {
                juce::Path p;
                p.addCentredArc(b.getCentreX(), b.getCentreY(),
                                b.getWidth() * 0.40f, b.getHeight() * 0.40f,
                                0.0f, 0.9f, juce::MathConstants<float>::twoPi, true);
                g.strokePath(p, juce::PathStrokeType(1.4f));
                juce::Path a;   // the arrow head, so it reads as "again"
                a.addTriangle(b.getRight() - 1.5f, b.getY() + 1.0f,
                              b.getRight() - 5.5f, b.getY() + 1.0f,
                              b.getRight() - 3.5f, b.getY() + 4.5f);
                g.fillPath(a);
            }
        }
    };

    //==========================================================================
    /** One LFO of the bank: pastille, live scope, readout, run / retrig. */
    struct Row : juce::Component
    {
        Row(LfoPanel& ownerIn, int indexIn) : owner(ownerIn), index(indexIn)
        {
            addAndMakeVisible(power);
            addAndMakeVisible(retrig);
            power.setTooltip("Run / stop this LFO. A stopped LFO releases what "
                             "it was driving — the parameter stays where it is.");
            retrig.setTooltip("Restart the cycle from its beginning.");
            power.onClick = [this]
            {
                auto c = bank().get(index);
                c.run = power.getToggleState();
                bank().set(index, c);
                owner.repaint();
            };
            retrig.onClick = [this] { bank().retrig(index); };

            // The two buttons are mappable targets like everything else.
            powerLearn_  = std::make_unique<MidiLearnAttachment>(
                owner.processor_.getMidiMap(), power,
                LfoMidiTargets::makeId(index, LfoMidiTargets::Run));
            retrigLearn_ = std::make_unique<MidiLearnAttachment>(
                owner.processor_.getMidiMap(), retrig,
                LfoMidiTargets::makeId(index, LfoMidiTargets::Retrig));

            power.setToggleState(bank().get(index).run, juce::dontSendNotification);
        }

        LfoBank& bank() { return owner.processor_.getLfoBank(); }

        void paint(juce::Graphics& g) override
        {
            const bool sel = owner.selected_ == index;
            const auto b   = getLocalBounds();

            if (sel)
            {
                g.setColour(juce::Colour(Sp3ctraTheme::kColMod).withAlpha(0.12f));
                g.fillRoundedRectangle(b.toFloat(), 3.0f);
                g.setColour(juce::Colour(Sp3ctraTheme::kColMod).withAlpha(0.8f));
                g.fillRect(0, 2, 2, b.getHeight() - 4);
            }
            else if (hover_)
            {
                g.setColour(juce::Colours::white.withAlpha(0.05f));
                g.fillRoundedRectangle(b.toFloat(), 3.0f);
            }
            // The freshly-selected row flashes in the modulation hue — the
            // same receipt the rest of the UI gives (Sp3ctraControls heat).
            if (const float h = Sp3ctraControls::heatSince(owner.flashMs_);
                h > 0.0f && owner.flashed_ == index)
            {
                g.setColour(juce::Colour(Sp3ctraTheme::kColMod).withAlpha(0.30f * h));
                g.fillRoundedRectangle(b.toFloat(), 3.0f);
            }

            const auto c = bank().get(index);

            // Pastille — the rack's recipe, in the modulation hue: the NUMBER
            // is the identity of an LFO, exactly as it is of a chain.
            ChainIdentity::drawPastille(g, juce::Rectangle<float>(6.0f,
                                            (float) (getHeight() - 15) * 0.5f, 15.0f, 15.0f),
                                        index + 1, juce::Colour(Sp3ctraTheme::kColMod),
                                        c.run ? 1.0f : 0.45f);

            drawScope(g, bank(), index, scopeArea().toFloat(), 1.0f);

            // Readout: the value in its own unit, and under it what the
            // clock divides — the live line rate (SCAN), the tempo in use
            // (SYNC: the host's or the bank's) — else the shape's name.
            const auto ro = readoutArea();
            g.setColour(juce::Colour(c.run ? 0xffb8c4d0 : 0xff6a7284));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            g.drawText(rateText(c), ro.withTrimmedBottom(9),
                       juce::Justification::centredRight, false);
            g.setColour(juce::Colour(0xff6a7284));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
            g.drawText(c.clock == LfoBank::Scan
                           ? juce::String(LfoBank::clockName(c.clock)) + " "
                                 + juce::String(bank().lineRate(), 0)
                       : c.clock == LfoBank::Sync
                           ? juce::String(LfoBank::clockName(c.clock)) + " "
                                 + juce::String(bank().effectiveBpm(), 0)
                           : juce::String(LfoBank::shapeName(c.shape)),
                       ro.withTrimmedTop(ro.getHeight() - 10),
                       juce::Justification::centredRight, false);
        }

        juce::Rectangle<int> scopeArea() const
        { return { 26, 3, juce::jmax(10, getWidth() - 26 - 62 - 44), getHeight() - 6 }; }
        juce::Rectangle<int> readoutArea() const
        { return { getWidth() - 62 - 44, 2, 60, getHeight() - 4 }; }

        void resized() override
        {
            const int d = juce::jmin(20, getHeight() - 4);
            const int y = (getHeight() - d) / 2;
            retrig.setBounds(getWidth() - 2 * d - 6, y, d, d);
            power .setBounds(getWidth() - d - 3,     y, d, d);
        }

        void mouseEnter(const juce::MouseEvent&) override { hover_ = true;  repaint(); }
        void mouseExit (const juce::MouseEvent&) override { hover_ = false; repaint(); }
        void mouseUp(const juce::MouseEvent& e) override
        {
            if (! contains(e.getPosition())) return;
            if (! e.mods.isPopupMenu()) { owner.selectLfo(index); return; }

            // The row's own menu: give the slot back. The mappings that
            // named this LFO go with it — a row driving nothing is a lie.
            const int n = owner.destinationsOf(index);
            juce::PopupMenu m;
            m.addItem(1, "Remove LFO " + juce::String(index + 1)
                         + (n > 0 ? " (" + juce::String(n)
                                       + (n > 1 ? " destinations)" : " destination)")
                                  : juce::String()));
            const int idx = index;
            LfoPanel* panel = &owner;
            m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this)
                                                      .withMousePosition(),
                            [panel, idx](int choice)
                            { if (choice == 1) panel->removeLfo(idx); });
        }

        /** The scope only: repainting the whole row at 25 Hz would fight the
         *  buttons' own hover repaints for nothing. */
        void refreshLive() { repaint(scopeArea().expanded(2)); repaint(readoutArea()); }

        LfoPanel& owner;
        int       index;
        IconButton power { IconButton::Power };
        IconButton retrig { IconButton::Retrig };

    private:
        bool hover_ { false };
        std::unique_ptr<MidiLearnAttachment> powerLearn_, retrigLearn_;
    };

    //==========================================================================
    /** The shape LIST — every shape visible at once, each cell drawing its own
     *  law (LfoBank::shapeAt): the icon IS the waveform, so nothing can drift
     *  between what the button shows and what the LFO plays. Click picks; the
     *  strip as a whole is the "lfo:N:shape" target, so a controller can sweep
     *  the list from the same right-click menu as anywhere else. */
    struct ShapeStrip : juce::Component,
                        public juce::SettableTooltipClient
    {
        std::function<void(int)> onPick;
        int  value { 0 };
        int  seed  { 0 };

        void paint(juce::Graphics& g) override
        {
            const juce::Colour acc = juce::Colour(Sp3ctraTheme::kColHandle);
            const juce::Colour mod = juce::Colour(Sp3ctraTheme::kColMod);
            for (int i = 0; i < LfoBank::NumShapes; ++i)
            {
                const auto  b   = cell(i).toFloat().reduced(1.0f);
                const bool  on  = (i == value);
                const bool  hot = (i == hover_);

                g.setColour(juce::Colour(Sp3ctraTheme::kColBarBg));
                g.fillRoundedRectangle(b, 2.0f);
                if (on || hot)
                {
                    g.setColour(acc.withAlpha(on ? 0.22f : 0.10f));
                    g.fillRoundedRectangle(b, 2.0f);
                }
                g.setColour(acc.withAlpha(on ? 0.75f : hot ? 0.45f : 0.18f));
                g.drawRoundedRectangle(b, 2.0f, 1.0f);

                // The law itself, one cycle wide.
                const auto p = b.reduced(4.0f, 5.0f);
                juce::Path path;
                const int N = juce::jmax(6, (int) p.getWidth());
                for (int k = 0; k <= N; ++k)
                {
                    const float t = (float) k / (float) N;
                    const float v = LfoBank::shapeAt(i, t, LfoBank::kDefSkew, 0, seed);
                    const float x = p.getX() + p.getWidth() * t;
                    const float y = p.getBottom() - p.getHeight() * v;
                    k ? path.lineTo(x, y) : path.startNewSubPath(x, y);
                }
                g.setColour(mod.withAlpha(on ? 1.0f : 0.55f));
                g.strokePath(path, juce::PathStrokeType(on ? 1.5f : 1.1f));
            }
        }

        juce::Rectangle<int> cell(int i) const
        {
            const float w = (float) getWidth() / (float) LfoBank::NumShapes;
            return juce::Rectangle<float>(w * (float) i, 0.0f, w,
                                          (float) getHeight()).toNearestInt();
        }
        int indexAt(juce::Point<int> p) const
        {
            for (int i = 0; i < LfoBank::NumShapes; ++i)
                if (cell(i).contains(p)) return i;
            return -1;
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            if (const int i = indexAt(e.getPosition()); i != hover_)
            {
                hover_ = i;
                setTooltip(i >= 0 ? juce::String(LfoBank::shapeName(i))
                                  : juce::String("Waveform"));
                repaint();
            }
        }
        void mouseExit(const juce::MouseEvent&) override { hover_ = -1; repaint(); }
        void mouseUp(const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) return;   // MidiLearnAttachment owns it
            if (const int i = indexAt(e.getPosition()); i >= 0 && onPick) onPick(i);
        }
        int hover_ { -1 };
    };

    //── The clock, as a three-way switch: the alternatives are the point ─────
    struct Segmented : juce::Component,
                       public juce::SettableTooltipClient
    {
        std::function<void(int)> onPick;
        juce::StringArray items;
        int value { 0 };

        void paint(juce::Graphics& g) override
        {
            const juce::Colour acc = juce::Colour(Sp3ctraTheme::kColHandle);
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
            for (int i = 0; i < items.size(); ++i)
            {
                const auto b  = cell(i).toFloat().reduced(1.0f, 0.0f);
                const bool on = (i == value), hot = (i == hover_);
                g.setColour(juce::Colour(Sp3ctraTheme::kColBarBg));
                g.fillRoundedRectangle(b, 2.0f);
                if (on || hot)
                {
                    g.setColour(acc.withAlpha(on ? 0.24f : 0.10f));
                    g.fillRoundedRectangle(b, 2.0f);
                }
                g.setColour(acc.withAlpha(on ? 0.75f : hot ? 0.45f : 0.18f));
                g.drawRoundedRectangle(b, 2.0f, 1.0f);
                g.setColour(juce::Colour(on ? 0xffeef4ff : 0xff8a93a4));
                g.drawText(items[i], cell(i), juce::Justification::centred, false);
            }
        }
        juce::Rectangle<int> cell(int i) const
        {
            const int n = juce::jmax(1, items.size());
            const float w = (float) getWidth() / (float) n;
            return juce::Rectangle<float>(w * (float) i, 0.0f, w,
                                          (float) getHeight()).toNearestInt();
        }
        int indexAt(juce::Point<int> p) const
        {
            for (int i = 0; i < items.size(); ++i)
                if (cell(i).contains(p)) return i;
            return -1;
        }
        void mouseMove(const juce::MouseEvent& e) override
        { if (const int i = indexAt(e.getPosition()); i != hover_) { hover_ = i; repaint(); } }
        void mouseExit(const juce::MouseEvent&) override { hover_ = -1; repaint(); }
        void mouseUp(const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) return;
            if (const int i = indexAt(e.getPosition()); i >= 0 && onPick) onPick(i);
        }
        int hover_ { -1 };
    };

    //── A rotary, the app's own (Sp3ctraLookAndFeel paints the arc) ──────────
    struct Knob : Sp3ctraGestureSlider
    {
        Knob()
        {
            setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            setScrollWheelEnabled(false);   // the zone-4 column scrolls instead
        }
        // The LookAndFeel reads the pointer state at paint time, and a plain
        // Slider only repaints on a value change (same fix as RangeKnob).
        void mouseEnter(const juce::MouseEvent& e) override
        { Sp3ctraGestureSlider::mouseEnter(e); repaint(); }
        void mouseExit(const juce::MouseEvent& e) override
        { Sp3ctraGestureSlider::mouseExit(e); repaint(); }
    };

    //==========================================================================
    /** The selected LFO's settings.
     *
     *      ∿  △  ◹  ◺  ⊓  ⋮  ≈          the shapes, all of them, drawn
     *      FREE   SYNC   SCAN            the clock, all of it
     *      (◯) RATE      (◯) SKEW   (◯) STEPS   (◯) TEMPO
     *          0.250 Hz      50 %        OFF        120 BPM
     *
     *  Nothing cycles blind here: every alternative is on screen, and the
     *  continuous values are rotaries with their reading beside them.
     *  The first rotary changes meaning with the clock — RATE in Hz, DIV in
     *  note values, LINES in scanned lines — because only one of the three
     *  exists at a time. The fourth cell is what SYNC divides: a TEMPO knob
     *  when nothing else sets one (the standalone has no transport), the
     *  host's tempo read-only — "DAW TEMPO" — when a DAW publishes one. It
     *  is empty under the other clocks. */
    struct Edit : juce::Component
    {
        explicit Edit(LfoPanel& ownerIn) : owner(ownerIn)
        {
            addAndMakeVisible(shapes);
            addAndMakeVisible(clocks);
            addAndMakeVisible(rate);
            addAndMakeVisible(skew);
            addAndMakeVisible(steps);
            addChildComponent(tempo);   // shown by refresh(): SYNC, no host tempo

            clocks.items = { LfoBank::clockName(LfoBank::Free),
                             LfoBank::clockName(LfoBank::Sync),
                             LfoBank::clockName(LfoBank::Scan) };

            shapes.onPick = [this](int s)
            { auto c = bank().get(index); c.shape = s; bank().set(index, c);
              refresh(); owner.repaintRows(); };
            clocks.onPick = [this](int k)
            { auto c = bank().get(index); c.clock = k; bank().set(index, c);
              refresh(); owner.repaintRows(); };

            rate.setTooltip("Cycle rate. FREE: hertz. SYNC: a division of the "
                            "host tempo. SCAN: a period in scanned lines - the "
                            "modulation then follows the paper, not the clock.");
            skew.setTooltip("Asymmetry - a fast rise that settles, or the "
                            "reverse. On SQR: the pulse width.");
            steps.setTooltip("Quantise the cycle into N holds - an LFO with "
                             "steps IS a stepper. OFF = continuous.");
            tempo.setTooltip("Tempo of the SYNC clock, in BPM - what the "
                             "divisions divide. Set it here in the standalone; "
                             "in a DAW the host's tempo takes over and is "
                             "read here instead.");
            // Skew is a CENTRED value: its arc grows from 12 o'clock like a
            // pan, so "no asymmetry" reads at a glance.
            skew.getProperties().set("rotaryFromCentre", true);

            rate.onValueChange = [this]
            {
                auto c = bank().get(index);
                if      (c.clock == LfoBank::Sync) c.div   = (int) rate.getValue();
                else if (c.clock == LfoBank::Scan) c.lines = (int) rate.getValue();
                else                               c.rate  = (float) rate.getValue();
                bank().set(index, c); owner.repaintRows(); repaint();
            };
            skew.onValueChange = [this]
            {
                auto c = bank().get(index); c.skew = (float) skew.getValue();
                bank().set(index, c); owner.repaintRows(); repaint();
            };
            steps.onValueChange = [this]
            {
                auto c = bank().get(index); c.steps = (int) steps.getValue();
                bank().set(index, c); owner.repaintRows(); repaint();
            };
            tempo.onValueChange = [this]
            {
                bank().setTempo((float) tempo.getValue());
                owner.repaintRows(); repaint();
            };
        }

        /** "120 BPM" / "127.5 BPM" — whole tempi stay whole. */
        static juce::String bpmText(double v)
        {
            const double r = std::round(v);
            return (std::abs(v - r) < 0.05 ? juce::String((int) r)
                                           : juce::String(v, 1)) + " BPM";
        }

        LfoBank& bank() { return owner.processor_.getLfoBank(); }

        /** Point the whole block at LFO `i` — rebuilds the MIDI attachments so
         *  each control's right-click menu names THAT LFO's target. */
        void bind(int i)
        {
            index = i;
            shapes.seed = i;   // the random shapes draw their own steps
            auto learn = [this, i](juce::Component& c, int which)
            {
                return std::make_unique<MidiLearnAttachment>(
                    owner.processor_.getMidiMap(), c, LfoMidiTargets::makeId(i, which));
            };
            shapeLearn_ = learn(shapes, LfoMidiTargets::ShapeSel);
            clockLearn_ = learn(clocks, LfoMidiTargets::ClockSel);
            skewLearn_  = learn(skew,   LfoMidiTargets::Skew);
            stepsLearn_ = learn(steps,  LfoMidiTargets::Steps);
            refresh();
        }

        /** Controls <- bank. The RATE rotary is re-ranged for the live clock. */
        void refresh()
        {
            const auto c = bank().get(index);
            shapes.value = c.shape;
            clocks.value = c.clock;

            if (c.clock == LfoBank::Sync)
            {
                rate.textFromValueFunction = [](double v)
                { return juce::String(LfoBank::divName((int) v)); };
                rate.setRange(0.0, LfoBank::kNumDivs - 1, 1.0);
                rate.setDoubleClickReturnValue(true, 2.0);
                rate.setValue(c.div, juce::dontSendNotification);
                rateLearn_ = std::make_unique<MidiLearnAttachment>(
                    owner.processor_.getMidiMap(), rate,
                    LfoMidiTargets::makeId(index, LfoMidiTargets::DivSel));
            }
            else if (c.clock == LfoBank::Scan)
            {
                rate.textFromValueFunction = [](double v)
                { return juce::String((int) v) + " li"; };
                rate.setRange(LfoBank::kMinLines, LfoBank::kMaxLines, 1.0);
                rate.setSkewFactorFromMidPoint(400.0);
                rate.setDoubleClickReturnValue(true, LfoBank::kDefLines);
                rate.setValue(c.lines, juce::dontSendNotification);
                rateLearn_.reset();   // no virtual target for the line period
            }
            else
            {
                rate.textFromValueFunction = [](double v)
                { return (v < 1.0 ? juce::String(v, 3) : juce::String(v, 2)) + " Hz"; };
                rate.setRange(LfoBank::kMinRate, LfoBank::kMaxRate, 0.001);
                rate.setSkewFactorFromMidPoint(0.5);
                rate.setDoubleClickReturnValue(true, LfoBank::kDefRate);
                rate.setValue(c.rate, juce::dontSendNotification);
                rateLearn_ = std::make_unique<MidiLearnAttachment>(
                    owner.processor_.getMidiMap(), rate,
                    LfoMidiTargets::makeId(index, LfoMidiTargets::Rate));
            }

            skew.textFromValueFunction = [](double v)
            { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
            skew.setRange(0.05, 0.95, 0.01);
            skew.setDoubleClickReturnValue(true, LfoBank::kDefSkew);
            skew.setValue(c.skew, juce::dontSendNotification);

            steps.textFromValueFunction = [](double v)
            { return v < 0.5 ? juce::String("OFF") : juce::String((int) v); };
            steps.setRange(0, LfoBank::kMaxSteps, 1);
            steps.setDoubleClickReturnValue(true, 0.0);
            steps.setValue(c.steps, juce::dontSendNotification);

            // The SYNC reference: the knob only where nothing else sets the
            // tempo. Under a DAW the cell turns into a readout (paint()).
            hosted_ = bank().hostBpm() > 0.0f;
            shownHostBpm_ = bank().hostBpm();
            tempo.textFromValueFunction = [](double v) { return bpmText(v); };
            tempo.setRange(LfoBank::kMinTempo, LfoBank::kMaxTempo, 0.1);
            tempo.setSkewFactorFromMidPoint(LfoBank::kDefTempo);
            tempo.setDoubleClickReturnValue(true, LfoBank::kDefTempo);
            tempo.setValue(bank().tempo(), juce::dontSendNotification);
            tempo.setVisible(c.clock == LfoBank::Sync && ! hosted_);

            // The boxes carry no suffix: the text function IS the unit, so it
            // has to be re-run after every rebind.
            rate.updateText(); skew.updateText(); steps.updateText(); tempo.updateText();
            shapes.repaint(); clocks.repaint();
            repaint();
        }

        /** Panel timer: a mapped controller - or another LFO - can be moving
         *  these very settings. Nothing is APVTS-attached here, so pull the
         *  bank at UI rate, except the control the pointer or the keyboard
         *  is on. */
        void syncFromBank()
        {
            // A host tempo appearing (or going) swaps the knob for the readout.
            if ((bank().hostBpm() > 0.0f) != hosted_) refresh();

            const auto c = bank().get(index);
            auto pull = [](Knob& s, double v)
            {
                if (s.isMouseButtonDown(true) || s.hasKeyboardFocus(true)) return false;
                if (std::abs(s.getValue() - v) <= 1.0e-4) return false;
                s.setValue(v, juce::dontSendNotification);
                return true;
            };
            bool moved = pull(rate, c.clock == LfoBank::Sync ? (double) c.div
                                  : c.clock == LfoBank::Scan ? (double) c.lines
                                                             : (double) c.rate);
            moved |= pull(skew,  c.skew);
            moved |= pull(steps, (double) c.steps);
            moved |= pull(tempo, (double) bank().tempo());
            if (hosted_ && std::abs(bank().hostBpm() - shownHostBpm_) >= 0.05f)
            { shownHostBpm_ = bank().hostBpm(); moved = true; }   // the DAW moved
            if (moved) repaint(readoutRow());   // the numbers live beside the knobs

            if (c.shape != shapes.value) { shapes.value = c.shape; shapes.repaint(); }
            if (c.clock != clocks.value) { clocks.value = c.clock; refresh(); }
        }

        void paint(juce::Graphics& g) override
        {
            g.setColour(juce::Colour(Sp3ctraTheme::kColBorder).withAlpha(0.6f));
            g.drawHorizontalLine(0, 0.0f, (float) getWidth());

            const auto c = bank().get(index);
            const juce::Colour lab = juce::Colour(Sp3ctraTheme::kColMod).withAlpha(0.65f);

            // Each rotary says WHAT it is and WHERE it stands, beside it: a
            // knob with nothing written next to it is a knob you have to turn
            // to find out. The fourth cell is the SYNC reference — the TEMPO
            // knob, or, under a DAW, its tempo as a plain readout: no knob,
            // and the value in the display colour, not the handle's, since
            // it is nothing you can touch here.
            const bool sync = c.clock == LfoBank::Sync;
            struct Cell { const char* name; Knob* knob; };
            const Cell cells[4] = {
                { sync ? "DIVISION" : c.clock == LfoBank::Scan ? "PERIOD" : "RATE", &rate },
                { "SKEW",  &skew  },
                { "STEPS", &steps },
                { sync ? (hosted_ ? "DAW TEMPO" : "TEMPO") : nullptr,
                  sync && ! hosted_ ? &tempo : nullptr } };
            for (int i = 0; i < 4; ++i)
            {
                if (cells[i].name == nullptr) continue;
                auto t = cells[i].knob != nullptr ? textOf(i)
                                                  : knobCell(i).reduced(2, 2);
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
                g.setColour(lab);
                g.drawText(cells[i].name, t.removeFromTop(11),
                           juce::Justification::bottomLeft, false);
                g.setFont(juce::FontOptions(Sp3ctraTheme::kFontBadge));
                if (auto* k = cells[i].knob)
                {
                    g.setColour(juce::Colour(Sp3ctraTheme::kColHandle));
                    g.drawText(k->getTextFromValue(k->getValue()), t,
                               juce::Justification::topLeft, false);
                }
                else
                {
                    g.setColour(juce::Colour(Sp3ctraTheme::kColMod));
                    g.drawText(bpmText(shownHostBpm_), t,
                               juce::Justification::topLeft, false);
                }
            }

            // What this LFO drives, and how to give it more.
            g.setColour(juce::Colour(0xff6a7284));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontMicro));
            const int n = destinationCount();
            g.drawText(n == 0
                         ? juce::String::fromUTF8("no destination \xE2\x80\x94 right-click a control \xE2\x96\xB8 Modulate")
                         : juce::String(n) + (n > 1 ? " destinations" : " destination")
                               + juce::String::fromUTF8(" \xC2\xB7 MIDI MAP"),
                       getLocalBounds().removeFromBottom(kCaptionH).reduced(2, 0),
                       juce::Justification::centredLeft, false);
        }

        int destinationCount() const
        {
            int n = 0;
            for (const auto& m : owner.processor_.getMidiMap().allMappings())
                if (m.type == MidiMappingEngine::kTypeLfo && m.number == index) ++n;
            return n;
        }

        //── Geometry ────────────────────────────────────────────────────────
        juce::Rectangle<int> knobRow() const
        {
            return { 0, kPad + kShapeH + kGapY + kClockH + kGapY,
                     getWidth(), kKnobH };
        }
        juce::Rectangle<int> readoutRow() const { return knobRow(); }
        /** Four cells, whatever the clock: SKEW and STEPS never move when
         *  the fourth (TEMPO, SYNC only) comes and goes. */
        juce::Rectangle<int> knobCell(int i) const
        {
            const int w = getWidth() / 4;
            return knobRow().withX(i * w).withWidth(w);
        }
        /** The text block of cell i — right of its knob. */
        juce::Rectangle<int> textOf(int i) const
        { return knobCell(i).withTrimmedLeft(kKnobD + 5).reduced(0, 2); }

        void resized() override
        {
            shapes.setBounds(0, kPad, getWidth(), kShapeH);
            clocks.setBounds(0, kPad + kShapeH + kGapY, getWidth(), kClockH);
            Knob* const knobs[4] = { &rate, &skew, &steps, &tempo };
            for (int i = 0; i < 4; ++i)
            {
                auto c = knobCell(i);
                knobs[i]->setBounds(c.getX(), c.getY(), kKnobD, kKnobD);
            }
        }

        LfoPanel&  owner;
        int        index { 0 };
        ShapeStrip shapes;
        Segmented  clocks;
        Knob       rate, skew, steps, tempo;

    private:
        bool  hosted_       { false };   ///< a DAW publishes the tempo
        float shownHostBpm_ { 0.0f };    ///< the readout's last value
        std::unique_ptr<MidiLearnAttachment> shapeLearn_, clockLearn_,
                                             rateLearn_, skewLearn_, stepsLearn_;
    };

    //==========================================================================
    /** Rows ← the bank's slots in use. Cheap no-op while the set is
     *  unchanged; a rebuild keeps the selection when its slot survives,
     *  else takes the first row (or none). Called by the timer too: a slot
     *  brought into use from a control's menu shows up on its own. */
    void syncActive()
    {
        const uint32_t mask = processor_.getLfoBank().activeMask();
        if (mask == rowsMask_) return;
        rowsMask_ = mask;

        rows_.clear();
        for (int i = 0; i < LfoBank::kNumLfos; ++i)
        {
            if (! (mask & (1u << i))) continue;
            auto row = std::make_unique<Row>(*this, i);
            rowsHolder_.addAndMakeVisible(row.get());
            rows_.push_back(std::move(row));
        }

        if (selected_ < 0 || ! (mask & (1u << selected_)))
            selected_ = rows_.empty() ? -1 : rows_.front()->index;
        if (selected_ >= 0) edit_->bind(selected_);

        resized();
        repaint();
        if (onContentChanged) onContentChanged();
    }

    int destinationsOf(int lfo) const
    {
        int n = 0;
        for (const auto& m : processor_.getMidiMap().allMappings())
            if (m.type == MidiMappingEngine::kTypeLfo && m.number == lfo) ++n;
        return n;
    }

    void removeLfo(int i)
    {
        auto& eng = processor_.getMidiMap();
        for (const auto& m : eng.allMappings())
            if (m.type == MidiMappingEngine::kTypeLfo && m.number == i)
                eng.removeMappingFor(m.paramId);
        processor_.getLfoBank().remove(i);
        syncActive();
    }

    void repaintRows()
    {
        for (auto& r : rows_)
        {
            r->power.setToggleState(processor_.getLfoBank().get(r->index).run,
                                    juce::dontSendNotification);
            r->repaint();
        }
        repaint();
    }

    void flash(int i)
    {
        flashed_ = i;
        flashMs_ = juce::Time::getMillisecondCounterHiRes();
        repaintRows();
    }

    void timerCallback() override
    {
        syncActive();   // "New LFO…" from a control's menu, or a removal elsewhere
        if (collapsed_) { repaint(0, 0, getWidth(), kHeaderH); return; }
        for (auto& r : rows_) r->refreshLive();
        if (edit_ && selected_ >= 0) edit_->syncFromBank();
        // The flash fades on its own clock; keep repainting while it lasts.
        if (Sp3ctraControls::heatSince(flashMs_) > 0.0f)
            for (auto& r : rows_) r->repaint();
    }

    Sp3ctraAudioProcessor& processor_;
    Chevron   chevron_;
    AddButton add_;
    uint32_t  rowsMask_ { 0xFFFFFFFFu };   ///< slots the rows were built for
    juce::Viewport viewport_;
    juce::Component rowsHolder_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::unique_ptr<Edit> edit_;
    bool  collapsed_ { false };
    int   selected_  { -1 };
    int   flashed_   { -1 };
    double flashMs_  { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LfoPanel)
};

inline const juce::Colour LfoPanel::kAccent { Sp3ctraTheme::kColMod };
