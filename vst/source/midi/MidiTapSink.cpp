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
    }

    void dispatch(const MidiTapEvent& e)
    {
        const int ch   = channel.load(std::memory_order_relaxed);
        const bool on  = (e.status == 0x90) && e.vel > 0;
        const auto msg = on ? juce::MidiMessage::noteOn (ch, (int) e.note, (juce::uint8) e.vel)
                            : juce::MidiMessage::noteOff(ch, (int) e.note);

        held[e.note] = on ? 1 : 0;
        emit(msg, e.t_us, on);
    }

    /** Send one message to whichever sinks are enabled. `tUs` is the producer's
     *  absolute monotonic stamp. */
    void emit(const juce::MidiMessage& msgIn, juce::uint64 tUs, bool isNoteOn)
    {
        if (port != nullptr)
        {
            // Schedule slightly INTO THE FUTURE and let JUCE's background
            // thread release it on time: sendMessageNow() would inherit this
            // thread's 10 ms poll jitter and deliver everything late and
            // bunched. A constant few ms of latency beats variable jitter.
            juce::MidiMessage m = msgIn;
            m.setTimeStamp(juceMsAtMono0 + (double) tUs * 1e-3
                           + portLatencyMs.load(std::memory_order_relaxed));
            juce::MidiBuffer one;
            one.addEvent(m, 0);
            port->sendBlockOfMessages(one, m.getTimeStamp(), 44100.0);
        }

        if (fileOpen.load(std::memory_order_acquire))
        {
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
    }

    /** Release every note this SINK believes is held (its own shadow). */
    void releaseHeld(juce::uint64 tUs)
    {
        const int ch = channel.load(std::memory_order_relaxed);
        for (int n = 0; n < 128; ++n)
        {
            if (! held[n]) continue;
            held[n] = 0;
            emit(juce::MidiMessage::noteOff(ch, n), tUs, false);
        }
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
    double        juceMsAtMono0 { 0.0 };
    std::atomic<double> portLatencyMs { 5.0 };
    std::atomic<int>    channel { 16 };

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
        const juce::String name = "Sp3ctra MIDI TAP " + juce::String(impl->slot + 1);
        impl->port = juce::MidiOutput::createNewDevice(name);
        if (impl->port == nullptr)
        {
            errOut = "Virtual MIDI ports are not available on this platform. "
                     "Pick a hardware or IAC destination instead.";
            std::lock_guard<std::mutex> lk(impl->errLock);
            impl->err = errOut;
            return false;
        }
        impl->openedPortName = name;
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
    impl->fileOpen.store(true, std::memory_order_release);
    return true;
}

void MidiTapSink::stopFile()
{
    if (! impl->fileOpen.load(std::memory_order_acquire)) return;

    // Release held notes BEFORE closing so the file has no dangling note-on.
    impl->releaseHeld(midi_tap_now_us());

    juce::MidiMessageSequence out;
    juce::File target;
    double bpm = 120.0;
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
        3, "Sp3ctra MIDI TAP " + juce::String(impl->slot + 1)), 0.0);
    track.addSequence(notesOut, 0.0);
    track.updateMatchedPairs();
    mf.addTrack(track);
    juce::ignoreUnused(kept, orphans);

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

void MidiTapSink::setFileMinNoteMs(double ms) noexcept
{ impl->fileMinNoteMs.store(juce::jmax(0.0, ms), std::memory_order_relaxed); }

void MidiTapSink::setQuantizeTicks(int ticks) noexcept
{ impl->quantTicks.store(juce::jmax(0, ticks), std::memory_order_relaxed); }

juce::String MidiTapSink::lastError() const
{
    std::lock_guard<std::mutex> lk(impl->errLock);
    return impl->err;
}
