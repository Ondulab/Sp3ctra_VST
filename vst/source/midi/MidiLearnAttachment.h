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
 *   - "Assign to CC · ch 1" ▸           the same binding WITHOUT the hardware:
 *                                          the channel, the controllers already
 *                                          received (each with what it drives),
 *                                          then the 128 numbered CCs in banks of
 *                                          sixteen. Learn answers "which knob is
 *                                          this?", the list answers "which CC do
 *                                          I want?" — the two questions every DAW
 *                                          puts side by side. Re-picking keeps the
 *                                          range, the law and the envelope.
 *   - "Edit MIDI mapping…"               when mapped — the MIDI CURVE window
 *                                          (input window / shape / hysteresis)
 *   - "Remove MIDI mapping (CC 21 · ch 1)" when mapped
 *   - "Modulate" ▸ LFO n… / New LFO       bind the control to an LFO in use,
 *                                          or bring a new one into use for it
 *                                          (midi/LfoBank.h). Same mapping
 *                                          table, same MIN/MAX window, same ⚙
 *                                          law — the LFO is just another
 *                                          source, so the entry sits in the
 *                                          same menu as MIDI Learn. The window
 *                                          opens centred on the control's
 *                                          current value, so arming a
 *                                          modulation never makes it jump.
 *
 * Pages with per-instance banks recreate this object when they rebind
 * (setInstance/setSlot/setEngineIndex) — exactly like their SliderAttachments,
 * so the popup always targets the SELECTED instance's bank.
 *
 * While a mapping exists, a small accent dot is painted in the top-right
 * corner of the control (non-intrusive confirmation). The dot is HOLLOW when
 * the controller also drives something else: sharing one CC across four
 * destinations is how a macro is built, so nothing forbids it — but a live
 * set is no place to discover it, and the menu names the co-owners.
 *
 * It is also THE wire that makes "being edited by MIDI" visible everywhere:
 * this object is the only one that knows (component, paramId) for every
 * mappable control of the interface, so when the editor drains the engine's
 * last-touched parameter (Sp3ctraControls::MidiTouch::note) the matching
 * attachment stamps its component's remote-edit heat and repaints it while
 * the glow lasts. Bars, toggles, combos and canvases then light through the
 * shared ladder (ui/Sp3ctraControls.h) exactly like a dragged handle.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MidiCcNames.h"
#include "MidiMappingEngine.h"
#include "../ui/Sp3ctraControls.h"
#include <utility>
#include <vector>

//==============================================================================
/** Shared right-click menu — used by MidiLearnAttachment and by canvas
 *  editors that resolve the parameter from the click position themselves
 *  (e.g. the EQ picks the nearest band node). */
namespace MidiLearnPopup
{
    /** How many "already received" controllers the list offers before falling
     *  back to the numbered banks. A rig sends a handful; a badly-behaved one
     *  sends dozens and must not push the banks off the screen. */
    inline constexpr int kMaxSeen = 24;

    /** Item ids of one parameter's learn entries (offset by `base` so a menu
     *  can host several parameters — one sub-menu each, see addItems). */
    enum Item { kCancelLearn = 1, kLearn = 2, kRemove = 3, kEdit = 4,
                kLfoFirst  = 5,                                ///< + LFO index
                kChanFirst = kLfoFirst + LfoBank::kNumLfos,    ///< + channel - 1
                kSeenFirst = kChanFirst + 16,                  ///< + rank in seenControllers()
                kCcFirst   = kSeenFirst + kMaxSeen,            ///< + CC number
                kInfo      = kCcFirst + 128,                   ///< disabled: co-owners
                kLfoNew    = kInfo + 1,                        ///< "New LFO…" (LfoBank slot)
                kItemCount = kLfoNew + 1 };

    /** Id base of the `index`-th parameter block in a menu that hosts several.
     *  Deliberately far above any host menu's own items: a page can add a
     *  fifth "Amount" step without silently hijacking a learn entry. */
    inline constexpr int subMenuBase(int index) noexcept
    { return 1000 + index * kItemCount; }

    /** The channel a LIST assignment targets, sticky for the session (0 = let
     *  the engine decide from what it has heard). A rig usually speaks on one
     *  channel; picking it again for every knob would be a tax. */
    inline int& stickyChannel() { static int ch = 0; return ch; }

    /** Which channel this parameter's list entries assign on: the choice the
     *  user made, else the channel it is already mapped on, else the one the
     *  last controller came in on, else 1. */
    inline int targetChannel(const MidiMappingEngine& engine, const juce::String& paramId)
    {
        if (const int sticky = stickyChannel(); sticky >= 1 && sticky <= 16)
            return sticky;
        int t = 0, c = 0, n = 0;
        if (engine.getMappingFor(paramId, t, c, n) && t != MidiMappingEngine::kTypeLfo
            && c >= 1 && c <= 16)
            return c;
        const int last = engine.lastCcChannel();
        return last >= 1 ? last : 1;
    }

    /** The controllers the rig has actually sent, (channel, cc) ascending —
     *  THE order addItems() shows and handle() decodes. Both walk this list,
     *  so the id of an entry is its rank in it. */
    inline std::vector<std::pair<int, int>> seenControllers(const MidiMappingEngine& engine)
    {
        std::vector<std::pair<int, int>> out;
        for (int ch = 1; ch <= 16 && (int) out.size() < kMaxSeen; ++ch)
            for (int cc = 0; cc < 128 && (int) out.size() < kMaxSeen; ++cc)
                if (engine.ccSeen(ch, cc))
                    out.emplace_back(ch, cc);
        return out;
    }

    /** " → 2 LUX · Gain" — what a controller already drives, or empty. The
     *  parameter the menu belongs to is skipped: its own entry is ticked. */
    inline juce::String ownerSuffix(const MidiMappingEngine& engine, const juce::String& paramId,
                                    int channel, int cc)
    {
        juce::StringArray owners = engine.paramsForEvent(MidiMappingEngine::kTypeCC, channel, cc);
        owners.removeString(paramId);
        if (owners.isEmpty())
            return {};
        juce::String s = juce::String::fromUTF8("   \xE2\x86\x92 ") + engine.targetLabel(owners[0]);
        if (owners.size() > 1)
            s << " +" << juce::String(owners.size() - 1);
        return s;
    }

    /** One CC entry: number, standard name, what it already drives, and a tick
     *  when it is this parameter's current mapping. Taken controllers stay
     *  ENABLED — one knob driving four destinations is a macro, not a
     *  mistake; the suffix is there so it is never an accident. */
    inline void addCcItem(juce::PopupMenu& menu, const MidiMappingEngine& engine,
                          const juce::String& paramId, int itemId, int channel, int cc,
                          bool withChannel)
    {
        int t = 0, c = 0, n = 0;
        const bool isMine = engine.getMappingFor(paramId, t, c, n)
                         && t == MidiMappingEngine::kTypeCC && c == channel && n == cc;
        juce::String text = MidiCcNames::label(cc);
        if (withChannel)
            text << juce::String::fromUTF8(" \xC2\xB7 ch ") << juce::String(channel);
        text << ownerSuffix(engine, paramId, channel, cc);
        menu.addItem(itemId, text, true, isMine);
    }

    /** The "Assign to" sub-menu: the same list every DAW puts next to its
     *  learn — pick the controller by number when the hardware is not at hand,
     *  is not yours, or sends nothing until the show starts. */
    inline void addAssignMenu(juce::PopupMenu& menu, const MidiMappingEngine& engine,
                              const juce::String& paramId, int base)
    {
        const int ch = targetChannel(engine, paramId);
        juce::PopupMenu assign;

        // Channel first — everything below assigns on it. Choosing one closes
        // the menu (a JUCE menu always does); the single-parameter path
        // re-opens it so the pick that follows is one gesture away.
        juce::PopupMenu chans;
        for (int c = 1; c <= 16; ++c)
            chans.addItem(base + kChanFirst + c - 1, "Channel " + juce::String(c), true, c == ch);
        assign.addSubMenu("Channel " + juce::String(ch), chans);

        // What the rig has actually sent since launch, with its own channel:
        // for a controller that has been moved once, this is the whole list.
        const auto seen = seenControllers(engine);
        if (! seen.empty())
        {
            assign.addSeparator();
            assign.addSectionHeader("Received");
            for (int i = 0; i < (int) seen.size(); ++i)
                addCcItem(assign, engine, paramId, base + kSeenFirst + i,
                          seen[(size_t) i].first, seen[(size_t) i].second,
                          /* withChannel */ true);
        }

        // The full 128, eight banks of sixteen — the fallback that always works.
        assign.addSeparator();
        for (int b = 0; b < 8; ++b)
        {
            juce::PopupMenu bank;
            for (int i = 0; i < 16; ++i)
            {
                const int cc = b * 16 + i;
                addCcItem(bank, engine, paramId, base + kCcFirst + cc, ch, cc,
                          /* withChannel */ false);
            }
            assign.addSubMenu("CC " + juce::String(b * 16)
                                  + juce::String::fromUTF8("\xE2\x80\x93")
                                  + juce::String(b * 16 + 15), bank);
        }

        // The channel rides in the title: it is what every entry below binds
        // on, and a menu that hides it invites the "why does nothing move?"
        // half-hour.
        menu.addSubMenu("Assign to CC" + juce::String::fromUTF8(" \xC2\xB7 ch ")
                            + juce::String(ch), assign);
    }

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
        // …and the same binding without the hardware: the numbered list.
        addAssignMenu(menu, engine, paramId, base);
        // Modulate ▸ LFO n — the LFO already driving this parameter is ticked.
        int curType = 0, curCh = 0, curNum = 0;
        const bool onLfo = engine.getMappingFor(paramId, curType, curCh, curNum)
                        && curType == MidiMappingEngine::kTypeLfo;
        // Only the LFOs the session USES are offered — plus "New LFO", which
        // brings one into use already driving this control: the way a bank
        // grows is by needing a modulation, not by visiting a settings page.
        juce::PopupMenu lfos;
        const auto* bank = engine.lfoSource();
        int listed = 0;
        for (int i = 0; i < LfoBank::kNumLfos; ++i)
        {
            if (bank == nullptr || ! bank->lfoExists(i)) continue;
            lfos.addItem(base + kLfoFirst + i, "LFO " + juce::String(i + 1),
                         true, onLfo && curNum == i);
            ++listed;
        }
        if (listed > 0) lfos.addSeparator();
        lfos.addItem(base + kLfoNew, juce::String::fromUTF8("New LFO\xE2\x80\xA6"),
                     listed < LfoBank::kNumLfos);
        menu.addSubMenu("Modulate", lfos);

        if (mapped.isNotEmpty())
        {
            menu.addItem(base + kEdit,
                         juce::String::fromUTF8("Edit MIDI mapping\xE2\x80\xA6"));
            menu.addItem(base + kRemove,
                         (onLfo ? "Remove modulation (" : "Remove MIDI mapping (")
                             + mapped + ")");
            // A shared controller is legitimate (that is how a macro is made)
            // but it must never be a discovery: name what else it moves.
            const juce::StringArray others = engine.eventSharedWith(paramId);
            if (! others.isEmpty())
            {
                juce::String also = mapped + " also drives: " + engine.targetLabel(others[0]);
                if (others.size() > 1)
                    also << " +" << juce::String(others.size() - 1);
                menu.addItem(base + kInfo, also, false, false);   // information only
            }
        }
    }

    /** True when `choice` is one of `base`'s channel picks — the single
     *  parameter path re-opens the menu after those, so the controller can be
     *  picked straight away. */
    inline bool isChannelChoice(int choice, int base = 0) noexcept
    {
        const int item = choice - base;
        return item >= kChanFirst && item < kChanFirst + 16;
    }

    /** Act on a chosen id from addItems(base). Returns false when `choice` is
     *  not one of this parameter's entries (dismissed, or another base). */
    inline bool handle(MidiMappingEngine& engine, const juce::String& paramId,
                       int choice, int base = 0)
    {
        const int item = choice - base;
        switch (item)
        {
            case kCancelLearn: engine.cancelLearn();             return true;
            case kLearn:       engine.startLearn(paramId);       return true;
            case kRemove:      engine.removeMappingFor(paramId); return true;
            case kEdit:        engine.requestEdit(paramId);      return true;
            default: break;
        }
        if (item >= kLfoFirst && item < kLfoFirst + LfoBank::kNumLfos)
        {
            engine.addLfoMapping(item - kLfoFirst, paramId);
            return true;
        }
        if (item == kLfoNew)
        {
            engine.addLfoMapping(MidiMappingEngine::kNewLfo, paramId);
            return true;
        }
        if (item >= kChanFirst && item < kChanFirst + 16)
        {
            stickyChannel() = item - kChanFirst + 1;
            return true;
        }
        if (item >= kSeenFirst && item < kSeenFirst + kMaxSeen)
        {
            // Same walk as addItems: the entry's rank IS its id.
            const auto seen = seenControllers(engine);
            const int rank = item - kSeenFirst;
            if (rank < (int) seen.size())
                engine.assignEvent(MidiMappingEngine::kTypeCC, seen[(size_t) rank].first,
                                   seen[(size_t) rank].second, paramId);
            return true;
        }
        if (item >= kCcFirst && item < kCcFirst + 128)
        {
            engine.assignEvent(MidiMappingEngine::kTypeCC, targetChannel(engine, paramId),
                               item - kCcFirst, paramId);
            return true;
        }
        return false;
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
        const juce::Component::SafePointer<juce::Component> safe { target };
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(target)
                                      .withMousePosition(),
            [&engine, paramId, safe](int choice)
            {
                handle(engine, paramId, choice);
                // A channel pick is only half a gesture: bring the list back so
                // the controller itself can be chosen without a second click.
                if (isChannelChoice(choice) && safe != nullptr)
                    juce::MessageManager::callAsync(
                        [&engine, paramId, safe]
                        { if (safe != nullptr) show(engine, paramId, safe.getComponent()); });
            });
    }
}

//==============================================================================
class MidiLearnAttachment : private juce::MouseListener,
                            private juce::ComponentListener,
                            private juce::ChangeListener,
                            private juce::Timer,
                            private Sp3ctraControls::MidiTouch::Sink
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
        Sp3ctraControls::MidiTouch::add(this);    // "a CC just moved you"
        refreshBadge();
    }

    ~MidiLearnAttachment() override
    {
        Sp3ctraControls::MidiTouch::remove(this);
        engine.removeChangeListener(this);
        component.removeMouseListener(this);
        component.removeComponentListener(this);
        badge_.setVisible(false);
    }

private:
    //==========================================================================
    // A controller moved this parameter → light the control it belongs to,
    // and keep it repainting until the glow has died out.
    //==========================================================================
    void midiTouched(const juce::String& touchedId) override
    {
        if (touchedId != paramId) return;
        Sp3ctraControls::touch(component);
        component.repaint();
        if (! isTimerRunning()) startTimerHz(30);
    }

    void timerCallback() override
    {
        component.repaint();
        if (Sp3ctraControls::heatOf(component) <= 0.0f)
            stopTimer();
    }

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
        bool shared { false };   ///< the same controller drives other parameters
        void paint(juce::Graphics& g) override
        {
            g.setColour(juce::Colour(0xffe0a24a).withAlpha(0.9f));
            const auto r = getLocalBounds().toFloat().reduced(1.0f);
            if (shared) g.drawEllipse(r.reduced(0.6f), 1.2f);   // a ring: shared CC
            else        g.fillEllipse(r);
        }
    };

    void refreshBadge()
    {
        int t, c, n;
        const bool mapped = engine.getMappingFor(paramId, t, c, n);
        if (mapped && badge_.getParentComponent() != &component)
            component.addAndMakeVisible(badge_);
        badge_.shared = mapped && ! engine.eventSharedWith(paramId).isEmpty();
        badge_.setVisible(mapped);
        badge_.repaint();   // solo → shared changes the dot, not its visibility
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
