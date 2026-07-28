/**
 * @file AppUpdater.cpp
 * @brief See AppUpdater.h — check / download / swap / relaunch.
 */
#include "AppUpdater.h"
#include "Sp3ctraVersion.h"
#include "logger.h"

#if JUCE_MAC
 #include <unistd.h>     // getpid
#elif JUCE_WINDOWS
 #include <process.h>    // _getpid
#endif

JUCE_IMPLEMENT_SINGLETON (AppUpdater)

namespace
{
constexpr const char* kReleasesApiUrl =
    "https://api.github.com/repos/Ondulab/Sp3ctra_VST/releases?per_page=20";

#if JUCE_MAC
constexpr const char* kManifestAssetName = "latest-macos.json";
#else
constexpr const char* kManifestAssetName = "latest-windows.json";
#endif

// GitHub rejects UA-less requests; Accept pins the API media type.
constexpr const char* kHttpHeaders =
    "User-Agent: Sp3ctra-Updater\r\nAccept: application/vnd.github+json";

juce::File updaterTempDir()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("Sp3ctraUpdate");
}
} // namespace

AppUpdater::AppUpdater() : juce::Thread ("Sp3ctra Update") {}

AppUpdater::~AppUpdater()
{
    signalThreadShouldExit();
    stopThread (10000);
    clearSingletonInstance();
}

// ─────────────────────────────────────────────────────────── public actions ──

void AppUpdater::startupCheck()
{
    if (startupCheckDone_.exchange (true))
        return;
    juce::Timer::callAfterDelay (2500, []
    {
        if (auto* u = AppUpdater::getInstanceWithoutCreating())
            u->check();
    });
}

void AppUpdater::check()
{
    const auto s = state_.load();
    if (s == State::downloading || s == State::installing
        || s == State::readyToRestart)
        return;
    startOp (Op::check);
}

void AppUpdater::downloadAndInstall()
{
    if (state_.load() != State::updateAvailable || ! canSelfInstall())
        return;
    startOp (Op::download);
}

void AppUpdater::restartNow()
{
    if (state_.load() != State::readyToRestart)
        return;

    juce::File app;
    { const juce::ScopedLock sl (lock_); app = installedApp_; }
    const auto tmp = updaterTempDir();

#if JUCE_MAC
    // The new bundle is already in place; a detached shell waits for this PID
    // to exit, then reopens the app. ChildProcess does not kill on destruction.
    auto script = tmp.getChildFile ("relaunch.sh");
    script.replaceWithText (
        "#!/bin/sh\n"
        "while /bin/kill -0 " + juce::String ((int) getpid()) + " 2>/dev/null; do /bin/sleep 0.2; done\n"
        "/usr/bin/open \"" + app.getFullPathName() + "\"\n");
    juce::ChildProcess relauncher;
    if (! relauncher.start (juce::StringArray { "/bin/sh", script.getFullPathName() }))
    {
        fail ("Could not launch the relaunch helper.");
        return;
    }
#elif JUCE_WINDOWS
    // The payload is only STAGED (a running .exe can't be replaced): the
    // script waits for this PID, renames the old exe, copies the new files
    // over and starts the app again.
    juce::File staged;
    { const juce::ScopedLock sl (lock_); staged = stagedDir_; }
    const auto exe    = app.getFullPathName();
    const auto appDir = app.getParentDirectory().getFullPathName();
    const auto pid    = juce::String ((int) _getpid());

    auto script = tmp.getChildFile ("update.cmd");
    script.replaceWithText (
        "@echo off\r\n"
        ":wait\r\n"
        "timeout /t 1 /nobreak >nul\r\n"
        "tasklist /FI \"PID eq " + pid + "\" | find \"" + pid + "\" >nul\r\n"
        "if not errorlevel 1 goto wait\r\n"
        "move /y \"" + exe + "\" \"" + exe + ".old\" >nul\r\n"
        "xcopy \"" + staged.getFullPathName() + "\\*\" \"" + appDir + "\\\" /e /y /i >nul\r\n"
        "del /f /q \"" + exe + ".old\" >nul 2>nul\r\n"
        "start \"\" \"" + exe + "\"\r\n");
    juce::ChildProcess relauncher;
    if (! relauncher.start (juce::StringArray { "cmd.exe", "/c", script.getFullPathName() }))
    {
        fail ("Could not launch the update helper.");
        return;
    }
#else
    return;
#endif

    log_info ("UPDATER", "Restarting to finish update");
    if (auto* a = juce::JUCEApplicationBase::getInstance())
        a->systemRequestedQuit();
}

bool AppUpdater::canSelfInstall()
{
    if (! juce::JUCEApplicationBase::isStandaloneApp())
        return false;
#if JUCE_MAC
    const auto app = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    if (! app.getFileName().endsWith (".app"))
        return false;
    // Gatekeeper-translocated apps run from a random read-only mount; the real
    // install location is unknown, so a self-swap is impossible.
    if (app.getFullPathName().contains ("/AppTranslocation/"))
        return false;
    return app.getParentDirectory().hasWriteAccess();
#elif JUCE_WINDOWS
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
        .getParentDirectory().hasWriteAccess();
#else
    return false;
#endif
}

juce::String AppUpdater::latestVersion() const
{
    const juce::ScopedLock sl (lock_);
    return latestVersion_;
}

juce::String AppUpdater::errorMessage() const
{
    const juce::ScopedLock sl (lock_);
    return error_;
}

juce::String AppUpdater::progressText() const
{
    const auto done = bytesDone_.load(), total = bytesTotal_.load();
    auto mb = [] (juce::int64 b) { return juce::String ((double) b / (1024.0 * 1024.0), 1); };
    if (total > 0)
        return mb (done) + " / " + mb (total) + " MB";
    return done > 0 ? mb (done) + " MB" : juce::String();
}

// ──────────────────────────────────────────────────────────── worker thread ──

void AppUpdater::startOp (Op op)
{
    if (isThreadRunning())
        return;                       // one operation at a time
    pendingOp_.store (op);
    startThread();
}

void AppUpdater::run()
{
    switch (pendingOp_.exchange (Op::none))
    {
        case Op::check:    runCheck();              break;
        case Op::download: runDownloadAndInstall(); break;
        case Op::none:                              break;
    }
}

void AppUpdater::setState (State s)
{
    state_.store (s);
    sendChangeMessage();
}

void AppUpdater::fail (const juce::String& why)
{
    { const juce::ScopedLock sl (lock_); error_ = why; }
    log_warning ("UPDATER", "%s", why.toRawUTF8());
    setState (State::failed);
}

juce::String AppUpdater::fetchText (const juce::String& url)
{
    auto stream = juce::URL (url).createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (10000)
            .withNumRedirectsToFollow (5)
            .withExtraHeaders (kHttpHeaders));
    return stream != nullptr ? stream->readEntireStreamAsString() : juce::String();
}

bool AppUpdater::parseVersion (const juce::String& s, int out[3])
{
    auto t = juce::StringArray::fromTokens (s.trim(), ".", {});
    if (t.size() != 3)
        return false;
    for (int i = 0; i < 3; ++i)
    {
        if (! t[i].containsOnly ("0123456789") || t[i].isEmpty())
            return false;
        out[i] = t[i].getIntValue();
    }
    return true;
}

bool AppUpdater::isNewerThanCurrent (const juce::String& v)
{
    int r[3];
    if (! parseVersion (v, r))
        return false;
    const int c[3] = { SP3CTRA_VERSION_MAJOR, SP3CTRA_VERSION_MINOR,
                       SP3CTRA_VERSION_PATCH };
    for (int i = 0; i < 3; ++i)
        if (r[i] != c[i])
            return r[i] > c[i];
    return false;
}

void AppUpdater::runCheck()
{
    setState (State::checking);

    const auto body = fetchText (kReleasesApiUrl);
    if (body.isEmpty())
        { fail ("Cannot reach github.com — check your internet connection."); return; }

    const auto releases = juce::JSON::parse (body);
    if (! releases.isArray())
        { fail ("Unexpected answer from GitHub."); return; }

    // Newest channel = highest vX.Y tag that publishes an update manifest
    // (older channels predate the manifest and are skipped).
    int bestMaj = -1, bestMin = -1;
    juce::String manifestUrl;
    juce::var bestAssets;
    for (const auto& r : *releases.getArray())
    {
        const auto tag = r["tag_name"].toString();   // "v1.4.0-beta.1"
        int maj = 0, min = 0;
        if (sscanf (tag.toRawUTF8(), "v%d.%d", &maj, &min) != 2)
            continue;
        if (maj < bestMaj || (maj == bestMaj && min <= bestMin))
            continue;
        const auto assets = r["assets"];
        if (! assets.isArray())
            continue;
        for (const auto& a : *assets.getArray())
            if (a["name"].toString() == kManifestAssetName)
            {
                bestMaj = maj; bestMin = min;
                manifestUrl = a["browser_download_url"].toString();
                bestAssets  = assets;
                break;
            }
    }
    if (manifestUrl.isEmpty())
        { fail ("No update information published yet."); return; }

    const auto manifest = juce::JSON::parse (fetchText (manifestUrl));
    const auto version  = manifest["version"].toString();
    const auto zipName  = manifest["standalone"].toString();
    if (version.isEmpty() || zipName.isEmpty())
        { fail ("Malformed update manifest."); return; }

    juce::String zipUrl;
    for (const auto& a : *bestAssets.getArray())
        if (a["name"].toString() == zipName)
            zipUrl = a["browser_download_url"].toString();
    if (zipUrl.isEmpty())
        { fail ("Update package not found in the release."); return; }

    if (! isNewerThanCurrent (version))
    {
        log_info ("UPDATER", "Up to date (installed %s, published %s)",
                  SP3CTRA_VERSION_STRING, version.toRawUTF8());
        setState (State::upToDate);
        return;
    }

    { const juce::ScopedLock sl (lock_);
      latestVersion_ = version; assetUrl_ = zipUrl; }
    log_info ("UPDATER", "Update available: %s (installed %s)",
              version.toRawUTF8(), SP3CTRA_VERSION_STRING);
    setState (State::updateAvailable);
}

void AppUpdater::runDownloadAndInstall()
{
    progress_.store (-1.0f);
    bytesDone_.store (0);
    bytesTotal_.store (0);
    setState (State::downloading);

    auto tmp = updaterTempDir();
    tmp.deleteRecursively();
    if (! tmp.createDirectory())
        { fail ("Cannot create the temporary update folder."); return; }

    const auto zip = tmp.getChildFile ("Sp3ctra-update.zip");
    juce::String err;
    if (! downloadAsset (zip, err))
        { fail (err); return; }

    setState (State::installing);
    if (! installFromZip (zip, err))
        { fail (err); return; }

    zip.deleteFile();
    setState (State::readyToRestart);
}

bool AppUpdater::downloadAsset (const juce::File& destZip, juce::String& err)
{
    juce::String url;
    { const juce::ScopedLock sl (lock_); url = assetUrl_; }

    auto stream = juce::URL (url).createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (15000)
            .withNumRedirectsToFollow (5)
            .withExtraHeaders ("User-Agent: Sp3ctra-Updater"));
    if (stream == nullptr)
        { err = "Download failed — check your internet connection."; return false; }

    const auto total = stream->getTotalLength();
    bytesTotal_.store (total > 0 ? total : 0);

    juce::FileOutputStream out (destZip);
    if (! out.openedOk())
        { err = "Cannot write the update file."; return false; }

    juce::HeapBlock<char> buffer (1 << 18);
    juce::int64 done = 0;
    while (! stream->isExhausted())
    {
        if (threadShouldExit())
            { err = "Update cancelled."; return false; }
        const auto n = stream->read (buffer, 1 << 18);
        if (n < 0)
            { err = "Download interrupted."; return false; }
        if (n == 0)
            break;
        out.write (buffer, (size_t) n);
        done += n;
        bytesDone_.store (done);
        if (total > 0)
            progress_.store ((float) done / (float) total);
    }
    out.flush();

    if (done == 0 || (total > 0 && done != total))
        { err = "Download incomplete."; return false; }
    log_info ("UPDATER", "Downloaded %lld bytes", (long long) done);
    return true;
}

bool AppUpdater::installFromZip (const juce::File& zip, juce::String& err)
{
    auto extracted = updaterTempDir().getChildFile ("extracted");
    extracted.deleteRecursively();
    extracted.createDirectory();

#if JUCE_MAC
    // ditto (not juce::ZipFile): the only extractor guaranteed to restore the
    // bundle's exec bits and symlinks, and the mirror of what the CI zips.
    juce::ChildProcess unzip;
    if (! unzip.start (juce::StringArray { "/usr/bin/ditto", "-x", "-k",
                                           zip.getFullPathName(),
                                           extracted.getFullPathName() })
        || ! unzip.waitForProcessToFinish (180000)
        || unzip.getExitCode() != 0)
        { err = "Could not extract the update."; return false; }

    juce::File newApp;
    for (const auto& e : juce::RangedDirectoryIterator (extracted, true, "*.app",
                                                        juce::File::findDirectories))
        if (e.getFile().getFileName() == "Sp3ctra.app")
            { newApp = e.getFile(); break; }
    if (newApp == juce::File())
        { err = "Sp3ctra.app not found in the update package."; return false; }

    const auto current = juce::File::getSpecialLocation (juce::File::currentApplicationFile);
    if (! canSelfInstall())
        { err = "This install location cannot be updated in place."; return false; }

    // Same-volume renames: move the RUNNING bundle aside (legal on macOS, the
    // process keeps its open files), then move the new one into its place.
    const auto previous = updaterTempDir().getChildFile ("Sp3ctra-previous.app");
    previous.deleteRecursively();
    if (! current.moveFileTo (previous))
        { err = "Could not move the current app aside."; return false; }
    if (! newApp.moveFileTo (current))
    {
        previous.moveFileTo (current);   // roll back
        err = "Could not install the new app.";
        return false;
    }

    // The zip came through our own process (no browser), so quarantine should
    // be absent — strip defensively, else Gatekeeper blocks the unsigned beta.
    juce::ChildProcess xattr;
    if (xattr.start (juce::StringArray { "/usr/bin/xattr", "-dr",
                                         "com.apple.quarantine",
                                         current.getFullPathName() }))
        xattr.waitForProcessToFinish (30000);

    { const juce::ScopedLock sl (lock_); installedApp_ = current; }
    log_info ("UPDATER", "Installed %s", current.getFullPathName().toRawUTF8());
    return true;

#elif JUCE_WINDOWS
    // Flat exe + Resources — no exec bits involved, juce::ZipFile is enough.
    juce::ZipFile archive (zip);
    if (archive.uncompressTo (extracted, true).failed())
        { err = "Could not extract the update."; return false; }

    juce::File newExe;
    for (const auto& e : juce::RangedDirectoryIterator (extracted, true, "Sp3ctra.exe",
                                                        juce::File::findFiles))
        { newExe = e.getFile(); break; }
    if (newExe == juce::File())
        { err = "Sp3ctra.exe not found in the update package."; return false; }

    if (! canSelfInstall())
        { err = "This install location cannot be updated in place."; return false; }

    // Swap is deferred to the restart script (can't replace a running exe).
    { const juce::ScopedLock sl (lock_);
      stagedDir_    = newExe.getParentDirectory();
      installedApp_ = juce::File::getSpecialLocation (juce::File::currentExecutableFile); }
    return true;

#else
    juce::ignoreUnused (zip);
    err = "In-app update is not supported on this platform.";
    return false;
#endif
}
