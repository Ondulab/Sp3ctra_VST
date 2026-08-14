/**
 * @file MachinePrefs.h
 * @brief Machine-scoped preferences — settings that describe THIS computer /
 * instrument installation, not the piece being composed.
 *
 * One PropertiesFile (…/Application Support/Sp3ctra/machine.settings on macOS,
 * %APPDATA%/Sp3ctra/machine.settings on Windows) holds the values that must
 * NOT travel inside a session folder or a DAW project blob: network/link
 * config (UDP port + address, device IP, sensor DPI), diagnostics (log level,
 * worker threads, MIDI-follow), the editor window layout, the real-time MIDI
 * OUT destination and the TTS voices install folder.
 *
 * Mechanism (identical in Standalone and DAW — this file has no dependency on
 * the SessionManager):
 *   • RESTORE — after every state restore (session project.sp3ctra OR DAW
 *     blob), Sp3ctraAudioProcessor::applyMachineParamOverrides() forces the
 *     machine copy of each id in kParams back onto the APVTS. Opening someone
 *     else's session can therefore never repoint your UDP socket or device IP.
 *     First run (no machine copy yet) seeds the copy from the restored value,
 *     so existing installs keep their settings (one-shot migration).
 *   • EDIT — the explicit user edit sites (Apply Link, ADVANCED menu, MIDI
 *     menu…) call saveParam() after writing the APVTS. parameterChanged does
 *     NOT write here: host automation or a mid-restore parameter storm can
 *     never leak a foreign blob's values into the machine file.
 *   • Non-param values (editor layout, midiTapDest, voice folder) read/write
 *     the PropertiesFile directly via file(), with the old in-state property
 *     as a read fallback for sessions saved before this scoping existed.
 *
 * Message-thread only (PropertiesFile timer-coalesced writes, ~2 s).
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

namespace MachinePrefs
{
    /** The shared machine PropertiesFile (lazy, auto-saving). */
    inline juce::PropertiesFile& file()
    {
        static juce::PropertiesFile props([]
        {
            juce::PropertiesFile::Options o;
            o.applicationName          = "machine";
            o.filenameSuffix           = ".settings";
            o.folderName               = "Sp3ctra";
            o.osxLibrarySubFolder      = "Application Support";
            o.millisecondsBeforeSaving = 2000;   // coalesced timer save
            return o;
        }());
        return props;
    }

    /** APVTS parameter ids scoped to this machine (see file header). */
    inline const char* const kParams[] = {
        "udpPort",  "udpByte1", "udpByte2", "udpByte3", "udpByte4",
        "deviceIpByte1", "deviceIpByte2", "deviceIpByte3", "deviceIpByte4",
        "sensorDpi", "logLevel", "luxstralNumWorkers", "midiFollowParam",
    };

    inline juce::String paramKey(const char* id)
    { return juce::String("param.") + id; }

    /** P9 — non-param machine flag: re-arm the transports a session saved
     *  RUNNING when it is reopened (score slots once their persisted take's
     *  frames are rebuilt, media sources and sequencers at end of restore).
     *  Machine-scoped opt-in (ADVANCED menu): the never-auto-run doctrine
     *  stays the default, and a foreign session/DAW project can never force
     *  sound on a machine that did not opt in. */
    inline constexpr const char* kResumePlaybackKey = "resumePlaybackOnLoad";
    inline bool resumePlaybackOnLoad()
    { return file().getBoolValue(kResumePlaybackKey, false); }

    /** Write the CURRENT denormalised value of @p id into the machine file.
     *  Call from the user edit site right after writing the APVTS. */
    inline void saveParam(juce::AudioProcessorValueTreeState& apvts, const char* id)
    {
        if (auto* p = apvts.getParameter(id))
            file().setValue(paramKey(id),
                            (double) p->convertFrom0to1(p->getValue()));
    }
}
