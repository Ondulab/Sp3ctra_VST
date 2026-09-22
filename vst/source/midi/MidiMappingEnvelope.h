/**
 * @file MidiMappingEnvelope.h
 * @brief The ENVELOPE of ONE MIDI mapping — a press no longer SETS the
 *        parameter, it PLAYS it: attack to the peak, decay to the sustain,
 *        release back to rest — edited in the MIDI CURVE window, run by
 *        MidiMappingEngine on the audio thread, persisted in the <MAP> node.
 *
 *   level                  ┌──── peak (OUT MAX, × velocity)
 *     1 ┤        ╭─────────╮
 *       │       ╱           ╲
 *       │      ╱             ╰──────────── sustain      (GATE: while held)
 *       │     ╱                            ╲
 *       │    ╱                              ╲
 *     0 ┤───╯                                ╰──────── rest (OUT MIN)
 *        └─ A ─┴─── D ────┴── (hold) ──┴──── R ────┴──▶ time
 *
 * Trigger — WHAT the mapped event does to the envelope:
 *   • GATE      press = attack → decay → hold the sustain; release = release.
 *               A key, a held pad, a CIS button.
 *   • ONE-SHOT  press = attack → decay → release at once, the release of the
 *               control is ignored. A hit, a tap — the CIS HIT / TAP
 *               gestures send a press+release pulse: this is their mode.
 *               With SUSTAIN at 100 % the DECAY is a HOLD at the peak
 *               (an AHR envelope, the percussive one).
 *   • LOOP      attack → decay repeat while the control is held (SUSTAIN =
 *               the trough), release = release. A strobe, a tremolo.
 *
 * The travel is normalised 0..1 and lands on the mapping's OUT MIN..MAX
 * (the panel row's two knobs): MIN is the rest, MAX the peak, and swapping
 * them inverts the whole envelope for free. VEL scales the peak by the
 * press value (note velocity, CC value — the CIS HIT sends its impact
 * strength): 0 = every press reaches the peak, 100 % = the peak IS the
 * velocity.
 *
 * Each timed segment bends through its own KNEE — a point the user puts
 * anywhere in the segment's box (x = when, y = how far along the travel):
 * the segment is the monotone cubic through (start, knee, end) — the
 * Fritsch–Carlson tangents of the POINTS transfer law, so it never
 * overshoots: a rise stays a rise. A knee on the diagonal is a straight
 * line; up-left is the analog feel (a fast start that settles — an RC
 * charge on the attack, an exponential fall on decay / release, the
 * default); down-right a slow start.
 *
 * Re-press / release land ON the drawn curve: a press while the envelope
 * is already up resumes the attack from the point of the curve that has
 * the current level (constant rate along the curve — a re-hit near the
 * peak is quick, never a full attack from the floor), a release during the
 * attack joins the release curve at its current level. What the window
 * draws is what the engine plays.
 *
 * RETRIG (the chip next to the triggers) changes the re-press only: the
 * level drops to the rest and the WHOLE attack replays — a drum-machine
 * re-hit, the same shape on every press whatever the envelope was doing.
 * Off (the default) is the legato resume above; with SUSTAIN at 100 % a
 * re-hit of a held or one-shot envelope is then invisible, which is
 * exactly when RETRIG is wanted. A LOOP's own repeats are not presses:
 * they keep restarting from the trough either way.
 *
 * This struct is the MESSAGE-THREAD description; the static evaluators are
 * the ONE law both the window (drawing) and the engine (running) use.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <atomic>
#include <cmath>

struct MidiMappingEnvelope
{
    enum class Trigger : int { Off = 0, Gate = 1, OneShot = 2, Loop = 3 };
    enum class Stage   : int { Idle = 0, Attack = 1, Decay = 2, Sustain = 3, Release = 4 };

    static constexpr float kMaxAttack  = 10.0f;   ///< seconds
    static constexpr float kMaxDecay   = 10.0f;
    static constexpr float kMaxRelease = 20.0f;
    static constexpr float kDefAttack  = 0.010f;
    static constexpr float kDefDecay   = 0.300f;
    static constexpr float kDefSustain = 1.0f;
    static constexpr float kDefRelease = 0.500f;
    static constexpr float kDefVel     = 0.0f;
    static constexpr float kKneeMinX   = 0.02f;   ///< a knee never sits on a segment's ends
    static constexpr float kKneeMaxX   = 0.98f;

    /** A segment's knee, in its own box: x = progress 0..1 (when), y =
     *  travel 0..1 from the segment's start level to its end level. */
    struct Knee { float x, y; };
    /** The analog feel: a quarter of the way in time, over half the travel. */
    static constexpr Knee kDefKnee { 0.25f, 0.55f };
    enum Seg : int { SegA = 0, SegD = 1, SegR = 2 };

    Trigger trigger { Trigger::Off };
    float attack  { kDefAttack  };   ///< seconds, rest → peak
    float decay   { kDefDecay   };   ///< seconds, peak → sustain
    float sustain { kDefSustain };   ///< 0..1 of the peak
    float release { kDefRelease };   ///< seconds, → rest
    float vel     { kDefVel     };   ///< 0..1 — how much the press value scales the peak
    Knee  knee[3] { kDefKnee, kDefKnee, kDefKnee };   ///< A, D, R bends
    bool  retrig  { false };         ///< a re-press replays the attack from the rest (off: resumes from the level)

    bool isOff() const noexcept { return trigger == Trigger::Off; }

    /** RESET of the window: times, sustain, velocity and knees back to the
     *  defaults; the trigger itself stays (RESET never switches a mode). */
    void resetShape() noexcept
    {
        attack = kDefAttack; decay = kDefDecay; sustain = kDefSustain;
        release = kDefRelease; vel = kDefVel;
        for (auto& k : knee) k = kDefKnee;
    }

    static Knee clampKnee(Knee k) noexcept
    {
        return { juce::jlimit(kKneeMinX, kKneeMaxX, k.x), juce::jlimit(0.0f, 1.0f, k.y) };
    }
    static bool isDefaultKnee(Knee k) noexcept
    {
        return std::abs(k.x - kDefKnee.x) < 1e-4f && std::abs(k.y - kDefKnee.y) < 1e-4f;
    }

    //==========================================================================
    // The segment law — shared by the window and the engine
    //==========================================================================
    /** A segment's travel at progress t (0..1): the monotone cubic through
     *  (0,0), the knee and (1,1) — Fritsch–Carlson tangents (as the POINTS
     *  transfer law): monotone, no overshoot, whatever the knee. */
    static float segY(Knee k, float t) noexcept
    {
        k = clampKnee(k);
        t = juce::jlimit(0.0f, 1.0f, t);
        const float d0 = k.y / k.x;
        const float d1 = (1.0f - k.y) / (1.0f - k.x);
        float m0 = d0, m1 = (d0 * d1 <= 0.0f) ? 0.0f : 0.5f * (d0 + d1), m2 = d1;
        auto limit = [](float d, float& ma, float& mb)
        {
            if (d == 0.0f) { ma = 0.0f; mb = 0.0f; return; }
            const float a = ma / d, b = mb / d, s = a * a + b * b;
            if (s > 9.0f) { const float f = 3.0f / std::sqrt(s); ma = f * a * d; mb = f * b * d; }
        };
        limit(d0, m0, m1);
        limit(d1, m1, m2);

        const bool  first = t <= k.x;
        const float xa = first ? 0.0f : k.x,  xb = first ? k.x : 1.0f;
        const float ya = first ? 0.0f : k.y,  yb = first ? k.y : 1.0f;
        const float ma = first ? m0 : m1,     mb = first ? m1 : m2;
        const float h  = xb - xa;
        const float u  = (t - xa) / h, u2 = u * u, u3 = u2 * u;
        const float h00 =  2.0f * u3 - 3.0f * u2 + 1.0f;
        const float h10 =         u3 - 2.0f * u2 + u;
        const float h01 = -2.0f * u3 + 3.0f * u2;
        const float h11 =         u3 -        u2;
        return juce::jlimit(0.0f, 1.0f, h00 * ya + h10 * h * ma + h01 * yb + h11 * h * mb);
    }

    /** The progress at which a segment has covered travel y (0..1) — the
     *  inverse of segY, by bisection on the monotone curve (retrigger /
     *  release join the drawn curve there). */
    static float segT(Knee k, float y) noexcept
    {
        y = juce::jlimit(0.0f, 1.0f, y);
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 18; ++i)
        {
            const float mid = 0.5f * (lo + hi);
            if (segY(k, mid) < y) lo = mid; else hi = mid;
        }
        return 0.5f * (lo + hi);
    }

    /** The peak a press of value v01 reaches: VEL blends "always 1" toward
     *  "the velocity itself". */
    float peakFor(float v01) const noexcept
    { return juce::jlimit(0.0f, 1.0f, 1.0f - vel * (1.0f - juce::jlimit(0.0f, 1.0f, v01))); }

    /** Nominal level along the DRAWN envelope, by stage and progress (peak
     *  1): what the window plots; the engine's live point sits on it. */
    float levelAt(Stage st, float t) const noexcept
    {
        switch (st)
        {
            case Stage::Attack:  return segY(knee[SegA], t);
            case Stage::Decay:   return 1.0f - (1.0f - sustain) * segY(knee[SegD], t);
            case Stage::Sustain: return sustain;
            case Stage::Release: return sustain * (1.0f - segY(knee[SegR], t));
            case Stage::Idle:    break;
        }
        return 0.0f;
    }

    /** The level a segment's knee sits at, in the drawn envelope (peak 1). */
    float kneeLevel(int seg) const noexcept
    {
        const float y = knee[seg].y;
        switch (seg)
        {
            case SegA: return y;
            case SegD: return 1.0f - (1.0f - sustain) * y;
            default:   return sustain * (1.0f - y);
        }
    }

    //==========================================================================
    // Persistence — <MAP> attributes, present only while the envelope is on
    // (pre-envelope sessions round-trip byte-identical).
    //==========================================================================
    void writeTo(juce::ValueTree& map) const
    {
        if (isOff()) return;
        map.setProperty("env",      (int) trigger, nullptr);
        map.setProperty("envA",     attack,        nullptr);
        map.setProperty("envD",     decay,         nullptr);
        map.setProperty("envS",     sustain,       nullptr);
        map.setProperty("envR",     release,       nullptr);
        if (vel != kDefVel) map.setProperty("envVel", vel, nullptr);
        if (retrig)         map.setProperty("envRetrig", 1, nullptr);
        static const char* const kKneeKeys[3] = { "envKA", "envKD", "envKR" };
        for (int i = 0; i < 3; ++i)
            if (! isDefaultKnee(knee[i]))
                map.setProperty(kKneeKeys[i],
                                juce::String(knee[i].x, 4) + "," + juce::String(knee[i].y, 4),
                                nullptr);
    }

    static MidiMappingEnvelope readFrom(const juce::ValueTree& map)
    {
        MidiMappingEnvelope e;
        e.trigger = (Trigger) juce::jlimit(0, 3, (int) map.getProperty("env", 0));
        if (e.isOff()) return e;
        e.attack  = juce::jlimit(0.0f, kMaxAttack,  (float) (double) map.getProperty("envA", (double) kDefAttack));
        e.decay   = juce::jlimit(0.0f, kMaxDecay,   (float) (double) map.getProperty("envD", (double) kDefDecay));
        e.sustain = juce::jlimit(0.0f, 1.0f,        (float) (double) map.getProperty("envS", (double) kDefSustain));
        e.release = juce::jlimit(0.0f, kMaxRelease, (float) (double) map.getProperty("envR", (double) kDefRelease));
        e.vel     = juce::jlimit(0.0f, 1.0f,        (float) (double) map.getProperty("envVel", (double) kDefVel));
        e.retrig  = ((int) map.getProperty("envRetrig", 0)) != 0;
        static const char* const kKneeKeys[3] = { "envKA", "envKD", "envKR" };
        for (int i = 0; i < 3; ++i)
        {
            const auto xy = juce::StringArray::fromTokens(
                map.getProperty(kKneeKeys[i], "").toString(), ",", "");
            if (xy.size() == 2)
                e.knee[i] = clampKnee({ xy[0].getFloatValue(), xy[1].getFloatValue() });
        }
        return e;
    }
};

//==============================================================================
/** The AUDIO-THREAD runner of one mapping's envelope. Parameters come in
 *  through atomics (the message thread writes them on every window edit —
 *  the running envelope follows at once, a sustain drag is heard live);
 *  the stage / progress / level are plain audio-only state, published as
 *  atomics for the window's live point and the panel row's VU. */
struct MidiEnvelopeRunner
{
    using Trigger = MidiMappingEnvelope::Trigger;
    using Stage   = MidiMappingEnvelope::Stage;

    // ── Parameters (message → audio) ────────────────────────────────────────
    std::atomic<int>   trigger { 0 };
    std::atomic<float> attack  { MidiMappingEnvelope::kDefAttack  };
    std::atomic<float> decay   { MidiMappingEnvelope::kDefDecay   };
    std::atomic<float> sustain { MidiMappingEnvelope::kDefSustain };
    std::atomic<float> release { MidiMappingEnvelope::kDefRelease };
    std::atomic<float> vel     { MidiMappingEnvelope::kDefVel     };
    std::atomic<int>   retrig  { 0 };   ///< 1 = a re-press replays the attack from the rest
    std::atomic<float> kx[3]   { MidiMappingEnvelope::kDefKnee.x, MidiMappingEnvelope::kDefKnee.x, MidiMappingEnvelope::kDefKnee.x };
    std::atomic<float> ky[3]   { MidiMappingEnvelope::kDefKnee.y, MidiMappingEnvelope::kDefKnee.y, MidiMappingEnvelope::kDefKnee.y };

    // ── State (audio thread only) ───────────────────────────────────────────
    Stage stage   { Stage::Idle };
    float tn      { 0.0f };   ///< progress along the nominal segment, 0..1
    float level   { 0.0f };   ///< 0..1 of the OUT span
    float peak    { 1.0f };   ///< this press's peak (velocity-scaled)
    float relFrom { 0.0f };   ///< the level the release curve is scaled from
    bool  gate    { false };  ///< the control is held
    bool  ccGate  { false };  ///< CC edge detector (≥ 64 = pressed)

    // ── Published (audio → UI) ──────────────────────────────────────────────
    std::atomic<int>   stageOut { 0 };
    std::atomic<float> tnOut    { 0.0f }, levelOut { 0.0f };

    /** Message thread, before the slot goes live / when the trigger is
     *  switched: parameters ← the description. */
    void load(const MidiMappingEnvelope& e) noexcept
    {
        attack .store(e.attack,  std::memory_order_relaxed);
        decay  .store(e.decay,   std::memory_order_relaxed);
        sustain.store(e.sustain, std::memory_order_relaxed);
        release.store(e.release, std::memory_order_relaxed);
        vel    .store(e.vel,     std::memory_order_relaxed);
        retrig .store(e.retrig ? 1 : 0, std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i)
        {
            const auto k = MidiMappingEnvelope::clampKnee(e.knee[i]);
            kx[i].store(k.x, std::memory_order_relaxed);
            ky[i].store(k.y, std::memory_order_relaxed);
        }
        trigger.store((int) e.trigger, std::memory_order_release);   // last: gates the audio side
    }

    /** Audio thread — the control was pressed with value v01. */
    void gateOn(float v01) noexcept
    {
        const float va = vel.load(std::memory_order_relaxed);
        peak = juce::jlimit(0.0f, 1.0f, 1.0f - va * (1.0f - juce::jlimit(0.0f, 1.0f, v01)));
        gate = true;
        // RETRIG: back to the rest first, so enterAttack() starts the curve
        // at t = 0 and the whole attack replays. Off: legato resume from
        // wherever the level is (the header's re-press rule).
        if (retrig.load(std::memory_order_relaxed) != 0)
            level = 0.0f;
        enterAttack();
    }

    /** Audio thread — the control was released. */
    void gateOff() noexcept
    {
        gate = false;
        if ((Trigger) trigger.load(std::memory_order_relaxed) == Trigger::OneShot)
            return;   // a one-shot runs its own course
        if (stage != Stage::Idle && stage != Stage::Release)
            enterRelease();
    }

    /** Audio thread — dt seconds elapsed. Returns true while there is a
     *  level to apply (including the final rest value after the release). */
    bool advance(float dt) noexcept
    {
        if (stage == Stage::Idle)
            return false;
        const auto tr = (Trigger) trigger.load(std::memory_order_relaxed);

        // A block may cross several zero-length stages (a 0 ms attack into
        // a 0 ms decay): walk them, bounded — a LOOP with every time at 0
        // must not spin.
        for (int guard = 0; guard < 6; ++guard)
        {
            switch (stage)
            {
                case Stage::Attack:
                {
                    const float len = attack.load(std::memory_order_relaxed);
                    tn = len > 1e-5f ? tn + dt / len : 1.0f;
                    if (tn >= 1.0f) { level = peak; enterDecay(); continue; }
                    level = peak * MidiMappingEnvelope::segY(kneeOf(0), tn);
                    break;
                }
                case Stage::Decay:
                {
                    const float sl  = sustain.load(std::memory_order_relaxed) * peak;
                    const float len = decay.load(std::memory_order_relaxed);
                    tn = len > 1e-5f ? tn + dt / len : 1.0f;
                    if (tn >= 1.0f)
                    {
                        level = sl;
                        if      (tr == Trigger::OneShot) enterRelease();
                        else if (tr == Trigger::Loop)    { if (gate) enterAttack(); else enterRelease(); }
                        else                             { if (gate) stage = Stage::Sustain; else enterRelease(); }
                        // Zero-length follow-ups are walked; a real stage
                        // starts on the next block.
                        if (stage == Stage::Sustain) break;
                        continue;
                    }
                    level = peak - (peak - sl) * MidiMappingEnvelope::segY(kneeOf(1), tn);
                    break;
                }
                case Stage::Sustain:
                    level = sustain.load(std::memory_order_relaxed) * peak;   // a live SUSTAIN drag is heard
                    break;
                case Stage::Release:
                {
                    const float len = release.load(std::memory_order_relaxed);
                    tn = len > 1e-5f ? tn + dt / len : 1.0f;
                    if (tn >= 1.0f) { level = 0.0f; stage = Stage::Idle; break; }
                    level = relFrom * (1.0f - MidiMappingEnvelope::segY(kneeOf(2), tn));
                    break;
                }
                case Stage::Idle: break;
            }
            break;
        }
        publish();
        return true;   // the Idle transition still applies its rest value once
    }

    /** Audio thread — the trigger was switched off / the slot re-armed. */
    void reset() noexcept
    {
        stage = Stage::Idle; tn = 0.0f; level = 0.0f; peak = 1.0f;
        relFrom = 0.0f; gate = false; ccGate = false;
        publish();
    }

private:
    MidiMappingEnvelope::Knee kneeOf(int i) const noexcept
    {
        return { kx[i].load(std::memory_order_relaxed), ky[i].load(std::memory_order_relaxed) };
    }

    /** Resume the attack from the point of the curve that has the current
     *  level (constant rate along the curve, never a full attack from the
     *  floor on a re-hit near the peak). */
    void enterAttack() noexcept
    {
        stage = Stage::Attack;
        tn = peak > 1e-4f ? MidiMappingEnvelope::segT(kneeOf(0), juce::jmin(1.0f, level / peak))
                          : 1.0f;
        if (tn >= 0.999f) { level = peak; enterDecay(); }
    }
    void enterDecay() noexcept { stage = Stage::Decay; tn = 0.0f; }
    /** Join the release curve at the current level: released from the
     *  sustain (or above it) the curve starts here; released mid-attack it
     *  starts partway down the drawn one. */
    void enterRelease() noexcept
    {
        stage   = Stage::Release;
        const float sl = sustain.load(std::memory_order_relaxed) * peak;
        relFrom = juce::jmax(level, sl);
        // The release curve's travel is 1 − level/relFrom at the join.
        tn = relFrom > 1e-4f ? MidiMappingEnvelope::segT(kneeOf(2), 1.0f - level / relFrom) : 1.0f;
    }
    void publish() noexcept
    {
        stageOut.store((int) stage, std::memory_order_relaxed);
        tnOut   .store(tn,          std::memory_order_relaxed);
        levelOut.store(level,       std::memory_order_relaxed);
    }
};
