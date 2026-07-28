/**
 * @file SessionManager.h
 * @brief Working-session (project) manager — Standalone-facing persistence.
 *
 * A "session" is a working DIRECTORY that holds the COMPLETE plugin state:
 *
 *   <SessionDir>/
 *     project.sp3ctra   XML of Sp3ctraAudioProcessor::captureFullState()
 *                       (APVTS + SCORE/SEQS/SAMPLER_SLOTS/MEDIA/CHAINS/…/MIDI)
 *     session.meta      bundle version + name + timestamps (independent of
 *                       the APVTS synthSplitVersion migration gate)
 *     banks/engineN.fsmp  recorded sampler audio (LuxSampler::saveToFile), one
 *                       per engine — the ONLY data absent from captureFullState
 *     assets/           copies of imported media (relative paths in the state)
 *     exports/          default destination of user exports
 *
 * There is ALWAYS an active session in Standalone: either a user-named folder
 * or the built-in "Global" folder under Application Support. "New / Open" only
 * switch the active folder; "Close" reverts to Global. The session auto-saves
 * continuously (see markStateDirty/markBanksDirty + autosaveTick).
 *
 * Hosting split (see PluginProcessor get/setStateInformation):
 *   • Standalone — the SessionManager is the source of truth. The host blob
 *     only stores a pointer to the active session dir.
 *   • DAW (VST3/AU) — the host project IS the session: captureFullState embeds
 *     the sampler banks so the project is self-contained; no SessionManager UI.
 *
 * All methods run on the message thread (file I/O + captureFullState).
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>   // juce::ValueTree
#include <atomic>
#include <functional>

class Sp3ctraAudioProcessor;

class SessionManager
{
public:
    explicit SessionManager(Sp3ctraAudioProcessor& proc);
    ~SessionManager();

    /** Bundle format version, stamped into session.meta. Independent of the
     *  APVTS state's synthSplitVersion. Bump on incompatible layout changes. */
    static constexpr int kBundleVersion = 1;

    //== Host / identity ======================================================
    bool         isStandalone() const;
    juce::File   sessionDir()  const { return sessionDir_; }
    juce::String sessionName() const { return sessionName_; }
    bool         isGlobal()    const { return sessionName_ == globalName(); }
    bool         hasUnsavedChanges() const
    { return stateDirty_.load() || banksDirty_.load(); }

    /** Fired (message thread) whenever the active session or its dirty/saved
     *  state changes — the banner label subscribes to refresh. */
    std::function<void()> onSessionChanged;

    //== Launch wiring (called from set/getStateInformation) ==================
    /** Standalone launch: open @p dir (empty → Global). Reads project.sp3ctra
     *  and applies it through Sp3ctraAudioProcessor::applyStateXml. */
    void openOnLaunch(const juce::File& dir);

    /** One-shot upgrade path: an old host blob still carries the full pre-session
     *  state. Seed the Global session's project.sp3ctra from it once, then open
     *  Global. @p fullState is the parsed legacy blob (may be null → just open
     *  Global with whatever is already on disk). */
    void migrateLegacyBlobIntoGlobal(std::unique_ptr<juce::XmlElement> fullState);

    /** Standalone getStateInformation: flush to disk and return the pointer
     *  blob {activeSessionDir} the host should persist. */
    juce::ValueTree makeStandaloneRefState();

    //== User actions (banner) ================================================
    bool newSession (const juce::File& parentDir, const juce::String& name);
    bool openSession(const juce::File& dir);
    bool saveAs     (const juce::File& parentDir, const juce::String& name);
    void closeSession();   // → Global

    //== Persistence ==========================================================
    void saveStateNow();   // write project.sp3ctra (+ session.meta) atomically
    void saveBanksNow();   // write banks/engineN.fsmp
    /** Called at the end of applyRestoredStateOnMessageThread when the restored
     *  state has NO embedded SAMPLER_BANKS (Standalone) — load sidecar banks.
     *  @returns true if at least one bank file was loaded. */
    bool onStateRestored();

    //== Chooser directories (see Sp3ctraPaths.h for the keys) ================
    /** Seed directory for a FileChooser. Exports land in the active session's
     *  exports/ folder (Standalone); everything else starts at the last
     *  directory used for @p key, falling back to @p osFallback. */
    juce::File startDirFor(const char* key, const juce::File& osFallback,
                           bool isExport = false) const;
    /** Remember the directory of @p chosenFileOrDir for @p key — call from
     *  EVERY chooser callback so the next open starts where the user was. */
    void rememberDirFor(const char* key, const juce::File& chosenFileOrDir);

    //== Autosave (message thread) ============================================
    void markStateDirty();
    void markBanksDirty();
    void setSuppressAutosave(bool s) { suppressAutosave_ = s; }
    /** Called from the processor's 30 ms timerCallback; @p nowMs is a monotonic
     *  millisecond clock. Debounced trailing-edge writes. */
    void autosaveTick(juce::int64 nowMs);

    //== Directory layout =====================================================
    static juce::File appSupportRoot();      // …/Application Support/Sp3ctra
    static juce::File globalSessionDir();    // …/Sp3ctra/Global
    static juce::String globalName() { return "Global"; }

    juce::File projectFile() const { return sessionDir_.getChildFile("project.sp3ctra"); }
    juce::File metaFile()    const { return sessionDir_.getChildFile("session.meta"); }
    juce::File banksDir()    const { return sessionDir_.getChildFile("banks"); }
    juce::File assetsDir()   const { return sessionDir_.getChildFile("assets"); }
    juce::File exportsDir()  const { return sessionDir_.getChildFile("exports"); }

private:
    void setActive(const juce::File& dir, const juce::String& name);
    void ensureSkeleton(const juce::File& dir);   // mkdir banks/assets/exports
    bool writeStateTo(const juce::File& dir);
    bool writeBanksTo(const juce::File& dir);
    bool loadBanksFrom(const juce::File& dir);
    void writeMeta(const juce::File& dir);
    static juce::String sanitizeName(const juce::String& name);

    // ── External assets (portable sessions) ──────────────────────────────────
    // The live state always holds ABSOLUTE paths; the transform happens only at
    // the session-file boundary so no consumer changes:
    //   save: copy referenced media into assets/<subdir>/ and rewrite the
    //         stored path to a session-relative "assets/…" (idempotent);
    //   load: resolve "assets/…" back to absolute against the session dir.
    // Known path-bearing properties: root scoreWavPath / luxgrainSamplePath,
    // LUXSTRAL_WAVETABLE.sourcePath, MEDIA_SOURCES.imagePath{N}/videoPath{N},
    // SAMPLER_SLOTS/Engine/Slot.srcImagePath.
    void relativizeAssetPaths(juce::ValueTree state, const juce::File& dir);
    void absolutizeAssetPaths(juce::XmlElement& xml, const juce::File& dir) const;
    /** Copy @p src into <dir>/assets/<subdir>/ (reused when already copied)
     *  and return the session-relative path, or "" to keep the absolute. */
    juce::String copyAssetIntoSession(const juce::File& src, const char* subdir,
                                      const juce::File& dir);

    Sp3ctraAudioProcessor& proc_;
    juce::File   sessionDir_;
    juce::String sessionName_ { globalName() };

    std::atomic<bool> stateDirty_ { false };
    std::atomic<bool> banksDirty_ { false };
    bool        suppressAutosave_ { false };
    bool        saving_           { false };
    juce::int64 lastStateDirtyMs_  { 0 };
    juce::int64 firstStateDirtyMs_ { 0 };
    juce::int64 lastBanksDirtyMs_  { 0 };
    juce::int64 lastSafetySaveMs_  { 0 };       // ~60 s net for un-hooked edits
    bool        wasRecording_      { false };   // falling-edge detect (autosaveTick)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SessionManager)
};
