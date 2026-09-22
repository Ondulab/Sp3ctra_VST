/**
 * @file LfoBank.h
 * @brief The MODULATION bank — eight free shapes that drive mapped parameters
 *        exactly the way a controller knob does.
 *
 * Doctrine: an LFO is not a new kind of TARGET, it is a new kind of SOURCE of
 * MidiMappingEngine. A mapping whose type is MidiMappingEngine::kTypeLfo names
 * an LFO of this bank instead of a CC, and everything the engine already gives
 * a mapping applies unchanged — the OUT MIN/MAX window (which IS the
 * modulation depth, in the parameter's own units, and inverts when MIN > MAX),
 * the transfer law of the ⚙ window, the identity label, the MIDI MAP row, the
 * persistence. The bank produces a bare 0..1; the mapping decides what that
 * means for its destination.
 *
 *   phase ──▶ shapeAt() ──▶ 0..1 ──▶ Slot::sweep (window · hyst · LUT · MIN/MAX)
 *
 * The shape law is ONE pure function (shapeAt) used by both the panel that
 * draws the scope and the audio thread that plays it: what the row draws is
 * what the destination gets. No LFO state is per-destination except the phase
 * OFFSET carried by the mapping slot — that is what lets one LFO drive four
 * parameters in quadrature and fabricate a complex motion from a simple shape.
 *
 * Clocks:
 *   FREE  free-running in Hz (0.01 .. 20).
 *   SYNC  a division of the tempo (1/1 .. 1/32). The tempo is the HOST's
 *         whenever it publishes one (a DAW's transport - read-only, shown),
 *         else the bank's own TEMPO (the standalone has no transport, so the
 *         panel's knob sets it, 20..300 BPM, persisted with the session).
 *   SCAN  a period in SCANNED LINES, advanced by the image pipeline's
 *         frame_seq — the modulation follows the paper travelling under the
 *         CIS, not the wall clock, and the acquisition gate slows it down with
 *         the scan. The one clock no other instrument has.
 *
 * The bank has SIXTEEN slots but a session only USES the ones it asked for:
 * an LFO is added when a modulation needs one ("Modulate ▸ New LFO", or the
 * panel's +) and removed when it is done with. Slots are addressed by index
 * everywhere — the mapping's `number`, the "lfo:2:rate" targets, the "LFO 3"
 * a MIDI MAP row prints — so an index is NEVER reassigned by a removal:
 * removing LFO 2 leaves a hole that the next add fills, and LFO 3 stays
 * LFO 3 for every row that names it. Sixteen is the ceiling of the target
 * encoding (LfoMidiTargets, 4 bits), not a promise of sixteen rows.
 *
 * The LFO parameters are NOT APVTS parameters: eight LFOs would add ~60 host
 * parameters of pure configuration. They live here, are persisted in the
 * session's own <LFOS> node, and are MIDI-controllable as VIRTUAL targets
 * (LfoMidiTargets, "lfo:3:rate") through the processor's IVirtualMidiSink —
 * which also means an LFO can modulate another LFO's rate for free.
 *
 * Threading contract:
 *   - tick() / lfoValue() / lfoRunning() : AUDIO thread, atomics only.
 *   - everything else                    : message thread.
 *   - retrig() is safe from both (a flag the audio thread consumes).
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>

//==============================================================================
/** What MidiMappingEngine needs of the bank — the engine never includes the
 *  bank itself (it only knows "there is a source with an index"). */
struct ILfoSource
{
    virtual ~ILfoSource() = default;

    /** Audio thread: the LFO's shape at (its phase + this destination's
     *  offset), 0..1. Pure — no state advances here. */
    virtual float lfoValue(int index, float phaseOffset01) const noexcept = 0;

    /** Audio thread: false when the LFO is stopped OR not in use — the
     *  engine then leaves the parameter alone (a stopped LFO releases what
     *  it was driving, exactly like an envelope whose trigger is off). */
    virtual bool  lfoRunning(int index) const noexcept = 0;

    /** Message thread: is this slot one the session uses? (A mapping may
     *  still name a removed one — its row stays, dormant, until deleted.) */
    virtual bool  lfoExists(int index) const noexcept = 0;

    /** Message thread: bring the lowest free slot into use, running, with
     *  the default shape. -1 when all sixteen are taken. */
    virtual int   lfoAdd() = 0;
};

//==============================================================================
class LfoBank : public ILfoSource
{
public:
    // JUCE_DECLARE_NON_COPYABLE (below) declares a copy constructor, which
    // suppresses the implicit default one — the bank is a plain member of the
    // processor, so it must be spelled out.
    LfoBank() { lfo_[0].active.store(true, std::memory_order_relaxed); }

    static constexpr int kNumLfos = 16;   ///< slots — see the header doc

    enum Shape { Sine = 0, Tri, RampUp, RampDown, Square, SampleHold, Random, NumShapes };
    enum Clock { Free = 0, Sync, Scan, NumClocks };

    static constexpr int   kNumDivs      = 6;
    static constexpr float kMinRate      = 0.01f;   ///< Hz
    static constexpr float kMaxRate      = 20.0f;
    static constexpr int   kMinLines     = 8;       ///< SCAN period, in scanned lines
    static constexpr int   kMaxLines     = 4000;
    static constexpr int   kMaxSteps     = 16;
    static constexpr float kDefRate      = 0.25f;
    static constexpr float kMinTempo     = 20.0f;   ///< BPM — the bank's own tempo
    static constexpr float kMaxTempo     = 300.0f;
    static constexpr float kDefTempo     = 120.0f;
    static constexpr int   kDefLines     = 500;
    static constexpr float kDefSkew      = 0.5f;

    //==========================================================================
    // The shape law — the ONE function the scope draws and the audio plays
    //==========================================================================
    static float wrap01(float p) noexcept { return p - std::floor(p); }

    /** Asymmetry: 0.5 = untouched, lower = a fast first half (a rise that
     *  settles), higher = a slow one. Square reads it as its pulse width. */
    static float warp(float p, float skew) noexcept
    {
        const float s = juce::jlimit(0.05f, 0.95f, skew);
        return p < s ? 0.5f * p / s
                     : 0.5f + 0.5f * (p - s) / (1.0f - s);
    }

    /** Deterministic 0..1 from an integer — the random shapes must draw the
     *  same steps the audio plays, so no RNG state is allowed. */
    static float hash01(int i) noexcept
    {
        uint32_t x = (uint32_t) i * 2654435761u;
        x ^= x >> 15; x *= 2246822519u;
        x ^= x >> 13; x *= 3266489917u;
        x ^= x >> 16;
        return (float) (x & 0xFFFFFFu) / (float) 0xFFFFFFu;
    }

    /** The shape at phase `p01` (any real — wrapped here), 0..1.
     *  `steps` > 0 quantises the phase into that many holds, which turns any
     *  shape into a stepper (and a SampleHold into an N-step sequencer);
     *  `seed` keeps two LFOs on random shapes from drawing the same steps. */
    static float shapeAt(int shape, float p01, float skew, int steps, int seed) noexcept
    {
        float p = wrap01(p01);
        const bool stochastic = (shape == SampleHold || shape == Random);
        if (steps > 0 && ! stochastic)
            p = std::floor(p * (float) steps) / (float) steps;

        switch (shape)
        {
            case Sine:     return 0.5f - 0.5f * std::cos(6.2831853f * warp(p, skew));
            case Tri:      { const float q = warp(p, skew); return q < 0.5f ? q * 2.0f : 2.0f - q * 2.0f; }
            case RampUp:   return warp(p, skew);
            case RampDown: return 1.0f - warp(p, skew);
            case Square:   return p < juce::jlimit(0.05f, 0.95f, skew) ? 1.0f : 0.0f;

            case SampleHold:
            {
                const int n = steps > 0 ? steps : 8;
                return hash01(step(p, n) + seed * 101);
            }
            case Random:
            {
                // Value noise: the same holds as S&H, joined by a smoothstep
                // — a drift that never jumps. Indices wrap so the cycle
                // closes on itself.
                const int   n = steps > 0 ? steps : 6;
                const float q = p * (float) n;
                const int   i = (int) std::floor(q);
                const float t = q - (float) i;
                const float a = hash01(((i     % n) + n) % n + seed * 101);
                const float b = hash01((((i + 1) % n) + n) % n + seed * 101);
                return a + (b - a) * (t * t * (3.0f - 2.0f * t));
            }
            default: return 0.0f;
        }
    }

    static const char* shapeName(int s) noexcept
    {
        switch (s)
        {
            case Sine:       return "SIN";
            case Tri:        return "TRI";
            case RampUp:     return "RAMP+";
            case RampDown:   return "RAMP-";
            case Square:     return "SQR";
            case SampleHold: return "S&H";
            case Random:     return "RND";
            default:         return "?";
        }
    }
    static const char* clockName(int c) noexcept
    {
        switch (c) { case Free: return "FREE"; case Sync: return "SYNC";
                     case Scan: return "SCAN"; default: return "?"; }
    }
    static const char* divName(int d) noexcept
    {
        static const char* n[kNumDivs] = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };
        return n[juce::jlimit(0, kNumDivs - 1, d)];
    }
    /** Period of a division, in beats (a quarter-note beat) — the acquisition
     *  gate's table, so both clocks agree on what "1/4" means. */
    static double divBeats(int d) noexcept
    {
        static const double b[kNumDivs] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
        return b[juce::jlimit(0, kNumDivs - 1, d)];
    }

    //==========================================================================
    /** One LFO, as the panel edits it. */
    struct Config
    {
        int   shape { Sine };
        int   clock { Free };
        float rate  { kDefRate };    ///< Hz — FREE
        int   div   { 2 };           ///< 1/4 — SYNC
        int   lines { kDefLines };   ///< scanned lines per cycle — SCAN
        float skew  { kDefSkew };
        int   steps { 0 };           ///< 0 = off
        bool  run   { true };
    };

    //==========================================================================
    // Audio thread
    //==========================================================================
    void prepare(double sampleRate) noexcept
    { sampleRate_.store(sampleRate, std::memory_order_relaxed); }

    /** Advance every running LFO by one block. `hostBpm` is what the host's
     *  play head published this block, 0 when it published none (the
     *  standalone, a host without transport) — the SYNC clock then runs on
     *  the bank's own tempo. `scanFrames` is the image pipeline's monotonic
     *  frame counter (AudioImageBuffers::frame_seq) — its DELTA is the SCAN
     *  clock, so a gated or stopped acquisition stops the modulation with
     *  it. */
    void tick(int numSamples, double hostBpm, uint64_t scanFrames) noexcept
    {
        hostBpm_.store(hostBpm > 0.0 ? (float) hostBpm : 0.0f, std::memory_order_relaxed);
        const double bpm = effectiveBpm();

        const double sr = sampleRate_.load(std::memory_order_relaxed);
        if (sr <= 0.0 || numSamples <= 0)
            return;
        const double dt = (double) numSamples / sr;

        // First block (or a counter reset): take the baseline, advance nothing.
        const uint64_t prev = lastScan_;
        lastScan_ = scanFrames;
        const double dLines = (scanFrames >= prev && prev != 0)
                            ? (double) (scanFrames - prev) : 0.0;

        // Measured scan rate, for the SCAN rows' readout — one pole, ~1 s, so
        // the number is readable rather than jittering with the block size.
        {
            const float inst = (float) (dLines / juce::jmax(1.0e-6, dt));
            const float k    = (float) juce::jlimit(0.0, 1.0, dt);
            linesPerSec_.store(linesPerSec_.load(std::memory_order_relaxed)
                                   + (inst - linesPerSec_.load(std::memory_order_relaxed)) * k,
                               std::memory_order_relaxed);
        }

        for (int i = 0; i < kNumLfos; ++i)
        {
            auto& l = lfo_[(size_t) i];

            if (l.retrig.exchange(false, std::memory_order_acq_rel))
                l.phase = 0.0;

            if (l.run.load(std::memory_order_relaxed))
            {
                double inc = 0.0;
                switch (l.clock.load(std::memory_order_relaxed))
                {
                    case Sync:
                        inc = (juce::jmax(1.0, bpm) / 60.0)
                            / divBeats(l.div.load(std::memory_order_relaxed)) * dt;
                        break;
                    case Scan:
                        inc = dLines / (double) juce::jmax(1, l.lines.load(std::memory_order_relaxed));
                        break;
                    case Free:
                    default:
                        inc = (double) l.rate.load(std::memory_order_relaxed) * dt;
                        break;
                }
                l.phase += inc;
                if (l.phase >= 1.0 || l.phase < 0.0)
                    l.phase -= std::floor(l.phase);
            }
            l.phaseOut.store((float) l.phase, std::memory_order_relaxed);
        }
    }

    float lfoValue(int index, float phaseOffset01) const noexcept override
    {
        if (index < 0 || index >= kNumLfos) return 0.0f;
        const auto& l = lfo_[(size_t) index];
        return shapeAt(l.shape.load(std::memory_order_relaxed),
                       l.phaseOut.load(std::memory_order_relaxed) + phaseOffset01,
                       l.skew .load(std::memory_order_relaxed),
                       l.steps.load(std::memory_order_relaxed),
                       index);
    }

    bool lfoRunning(int index) const noexcept override
    {
        return index >= 0 && index < kNumLfos
            && lfo_[(size_t) index].active.load(std::memory_order_relaxed)
            && lfo_[(size_t) index].run   .load(std::memory_order_relaxed);
    }

    bool lfoExists(int index) const noexcept override
    {
        return index >= 0 && index < kNumLfos
            && lfo_[(size_t) index].active.load(std::memory_order_relaxed);
    }

    int lfoAdd() override
    {
        for (int i = 0; i < kNumLfos; ++i)
            if (! lfo_[(size_t) i].active.load(std::memory_order_relaxed))
            {
                set(i, Config{});   // a fresh LFO, whatever the slot held before
                lfo_[(size_t) i].active.store(true, std::memory_order_relaxed);
                edited();
                return i;
            }
        return -1;
    }

    /** Give the slot back. The LFO stops driving anything at once (it no
     *  longer "runs"); the mappings that name it are the caller's to drop
     *  — the bank does not know them. */
    void remove(int i)
    {
        if (i < 0 || i >= kNumLfos) return;
        lfo_[(size_t) i].active.store(false, std::memory_order_relaxed);
        edited();
    }

    void setActive(int i, bool on)
    {
        if (i < 0 || i >= kNumLfos) return;
        if (lfo_[(size_t) i].active.exchange(on, std::memory_order_relaxed) != on)
            edited();
    }

    int activeCount() const noexcept
    {
        int n = 0;
        for (int i = 0; i < kNumLfos; ++i) if (lfoExists(i)) ++n;
        return n;
    }
    /** Bit i set = slot i in use. Cheap to poll, cheap to compare. */
    uint32_t activeMask() const noexcept
    {
        uint32_t m = 0;
        for (int i = 0; i < kNumLfos; ++i) if (lfoExists(i)) m |= (1u << i);
        return m;
    }

    /** Restart a cycle — message thread (a button) or audio thread (a mapped
     *  action target): the flag is consumed by the next tick(). */
    void retrig(int index) noexcept
    {
        if (index >= 0 && index < kNumLfos)
            lfo_[(size_t) index].retrig.store(true, std::memory_order_release);
    }

    //==========================================================================
    // Audio thread — the LFO's own parameters, played by a mapped controller
    //--------------------------------------------------------------------------
    // Same fields as set(), one at a time, from a normalised 0..1 — atomics
    // only, no autosave hook (a std::function call has no place on the audio
    // thread): the edit raises a flag the message thread drains instead.
    //==========================================================================
    void applyVirtual(int index, int which, float norm01) noexcept;
    float readVirtual(int index, int which) const noexcept;

    /** Message thread: true once after a mapped controller changed an LFO —
     *  the processor's timer turns it into a session autosave. */
    bool takeExternallyEdited() noexcept
    { return extEdit_.exchange(false, std::memory_order_acq_rel); }

    //==========================================================================
    // Message thread — configuration
    //==========================================================================
    Config get(int i) const
    {
        Config c;
        if (i < 0 || i >= kNumLfos) return c;
        const auto& l = lfo_[(size_t) i];
        c.shape = l.shape.load(std::memory_order_relaxed);
        c.clock = l.clock.load(std::memory_order_relaxed);
        c.rate  = l.rate .load(std::memory_order_relaxed);
        c.div   = l.div  .load(std::memory_order_relaxed);
        c.lines = l.lines.load(std::memory_order_relaxed);
        c.skew  = l.skew .load(std::memory_order_relaxed);
        c.steps = l.steps.load(std::memory_order_relaxed);
        c.run   = l.run  .load(std::memory_order_relaxed);
        return c;
    }

    void set(int i, const Config& c)
    {
        if (i < 0 || i >= kNumLfos) return;
        auto& l = lfo_[(size_t) i];
        l.shape.store(juce::jlimit(0, NumShapes - 1, c.shape), std::memory_order_relaxed);
        l.clock.store(juce::jlimit(0, NumClocks - 1, c.clock), std::memory_order_relaxed);
        l.rate .store(juce::jlimit(kMinRate, kMaxRate, c.rate), std::memory_order_relaxed);
        l.div  .store(juce::jlimit(0, kNumDivs - 1, c.div),     std::memory_order_relaxed);
        l.lines.store(juce::jlimit(kMinLines, kMaxLines, c.lines), std::memory_order_relaxed);
        l.skew .store(juce::jlimit(0.05f, 0.95f, c.skew),       std::memory_order_relaxed);
        l.steps.store(juce::jlimit(0, kMaxSteps, c.steps),      std::memory_order_relaxed);
        l.run  .store(c.run, std::memory_order_relaxed);
        edited();
    }

    /** The phase a row draws its dot at (0..1). Message thread. */
    float phaseOf(int i) const noexcept
    {
        return (i >= 0 && i < kNumLfos)
             ? lfo_[(size_t) i].phaseOut.load(std::memory_order_relaxed) : 0.0f;
    }

    /** Frequency the LFO is actually running at, for the row's readout —
     *  resolved through its clock (SCAN needs the live line rate, which the
     *  processor publishes here once per block). Message thread. */
    float freqOf(int i) const noexcept
    {
        if (i < 0 || i >= kNumLfos) return 0.0f;
        const auto& l = lfo_[(size_t) i];
        switch (l.clock.load(std::memory_order_relaxed))
        {
            case Sync: return (float) ((effectiveBpm() / 60.0)
                                       / divBeats(l.div.load(std::memory_order_relaxed)));
            case Scan: return linesPerSec_.load(std::memory_order_relaxed)
                            / (float) juce::jmax(1, l.lines.load(std::memory_order_relaxed));
            default:   return l.rate.load(std::memory_order_relaxed);
        }
    }

    /** The measured scan rate (lines/s) tick() saw — the SCAN rows' readout. */
    float lineRate() const noexcept { return linesPerSec_.load(std::memory_order_relaxed); }

    //── The SYNC clock's tempo ─────────────────────────────────────────────
    /** The bank's own tempo (BPM) — what SYNC runs on when no host publishes
     *  one. Persisted with the bank. */
    float tempo() const noexcept { return tempo_.load(std::memory_order_relaxed); }
    void  setTempo(float bpm)
    {
        const float t = juce::jlimit(kMinTempo, kMaxTempo, bpm);
        if (std::abs(t - tempo()) < 1.0e-4f) return;
        tempo_.store(t, std::memory_order_relaxed);
        edited();
    }
    /** What the host's play head published last block, 0 when it published
     *  none — the panel shows it read-only instead of the tempo knob. */
    float hostBpm() const noexcept { return hostBpm_.load(std::memory_order_relaxed); }
    /** The tempo SYNC actually runs on: the host's when there is one, else
     *  the bank's own. Any thread. */
    double effectiveBpm() const noexcept
    {
        const float h = hostBpm();
        return (double) juce::jmax(1.0f, h > 0.0f ? h : tempo());
    }

    /** Session autosave hook — every configuration edit calls it. */
    std::function<void()> onEdited;

    //==========================================================================
    // Message thread — persistence (<LFOS> next to <CHAINS> / <MIDI_MAPPINGS>)
    //==========================================================================
    juce::ValueTree toValueTree() const
    {
        juce::ValueTree root("LFOS");
        root.setProperty("tempo", tempo(), nullptr);
        for (int i = 0; i < kNumLfos; ++i)
        {
            const Config c = get(i);
            juce::ValueTree n("LFO");
            n.setProperty("idx",    i,       nullptr);
            n.setProperty("active", lfoExists(i), nullptr);
            n.setProperty("shape",  c.shape, nullptr);
            n.setProperty("clock", c.clock, nullptr);
            n.setProperty("rate",  c.rate,  nullptr);
            n.setProperty("div",   c.div,   nullptr);
            n.setProperty("lines", c.lines, nullptr);
            n.setProperty("skew",  c.skew,  nullptr);
            n.setProperty("steps", c.steps, nullptr);
            n.setProperty("run",   c.run,   nullptr);
            root.appendChild(n, nullptr);
        }
        return root;
    }

    void restoreFromValueTree(const juce::ValueTree& tree)
    {
        if (! tree.isValid() || ! tree.hasType("LFOS"))
            return;
        // Loading a session is not editing it: park the autosave hook so the
        // restore does not mark the state dirty eight times over.
        const auto hook = onEdited;
        onEdited = nullptr;
        const juce::ScopeGuard restoreHook { [this, hook] { onEdited = hook; } };
        // Sessions from before the bank had a tempo ran SYNC at 120: keep that.
        tempo_.store(juce::jlimit(kMinTempo, kMaxTempo,
                                  (float) (double) tree.getProperty("tempo", kDefTempo)),
                     std::memory_order_relaxed);
        for (const auto& n : tree)
        {
            if (! n.hasType("LFO")) continue;
            const int i = (int) n.getProperty("idx", -1);
            if (i < 0 || i >= kNumLfos) continue;
            Config c;
            c.shape = (int)   n.getProperty("shape", c.shape);
            c.clock = (int)   n.getProperty("clock", c.clock);
            c.rate  = (float) (double) n.getProperty("rate", c.rate);
            c.div   = (int)   n.getProperty("div",   c.div);
            c.lines = (int)   n.getProperty("lines", c.lines);
            c.skew  = (float) (double) n.getProperty("skew", c.skew);
            c.steps = (int)   n.getProperty("steps", c.steps);
            c.run   = (bool)  n.getProperty("run",   c.run);
            set(i, c);
            // Sessions from before the slots were "in use or not" saved all
            // eight without the flag: keep the ones that were configured
            // (the processor also revives any a mapping still names).
            const bool active = n.hasProperty("active")
                              ? (bool) n.getProperty("active")
                              : ! isDefault(c);
            lfo_[(size_t) i].active.store(active, std::memory_order_relaxed);
        }
    }

private:
    void edited() const { if (onEdited) onEdited(); }

    static bool isDefault(const Config& c) noexcept
    {
        const Config d;
        return c.shape == d.shape && c.clock == d.clock && c.div == d.div
            && c.lines == d.lines && c.steps == d.steps
            && std::abs(c.rate - d.rate) < 1e-6f && std::abs(c.skew - d.skew) < 1e-6f;
    }

    /** The step index of a phase in an n-step cycle, wrapped. */
    static int step(float p, int n) noexcept
    {
        const int i = (int) std::floor(p * (float) juce::jmax(1, n));
        return ((i % n) + n) % n;
    }

    struct Lfo
    {
        std::atomic<int>   shape { Sine };
        std::atomic<int>   clock { Free };
        std::atomic<float> rate  { kDefRate };
        std::atomic<int>   div   { 2 };
        std::atomic<int>   lines { kDefLines };
        std::atomic<float> skew  { kDefSkew };
        std::atomic<int>   steps { 0 };
        std::atomic<bool>  run   { true };
        std::atomic<bool>  active { false };   ///< in use by the session

        std::atomic<bool>  retrig { false };
        std::atomic<float> phaseOut { 0.0f };   ///< published for the UI + lfoValue
        double             phase { 0.0 };       ///< audio thread only
    };

    std::array<Lfo, kNumLfos> lfo_;
    std::atomic<bool>   extEdit_ { false };
    std::atomic<double> sampleRate_ { 0.0 };
    std::atomic<float>  linesPerSec_ { 0.0f };
    std::atomic<float>  tempo_   { kDefTempo }; ///< the bank's own, BPM
    std::atomic<float>  hostBpm_ { 0.0f };      ///< the host's last word, 0 = none
    uint64_t            lastScan_ { 0 };        ///< audio thread only

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LfoBank)
};

//==============================================================================
/** The LFO's OWN parameters as virtual MIDI targets — "lfo:{index}:{what}".
 *
 *  A knob on the rate, a pad on RETRIG, a CIS HIT that restarts a cycle: the
 *  bank is played like anything else. And since these are ordinary mapping
 *  targets, an LFO can be the SOURCE of a mapping whose target is another
 *  LFO's rate — cross-modulation with no extra machinery.
 *
 *  kFlag is bit 27, disjoint from the sampler encoding (engine << 16, bits
 *  0..19), from EqHandleMidiTargets (bit 28) and from the processor's freeze
 *  (bit 29) / diff (bit 30) flags.
 */
namespace LfoMidiTargets
{
    constexpr int kFlag = 0x08000000;

    // Appended, never reordered: the persisted id is the TOKEN ("lfo:2:rate"),
    // but a saved targetId encoding must keep meaning across versions.
    enum Which { Rate = 0, Skew, Steps, ShapeSel, DivSel, Run, Retrig,
                 ClockSel, WhichCount };

    inline const char* whichToken(int w) noexcept
    {
        switch (w)
        {
            case Rate:     return "rate";
            case Skew:     return "skew";
            case Steps:    return "steps";
            case ShapeSel: return "shape";
            case DivSel:   return "div";
            case Run:      return "run";
            case Retrig:   return "retrig";
            case ClockSel: return "clock";
            default:       return "";
        }
    }
    /** The name the identity label shows after "LFO 3 · ". */
    inline const char* whichName(int w) noexcept
    {
        switch (w)
        {
            case Rate:     return "Rate";
            case Skew:     return "Skew";
            case Steps:    return "Steps";
            case ShapeSel: return "Shape";
            case DivSel:   return "Division";
            case Run:      return "Run";
            case Retrig:   return "Retrig";
            case ClockSel: return "Clock";
            default:       return "";
        }
    }

    inline juce::String makeId(int index, int which)
    { return "lfo:" + juce::String(index) + ":" + whichToken(which); }

    inline int  encode (int index, int which) noexcept
    { return kFlag | ((index & 0xF) << 8) | (which & 0xF); }
    inline bool isLfo  (int t) noexcept { return (t & kFlag) != 0; }
    inline int  tIndex (int t) noexcept { return (t >> 8) & 0xF; }
    inline int  tWhich (int t) noexcept { return t & 0xF; }

    /** "lfo:3:rate" → targetId, or -1 when it is not one of ours. Message
     *  thread (String work). */
    inline int resolve(const juce::String& id)
    {
        if (! id.startsWith("lfo:")) return -1;
        const juce::String rest = id.substring(4);
        const int colon = rest.indexOfChar(':');
        if (colon <= 0) return -1;
        const juce::String num = rest.substring(0, colon);
        if (! num.containsOnly("0123456789")) return -1;
        const int index = num.getIntValue();
        if (index < 0 || index >= LfoBank::kNumLfos) return -1;
        const juce::String what = rest.substring(colon + 1);
        for (int w = 0; w < WhichCount; ++w)
            if (what == whichToken(w))
                return encode(index, w);
        return -1;
    }

    /** IVirtualMidiSink::virtualSteps code: >= 1 value target, -2 one-shot. */
    inline int steps(int which) noexcept
    {
        switch (which)
        {
            case ShapeSel: return LfoBank::NumShapes;      // discrete list
            case DivSel:   return LfoBank::kNumDivs;
            case ClockSel: return LfoBank::NumClocks;
            case Steps:    return LfoBank::kMaxSteps + 1;  // 0 (off) .. 16
            case Run:      return 2;                       // toggle
            case Retrig:   return -2;                      // one-shot action
            default:       return 0;                       // continuous
        }
    }

    /** Normalised 0..1 → the parameter's own value, and back. The rate is
     *  exponential so the low end (a drift under 0.1 Hz) keeps resolution. */
    inline float rateFromNorm(float n) noexcept
    {
        return LfoBank::kMinRate
             * std::pow(LfoBank::kMaxRate / LfoBank::kMinRate, juce::jlimit(0.0f, 1.0f, n));
    }
    inline float normFromRate(float hz) noexcept
    {
        const float r = juce::jlimit(LfoBank::kMinRate, LfoBank::kMaxRate, hz);
        return std::log(r / LfoBank::kMinRate)
             / std::log(LfoBank::kMaxRate / LfoBank::kMinRate);
    }
} // namespace LfoMidiTargets

//==============================================================================
// LfoBank — the audio-thread setters (defined here: they speak in
// LfoMidiTargets' Which vocabulary, which the class above cannot see yet).
//==============================================================================
inline void LfoBank::applyVirtual(int index, int which, float norm01) noexcept
{
    if (index < 0 || index >= kNumLfos) return;
    auto& l = lfo_[(size_t) index];
    const float n = juce::jlimit(0.0f, 1.0f, norm01);
    switch (which)
    {
        case LfoMidiTargets::Rate:
            l.rate.store(LfoMidiTargets::rateFromNorm(n), std::memory_order_relaxed); break;
        case LfoMidiTargets::Skew:
            l.skew.store(0.05f + 0.9f * n, std::memory_order_relaxed); break;
        case LfoMidiTargets::Steps:
            l.steps.store((int) std::lround(n * (float) kMaxSteps), std::memory_order_relaxed); break;
        case LfoMidiTargets::ShapeSel:
            l.shape.store((int) std::lround(n * (float) (NumShapes - 1)), std::memory_order_relaxed); break;
        case LfoMidiTargets::DivSel:
            l.div.store((int) std::lround(n * (float) (kNumDivs - 1)), std::memory_order_relaxed); break;
        case LfoMidiTargets::ClockSel:
            l.clock.store((int) std::lround(n * (float) (NumClocks - 1)), std::memory_order_relaxed); break;
        case LfoMidiTargets::Run:
            l.run.store(n >= 0.5f, std::memory_order_relaxed); break;
        case LfoMidiTargets::Retrig:
            if (n >= 0.5f) retrig(index);
            return;   // an action changes no configuration to save
        default: return;
    }
    extEdit_.store(true, std::memory_order_release);
}

inline float LfoBank::readVirtual(int index, int which) const noexcept
{
    if (index < 0 || index >= kNumLfos) return 0.0f;
    const auto& l = lfo_[(size_t) index];
    switch (which)
    {
        case LfoMidiTargets::Rate:
            return LfoMidiTargets::normFromRate(l.rate.load(std::memory_order_relaxed));
        case LfoMidiTargets::Skew:
            return (l.skew.load(std::memory_order_relaxed) - 0.05f) / 0.9f;
        case LfoMidiTargets::Steps:
            return (float) l.steps.load(std::memory_order_relaxed) / (float) kMaxSteps;
        case LfoMidiTargets::ShapeSel:
            return (float) l.shape.load(std::memory_order_relaxed) / (float) (NumShapes - 1);
        case LfoMidiTargets::DivSel:
            return (float) l.div.load(std::memory_order_relaxed) / (float) (kNumDivs - 1);
        case LfoMidiTargets::ClockSel:
            return (float) l.clock.load(std::memory_order_relaxed) / (float) (NumClocks - 1);
        case LfoMidiTargets::Run:
            return l.run.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        default: return 0.0f;
    }
}
