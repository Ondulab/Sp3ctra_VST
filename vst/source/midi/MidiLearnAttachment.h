/**
 * @file MidiLearnAttachment.h
 * @brief Right-click "MIDI Learn" popup for any parameter-bound control.
 *
 * Drop one next to each Slider/Button/ComboBox attachment:
 *
 *   learnAtt_ = std::make_unique<MidiLearnAttachment>(
 *       processor.getMidiMap(), someSlider, paramId);
 *
 * Right-click on the control then shows:
 *   - "MIDI Learn"                        arm the capture for this parameter
 *   - "Learning… (cancel)"                while armed for this parameter
 *   - "Remove MIDI mapping (CC 21 · ch 1)" when mapped
 *
 * Pages with per-instance banks recreate this object when they rebind
 * (setInstance/setSlot/setEngineIndex) — exactly like their SliderAttachments,
 * so the popup always targets the SELECTED instance's bank.
 *
 * While a mapping exists, a small accent dot is painted in the top-right
 * corner of the control (non-intrusive confirmation).
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MidiMappingEngine.h"

//==============================================================================
/** Shared right-click menu — used by MidiLearnAttachment and by canvas
 *  editors that resolve the parameter from the click position themselves
 *  (e.g. the EQ picks the nearest band node). */
namespace MidiLearnPopup
{
    /** Item ids of one parameter's learn entries (offset by `base` so a menu
     *  can host several parameters — one sub-menu each, see addItems). */
    enum Item { kCancelLearn = 1, kLearn = 2, kRemove = 3, kItemCount = 4 };

    /** Append the learn entries of `paramId` to `menu`, ids `base + Item`. */
    inline void addItems(juce::PopupMenu& menu, MidiMappingEngine& engine,
                         const juce::String& paramId, int base = 0)
    {
        const juce::String mapped = engine.mappingDescription(paramId);
        const bool learningThis = engine.isLearning()
                               && engine.learningParamId() == paramId;

        if (learningThis)
            menu.addItem(base + kCancelLearn,
                         juce::String::fromUTF8("Learning\xE2\x80\xA6 (cancel)"), true, true);
        else
            menu.addItem(base + kLearn, "MIDI Learn");
        if (mapped.isNotEmpty())
            menu.addItem(base + kRemove, "Remove MIDI mapping (" + mapped + ")");
    }

    /** Act on a chosen id from addItems(base). Returns false when `choice` is
     *  not one of this parameter's entries (dismissed, or another base). */
    inline bool handle(MidiMappingEngine& engine, const juce::String& paramId,
                       int choice, int base = 0)
    {
        switch (choice - base)
        {
            case kCancelLearn: engine.cancelLearn();             return true;
            case kLearn:       engine.startLearn(paramId);       return true;
            case kRemove:      engine.removeMappingFor(paramId); return true;
            default:           return false;
        }
    }

    inline void show(MidiMappingEngine& engine, const juce::String& paramId,
                     juce::Component* target)
    {
        juce::PopupMenu menu;
        addItems(menu, engine, paramId);

        // withMousePosition() AFTER withTargetComponent() so the popup opens at
        // the cursor. Canvas editors (EQ, envelope) pass the whole component as
        // target; without this the menu would anchor to the component's top-left
        // corner instead of where the user right-clicked.
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(target)
                                      .withMousePosition(),
            [&engine, paramId](int choice) { handle(engine, paramId, choice); });
    }
}

//==============================================================================
class MidiLearnAttachment : private juce::MouseListener,
                            private juce::ComponentListener,
                            private juce::ChangeListener
{
public:
    MidiLearnAttachment(MidiMappingEngine& engineIn,
                        juce::Component& componentIn,
                        juce::String paramIdIn)
        : engine(engineIn), component(componentIn), paramId(std::move(paramIdIn))
    {
        component.addMouseListener(this, true);   // true: reach child widgets
        component.addComponentListener(this);
        engine.addChangeListener(this);           // learn lands asynchronously
        refreshBadge();
    }

    ~MidiLearnAttachment() override
    {
        engine.removeChangeListener(this);
        component.removeMouseListener(this);
        component.removeComponentListener(this);
        badge_.setVisible(false);
    }

private:
    //==========================================================================
    void mouseUp(const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            return;
        MidiLearnPopup::show(engine, paramId, &component);
        // The badge refreshes through the engine's ChangeBroadcaster once the
        // menu action lands (add/remove/learn completion).
    }

    //==========================================================================
    // Mapped badge — a tiny accent dot overlaid on the control's corner.
    //==========================================================================
    struct Badge : juce::Component
    {
        Badge() { setInterceptsMouseClicks(false, false); }
        void paint(juce::Graphics& g) override
        {
            g.setColour(juce::Colour(0xffe0a24a).withAlpha(0.9f));
            g.fillEllipse(getLocalBounds().toFloat().reduced(1.0f));
        }
    };

    void refreshBadge()
    {
        int t, c, n;
        const bool mapped = engine.getMappingFor(paramId, t, c, n);
        if (mapped && badge_.getParentComponent() != &component)
            component.addAndMakeVisible(badge_);
        badge_.setVisible(mapped);
        layoutBadge();
    }

    void layoutBadge()
    {
        constexpr int d = 7;
        badge_.setBounds(component.getWidth() - d - 1, 1, d, d);
    }

    void componentMovedOrResized(juce::Component&, bool, bool) override
    { layoutBadge(); }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    { refreshBadge(); }

    //==========================================================================
    MidiMappingEngine& engine;
    juce::Component&   component;
    juce::String       paramId;
    Badge              badge_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiLearnAttachment)
};
