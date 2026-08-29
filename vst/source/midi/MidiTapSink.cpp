/*
 * MidiTapSink.cpp — see MidiTapSink.h for the threading contract.
 *
 * Author: zhonx
 */
#include "MidiTapSink.h"
#include "../processing/midi_tap.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace
{
/* Ticks per quarter note written into every SMF. 960 gives ~0.5 ms of grid at
 * 120 BPM — an order finer than the ~4 ms scanline period, so the extraction
 * timing, not the file resolution, is the limiting factor. */
constexpr int    kPPQ           = 960;
constexpr int    kPollMs        = 10;
/* A take is capped so a forgotten REC cannot eat memory without bound: 1M
 * events ~= 64 MB of MidiMessageSequence. Surfaced through lastError(). */
constexpr int    kMaxFileEvents = 1000000;
}

struct MidiTapSink::Impl
{
    explicit Impl(int s) : slot(s)
    {
        // Anchor at the current write position: a freshly created sink must not
        // replay whatever the probe pushed before it existed.
        if (auto* st = midi_tap_instance(slot))
        {
            cursor = midi_tap_ring_writepos(st);
            lastGen = midi_tap_generation(st);
        }
        thread = std::thread([this] { run(); });
    }

    ~Impl()
    {
        quit.store(true, std::memory_order_release);
        cv.notify_all();
        if (thread.joinable()) thread.join();
        // Never leave a destination holding notes we will no longer release.
        releaseHeld(midi_tap_now_us());
        port.reset();
    }

    //── Drain thread ──────────────────────────────────────────────────────────
    void run()
    {
        while (! quit.load(std::memory_order_acquire))
        {
            {
                std::unique_lock<std::mutex> lk(sleepLock);
                cv.wait_for(lk, std::chrono::milliseconds(kPollMs));
            }
            drain();
        }
    }

    void drain()
    {
        auto* st = midi_tap_instance(slot);
        if (st == nullptr) return;

        // Generation FIRST: a re-init resets write_index to 0 under a cursor
        // that may be far ahead, so every other computation is meaningless
        // until we re-anchor (VideoScrollRenderCore uses the same ordering).
        const uint32_t gen = midi_tap_generation(st);
        if (gen != lastGen)
        {
            lastGen = gen;
            releaseHeld(midi_tap_now_us());
            cursor = midi_tap_ring_writepos(st);
            return;
        }

        const bool wantPort = port != nullptr;
        const bool wantFile = fileOpen.load(std::memory_order_acquire);
        if (! wantPort && ! wantFile)
        {
            // Nothing consumes: keep re-anchoring so arming later does not dump
            // a 256 ms backlog of stale notes.
            cursor = midi_tap_ring_writepos(st);
            return;
        }

        // Live-output mode edges (drain thread owns the live MPE state).
        // Any flip changes the stream's identity, so everything sounding on
        // the port is released first — stuck notes are the alternative. The
        // dense restrikes re-open the surviving notes within one retrig.
        const bool wantMpe = portMpe.load(std::memory_order_relaxed);
        const bool wantOut = outOn.load(std::memory_order_relaxed);
        if (wantMpe != mpeLiveNow || wantOut != outOnNow)
        {
            const juce::uint64 now = midi_tap_now_us();
            mpeLiveAllOff(now);
            const int ch = channel.load(std::memory_order_relaxed);
            for (int n = 0; n < 128; ++n)
                if (held[n])
                    portSend(juce::MidiMessage::noteOff(ch, n), now);
            // Belt and braces — the PANIC: CC 120 (All Sound Off) + CC 123
            // (All Notes Off) on every channel also clears voices the
            // receiver holds from a state we no longer track. Toggling OUT
            // off is therefore a true general note-off, whatever happened.
            for (int c = 1; c <= 16; ++c)
            {
                portSend(juce::MidiMessage::allSoundOff(c), now);
                portSend(juce::MidiMessage::allNotesOff(c), now);
            }
            mpeLiveNow = wantMpe;
            outOnNow   = wantOut;
            if (mpeLiveNow && outOnNow)
                sendMpeZoneConfig(now);

            // Re-strike everything the EXTRACTOR still holds, in the new
            // mode: on static content (held lines) no dense restrike is
            // coming to re-open the voices, so without this a mode flip
            // mid-hold would simply mute the stream for good. The band's
            // latched velocity is a plain byte — safe to read across.
            if (outOnNow)
                for (int n = 0; n < 128; ++n)
                {
                    if (! held[n]) continue;
                    const uint8_t vel = st->bands[n].vel != 0
                                            ? st->bands[n].vel : (uint8_t) 100;
                    if (mpeLiveNow)
                    {
                        MidiTapEvent e {};
                        e.t_us = now; e.status = 0x90;
                        e.note = (uint8_t) n; e.vel = vel;
                        mpeLiveNote(e);
                    }
                    else
                    {
                        portSend(juce::MidiMessage::noteOn(
                            ch, n, (juce::uint8) scaledVel((int) vel)), now);
                    }
                }
        }

        uint32_t dropped = 0;
        uint32_t avail = midi_tap_ring_available(st, cursor, &dropped);
        if (dropped > 0)
        {
            // We lost the oldest events: our held[] shadow no longer matches
            // reality, so release everything rather than risk hung notes.
            releaseHeld(midi_tap_now_us());
            overruns.fetch_add(1, std::memory_order_relaxed);
        }

        for (uint32_t k = 0; k < avail; ++k)
        {
            MidiTapEvent e;
            if (! midi_tap_ring_get(st, cursor + k, &e))
                continue;   // torn slot — discard, but still advance the cursor
            dispatch(e);
        }
        cursor += avail;

        // A held-back off whose restrike never came (the off closed a phrase
        // and the stream went quiet) must not dangle into a STUCK note:
        // restrike pairs share one stamp, so past a few ms nothing can claim
        // it any more — release it.
        if (lPendingOff.valid
            && midi_tap_now_us() - lPendingOff.t > 20000ull)
            mpeLiveFlushPendingOff();

        // Level moves must be HEARD on held notes too, not only at the next
        // strike: re-express every live MPE voice (pressure + CC11), or the
        // whole classic channel (CC11 expression) in plain-notes mode.
        const float lvl = outLevel.load(std::memory_order_relaxed);
        if (outOnNow && port != nullptr
            && std::abs(lvl - outLevelNow) > 0.005f)
        {
            const juce::uint64 now = midi_tap_now_us();
            if (mpeLiveNow)
            {
                for (int n = 0; n < 128; ++n)
                {
                    const int c = lChanOfBand[n];
                    if (c == 0) continue;
                    const int v = scaledVel((int) lLastVel[n]);
                    portSend(juce::MidiMessage::channelPressureChange(c, v), now);
                    portSend(juce::MidiMessage::controllerEvent(c, 11, v), now);
                }
            }
            else
            {
                portSend(juce::MidiMessage::controllerEvent(
                    channel.load(std::memory_order_relaxed), 11,
                    juce::jlimit(0, 127, (int) std::lround(127.0f * lvl))), now);
            }
            outLevelNow = lvl;
        }
    }

    void dispatch(const MidiTapEvent& e)
    {
        if (e.status == 0xE0)
        {
            // Crest bend: per-voice pitch — MPE consumers only (the plain
            // port and the note file are single-channel views where a global
            // bend would wobble the whole chord).
            mpeBend(e);        // file translator
            mpeLiveBend(e);    // live translator (port)
            return;
        }
        if (e.status != 0x90 && e.status != 0x80)
            return;

        const int ch   = channel.load(std::memory_order_relaxed);
        const bool on  = (e.status == 0x90) && e.vel > 0;

        held[e.note] = on ? 1 : 0;
        emitFile(on ? juce::MidiMessage::noteOn (ch, (int) e.note, (juce::uint8) e.vel)
                    : juce::MidiMessage::noteOff(ch, (int) e.note), e.t_us, on);

        if (outOnNow && ! mpeLiveNow)
            portSend(on ? juce::MidiMessage::noteOn (ch, (int) e.note,
                                                     (juce::uint8) scaledVel(e.vel))
                        : juce::MidiMessage::noteOff(ch, (int) e.note), e.t_us);

        mpeNote(e);            // file translator
        mpeLiveNote(e);        // live translator (port)
    }

    int scaledVel(int vel) const noexcept
    {
        // Never below 1: a zero note-on would read as a note-off downstream.
        return juce::jlimit(1, 127, (int) std::lround(
            (double) vel * (double) outLevel.load(std::memory_order_relaxed)));
    }

    //── MPE view of the take ──────────────────────────────────────────────────
    // Every take is ALSO written as "<stem>_MPE.mid": one MPE member channel
    // (2..16) per sounding note, the 0xE0 crest stream as per-channel pitch
    // bend (range +/-2 st) and the dense restrikes folded into CHANNEL
    // PRESSURE — the envelope becomes a continuous controller instead of a
    // machine-gun of off/on pairs. Voices beyond the 15-channel budget are
    // simply absent here (the note file still has everything): the two files
    // are the two capture philosophies, side by side.
    //
    // All mpe* state below is touched by the DRAIN THREAD only; startFile
    // resets it before fileOpen turns visible, stopFile snapshots under
    // fileLock after it turned false.

    static juce::MidiMessage mpeBendMsg(int chan, int cb)
    {
        // +/-2 st member range: 100 cents = 4096 wheel units.
        const int wheel = juce::jlimit(0, 0x3FFF, 0x2000 + cb * 0x2000 / 200);
        return juce::MidiMessage::pitchWheel(chan, wheel);
    }

    void mpeAdd(const juce::MidiMessage& msgIn, juce::uint64 tUs)
    {
        std::lock_guard<std::mutex> lk(fileLock);
        if (! fileOpen.load(std::memory_order_relaxed)) return;
        if (mpeSeq.getNumEvents() >= kMaxFileEvents) { truncated = true; return; }
        const double sec = (tUs > t0Us) ? (double) (tUs - t0Us) * 1e-6 : 0.0;
        double tick = sec * (fileBpm / 60.0) * (double) kPPQ;
        if (tick < mpeLastTick) tick = mpeLastTick;
        mpeLastTick = tick;
        juce::MidiMessage m = msgIn;
        m.setTimeStamp(tick);
        mpeSeq.addEvent(m);
    }

    void mpeFlushPendingOff()
    {
        if (! mpePendingOff.valid) return;
        mpePendingOff.valid = false;
        const int c = mpeChanOfBand[mpePendingOff.note];
        if (c == 0) return;
        mpeAdd(juce::MidiMessage::noteOff(c, (int) mpePendingOff.note),
               mpePendingOff.t);
        mpeBusy &= (uint16_t) ~(1u << c);
        mpeChanOfBand[mpePendingOff.note] = 0;
    }

    void mpeForceOff(int note, juce::uint64 tUs)
    {
        if (mpePendingOff.valid && mpePendingOff.note == (uint8_t) note)
            mpePendingOff.valid = false;
        const int c = mpeChanOfBand[note];
        if (c == 0) return;
        mpeAdd(juce::MidiMessage::noteOff(c, note), tUs);
        mpeBusy &= (uint16_t) ~(1u << c);
        mpeChanOfBand[note] = 0;
    }

    void mpeBend(const MidiTapEvent& e)
    {
        if (! fileOpen.load(std::memory_order_acquire)) return;
        mpeFlushPendingOff();
        mpeLatestCb[e.note] = (int8_t) e.flags;
        const int c = mpeChanOfBand[e.note];
        if (c != 0)
            mpeAdd(mpeBendMsg(c, mpeLatestCb[e.note]), e.t_us);
    }

    void mpeNote(const MidiTapEvent& e)
    {
        if (! fileOpen.load(std::memory_order_acquire)) return;

        const bool on = (e.status == 0x90) && e.vel > 0;
        if (! on)
        {
            mpeFlushPendingOff();
            // Hold the off one event: if the next event is an on of the SAME
            // note at the SAME stamp it was a dense restrike, and the pair
            // folds into channel pressure instead of retriggering the voice.
            mpePendingOff = { true, e.note, e.t_us };
            return;
        }

        if (mpePendingOff.valid && mpePendingOff.note == e.note
            && mpePendingOff.t == e.t_us)
        {
            mpePendingOff.valid = false;
            const int c = mpeChanOfBand[e.note];
            if (c != 0)
            {
                // Pressure AND CC11: far more renderers map expression to
                // volume than channel pressure — belt and braces.
                mpeAdd(juce::MidiMessage::channelPressureChange(c, (int) e.vel),
                       e.t_us);
                mpeAdd(juce::MidiMessage::controllerEvent(c, 11, (int) e.vel),
                       e.t_us);
                return;
            }
            // Unmapped restrike (starved of a channel earlier): fall through
            // and open it as a fresh voice — channels may have freed since.
        }
        else
        {
            mpeFlushPendingOff();
        }
        int c = mpeChanOfBand[e.note];
        if (c == 0)
        {
            // Channel 10 is NEVER allocated: GM renderers hardwire it to
            // percussion, and a voice landing there plays as a drum kit
            // (MuseScore grew a "Drumset" track from our takes). 14 voices
            // is the GM-safe MPE budget.
            for (int m = 2; m <= 16; ++m)
                if (m != 10 && (mpeBusy & (1u << m)) == 0) { c = m; break; }
            if (c == 0) return;   // all voices busy — this one lives in the
                                  // note file only
            mpeBusy |= (uint16_t) (1u << c);
            mpeChanOfBand[e.note] = c;
        }
        mpeAdd(mpeBendMsg(c, mpeLatestCb[e.note]), e.t_us);
        mpeAdd(juce::MidiMessage::noteOn(c, (int) e.note, (juce::uint8) e.vel),
               e.t_us);
        mpeAdd(juce::MidiMessage::channelPressureChange(c, (int) e.vel), e.t_us);
        mpeAdd(juce::MidiMessage::controllerEvent(c, 11, (int) e.vel), e.t_us);
    }

    /** Schedule one message on the port. Slightly INTO THE FUTURE, released
     *  by JUCE's background thread: sendMessageNow() would inherit this
     *  thread's 10 ms poll jitter and deliver everything late and bunched —
     *  a constant few ms of latency beats variable jitter. */
    void portSend(const juce::MidiMessage& msgIn, juce::uint64 tUs)
    {
        if (port == nullptr) return;
        juce::MidiMessage m = msgIn;
        m.setTimeStamp(juceMsAtMono0 + (double) tUs * 1e-3
                       + portLatencyMs.load(std::memory_order_relaxed));
        juce::MidiBuffer one;
        one.addEvent(m, 0);
        port->sendBlockOfMessages(one, m.getTimeStamp(), 44100.0);
    }

    /** Append one message to the (dense) note-file take. */
    void emitFile(const juce::MidiMessage& msgIn, juce::uint64 tUs, bool isNoteOn)
    {
        if (! fileOpen.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lk(fileLock);
        if (! fileOpen.load(std::memory_order_relaxed)) return;
        if (seq.getNumEvents() >= kMaxFileEvents)
        {
            truncated = true;
            return;
        }
        const double sec = (tUs > t0Us) ? (double) (tUs - t0Us) * 1e-6 : 0.0;
        double tick = sec * (fileBpm / 60.0) * (double) kPPQ;
        if (tick < lastTick) tick = lastTick;   // strictly non-decreasing
        lastTick = tick;

        juce::MidiMessage m = msgIn;
        m.setTimeStamp(tick);
        seq.addEvent(m);
        if (isNoteOn) notes.fetch_add(1, std::memory_order_relaxed);
    }

    /** Release every note this SINK believes is held (its own shadow). */
    void releaseHeld(juce::uint64 tUs)
    {
        const int ch = channel.load(std::memory_order_relaxed);
        for (int n = 0; n < 128; ++n)
        {
            if (! held[n]) continue;
            held[n] = 0;
            emitFile(juce::MidiMessage::noteOff(ch, n), tUs, false);
            portSend(juce::MidiMessage::noteOff(ch, n), tUs);
            mpeForceOff(n, tUs);
        }
        mpeLiveAllOff(tUs);
    }

    //── Live MPE translator (port stream) ─────────────────────────────────────
    // The live twin of the file translator below: SEPARATE voice state, so a
    // take opening/closing can never steal or reset the channels of notes
    // currently sounding on the port. Drain-thread only. Velocities are
    // scaled by outLevel (monitoring gain); the files never are.

    void sendMpeZoneConfig(juce::uint64 tUs)
    {
        // Lower zone, 15 members, member bend range pinned to ±2 st (the
        // scale of mpeBendMsg). Synths that ignore RPNs need their MPE bend
        // range set to ±2 by hand or the vibrato depth reads wrong.
        portSend(juce::MidiMessage::controllerEvent(1, 101, 0), tUs);
        portSend(juce::MidiMessage::controllerEvent(1, 100, 6), tUs);
        portSend(juce::MidiMessage::controllerEvent(1,   6, 15), tUs);
        for (int c = 2; c <= 16; ++c)
        {
            portSend(juce::MidiMessage::controllerEvent(c, 101, 0), tUs);
            portSend(juce::MidiMessage::controllerEvent(c, 100, 0), tUs);
            portSend(juce::MidiMessage::controllerEvent(c,   6, 2), tUs);
        }
    }

    void mpeLiveAllOff(juce::uint64 tUs)
    {
        lPendingOff.valid = false;
        for (int n = 0; n < 128; ++n)
        {
            const int c = lChanOfBand[n];
            if (c == 0) continue;
            portSend(juce::MidiMessage::noteOff(c, n), tUs);
            lChanOfBand[n] = 0;
        }
        lBusy = 0;
    }

    void mpeLiveFlushPendingOff()
    {
        if (! lPendingOff.valid) return;
        lPendingOff.valid = false;
        const int c = lChanOfBand[lPendingOff.note];
        if (c == 0) return;
        portSend(juce::MidiMessage::noteOff(c, (int) lPendingOff.note),
                 lPendingOff.t);
        lBusy &= (uint16_t) ~(1u << c);
        lChanOfBand[lPendingOff.note] = 0;
    }

    void mpeLiveBend(const MidiTapEvent& e)
    {
        lLatestCb[e.note] = (int8_t) e.flags;   // track even while muted
        if (! (outOnNow && mpeLiveNow)) return;
        mpeLiveFlushPendingOff();
        const int c = lChanOfBand[e.note];
        if (c != 0)
            portSend(mpeBendMsg(c, lLatestCb[e.note]), e.t_us);
    }

    void mpeLiveNote(const MidiTapEvent& e)
    {
        if (! (outOnNow && mpeLiveNow)) return;

        const bool on = (e.status == 0x90) && e.vel > 0;
        if (! on)
        {
            mpeLiveFlushPendingOff();
            lPendingOff = { true, e.note, e.t_us };
            return;
        }

        if (lPendingOff.valid && lPendingOff.note == e.note
            && lPendingOff.t == e.t_us)
        {
            lPendingOff.valid = false;
            const int c = lChanOfBand[e.note];
            if (c != 0)
            {
                // Dense restrike → continuous envelope, no retrigger.
                lLastVel[e.note] = e.vel;
                const int v = scaledVel((int) e.vel);
                portSend(juce::MidiMessage::channelPressureChange(c, v), e.t_us);
                portSend(juce::MidiMessage::controllerEvent(c, 11, v), e.t_us);
                return;
            }
            // Unmapped restrike (voice was starved of a channel): open fresh.
        }
        else
        {
            mpeLiveFlushPendingOff();
        }

        int c = lChanOfBand[e.note];
        if (c == 0)
        {
            // Skip channel 10 — GM percussion (see the file translator).
            for (int m = 2; m <= 16; ++m)
                if (m != 10 && (lBusy & (1u << m)) == 0) { c = m; break; }
            if (c == 0) return;   // all voices busy — silent on the port
            lBusy |= (uint16_t) (1u << c);
            lChanOfBand[e.note] = c;
        }
        lLastVel[e.note] = e.vel;
        const int v = scaledVel((int) e.vel);
        portSend(mpeBendMsg(c, lLatestCb[e.note]), e.t_us);
        portSend(juce::MidiMessage::noteOn(c, (int) e.note, (juce::uint8) v), e.t_us);
        portSend(juce::MidiMessage::channelPressureChange(c, v), e.t_us);
        portSend(juce::MidiMessage::controllerEvent(c, 11, v), e.t_us);
    }

    //── State ─────────────────────────────────────────────────────────────────
    const int slot;

    std::thread              thread;
    std::atomic<bool>        quit { false };
    std::mutex               sleepLock;
    std::condition_variable  cv;

    uint32_t cursor  { 0 };
    uint32_t lastGen { 0 };
    uint8_t  held[128] {};
    std::atomic<int> overruns { 0 };

    // Port sink
    std::unique_ptr<juce::MidiOutput> port;
    juce::String  openedPortName;
    juce::String  displayName;    // "CHAIN 4a" — message thread only
    double        juceMsAtMono0 { 0.0 };
    std::atomic<double> portLatencyMs { 5.0 };
    std::atomic<int>    channel { 16 };

    // Live-output shaping (message thread writes, drain thread reads; the
    // *Now copies are the drain thread's view, flipped with edge handling).
    std::atomic<bool>  portMpe  { false };
    std::atomic<bool>  outOn    { true };
    std::atomic<float> outLevel { 1.0f };
    bool mpeLiveNow { false };
    bool outOnNow   { true };

    // Live MPE voice state (drain thread only — independent of the file's)
    uint8_t  lChanOfBand[128] {};
    uint16_t lBusy { 0 };
    int8_t   lLatestCb[128] {};
    uint8_t  lLastVel[128] {};      // unscaled — re-expressed on Level moves
    float    outLevelNow { 1.0f };  // last level the port was told about
    struct { bool valid; uint8_t note; juce::uint64 t; }
             lPendingOff { false, 0, 0 };

    // File sink
    std::mutex               fileLock;
    std::atomic<bool>        fileOpen { false };
    juce::MidiMessageSequence seq;
    juce::File                fileTarget;
    double                    fileBpm   { 120.0 };
    juce::uint64              t0Us      { 0 };
    double                    lastTick  { 0.0 };
    bool                      truncated { false };
    std::atomic<int>          notes { 0 };
    std::atomic<double>       fileMinNoteMs { 0.0 };
    std::atomic<int>          quantTicks { 0 };

    // MPE file sink (drain-thread state; see the MPE block comment above)
    juce::MidiMessageSequence mpeSeq;
    juce::File                mpeTarget;
    double                    mpeLastTick { 0.0 };
    uint8_t                   mpeChanOfBand[128] {};
    uint16_t                  mpeBusy { 0 };
    int8_t                    mpeLatestCb[128] {};
    struct { bool valid; uint8_t note; juce::uint64 t; }
                              mpePendingOff { false, 0, 0 };

    mutable std::mutex errLock;
    juce::String       err;
};

//==============================================================================
MidiTapSink::MidiTapSink(int slot) : impl(std::make_unique<Impl>(slot)) {}
MidiTapSink::~MidiTapSink() = default;

bool MidiTapSink::openPort(const juce::String& deviceName, juce::String& errOut)
{
    closePort();
    if (deviceName.isEmpty())
        return true;   // "None" — closing is success

    if (deviceName == "Virtual")
    {
        // createNewDevice only exists on CoreMIDI/ALSA — JUCE does not declare
        // it on Windows, so the call must be compiled out, not just null-checked.
#if JUCE_LINUX || JUCE_MAC || JUCE_IOS
        const juce::String name = impl->displayName.isNotEmpty()
            ? "Sp3ctra " + impl->displayName
            : "Sp3ctra MIDI TAP " + juce::String(impl->slot + 1);
        impl->port = juce::MidiOutput::createNewDevice(name);
#endif
        if (impl->port == nullptr)
        {
            errOut = "Virtual MIDI ports are not available on this platform. "
                     "Pick a hardware or IAC destination instead.";
            std::lock_guard<std::mutex> lk(impl->errLock);
            impl->err = errOut;
            return false;
        }
#if JUCE_LINUX || JUCE_MAC || JUCE_IOS
        impl->openedPortName = name;
#endif
    }
    else
    {
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            if (d.name == deviceName)
            {
                impl->port = juce::MidiOutput::openDevice(d.identifier);
                break;
            }
        if (impl->port == nullptr)
        {
            errOut = "MIDI destination \"" + deviceName + "\" is not available.";
            std::lock_guard<std::mutex> lk(impl->errLock);
            impl->err = errOut;
            return false;
        }
        impl->openedPortName = deviceName;
    }

    // Calibrate the monotonic→JUCE-ms offset ONCE, at open: every subsequent
    // event timestamp is derived from it, so the whole take shares one mapping.
    impl->juceMsAtMono0 = juce::Time::getMillisecondCounterHiRes()
                        - (double) midi_tap_now_us() * 1e-3;
    impl->port->startBackgroundThread();
    return true;
}

void MidiTapSink::closePort()
{
    if (impl->port == nullptr) return;
    impl->releaseHeld(midi_tap_now_us());
    impl->port->stopBackgroundThread();
    impl->port.reset();
    impl->openedPortName = {};
}

bool MidiTapSink::isPortOpen() const noexcept { return impl->port != nullptr; }
juce::String MidiTapSink::portName() const { return impl->openedPortName; }

bool MidiTapSink::startFile(const juce::File& out, double bpm,
                            juce::uint64 t0UsIn, juce::String& errOut)
{
    if (impl->fileOpen.load(std::memory_order_acquire))
    {
        errOut = "Already recording";
        return false;
    }
    if (! out.getParentDirectory().createDirectory())
    {
        errOut = "Cannot create " + out.getParentDirectory().getFullPathName();
        return false;
    }

    std::lock_guard<std::mutex> lk(impl->fileLock);
    impl->seq.clear();
    impl->fileTarget = out;
    impl->fileBpm    = (bpm > 1.0) ? bpm : 120.0;
    impl->t0Us       = t0UsIn;
    impl->lastTick   = 0.0;
    impl->truncated  = false;
    impl->notes.store(0, std::memory_order_relaxed);

    // MPE sibling take — reset BEFORE fileOpen turns visible, so the drain
    // thread (which only touches mpe state while fileOpen) sees it fresh.
    impl->mpeSeq.clear();
    impl->mpeTarget = out.getSiblingFile(
        out.getFileNameWithoutExtension() + "_MPE.mid");
    impl->mpeLastTick = 0.0;
    impl->mpeBusy     = 0;
    impl->mpePendingOff.valid = false;
    memset(impl->mpeChanOfBand, 0, sizeof(impl->mpeChanOfBand));
    memset(impl->mpeLatestCb,   0, sizeof(impl->mpeLatestCb));

    impl->fileOpen.store(true, std::memory_order_release);
    return true;
}

void MidiTapSink::stopFile()
{
    if (! impl->fileOpen.load(std::memory_order_acquire)) return;

    // Release held notes BEFORE closing so the file has no dangling note-on.
    impl->releaseHeld(midi_tap_now_us());

    juce::MidiMessageSequence out, mpeOut;
    juce::File target, mpeTarget;
    double bpm = 120.0, mpeEndTick = 0.0;
    bool   truncated = false;
    double minMs = impl->fileMinNoteMs.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(impl->fileLock);
        impl->fileOpen.store(false, std::memory_order_release);
        out       = impl->seq;
        target    = impl->fileTarget;
        bpm       = impl->fileBpm;
        truncated = impl->truncated;
        impl->seq.clear();

        mpeOut     = impl->mpeSeq;
        mpeTarget  = impl->mpeTarget;
        mpeEndTick = impl->mpeLastTick;
        impl->mpeSeq.clear();
        // Close every voice still mapped (releaseHeld above covers the sink
        // shadow; this sweeps whatever a mid-take discontinuity left behind).
        for (int n = 0; n < 128; ++n)
            if (impl->mpeChanOfBand[n] != 0)
            {
                mpeOut.addEvent(juce::MidiMessage::noteOff(
                                    (int) impl->mpeChanOfBand[n], n), mpeEndTick);
                impl->mpeChanOfBand[n] = 0;
            }
        impl->mpeBusy = 0;
        impl->mpePendingOff.valid = false;
    }

    out.updateMatchedPairs();

    // Optional file-only short-note filter (see the header: this makes the file
    // differ from what the ports emitted, hence off by default).
    if (minMs > 0.0)
    {
        const double minTicks = minMs * 1e-3 * (bpm / 60.0) * (double) kPPQ;
        for (int i = out.getNumEvents(); --i >= 0;)
        {
            auto* ev = out.getEventPointer(i);
            if (ev == nullptr || ! ev->message.isNoteOn() || ev->noteOffObject == nullptr)
                continue;
            if (ev->noteOffObject->message.getTimeStamp() - ev->message.getTimeStamp() < minTicks)
                out.deleteEvent(i, true);   // true = also delete the matched note-off
        }
        out.updateMatchedPairs();
    }

    // Rebuild from MATCHED PAIRS only. Two things happen here:
    //
    //  1. Orphan note-offs are dropped. The sink's held[] shadow is maintained
    //     continuously for the real-time port, so a note that was already
    //     sounding when the take opened would contribute a note-off with no
    //     note-on in the file (the raw takes showed 87 offs for 85 ons).
    //  2. Optional quantization snaps BOTH ends to the grid. MuseScore 4 has no
    //     per-score MIDI import panel any more, so a raw-timed file gets
    //     whatever the importer invents — usually tuplets everywhere.
    const int    qt   = impl->quantTicks.load(std::memory_order_relaxed);
    const double grid = (qt > 0) ? (double) qt : 0.0;
    auto snap = [grid](double t) { return grid > 0.0 ? std::round(t / grid) * grid : t; };

    juce::MidiMessageSequence notesOut;
    // Latest quantized end per pitch. Snapping can pull two consecutive notes of
    // the SAME pitch onto the same grid point, which emits on/on/off/off — most
    // players kill such a note at the first off. Events arrive time-sorted, so
    // holding the previous end per note number is enough to keep pairs disjoint,
    // and since that end is itself on-grid the pushed start stays on-grid too.
    double lastEnd[128];
    for (double& e : lastEnd) e = -1.0e9;

    int kept = 0, orphans = 0;
    for (int i = 0; i < out.getNumEvents(); ++i)
    {
        auto* ev = out.getEventPointer(i);
        if (ev == nullptr) continue;
        if (! ev->message.isNoteOn())
        {
            if (ev->message.isNoteOff() && ev->noteOffObject == nullptr)
                ++orphans;             // counted, then dropped
            continue;                  // note-offs are emitted with their pair
        }

        double on  = ev->message.getTimeStamp();
        double off = (ev->noteOffObject != nullptr)
                   ? ev->noteOffObject->message.getTimeStamp()
                   : on + (grid > 0.0 ? grid : (double) kPPQ / 4.0);

        if (grid > 0.0)
        {
            on  = snap(on);
            off = snap(off);
            // A note shorter than the grid must survive as ONE grid unit
            // rather than collapse to zero length (which players discard).
            if (off <= on) off = on + grid;
        }
        else if (off <= on)
        {
            off = on + 1.0;
        }

        const int note = juce::jlimit(0, 127, ev->message.getNoteNumber());
        if (on < lastEnd[note])
        {
            on = lastEnd[note];
            if (off <= on) off = on + (grid > 0.0 ? grid : 1.0);
        }
        lastEnd[note] = off;

        auto n = ev->message;
        n.setTimeStamp(on);
        notesOut.addEvent(n);
        notesOut.addEvent(juce::MidiMessage::noteOff(n.getChannel(),
                                                     n.getNoteNumber()), off);
        ++kept;
    }
    notesOut.updateMatchedPairs();

    juce::MidiFile mf;
    mf.setTicksPerQuarterNote(kPPQ);

    juce::MidiMessageSequence track;
    track.addEvent(juce::MidiMessage::tempoMetaEvent(
        (int) (60000000.0 / ((bpm > 1.0) ? bpm : 120.0))), 0.0);
    // Without an explicit meter every importer assumes 4/4 and drops bar lines
    // wherever it likes — the takes had no time signature at all.
    track.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4, 4), 0.0);
    track.addEvent(juce::MidiMessage::textMetaEvent(
        3, "Sp3ctra MIDI TAP " + juce::String(impl->slot + 1)
           + (impl->displayName.isNotEmpty() ? " - " + impl->displayName
                                             : juce::String())), 0.0);
    // GM 17 "Drawbar Organ" (program 16): instant attack, infinite sustain,
    // near-sine drawbars — the closest GM voice to LuxStral's held partials.
    // Without this every player opens the take on a decaying piano, which
    // contradicts the held-note capture. Players free to ignore it, ignore it.
    track.addEvent(juce::MidiMessage::programChange(
        impl->channel.load(std::memory_order_relaxed), 16), 0.0);
    track.addSequence(notesOut, 0.0);
    track.updateMatchedPairs();
    mf.addTrack(track);
    juce::ignoreUnused(kept, orphans);

    // ── The MPE sibling: same take, the other capture philosophy ──────────────
    juce::MidiFile mpeMf;
    mpeMf.setTicksPerQuarterNote(kPPQ);
    {
        juce::MidiMessageSequence t;
        t.addEvent(juce::MidiMessage::tempoMetaEvent(
            (int) (60000000.0 / ((bpm > 1.0) ? bpm : 120.0))), 0.0);
        t.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4, 4), 0.0);
        t.addEvent(juce::MidiMessage::textMetaEvent(
            3, "Sp3ctra MIDI TAP " + juce::String(impl->slot + 1)
               + (impl->displayName.isNotEmpty() ? " - " + impl->displayName
                                                 : juce::String())
               + " (MPE)"), 0.0);
        // MPE lower zone: channel 1 master, 15 members (RPN 6 = 15), and the
        // member bend range pinned to +/-2 st (RPN 0) — mpeBendMsg's scale.
        t.addEvent(juce::MidiMessage::controllerEvent(1, 101, 0), 0.0);
        t.addEvent(juce::MidiMessage::controllerEvent(1, 100, 6), 0.0);
        t.addEvent(juce::MidiMessage::controllerEvent(1,   6, 15), 0.0);
        for (int c = 2; c <= 16; ++c)
        {
            t.addEvent(juce::MidiMessage::controllerEvent(c, 101, 0), 0.0);
            t.addEvent(juce::MidiMessage::controllerEvent(c, 100, 0), 0.0);
            t.addEvent(juce::MidiMessage::controllerEvent(c,   6, 2), 0.0);
            t.addEvent(juce::MidiMessage::programChange(c, 16), 0.0);
        }
        t.addSequence(mpeOut, 0.0);
        t.updateMatchedPairs();
        mpeMf.addTrack(t);
    }

    juce::String problem;
    if (target != juce::File{})
    {
        target.deleteFile();
        if (auto stream = target.createOutputStream())
        {
            if (! mf.writeTo(*stream))
                problem = "Could not write " + target.getFullPathName();
        }
        else
        {
            problem = "Could not open " + target.getFullPathName();
        }
    }
    if (mpeTarget != juce::File{})
    {
        mpeTarget.deleteFile();
        if (auto stream = mpeTarget.createOutputStream())
        {
            if (! mpeMf.writeTo(*stream) && problem.isEmpty())
                problem = "Could not write " + mpeTarget.getFullPathName();
        }
        else if (problem.isEmpty())
        {
            problem = "Could not open " + mpeTarget.getFullPathName();
        }
    }
    if (truncated && problem.isEmpty())
        problem = "Recording exceeded " + juce::String(kMaxFileEvents)
                + " events and was truncated.";
    if (problem.isNotEmpty())
    {
        std::lock_guard<std::mutex> lk(impl->errLock);
        impl->err = problem;
    }
}

bool MidiTapSink::isFileOpen() const noexcept
{ return impl->fileOpen.load(std::memory_order_acquire); }

int MidiTapSink::noteCount() const noexcept
{ return impl->notes.load(std::memory_order_relaxed); }

void MidiTapSink::setChannel(int ch) noexcept
{ impl->channel.store(juce::jlimit(1, 16, ch), std::memory_order_relaxed); }

void MidiTapSink::setPortLatencyMs(double ms) noexcept
{ impl->portLatencyMs.store(juce::jlimit(0.0, 100.0, ms), std::memory_order_relaxed); }

void MidiTapSink::setPortMpe(bool mpe) noexcept
{ impl->portMpe.store(mpe, std::memory_order_relaxed); }

void MidiTapSink::setOutEnabled(bool on) noexcept
{ impl->outOn.store(on, std::memory_order_relaxed); }

void MidiTapSink::setOutLevel(double level01) noexcept
{ impl->outLevel.store((float) juce::jlimit(0.0, 1.0, level01), std::memory_order_relaxed); }

void MidiTapSink::setDisplayName(const juce::String& name)
{ impl->displayName = name; }

void MidiTapSink::setFileMinNoteMs(double ms) noexcept
{ impl->fileMinNoteMs.store(juce::jmax(0.0, ms), std::memory_order_relaxed); }

void MidiTapSink::setQuantizeTicks(int ticks) noexcept
{ impl->quantTicks.store(juce::jmax(0, ticks), std::memory_order_relaxed); }

juce::String MidiTapSink::lastError() const
{
    std::lock_guard<std::mutex> lk(impl->errLock);
    return impl->err;
}
