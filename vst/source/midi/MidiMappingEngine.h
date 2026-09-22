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
 * Transfer law (MIDI CURVE window, MidiMappingCurve.h): every continuous
 * sweep runs through the slot's pipeline — input window → hysteresis →
 * baked shape LUT → output range (Slot::sweep). The message thread bakes
 * the curve into a double-buffered atomic LUT and flips an index; the audio
 * thread only ever reads. The slot also publishes the last (in, out) pair
 * for the window's live dot.
 *
 * Envelope (MIDI CURVE window, MidiMappingEnvelope.h): with a trigger set,
 * the mapped event no longer SETS the parameter — a press starts the
 * slot's envelope (attack → decay → sustain), a release ends it (release),
 * and tick() — called once per audio block after processMidi — runs every
 * live envelope and applies its level on OUT MIN..MAX through the same
 * setValueNotifyingHost / virtualApply paths. Block-rate modulation: what
 * a host's automation lane delivers. Toggle / cycle / action semantics are
 * bypassed while the envelope is on (the envelope IS the event's meaning).
 *
 * LFO source (midi/LfoBank.h): a slot whose type is kTypeLfo is not driven by
 * an event at all — its `number` names an LFO of the bank and tick() reads
 * that LFO's shape once per block, through the SAME Slot::sweep pipeline a CC
 * goes through. The mapping's MIN/MAX window therefore IS the modulation
 * depth, its ⚙ law shapes the modulation, and its `phase` offsets this
 * destination along the shared cycle (one LFO, four destinations in
 * quadrature). A stopped LFO writes nothing — it releases what it drove.
 *
 * The window FOLLOWS the hand. An LFO writes its destination every block, so
 * without this the parameter would be confiscated: turning the control would
 * be undone a millisecond later and the centre of the modulation could never
 * be aimed. So the slot remembers what it last wrote (read BACK from the
 * target, so a stepped parameter's snapping is not mistaken for a move) and,
 * when it finds the value somewhere else, concludes that someone else moved
 * it — a hand on the knob, a canvas editor, the host's automation — and
 * SLIDES its window under them, keeping its width and its phase offset. You
 * aim with the control itself; the two MIN/MAX knobs of the MIDI MAP row set
 * how far the modulation reaches around wherever you left it.
 *
 * Persistence: toValueTree() / restoreFromValueTree() — a <MIDI_MAPPINGS>
 * child of apvts.state, saved next to <CHAINS> (see getStateInformation).
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "LfoBank.h"
#include "MidiMappingCurve.h"
#include "MidiMappingEnvelope.h"
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <vector>

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

    /// Slot::type — 1 = Note, 2 = CC (a MIDI event), 3 = an LFO of the bank
    /// (no event at all: tick() plays it). Persisted as-is in <MAP type=…>.
    static constexpr int kTypeNote = 1, kTypeCC = 2, kTypeLfo = 3;

    /// Below this much movement (normalised) an LFO's block-rate write is
    /// skipped: every applied value costs a parameterChanged all the way
    /// down, and 1/10000 of a course is under what any destination resolves.
    static constexpr float kLfoDeadband = 1.0e-4f;

    /// How far the target must sit from what this slot last wrote before the
    /// move is credited to someone else (and the window slides to follow).
    /// Above the float noise of a normalised round-trip, far below anything
    /// a hand does.
    static constexpr float kLfoAdoptEps = 1.0e-5f;

    explicit MidiMappingEngine(juce::AudioProcessorValueTreeState& apvtsIn)
        : apvts(apvtsIn) {}

    ~MidiMappingEngine() override { stopTimer(); }

    /** Register the sink for NON-APVTS "virtual" targets (sampler play params /
     *  action buttons). Call once at construction, before any restore. The sink
     *  must outlive the engine. */
    void setVirtualSink(IVirtualMidiSink* sink) noexcept { sink_ = sink; }

    /** Register the LFO bank that kTypeLfo mappings read. Call once at
     *  construction, before any restore; the source must outlive the engine. */
    void setLfoSource(ILfoSource* lfos) noexcept { lfos_ = lfos; }
    ILfoSource* lfoSource() const noexcept { return lfos_; }

    //==========================================================================
    // Audio thread
    //==========================================================================
    void processMidi(const juce::MidiBuffer& midi) noexcept
    {
        for (const auto metadata : midi)
        {
            if (metadata.numBytes > 3) continue;
            const auto msg = metadata.getMessage();

            int type = 0, number = 0;   // type: 1 = Note, 2 = CC
            bool isNoteOff = false;     // routed to momentary VIRTUAL action targets
            if (msg.isController())                          { type = 2; number = msg.getControllerNumber(); }
            else if (msg.isNoteOn() && msg.getVelocity() > 0){ type = 1; number = msg.getNoteNumber(); }
            else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0))
                                                             { type = 1; number = msg.getNoteNumber(); isNoteOff = true; }
            else continue;   // other messages never match

            // Which controllers this rig actually sends. The right-click
            // "Assign to" list shows those first, so a knob can be picked by
            // its number without moving it — the answer MIDI Learn gives, for
            // the times the hardware is not at hand. Lock-free: one relaxed
            // fetch_or on a 128-bit-per-channel presence map.
            if (type == 2)
            {
                const int ch = juce::jlimit(1, 16, msg.getChannel());
                seenCc_[ch - 1][number >> 5]
                    .fetch_or(1u << (number & 31), std::memory_order_relaxed);
                lastCcChannel_.store(ch, std::memory_order_relaxed);
            }

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

                // An armed envelope owns the event: press = gate on (with
                // the press value for VEL), release = gate off; the level
                // is applied by tick(), not here. Action targets (sampler
                // REC/PLAY…) keep their press semantics — an envelope has
                // no meaning on a pulse.
                if (s.env.trigger.load(std::memory_order_acquire) != 0
                    && (p != nullptr || (sink_ != nullptr && sink_->virtualSteps(vt) >= 0)))
                {
                    if (msg.isController())
                    {
                        const int v = msg.getControllerValue();
                        if (v >= 64)
                        {
                            if (s.env.ccGate) continue;   // still pressed: no retrigger
                            s.env.ccGate = true;
                            s.env.gateOn((float) v / 127.0f);
                        }
                        else
                        {
                            if (s.env.ccGate) { s.env.ccGate = false; s.env.gateOff(); }
                            continue;   // a release is not a "move": no follow
                        }
                    }
                    else if (isNoteOff) { s.env.gateOff(); continue; }
                    else                  s.env.gateOn((float) msg.getVelocity() / 127.0f);
                }
                // Continuous sweeps run through the slot's transfer law
                // (input window → hysteresis → shape LUT → MIN/MAX output
                // range — Slot::sweep); toggle / cycle semantics ignore it.
                else if (p != nullptr)
                {
                    if (isNoteOff) continue;   // APVTS params ignore note-offs (v1)
                    applyEvent(*p, msg, s);
                }
                else
                {
                    applyVirtual(vt, msg, isNoteOff, s);
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

    /** Audio thread, once per block AFTER processMidi (both buffers): run
     *  every live envelope by numSamples and apply its level. Nothing to do
     *  — no armed envelope, or all at rest — costs one atomic read per
     *  slot. */
    void tick(int numSamples) noexcept
    {
        const double sr = sampleRate_.load(std::memory_order_relaxed);
        if (sr <= 0.0 || numSamples <= 0)
            return;
        const float dt = (float) ((double) numSamples / sr);

        for (auto& s : slots_)
        {
            // ── An LFO plays this slot: no event ever reaches it, the shape
            //    IS the input. Same sweep pipeline as a controller sweep, so
            //    the window / law / MIN-MAX of the row all apply.
            if (s.type.load(std::memory_order_relaxed) == kTypeLfo)
            {
                if (lfos_ == nullptr) continue;
                auto* p      = s.param  .load(std::memory_order_acquire);
                const int vt = s.vtarget.load(std::memory_order_acquire);
                if (p == nullptr && vt < 0) continue;

                const int idx = s.number.load(std::memory_order_relaxed);
                if (! lfos_->lfoRunning(idx))
                    continue;   // stopped: the parameter is released where it is

                // Where does the target actually stand? If it is not where we
                // left it, a hand (or the host) has aimed it: slide the window
                // under them instead of dragging the value back. The shift
                // keeps the CURRENT phase offset, so nothing jumps at the
                // moment of adoption — the next write lands exactly where the
                // user let go.
                // Only a CONTINUOUS target can be aimed this way: a toggle or
                // a choice list snaps, and its read-back would read as a hand
                // on every block — the window would walk off on its own.
                const int tsteps = (p != nullptr)
                                 ? p->getNumSteps()
                                 : (sink_ != nullptr ? sink_->virtualSteps(vt) : 0);
                const bool aimable = (tsteps == 0 || tsteps == 1 || tsteps > 32);

                const float cur = (p != nullptr) ? p->getValue()
                                : (sink_ != nullptr ? sink_->virtualRead(vt) : 0.0f);
                if (aimable && s.lastApplied >= 0.0f
                    && std::abs(cur - s.lastApplied) > kLfoAdoptEps)
                {
                    const float lo = s.lo.load(std::memory_order_relaxed);
                    const float hi = s.hi.load(std::memory_order_relaxed);
                    float shift = cur - s.lastApplied;
                    // Keep the whole window inside 0..1 — a modulation pushed
                    // against a bound narrows nothing, it just stops sliding.
                    const float lowest = juce::jmin(lo, hi), highest = juce::jmax(lo, hi);
                    shift = juce::jlimit(-lowest, 1.0f - highest, shift);
                    if (shift != 0.0f)
                    {
                        s.lo.store(lo + shift, std::memory_order_relaxed);
                        s.hi.store(hi + shift, std::memory_order_relaxed);
                        rangeShifted_.store(true, std::memory_order_release);
                    }
                }

                const float out = s.sweep(lfos_->lfoValue(
                                      idx, s.phase.load(std::memory_order_relaxed)));
                if (std::abs(out - s.lastApplied) < kLfoDeadband)
                    continue;   // sweep() still published the live pair for the VU
                if (p != nullptr)          p->setValueNotifyingHost(out);
                else if (sink_ != nullptr) sink_->virtualApply(vt, out);
                // Read BACK what the target now holds: a stepped parameter
                // snaps, and the difference must not read as a hand next block.
                s.lastApplied = (p != nullptr) ? p->getValue()
                              : (sink_ != nullptr ? sink_->virtualRead(vt) : out);
                continue;
            }

            if (s.env.trigger.load(std::memory_order_acquire) == 0)
            {
                // Switched off from the window while it was running: the
                // parameter stays where the envelope left it, the runner
                // goes quiet (audio side owns the state).
                if (s.env.stage != MidiMappingEnvelope::Stage::Idle) s.env.reset();
                continue;
            }
            if (s.env.stage == MidiMappingEnvelope::Stage::Idle)
                continue;

            auto* p      = s.param  .load(std::memory_order_acquire);
            const int vt = s.vtarget.load(std::memory_order_acquire);
            if (p == nullptr && vt < 0) { s.env.reset(); continue; }   // slot freed mid-flight
            if (! s.env.advance(dt))
                continue;

            const float l   = s.lo.load(std::memory_order_relaxed);
            const float h   = s.hi.load(std::memory_order_relaxed);
            const float out = l + (h - l) * s.env.level;
            s.lastIn .store(s.env.level, std::memory_order_relaxed);   // the row's VU (virtual targets)
            s.lastOut.store(out,         std::memory_order_relaxed);
            if (p != nullptr)            p->setValueNotifyingHost(out);
            else if (sink_ != nullptr)   sink_->virtualApply(vt, out);
        }
    }

    /** Message thread: true once after an LFO window slid to follow a hand —
     *  the panel refreshes its MIN/MAX knobs and the session is marked dirty. */
    bool takeRangeShifted() noexcept
    { return rangeShifted_.exchange(false, std::memory_order_acq_rel); }

    /** prepareToPlay: the envelope clock. */
    void prepare(double sampleRate) noexcept
    { sampleRate_.store(sampleRate, std::memory_order_relaxed); }

    //==========================================================================
    // Message thread — mapping edits
    //==========================================================================
    /** Install (or replace) the mapping of one parameter. Returns false when
     *  the parameter id is unknown or the table is full. lo/hi is the
     *  normalised output range the controller sweeps, follow the
     *  MIDI-follow opt-out, curve the transfer law and env the envelope
     *  (restore passes the saved ones; a fresh learn starts at the full
     *  0..1, following, linear, no envelope). */
    bool addMapping(int type, int channel, int number, const juce::String& paramId,
                    float lo = 0.0f, float hi = 1.0f, bool follow = true,
                    const MidiMappingCurve* curve = nullptr,
                    const MidiMappingEnvelope* env = nullptr,
                    float phase = 0.0f);

    /** Bind `paramId` to LFO `lfoIndex` of the bank — or to a NEW one when
     *  `lfoIndex` is kNewLfo (the bank brings its lowest free slot into use)
     *  — with the window centred on where the parameter stands right now
     *  (± `halfSpan` of its course): assigning a modulation must not make
     *  the sound jump. MIDI-follow is off — an LFO moves all the time and
     *  would navigate non-stop. Message thread. */
    static constexpr int kNewLfo = -1;
    bool addLfoMapping(int lfoIndex, const juce::String& paramId,
                       float halfSpan = 0.18f);

    /** Fired after addLfoMapping succeeded — the editor reveals the new row so
     *  the two knobs that set the modulation's reach are seen at least once. */
    std::function<void(const juce::String&)> onLfoMappingAdded;

    /** Point a parameter at an event CHOSEN from a list (the right-click
     *  "Assign to" menu) instead of learnt from the hardware. Same result as
     *  a learn, except that an existing mapping keeps its range, its transfer
     *  law and its envelope: re-picking a controller is a re-wire, not a
     *  reset. Leaving an LFO does restore the full course — an LFO window is
     *  a depth around the current value and means nothing to a CC sweep.
     *  Message thread. */
    bool assignEvent(int type, int channel, int number, const juce::String& paramId);

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

    /** One row of the MIDI MAP panel: the mapped event, its target parameter
     *  and the normalised range the controller sweeps (lo > hi inverts). */
    struct MappingInfo
    {
        juce::String paramId;
        int   type    { 0 };     // 1 = Note, 2 = CC
        int   channel { 0 };
        int   number  { 0 };
        float lo { 0.0f }, hi { 1.0f };
        float phase { 0.0f };    // LFO destinations: offset along the cycle
        bool  follow { true };   // MIDI-follow navigates here on a move
        bool  curved { false };  // a non-neutral transfer law (window / shape / hysteresis)
        bool  enveloped { false }; // an envelope trigger is armed
    };

    /** Every occupied mapping slot, table order. Message thread. */
    std::vector<MappingInfo> allMappings() const;

    /** Normalised output range of one mapping (0..1 defaults). */
    bool getMappingRange(const juce::String& paramId, float& lo, float& hi) const;

    /** Set the range a mapped controller sweeps (MIDI MAP panel MIN/MAX).
     *  Message thread. Persists via onMappingsEdited but deliberately does
     *  NOT broadcast a ChangeMessage: the MIN/MAX bars call this on every
     *  drag tick, and a broadcast would make the panel rebuild the very
     *  slider being dragged. */
    void setMappingRange(const juce::String& paramId, float lo, float hi);

    /** Phase offset of an LFO destination (0..1 of the cycle). Same
     *  no-broadcast contract as setMappingRange. Message thread. */
    float getMappingPhase(const juce::String& paramId) const;
    void  setMappingPhase(const juce::String& paramId, float phase01);

    /** Per-mapping MIDI-follow opt-out (MIDI MAP panel toggle). True for
     *  unmapped ids so the editor's gate defaults open. Message thread. */
    bool mappingFollows(const juce::String& paramId) const;
    /** Same no-broadcast contract as setMappingRange (the toggle is the only
     *  writer while the panel shows). Message thread. */
    void setMappingFollow(const juce::String& paramId, bool shouldFollow);

    /** The transfer law of one mapping (MIDI CURVE window). False when
     *  unmapped. Message thread. */
    bool getMappingCurve(const juce::String& paramId, MidiMappingCurve& out) const;
    /** Replace it: bakes the lock-free LUT, persists via onMappingsEdited
     *  and — same contract as setMappingRange — does NOT broadcast (the
     *  window calls this on every drag tick). Message thread. */
    void setMappingCurve(const juce::String& paramId, const MidiMappingCurve& curve);
    /** Where the controller last put this mapping: raw input (value / 127)
     *  and the normalised output actually applied. False until the first
     *  event lands. The window's live dot. Message thread. */
    bool lastMappedValue(const juce::String& paramId, float& in01, float& out01) const;

    /** The envelope of one mapping (MIDI CURVE window). False when
     *  unmapped. Message thread. */
    bool getMappingEnvelope(const juce::String& paramId, MidiMappingEnvelope& out) const;
    /** Replace it — the runner's atomics follow at once (a running
     *  envelope hears the edit), persists via onMappingsEdited and — same
     *  contract as setMappingCurve — does NOT broadcast. Message thread. */
    void setMappingEnvelope(const juce::String& paramId, const MidiMappingEnvelope& env);
    /** Where the envelope is RIGHT NOW: stage, progress along its nominal
     *  segment and level (0..1 of the OUT span). False when unmapped or no
     *  envelope armed. The window's live point. Message thread. */
    bool envelopeLive(const juce::String& paramId, MidiMappingEnvelope::Stage& stage,
                      float& t01, float& level01) const;

    /** "Edit MIDI mapping…" chosen in a control's right-click menu — the
     *  editor hooks the MIDI CURVE window here. Message thread. */
    std::function<void(const juce::String&)> onEditRequested;
    void requestEdit(const juce::String& paramId)
    { if (onEditRequested) onEditRequested(paramId); }

    /** Reverse lookup: the parameter mapped to (type, channel, number), or an
     *  empty string. Message thread (paramId mirror) — used by the CIS
     *  CONTROLS page to show what each button / axis drives. */
    juce::String paramForEvent(int type, int channel, int number) const
    {
        for (const auto& s : slots_)
            if (s.paramId.isNotEmpty()
                && s.type   .load(std::memory_order_relaxed) == type
                && s.channel.load(std::memory_order_relaxed) == channel
                && s.number .load(std::memory_order_relaxed) == number)
                return s.paramId;
        return {};
    }

    /** EVERY parameter mapped to (type, channel, number). A controller may
     *  legitimately drive several destinations — that is how a macro knob is
     *  built — so nothing here forbids it; the right-click list and the
     *  mapped badge just make sure it is never a surprise. Message thread. */
    juce::StringArray paramsForEvent(int type, int channel, int number) const
    {
        juce::StringArray out;
        for (const auto& s : slots_)
            if (s.paramId.isNotEmpty()
                && s.type   .load(std::memory_order_relaxed) == type
                && s.channel.load(std::memory_order_relaxed) == channel
                && s.number .load(std::memory_order_relaxed) == number)
                out.add(s.paramId);
        return out;
    }

    /** The OTHER parameters driven by the same event as `paramId` (empty when
     *  unmapped or alone on its controller). Message thread. */
    juce::StringArray eventSharedWith(const juce::String& paramId) const
    {
        int t = 0, c = 0, n = 0;
        if (! getMappingFor(paramId, t, c, n) || t == kTypeLfo)
            return {};
        juce::StringArray others = paramsForEvent(t, c, n);
        others.removeString(paramId);
        return others;
    }

    /** Has controller `cc` been seen on `channel` since launch? Message
     *  thread (the audio thread only ever sets bits). */
    bool ccSeen(int channel, int cc) const noexcept
    {
        if (channel < 1 || channel > 16 || cc < 0 || cc > 127) return false;
        return (seenCc_[channel - 1][cc >> 5].load(std::memory_order_relaxed)
                & (1u << (cc & 31))) != 0;
    }

    /** The channel the last CC arrived on (1..16), or 0 when none has. */
    int lastCcChannel() const noexcept
    { return lastCcChannel_.load(std::memory_order_relaxed); }

    /** Optional: how to NAME a parameter in a menu ("4 VIDEO · DC BLOCK ·
     *  Amount"). The editor installs it from ui/ParamIdentity.h — the engine
     *  knows ids, not identities. Unset → the raw id. Message thread. */
    std::function<juce::String(const juce::String&)> describeTarget;
    juce::String targetLabel(const juce::String& paramId) const
    {
        if (describeTarget)
            if (const juce::String s = describeTarget(paramId); s.isNotEmpty())
                return s;
        return paramId;
    }

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
        // Normalised output range (MIN/MAX, lo > hi inverts) — written on the
        // message thread (setMappingRange), read per event on the audio one.
        std::atomic<float> lo { 0.0f }, hi { 1.0f };
        // MIDI-follow opt-out (message thread both sides — the editor's gate
        // reads it at UI rate; atomic for symmetry with its neighbours).
        std::atomic<bool> follow { true };

        // ── Transfer law (MidiMappingCurve.h) ──────────────────────────────
        // Input window + hysteresis loop width: written by bakeCurve (message
        // thread), read per event. The shape is a double-buffered LUT: the
        // message thread bakes into the inactive buffer then flips lutActive
        // (release); -1 = identity, no LUT read at all (the default).
        std::atomic<float> inLo { 0.0f }, inHi { 1.0f };
        std::atomic<float> hyst { 0.0f };
        std::atomic<int>   lutActive { -1 };
        std::atomic<float> lut[2][MidiMappingCurve::kLutN];
        // Phase offset of this DESTINATION along its LFO's cycle (0..1) —
        // message thread writes, audio thread reads once per block. What
        // makes one shape drive four parameters in quadrature.
        std::atomic<float> phase { 0.0f };
        // Backlash state — AUDIO thread only (never touched from the message
        // thread; a stale value self-corrects on the next event). -1 = unset.
        float hystX { -1.0f };
        // Last value an LFO actually applied — the deadband's memory. AUDIO
        // thread only, same contract as hystX.
        float lastApplied { -2.0f };
        // Last (raw input, applied output) pair — the window's live dot.
        std::atomic<float> lastIn { -1.0f }, lastOut { -1.0f };
        // Message-thread mirror of the law (window edits, persistence).
        MidiMappingCurve curve;

        // ── Envelope (MidiMappingEnvelope.h) ────────────────────────────────
        // The runner: parameters as atomics (message → audio), stage / level
        // audio-only, live point published. `envelope` = the message-thread
        // mirror (window edits, persistence).
        MidiEnvelopeRunner  env;
        MidiMappingEnvelope envelope;

        // Message-thread-only mirror used for lookups/persistence. Non-empty
        // iff the slot is occupied (either kind).
        juce::String paramId;

        /** Audio thread: the full pipeline of a continuous sweep — raw 0..1
         *  in, normalised parameter value out. Publishes the live pair. */
        float sweep(float v01) noexcept
        {
            const float wl = inLo.load(std::memory_order_relaxed);
            const float wh = inHi.load(std::memory_order_relaxed);
            float x = (wh - wl) > 1e-6f ? (v01 - wl) / (wh - wl)
                                        : (v01 >= wh ? 1.0f : 0.0f);
            x = juce::jlimit(0.0f, 1.0f, x);

            // Backlash hysteresis: the internal position only moves once the
            // input has travelled more than half the loop width away from
            // it, then renormalised so the extremes are still reached (the
            // window draws exactly this law — MidiMappingCurve::hystBranch).
            const float h = 0.5f * hyst.load(std::memory_order_relaxed);
            if (h > 0.0f)
            {
                float xs = hystX;
                if      (xs < 0.0f)   xs = juce::jlimit(h, 1.0f - h, x);
                else if (x > xs + h)  xs = x - h;
                else if (x < xs - h)  xs = x + h;
                hystX = xs;
                x = juce::jlimit(0.0f, 1.0f, (xs - h) / (1.0f - 2.0f * h));
            }
            else
                hystX = -1.0f;

            float y = x;
            if (const int li = lutActive.load(std::memory_order_acquire); li >= 0)
            {
                const float pos = x * (float) (MidiMappingCurve::kLutN - 1);
                const int   i   = juce::jmin((int) pos, MidiMappingCurve::kLutN - 2);
                const float t   = pos - (float) i;
                const float a   = lut[li][i]    .load(std::memory_order_relaxed);
                const float b   = lut[li][i + 1].load(std::memory_order_relaxed);
                y = a + (b - a) * t;
            }

            const float l   = lo.load(std::memory_order_relaxed);
            const float hh  = hi.load(std::memory_order_relaxed);
            const float out = l + (hh - l) * y;
            lastIn .store(v01, std::memory_order_relaxed);
            lastOut.store(out, std::memory_order_relaxed);
            return out;
        }
    };

    /** Absolute CC / velocity sweeps run through the slot's transfer law
     *  (Slot::sweep — window, hysteresis, shape, MIN/MAX). Toggle / cycle
     *  semantics ignore it — a switch has no useful sub-range. */
    static void applyEvent(juce::RangedAudioParameter& p,
                           const juce::MidiMessage& msg,
                           Slot& s) noexcept
    {
        const int  steps        = p.getNumSteps();
        const bool boolLike     = (steps == 2);
        const bool discreteLike = (steps > 2 && steps <= 32);   // ComboBox-sized

        if (msg.isController())
        {
            const int v = msg.getControllerValue();
            p.setValueNotifyingHost(boolLike
                ? (v >= 64 ? 1.0f : 0.0f)
                : s.sweep((float) v / 127.0f));
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
                p.setValueNotifyingHost(s.sweep((float) msg.getVelocity() / 127.0f));
        }
    }

    /** Apply one MIDI event to a VIRTUAL target through the sink. Mirrors
     *  applyEvent's value semantics (CC absolute / Note toggle-or-cycle,
     *  the slot's transfer law on continuous sweeps) and adds press/release
     *  handling for action targets (steps < 0). RT-safe. */
    void applyVirtual(int targetId, const juce::MidiMessage& msg, bool isNoteOff,
                      Slot& s) noexcept
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
            sink_->virtualApply(targetId, boolLike
                ? (v >= 64 ? 1.0f : 0.0f)
                : s.sweep((float) v / 127.0f));
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
                sink_->virtualApply(targetId, s.sweep((float) msg.getVelocity() / 127.0f));
        }
    }

    void timerCallback() override;   // finishes a pending learn

    int  slotIndexFor(const juce::String& paramId) const;   // -1 when unmapped
    void bakeCurve(Slot& s);   // message thread: curve mirror → atomics + LUT flip
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
    ILfoSource* lfos_ = nullptr;         // the modulation bank (set once at ctor)
    std::atomic<bool> rangeShifted_ { false };   // audio → message: a window moved
    Slot slots_[kMaxMappings];
    std::atomic<double> sampleRate_ { 0.0 };   // envelope clock (prepare)

    // MIDI learn state — armed/result cross threads, paramId message-only.
    std::atomic<bool> learnArmed_  { false };
    std::atomic<int>  learnResult_ { kNoResult };
    juce::String      learnParamId_;

    // Controllers seen since launch — 128 bits per channel, set by the audio
    // thread (relaxed fetch_or), read by the menus. Never cleared: a knob
    // moved once at soundcheck is still the knob to offer at 2 a.m.
    std::atomic<uint32_t> seenCc_[16][4] {};
    std::atomic<int>      lastCcChannel_ { 0 };

    // MIDI-follow state — audio thread publishes (slot + generation), the
    // editor's poll consumes on the message thread (lastSeenTouchGen_).
    std::atomic<int>      touchedSlot_ { -1 };
    std::atomic<uint32_t> touchGen_    { 0 };
    uint32_t              lastSeenTouchGen_ { 0 };   // message-thread only

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiMappingEngine)
};
