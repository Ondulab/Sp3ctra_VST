/**
 * @file Sp3ctraGestures.h
 * @brief THE two "extra" gestures of every value control of the Sp3ctra UI —
 *        bar, fader, knob, or a hand-painted canvas handle — in one place:
 *
 *   • DOUBLE-CLICK  = back to the DEFAULT of everything the control drives;
 *   • LONG PRESS    = TYPE the value(s): the pointer stays still for
 *                     kLongPressMs and an entry bubble opens over the
 *                     control (one field per parameter of the handle);
 *                     Return commits, Esc / a click outside cancels.
 *   • right-click stays the MIDI Learn menu, the wheel never edits.
 *
 * Widgets get the pair from Sp3ctraGestureSlider (which routes here). A
 * canvas editor adds a `Hold` member and answers two questions — which
 * `Bound`s a handle drives (`Fields`), and where the bubble should sit —
 * and gets exactly the same behaviour as a bar slider, so a user who has
 * learnt one control has learnt them all.
 *
 * Canvas recipe (see ReverbEditorComponent for a two-handle instance):
 *
 *   Sp3ctraGestures::Hold hold_;
 *   mouseDown:        if (e.getNumberOfClicks() != 1) return;   // 2nd click → mouseDoubleClick
 *                     … open the drag gesture …
 *                     if (handle) hold_.arm(e, [this] { holdToType(); });
 *   mouseDrag:        if (hold_.fired()) return;  hold_.moved(e);  …
 *   mouseUp:          const bool held = hold_.release();  if (! held) … end the gesture …
 *   mouseDoubleClick: Sp3ctraGestures::toDefault(fieldsOf(handleAt(e)));
 *   holdToType():     end the open gesture, then
 *                     Sp3ctraGestures::openEntry(*this, hold_.anchor(*this), fieldsOf(h));
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>
#include <utility>
#include <vector>
#include "../UITheme.h"
#include "Sp3ctraControls.h"

namespace Sp3ctraGestures
{
    /** Hold-to-type delay — one number for the whole UI. */
    constexpr int kLongPressMs = 600;

    //══════════════════════════════════════════════════════════════════════════
    // 1. Hold — the long-press detector of a hand-painted control
    //══════════════════════════════════════════════════════════════════════════
    /** Arm it on the press, feed it the drags, release it on the mouse-up. It
     *  fires ONCE when the pointer has stayed still for kLongPressMs; a hand
     *  that moves is not a holding hand. Once fired, the rest of that gesture
     *  belongs to the entry bubble: the owner skips its drag / release work. */
    class Hold : private juce::Timer
    {
    public:
        Hold() = default;
        ~Hold() override { stopTimer(); }

        /** Press: start watching. `onHold` runs on the message thread while
         *  the button is still down — the owner ends its open gesture there. */
        void arm(const juce::MouseEvent& e, std::function<void()> onHold)
        {
            fn_    = std::move(onHold);
            fired_ = false;
            press_ = e.position;
            startTimer(kLongPressMs);
        }

        /** Drag: a travelling pointer cancels the hold. */
        void moved(const juce::MouseEvent& e)
        {
            if (isTimerRunning() && e.mouseWasDraggedSinceMouseDown())
                cancel();
        }

        /** Release: stop watching; answers whether the hold had fired (the
         *  owner then skips its click / gesture-end work — it already
         *  happened when the hold fired). */
        bool release()
        {
            cancel();
            const bool f = fired_;
            fired_ = false;
            return f;
        }

        void cancel() { stopTimer(); fn_ = nullptr; }

        /** True between the hold firing and the release. */
        bool fired() const noexcept { return fired_; }

        /** Where the bubble should point: a small square around the press,
         *  in screen coordinates. */
        juce::Rectangle<int> anchor(const juce::Component& owner) const
        {
            return owner.localAreaToGlobal(
                juce::Rectangle<int>(juce::roundToInt(press_.x) - 6,
                                     juce::roundToInt(press_.y) - 6, 12, 12));
        }

    private:
        void timerCallback() override
        {
            stopTimer();
            fired_ = true;
            if (auto f = std::move(fn_)) f();
        }

        std::function<void()> fn_;
        juce::Point<float>    press_;
        bool                  fired_ = false;
    };

    //══════════════════════════════════════════════════════════════════════════
    // 2. Fields — what a control lets you type
    //══════════════════════════════════════════════════════════════════════════
    /** One line of the entry bubble: a label, the current text, and how a
     *  new text lands. `commit` is only called for a field whose text
     *  changed, so an untouched line rewrites nothing (and lights nothing). */
    struct Field
    {
        juce::String label;
        juce::String text;
        std::function<void(const juce::String&)> commit;
    };
    using Fields = std::vector<Field>;

    /** A bound parameter of a canvas editor — text and parsing are the
     *  parameter's own (dB, Hz, "x2.00"…), the write is a complete gesture. */
    inline Field fieldOf(const juce::String& label, Sp3ctraControls::Bound& b)
    {
        Field f;
        f.label = label;
        if (b.param == nullptr) return f;
        f.text   = b.param->getCurrentValueAsText();
        f.commit = [bp = &b](const juce::String& t)
        {
            if (bp->param == nullptr) return;
            bp->setComplete(bp->param->convertFrom0to1(bp->param->getValueForText(t)));
        };
        return f;
    }

    /** A raw APVTS parameter written directly (no Bound): one host gesture. */
    inline Field fieldOf(const juce::String& label, juce::RangedAudioParameter& p)
    {
        Field f;
        f.label  = label;
        f.text   = p.getCurrentValueAsText();
        f.commit = [pp = &p](const juce::String& t)
        {
            pp->beginChangeGesture();
            pp->setValueNotifyingHost(pp->getValueForText(t));
            pp->endChangeGesture();
        };
        return f;
    }

    /** A slider (a value box): its own text conversions, notification sync. */
    inline Field fieldOf(const juce::String& label, juce::Slider& s)
    {
        Field f;
        f.label  = label;
        f.text   = s.getTextFromValue(s.getValue());
        f.commit = [sp = juce::Component::SafePointer<juce::Slider>(&s)](const juce::String& t)
        {
            if (auto* sl = sp.getComponent())
                sl->setValue(sl->getValueFromText(t), juce::sendNotificationSync);
        };
        return f;
    }

    /** Anything else: the owner supplies the text and the landing. */
    inline Field fieldOf(const juce::String& label, const juce::String& text,
                         std::function<void(const juce::String&)> commit)
    {
        return { label, text, std::move(commit) };
    }

    /** A canvas handle's parameters — label + binding, in the order the
     *  bubble lists them. One answer per handle serves both gestures. */
    using BoundList = std::vector<std::pair<juce::String, Sp3ctraControls::Bound*>>;

    /** The double-click: every bound of the handle back to its default. */
    inline void toDefault(const BoundList& bounds)
    {
        for (const auto& [label, b] : bounds)
            if (b != nullptr) b->toDefault();
    }

    inline Fields fieldsOf(const BoundList& bounds)
    {
        Fields f;
        for (const auto& [label, b] : bounds)
            if (b != nullptr) f.push_back(fieldOf(label, *b));
        return f;
    }

    //══════════════════════════════════════════════════════════════════════════
    // 3. The entry bubble
    //══════════════════════════════════════════════════════════════════════════
    /** One TextEditor per field, labels on the left when any field has one.
     *  Return commits every CHANGED field through its own conversion and
     *  closes; Esc (or a click outside — a CallOutBox) leaves everything as
     *  it was. Tab walks the fields. */
    class EntryBubble : public juce::Component
    {
    public:
        EntryBubble(juce::Component& owner, Fields fields)
            : owner_(&owner), fields_(std::move(fields))
        {
            const float fontPx = (float) Sp3ctraTheme::kFontSmall + 2.0f;
            const juce::Font font { juce::FontOptions(fontPx) };
            int labelW = 0;
            for (const auto& f : fields_)
                if (f.label.isNotEmpty())
                    labelW = juce::jmax(labelW, (int) std::ceil(juce::GlyphArrangement::getStringWidth(font, f.label)) + 8);
            labelW_ = juce::jmin(labelW, 110);

            for (const auto& f : fields_)
            {
                auto lab = std::make_unique<juce::Label>();
                lab->setText(f.label, juce::dontSendNotification);
                lab->setFont(font);
                lab->setJustificationType(juce::Justification::centredRight);
                lab->setColour(juce::Label::textColourId, juce::Colour(Sp3ctraTheme::kColTextMuted));
                lab->setBorderSize(juce::BorderSize<int>(0, 0, 0, 6));
                addAndMakeVisible(*lab);
                labels_.push_back(std::move(lab));

                auto ed = std::make_unique<juce::TextEditor>();
                ed->setJustification(juce::Justification::centred);
                ed->setFont(font);
                ed->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff181820));
                ed->setColour(juce::TextEditor::textColourId,       juce::Colours::white);
                ed->setColour(juce::TextEditor::outlineColourId,
                              juce::Colour(Sp3ctraTheme::kColHandle).withAlpha(0.55f));
                ed->setColour(juce::TextEditor::focusedOutlineColourId,
                              juce::Colour(Sp3ctraTheme::kColHandle));
                ed->setSelectAllWhenFocused(true);
                ed->setText(f.text, juce::dontSendNotification);
                ed->onReturnKey = [this] { commit(); };
                ed->onEscapeKey = [this] { dismiss(); };
                addAndMakeVisible(*ed);
                editors_.push_back(std::move(ed));
            }

            const int n = (int) fields_.size();
            setSize(kPad * 2 + labelW_ + kEditorW,
                    kPad * 2 + n * kRowH + juce::jmax(0, n - 1) * kRowGap);
        }

        void resized() override
        {
            int y = kPad;
            for (size_t i = 0; i < editors_.size(); ++i)
            {
                labels_[i] ->setBounds(kPad, y, labelW_, kRowH);
                editors_[i]->setBounds(kPad + labelW_, y, kEditorW, kRowH);
                y += kRowH + kRowGap;
            }
        }

        void grabFocus()
        {
            if (! editors_.empty()) editors_.front()->grabKeyboardFocus();
        }

    private:
        static constexpr int kPad = 4, kRowH = 24, kRowGap = 3, kEditorW = 84;

        void commit()
        {
            if (owner_.getComponent() != nullptr)
                for (size_t i = 0; i < fields_.size(); ++i)
                {
                    const auto t = editors_[i]->getText().trim();
                    if (t.isNotEmpty() && t != fields_[i].text && fields_[i].commit)
                        fields_[i].commit(t);
                }
            dismiss();
        }

        void dismiss()
        {
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        }

        juce::Component::SafePointer<juce::Component> owner_;   ///< guards the commits
        Fields fields_;
        int    labelW_ = 0;
        std::vector<std::unique_ptr<juce::Label>>      labels_;
        std::vector<std::unique_ptr<juce::TextEditor>> editors_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EntryBubble)
    };

    /** Open the entry bubble pointing at `screenArea`. Fields without a
     *  landing (an unbound parameter) are dropped; nothing opens when none
     *  is left. */
    inline void openEntry(juce::Component& owner, juce::Rectangle<int> screenArea, Fields fields)
    {
        Fields live;
        for (auto& f : fields)
            if (f.commit) live.push_back(std::move(f));
        if (live.empty()) return;

        auto content = std::make_unique<EntryBubble>(owner, std::move(live));
        auto* entry  = content.get();
        juce::CallOutBox::launchAsynchronously(std::move(content), screenArea, nullptr);
        entry->grabFocus();
    }

    /** Same, pointing at the owner itself (a widget). */
    inline void openEntry(juce::Component& owner, Fields fields)
    {
        openEntry(owner, owner.getScreenBounds(), std::move(fields));
    }

    /** The long press of a canvas handle: one line per bound parameter. */
    inline void openEntry(juce::Component& owner, juce::Rectangle<int> screenArea,
                          const BoundList& bounds)
    {
        openEntry(owner, screenArea, fieldsOf(bounds));
    }
} // namespace Sp3ctraGestures
