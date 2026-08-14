/**
 * @file MidiTapSink.h
 * @brief Non-RT drain of ONE MIDI TAP probe ring into two sinks: a .mid file
 *        and a MIDI output port (virtual or hardware).
 *
 * ONE dedicated thread, ONE ring cursor, two independent enables — so the file
 * and the port can never disagree about what was emitted. The THIRD sink (the
 * plugin MIDI bus) is deliberately NOT here: it lives in processBlock with its
 * own cursor, which is legal because the probe ring is a BROADCAST WINDOW, not
 * an SPSC queue (the producer never reads a consumer cursor — see midi_tap.h).
 *
 * Threading contract:
 *   - openPort / closePort / startFile / stopFile / setters : message thread.
 *   - run()                                                 : the drain thread
 *     (10 ms poll). It is the ONLY reader of `cursor_`.
 *   - The PRODUCER never enters this file and never takes `fileLock_`, so the
 *     RT path stays lock-free no matter what this class does.
 *
 * Recovery: the sink keeps its own held[128] shadow and force-releases it on a
 * ring generation change (module removed / pool re-init) or a detected overrun.
 * Without that, a gap in the ring leaves hung notes on the destination.
 */
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <memory>

class MidiTapSink
{
public:
    /** `slot` is the probe's pool slot (0..7) — its ring is midi_tap_instance(slot). */
    explicit MidiTapSink(int slot);
    ~MidiTapSink();

    //── Real-time port sink ───────────────────────────────────────────────────
    /** Open a MIDI destination. An EMPTY `deviceName` closes the port; the
     *  sentinel "Virtual" creates a virtual source named "Sp3ctra MIDI TAP n"
     *  (macOS/Linux only — returns false with an explanation on Windows).
     *  Any other name is matched against MidiOutput::getAvailableDevices(). */
    bool openPort(const juce::String& deviceName, juce::String& err);
    void closePort();                      // flushes held notes first
    bool isPortOpen() const noexcept;
    juce::String portName() const;

    //── File sink ─────────────────────────────────────────────────────────────
    /** Begin a .mid take. `t0Us` is the COMMON master epoch shared by every
     *  probe, so N files land on ONE timeline (a probe whose flux starts 4 s
     *  after REC gets its first note 4 s in, not at tick 0).
     *  Does NOT re-anchor the ring cursor: the thread has been draining all
     *  along for the port, and re-anchoring would drop the events between arm
     *  and file open. Events older than t0 simply clamp to tick 0. */
    bool startFile(const juce::File& out, double bpm, juce::uint64 t0Us, juce::String& err);
    /** Release held notes, write the SMF and close. Safe when no file is open. */
    void stopFile();
    bool isFileOpen() const noexcept;
    int  noteCount() const noexcept;        // note-ons written to the current take

    //── Settings (message thread) ─────────────────────────────────────────────
    void setChannel(int ch1to16) noexcept;
    void setPortLatencyMs(double ms) noexcept;
    /** Write-time rhythmic quantization, in TICKS of the 960 PPQ grid
     *  (0 = off, raw timing). Applied to both ends of every note when the file
     *  is written, so the .mid is readable by ANY player — MuseScore 4 dropped
     *  the per-score MIDI import panel, so a raw-timed file gets whatever
     *  quantization the importer feels like (typically a thicket of tuplets).
     *  Affects the FILE ONLY: the real-time sinks stay sample-honest. */
    void setQuantizeTicks(int ticks) noexcept;
    /** File-only retroactive filter: drop on/off pairs shorter than this at
     *  close time. 0 = off (default) — anything else makes the FILE differ from
     *  what the ports actually emitted, which is why it is opt-in. */
    void setFileMinNoteMs(double ms) noexcept;

    juce::String lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiTapSink)
};
