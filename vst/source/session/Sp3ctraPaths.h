/**
 * @file Sp3ctraPaths.h
 * @brief Global "last directory used" memory for every file chooser.
 *
 * One tiny PropertiesFile (…/Application Support/Sp3ctra/paths.settings on
 * macOS, %APPDATA%/Sp3ctra/paths.settings on Windows) maps a purpose key to
 * the directory the user last picked for that purpose. This replaces every
 * hardcoded getSpecialLocation(...) chooser default across the app.
 *
 * Usage (always through SessionManager::startDirFor / rememberDirFor — the
 * session layer decides when the active session's exports/ folder wins):
 *   • startDirFor(key, osFallback, isExport)  → seed dir for a FileChooser
 *   • rememberDirFor(key, chosenFileOrDir)    → call in EVERY chooser callback
 *
 * Message-thread only. Keys live in PathKeys:: so call sites can't typo.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

namespace PathKeys
{
    // Imports (seed = last used; the user's media library stays external)
    constexpr const char* wavImport     = "wavImport";      // SCORE page WAV
    constexpr const char* mediaImport   = "mediaImport";    // IMAGE/VIDEO sources
    constexpr const char* grainSample   = "grainSample";    // LuxGrain material
    constexpr const char* timbreSource  = "timbreSource";   // LuxStral timbre WAV
    constexpr const char* midiImport    = "midiImport";     // MIDI SCORE .mid
    constexpr const char* slotImport    = "slotImport";     // sampler .fslot/image
    constexpr const char* chainPreset   = "chainPreset";    // .sp3chain load
    constexpr const char* midiMap       = "midiMap";        // .sp3midi import/export
    constexpr const char* voicesFolder  = "voicesFolder";   // TTS voices dir
    constexpr const char* firmware      = "firmware";       // device .bin upload
    // Exports (seed = active session exports/ when standalone, else last used)
    constexpr const char* exportDir     = "exportDir";      // renders, PNG/WAV/.fslot
    constexpr const char* videoCapture  = "videoCapture";   // VIDEO MIX .mov
    constexpr const char* sessionParent = "sessionParent";  // where sessions are created
}

namespace Sp3ctraPaths
{
    /** The shared paths PropertiesFile (lazy, auto-saving). */
    inline juce::PropertiesFile& file()
    {
        static juce::PropertiesFile props([]
        {
            juce::PropertiesFile::Options o;
            o.applicationName         = "paths";
            o.filenameSuffix          = ".settings";
            o.folderName              = "Sp3ctra";
            o.osxLibrarySubFolder     = "Application Support";
            o.millisecondsBeforeSaving = 2000;   // coalesced timer save
            return o;
        }());
        return props;
    }

    /** Last directory remembered for @p key, or @p fallback when none/gone. */
    inline juce::File lastDir(const char* key, const juce::File& fallback)
    {
        const juce::String stored = file().getValue(juce::String("dir.") + key);
        if (stored.isNotEmpty())
        {
            const juce::File dir(stored);
            if (dir.isDirectory())
                return dir;
        }
        return fallback;
    }

    /** Remember @p fileOrDir (its parent when it is a file) for @p key. */
    inline void setLastDir(const char* key, const juce::File& fileOrDir)
    {
        const juce::File dir = fileOrDir.isDirectory()
                             ? fileOrDir : fileOrDir.getParentDirectory();
        if (dir == juce::File() || ! dir.isDirectory())
            return;
        file().setValue(juce::String("dir.") + key, dir.getFullPathName());
    }
}
