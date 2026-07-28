/**
 * @file AppUpdater.h
 * @brief In-app update engine for the beta release channel.
 *
 * Checks the public GitHub releases of Ondulab/Sp3ctra_VST for a newer build:
 * the CI publishes a tiny `latest-macos.json` / `latest-windows.json` asset
 * next to the zips, carrying the FULL x.y.z version (the release tag and zip
 * names only carry x.y — every green master build re-uploads the same asset
 * names with a bumped patch).
 *
 * Standalone builds can then download the Standalone zip and swap it in
 * place:
 *   - macOS: unzip with /usr/bin/ditto (preserves bundle exec bits), move the
 *     running .app aside and move the new one in, then relaunch via a tiny
 *     shell script that waits for this PID to exit.
 *   - Windows: extract next to the app, then a .cmd script performs the swap
 *     AFTER the process exits (a running .exe can be renamed, not replaced).
 * Plugin builds (VST3/AU in a DAW) never self-install — the UI falls back to
 * the download page link.
 *
 * All network/disk work runs on this object's own thread; UI observes it as a
 * juce::ChangeBroadcaster (sendChangeMessage is thread-safe). Singleton so the
 * startup check and any number of editors/dialogs share one state machine.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class AppUpdater : public juce::Thread,
                   public juce::ChangeBroadcaster,
                   private juce::DeletedAtShutdown
{
public:
    enum class State
    {
        idle,             // nothing happened yet
        checking,         // fetching release info
        upToDate,         // check done, no newer build
        updateAvailable,  // newer build found (latestVersion())
        downloading,      // fetching the Standalone zip (progress())
        installing,       // unzip + swap in progress
        readyToRestart,   // installed (macOS) / staged (Windows) — restart to run it
        failed            // errorMessage() says why
    };

    /** Once per process, standalone only: silent check a moment after launch
        (delayed so it never competes with audio/UI bring-up). */
    void startupCheck();

    /** Manual check. Ignored while a download/install is in flight. */
    void check();

    /** Fetch + install the new Standalone. Only valid from updateAvailable,
        and only when canSelfInstall(). */
    void downloadAndInstall();

    /** Quit and relaunch the (new) app. Only valid from readyToRestart. */
    void restartNow();

    State state() const noexcept { return state_.load(); }
    float progress() const noexcept { return progress_.load(); }  // 0..1, <0 = unknown
    juce::String latestVersion() const;
    juce::String errorMessage() const;
    juce::String progressText() const;   // "42.5 / 180.3 MB"

    /** True when this build can replace itself on disk (standalone, writable
        install dir, not Gatekeeper-translocated). */
    static bool canSelfInstall();

    JUCE_DECLARE_SINGLETON (AppUpdater, false)

private:
    AppUpdater();
    ~AppUpdater() override;

    enum class Op { none, check, download };
    void startOp (Op op);
    void run() override;

    void runCheck();
    void runDownloadAndInstall();
    bool downloadAsset (const juce::File& destZip, juce::String& err);
    bool installFromZip (const juce::File& zip, juce::String& err);

    void setState (State s);
    void fail (const juce::String& why);

    static juce::String fetchText (const juce::String& url);
    static bool parseVersion (const juce::String& s, int out[3]);
    static bool isNewerThanCurrent (const juce::String& v);

    std::atomic<State> state_    { State::idle };
    std::atomic<Op>    pendingOp_{ Op::none };
    std::atomic<float> progress_ { -1.0f };
    std::atomic<juce::int64> bytesDone_ { 0 }, bytesTotal_ { 0 };
    std::atomic<bool>  startupCheckDone_ { false };

    mutable juce::CriticalSection lock_;   // guards the strings below
    juce::String latestVersion_, assetUrl_, error_;
    juce::File stagedDir_;                 // Windows: extracted payload dir
    juce::File installedApp_;              // what restartNow() relaunches

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppUpdater)
};
