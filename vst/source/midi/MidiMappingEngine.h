/**
 * @file MidiMappingEngine.h
 * @brief Generic MIDI CC/Note → APVTS parameter mapping with MIDI-learn.
 *
 * Lets the user bind ANY play parameter to a hardware controller knob/button.
 * Mappings reference parameter IDs, so per-instance banks (luxpitch{N}_*,
 * luxreverb{N}_*, videoScroll{N}_*, luxSamplerB*…) keep the binding attached
 * to ONE module instance regardless of which chain hosts it.
 *
 * Threading contract:
 *   - processMidi()  : audio thread. Lock-free — fixed slot table read through
 *                      atomics, no allocation, no locks, no logging. Values are
 *                      applied via setValueNotifyingHost() (same path as host
 *                      automation; our parameterChanged handlers are RT-safe).
 *   - everything else: message thread only (add/remove/learn/persist).
 *
 * Slot lifecycle (message thread writes, audio thread reads):
 *   install : write type/channel/number first, then param (release store) —
 *             the audio thread only matches a slot once param is non-null.
 *   remove  : param = nullptr (release store); stale fields are harmless.
 *
 * Event semantics (v1):
 *   - CC   on continuous param : absolute — value/127 → normalized range.
 *   - CC   on 2-step param     : >= 64 → on, < 64 → off (momentary).
 *   - CC   on discrete param   : absolute — the CC sweeps the choice list
 *                                (JUCE quantizes the normalized value).
 *   - Note on 2-step param     : toggle on NoteOn (velocity > 0).
 *   - Note on discrete param   : CYCLE to the next choice on NoteOn (wraps) —
 *                                a pad steps through a mode list. "Discrete"
 *                                = 3..32 steps (ComboBox-sized).
 *   - Note on continuous param : velocity/127 on NoteOn.
 *   NoteOffs never match (toggle-on-press semantics).
 *
 * MIDI Learn: startLearn(paramId) arms an atomic capture; the audio thread
 * stores the first NoteOn/CC event (any channel); a message-thread timer
 * (this class) polls the result and installs the mapping, replacing any
 * previous mapping of the same parameter.
 *
 * Persistence: toValueTree() / restoreFromValueTree() — a <MIDI_MAPPINGS>
 * child of apvts.state, saved next to <CHAINS> (see getStateInformation).
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cmath>

//==============================================================================
/** Sink for NON-APVTS "virtual" mapping targets — parameters/actions that live
 *  outside the APVTS (e.g. the LuxSampler per-slot play params, which are stored
 *  directly in the engine, not as host parameters).
 *
 *  A virtual target is addressed by an opaque integer @c targetId that the sink
 *  owner encodes/decodes (the engine never interprets it). @c virtualResolve maps
 *  a synthetic paramId string (e.g. "smp:e0:s3:speed") to a targetId on the
 *  message thread; every other call runs on the AUDIO thread and MUST be RT-safe
 *  (atomic reads/writes only — no allocation, no locking, no I/O).
 *
 *  @c virtualSteps encodes the target's kind, mirroring RangedAudioParameter's
 *  getNumSteps() for value targets and adding two negative codes for actions:
 *      >= 1 : value target (1/0 = continuous, 2 = 2-state, 3..32 = discrete list)
 *      -1   : momentary action (press on NoteOn / CC>=64, release on NoteOff/CC<64)
 *      -2   : one-shot action  (fires on press only)
 */
struct IVirtualMidiSink
{
    virtual ~IVirtualMidiSink() = default;

    /** Message thread: synthetic paramId → targetId (>= 0), or -1 if not ours. */
    virtual int   virtualResolve(const juce::String& paramId) const = 0;

    /** Audio thread: step/kind code for @p targetId (see class doc). */
    virtual int   virtualSteps  (int targetId) const noexcept = 0;
    /** Audio thread: current value normalised to 0..1 (for toggle / cycle). */
    virtual float virtualRead   (int targetId) const noexcept = 0;
    /** Audio thread: apply a normalised value, or trigger an action "press". */
    virtual void  virtualApply  (int targetId, float norm01) noexcept = 0;
    /** Audio thread: action "release" (momentary targets only). */
    virtual void  virtualRelease(int targetId) noexcept = 0;
};

class MidiMappingEngine : public juce::ChangeBroadcaster,
                          private juce::Timer
{
public:
    static constexpr int kMaxMappings = 128;

    explicit MidiMappingEngine(juce::AudioProcessorValueTreeState& apvtsIn)
        : apvts(apvtsIn) {}

    ~MidiMappingEngine() override { stopTimer(); }

    /** Register the sink for NON-APVTS "virtual" targets (sampler play params /
     *  action buttons). Call once at construction, before any restore. The sink
     *  must outlive the engine. */
    void setVirtualSink(IVirtualMidiSink* sink) noexcept { sink_ = sink; }

    //==========================================================================
    // Audio thread
    //==========================================================================
    void processMidi(const juce::MidiBuffer& midi) noexcept
    {
        for (const auto metadata : midi)
        {
            const auto msg = metadata.getMessage();

            int type = 0, number = 0;   // type: 1 = Note, 2 = CC
            bool isNoteOff = false;     // routed to momentary VIRTUAL action targets
            if (msg.isController())                          { type = 2; number = msg.getControllerNumber(); }
            else if (msg.isNoteOn() && msg.getVelocity() > 0){ type = 1; number = msg.getNoteNumber(); }
            else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0))
                                                             { type = 1; number = msg.getNoteNumber(); isNoteOff = true; }
            else continue;   // other messages never match

            // MIDI Learn capture — first matching PRESS event on ANY channel wins.
            // Note-offs never arm a mapping (toggle-on-press semantics).
            if (! isNoteOff
                && learnArmed_.load(std::memory_order_acquire)
                && learnResult_.load(std::memory_order_relaxed) == kNoResult)
            {
                learnResult_.store(encodeEvent(type, msg.getChannel(), number),
                                   std::memory_order_release);
                learnArmed_.store(false, std::memory_order_release);
                continue;   // don't also apply the learning event
            }

            for (auto& s : slots_)
            {
                auto* p        = s.param  .load(std::memory_order_acquire);
                const int vt   = s.vtarget.load(std::memory_order_acquire);
                if (p == nullptr && vt < 0)                             continue;
                if (s.type   .load(std::memory_order_relaxed) != type)  continue;
                if (s.number .load(std::memory_order_relaxed) != number)continue;
                if (s.channel.load(std::memory_order_relaxed) != msg.getChannel())
                    continue;

                if (p != nullptr)
                {
                    if (isNoteOff) continue;   // APVTS params ignore note-offs (v1)
                    applyEvent(*p, msg);
                }
                else
                {
                    applyVirtual(vt, msg, isNoteOff);
                }
                // Publish which slot a controller just moved so the editor can
                // auto-navigate to the owning module (MIDI-follow). Lock-free:
                // slot index + a monotonic generation; the paramId is read on
                // the message thread. Played notes never reach here (they go to
                // the samplers), so only *parameter* moves flag navigation.
                touchedSlot_.store((int) (&s - slots_), std::memory_order_relaxed);
                touchGen_   .fetch_add(1u, std::memory_order_release);
            }
        }
    }

    //==========================================================================
    // Message thread — mapping edits
    //==========================================================================
    /** Install (or replace) the mapping of one parameter. Returns false when
     *  the parameter id is unknown or the table is full. */
    bool addMapping(int type, int channel, int number, const juce::String& paramId);

    /** Remove the mapping of one parameter (no-op when unmapped). */
    void removeMappingFor(const juce::String& paramId);

    /** Release EVERY mapping (header MIDI menu "clear all"). Message thread. */
    void clearAll();

    /** Number of occupied mapping slots. Message thread (paramId mirror). */
    int numMappings() const noexcept
    {
        int n = 0;
        for (const auto& s : slots_)
            if (s.paramId.isNotEmpty()) ++n;
        return n;
    }

    /** True + fills the out-params when the parameter is mapped. */
    bool getMappingFor(const juce::String& paramId,
                       int& type, int& channel, int& number) const;

    /** Human label for a mapped parameter ("CC 21 · ch 1"), empty if unmapped. */
    juce::String mappingDescription(const juce::String& paramId) const;

    //==========================================================================
    // Message thread — MIDI learn
    //==========================================================================
    void startLearn(const juce::String& paramId)
    {
        learnParamId_ = paramId;
        learnResult_.store(kNoResult, std::memory_order_relaxed);
        learnArmed_ .store(true, std::memory_order_release);
        startTimer(30);   // poll the capture until it lands or learn is cancelled
    }

    void cancelLearn()
    {
        learnArmed_ .store(false, std::memory_order_release);
        learnResult_.store(kNoResult, std::memory_order_relaxed);
        learnParamId_.clear();
        stopTimer();
    }

    bool isLearning() const noexcept
    {
        return learnArmed_.load(std::memory_order_acquire)
            || learnResult_.load(std::memory_order_relaxed) != kNoResult;
    }

    const juce::String& learningParamId() const noexcept { return learnParamId_; }

    //==========================================================================
    // Message thread — MIDI-follow (auto-navigate to the touched module)
    //==========================================================================
    /** True + fills paramId when a MIDI controller moved a mapped parameter
     *  since the previous call (edge-triggered via a generation counter). Used
     *  by the editor to jump to the module that owns the parameter. Consuming
     *  advances the baseline, so a single move fires exactly one navigation. */
    bool takeLastTouchedParam(juce::String& outParamId) noexcept
    {
        const uint32_t gen = touchGen_.load(std::memory_order_acquire);
        if (gen == lastSeenTouchGen_)
            return false;
        lastSeenTouchGen_ = gen;
        const int idx = touchedSlot_.load(std::memory_order_relaxed);
        if (idx < 0 || idx >= kMaxMappings)
            return false;
        // A live slot has exactly one active field: an APVTS param, OR a virtual
        // sampler target (vtarget >= 0). Removal clears both — so reject only when
        // neither is set. Virtual sampler targets (REC/PLAY/value params) then
        // navigate to the sampler page like any mapped parameter.
        if (slots_[idx].param  .load(std::memory_order_relaxed) == nullptr
            && slots_[idx].vtarget.load(std::memory_order_relaxed) < 0)
            return false;   // the slot was removed since the event
        outParamId = slots_[idx].paramId;   // message-thread-only mirror — safe here
        return outParamId.isNotEmpty();
    }

    /** Sync the follow baseline to "now" WITHOUT navigating — call when an
     *  editor opens so a move that happened while it was closed doesn't jump. */
    void resetTouchBaseline() noexcept
    { lastSeenTouchGen_ = touchGen_.load(std::memory_order_acquire); }

    // Mapping changes (learn completion, add/remove, session restore) are
    // broadcast as ChangeMessages — MidiLearnAttachment badges listen.

    //==========================================================================
    // Message thread — persistence
    //==========================================================================
    juce::ValueTree toValueTree() const;
    void restoreFromValueTree(const juce::ValueTree& tree);

private:
    //==========================================================================
    static constexpr int kNoResult = -1;

    static int encodeEvent(int type, int channel, int number) noexcept
    { return (type << 16) | ((channel & 0xFF) << 8) | (number & 0x7F); }

    static void applyEvent(juce::RangedAudioParameter& p,
                           const juce::MidiMessage& msg) noexcept
    {
        const int  steps        = p.getNumSteps();
        const bool boolLike     = (steps == 2);
        const bool discreteLike = (steps > 2 && steps <= 32);   // ComboBox-sized

        if (msg.isController())
        {
            const int v = msg.getControllerValue();
            p.setValueNotifyingHost(boolLike ? (v >= 64 ? 1.0f : 0.0f)
                                             : (float) v / 127.0f);
        }
        else // NoteOn (velocity > 0 — filtered by the caller)
        {
            if (boolLike)
                p.setValueNotifyingHost(p.getValue() >= 0.5f ? 0.0f : 1.0f);
            else if (discreteLike)
            {
                // Cycle to the next choice (wraps) — a pad steps the list.
                const int cur  = (int) std::lround(p.getValue() * (float) (steps - 1));
                const int next = (cur + 1) % steps;
                p.setValueNotifyingHost((float) next / (float) (steps - 1));
            }
            else
                p.setValueNotifyingHost((float) msg.getVelocity() / 127.0f);
        }
    }

    /** Apply one MIDI event to a VIRTUAL target through the sink. Mirrors
     *  applyEvent's value semantics (CC absolute / Note toggle-or-cycle) and
     *  adds press/release handling for action targets (steps < 0). RT-safe. */
    void applyVirtual(int targetId, const juce::MidiMessage& msg, bool isNoteOff) noexcept
    {
        if (sink_ == nullptr) return;
        const int steps = sink_->virtualSteps(targetId);

        if (steps < 0)   // action: -1 = momentary (press/release), -2 = one-shot
        {
            if (msg.isController())
            {
                if (msg.getControllerValue() >= 64) sink_->virtualApply(targetId, 1.0f);
                else if (steps == -1)               sink_->virtualRelease(targetId);
            }
            else if (isNoteOff)
            {
                if (steps == -1) sink_->virtualRelease(targetId);
            }
            else
                sink_->virtualApply(targetId, 1.0f);   // NoteOn press
            return;
        }

        if (isNoteOff) return;   // value targets ignore note-offs

        const bool boolLike     = (steps == 2);
        const bool discreteLike = (steps > 2 && steps <= 32);   // ComboBox-sized

        if (msg.isController())
        {
            const int v = msg.getControllerValue();
            sink_->virtualApply(targetId, boolLike ? (v >= 64 ? 1.0f : 0.0f)
                                                   : (float) v / 127.0f);
        }
        else // NoteOn (velocity > 0 — filtered by the caller)
        {
            if (boolLike)
            {
                const float cur = sink_->virtualRead(targetId);
                sink_->virtualApply(targetId, cur >= 0.5f ? 0.0f : 1.0f);
            }
            else if (discreteLike)
            {
                const int cur  = (int) std::lround(sink_->virtualRead(targetId) * (float) (steps - 1));
                const int next = (cur + 1) % steps;
                sink_->virtualApply(targetId, (float) next / (float) (steps - 1));
            }
            else
                sink_->virtualApply(targetId, (float) msg.getVelocity() / 127.0f);
        }
    }

    void timerCallback() override;   // finishes a pending learn

    struct Slot
    {
        // Audio-thread-visible fields (see slot lifecycle in the header doc).
        // Exactly one of {param, vtarget} is active at a time: param for APVTS
        // targets, vtarget (>= 0) for virtual (sampler play-param) targets.
        std::atomic<juce::RangedAudioParameter*> param   { nullptr };
        std::atomic<int>                         vtarget { -1 };
        std::atomic<int> type    { 0 };
        std::atomic<int> channel { 0 };
        std::atomic<int> number  { 0 };
        // Message-thread-only mirror used for lookups/persistence. Non-empty
        // iff the slot is occupied (either kind).
        juce::String paramId;
    };

    int  slotIndexFor(const juce::String& paramId) const;   // -1 when unmapped
    void notifyChanged()
    {
        sendChangeMessage();
        if (onMappingsEdited) onMappingsEdited();
    }

public:
    /** Fired on every mapping table mutation (learn, remove, restore) — the
     *  processor hooks the session autosave here (markStateDirty). */
    std::function<void()> onMappingsEdited;

private:

    juce::AudioProcessorValueTreeState& apvts;
    IVirtualMidiSink* sink_ = nullptr;   // NON-APVTS targets (set once at ctor)
    Slot slots_[kMaxMappings];

    // MIDI learn state — armed/result cross threads, paramId message-only.
    std::atomic<bool> learnArmed_  { false };
    std::atomic<int>  learnResult_ { kNoResult };
    juce::String      learnParamId_;

    // MIDI-follow state — audio thread publishes (slot + generation), the
    // editor's poll consumes on the message thread (lastSeenTouchGen_).
    std::atomic<int>      touchedSlot_ { -1 };
    std::atomic<uint32_t> touchGen_    { 0 };
    uint32_t              lastSeenTouchGen_ { 0 };   // message-thread only

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiMappingEngine)
};
