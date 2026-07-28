#include "SessionManager.h"
#include "Sp3ctraPaths.h"
#include "../PluginProcessor.h"
#include "../licensing/LicenseManager.h"
#include "../utils/logger.h"

//== Directory layout =========================================================
juce::File SessionManager::appSupportRoot()
{
#if JUCE_WINDOWS
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("Sp3ctra");
#else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("Application Support/Sp3ctra");
#endif
}

juce::File SessionManager::globalSessionDir()
{
    return appSupportRoot().getChildFile("Sessions").getChildFile(globalName());
}

juce::String SessionManager::sanitizeName(const juce::String& name)
{
    auto s = name.trim();
    // Strip path separators / illegal filename characters; keep it human.
    s = s.replaceCharacters("/\\:*?\"<>|", "_________");
    if (s.isEmpty()) s = "Session";
    return s;
}

//== Construction =============================================================
SessionManager::SessionManager(Sp3ctraAudioProcessor& proc)
    : proc_(proc)
{
    // Default to the always-active Global session until the host wires us up
    // (openOnLaunch / migrateLegacyBlobIntoGlobal in set/getStateInformation).
    sessionDir_  = globalSessionDir();
    sessionName_ = globalName();
}

SessionManager::~SessionManager() = default;

bool SessionManager::isStandalone() const
{
    return proc_.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
}

//== Active session ===========================================================
void SessionManager::setActive(const juce::File& dir, const juce::String& name)
{
    sessionDir_  = dir;
    sessionName_ = name;
    ensureSkeleton(dir);
    stateDirty_.store(false);
    banksDirty_.store(false);
    if (onSessionChanged) onSessionChanged();
    log_info("VST", "Session active: '%s' (%s)",
             sessionName_.toRawUTF8(), sessionDir_.getFullPathName().toRawUTF8());
}

void SessionManager::ensureSkeleton(const juce::File& dir)
{
    dir.createDirectory();
    dir.getChildFile("banks").createDirectory();
    dir.getChildFile("assets").createDirectory();
    dir.getChildFile("exports").createDirectory();
}

void SessionManager::writeMeta(const juce::File& dir)
{
    juce::ValueTree meta("Sp3ctraSessionMeta");
    meta.setProperty("bundleVersion", kBundleVersion, nullptr);
    meta.setProperty("name",          sessionName_,   nullptr);
    meta.setProperty("modified",      juce::Time::getCurrentTime().toISO8601(true), nullptr);
    if (auto xml = meta.createXml())
        xml->writeTo(dir.getChildFile("session.meta"));
}

//== External assets (portable sessions) ======================================
juce::String SessionManager::copyAssetIntoSession(const juce::File& src,
                                                  const char* subdir,
                                                  const juce::File& dir)
{
    const juce::File assets = dir.getChildFile("assets").getChildFile(subdir);
    assets.createDirectory();

    juce::File target = assets.getChildFile(src.getFileName());
    if (target.existsAsFile())
    {
        // Same name + same size → assume the same file, reuse the copy.
        if (target.getSize() != src.getSize())
            target = target.getNonexistentSibling();
    }
    if (! target.existsAsFile() && ! src.copyFileTo(target))
    {
        log_warning("VST", "Session asset copy failed: %s",
                    src.getFullPathName().toRawUTF8());
        return {};   // keep the absolute reference
    }
    return target.getRelativePathFrom(dir);
}

void SessionManager::relativizeAssetPaths(juce::ValueTree state, const juce::File& dir)
{
    auto handle = [this, &dir](juce::ValueTree t, const juce::Identifier& prop,
                               const char* subdir)
    {
        const juce::String stored = t.getProperty(prop, "").toString();
        if (stored.isEmpty() || ! juce::File::isAbsolutePath(stored))
            return;                                   // empty or already relative
        const juce::File src(stored);
        if (! src.existsAsFile())
            return;                                   // missing → keep as-is
        if (src.isAChildOf(dir))                      // already inside the session
        {
            t.setProperty(prop, src.getRelativePathFrom(dir), nullptr);
            return;
        }
        const juce::String rel = copyAssetIntoSession(src, subdir, dir);
        if (rel.isNotEmpty())
            t.setProperty(prop, rel, nullptr);
    };

    handle(state, "scoreWavPath",      "audio");
    handle(state, "luxgrainSamplePath","audio");
    if (auto wt = state.getChildWithName("LUXSTRAL_WAVETABLE"); wt.isValid())
        handle(wt, "sourcePath", "audio");
    if (auto ms = state.getChildWithName("MEDIA_SOURCES"); ms.isValid())
        for (int s = 0; s < 8; ++s)
        {
            const juce::String sfx = s == 0 ? juce::String() : juce::String(s);
            handle(ms, juce::Identifier("imagePath" + sfx), "images");
            handle(ms, juce::Identifier("videoPath" + sfx), "video");
        }
    if (auto slots = state.getChildWithName("SAMPLER_SLOTS"); slots.isValid())
        for (auto eng : slots)
            for (auto slot : eng)
                handle(slot, "srcImagePath", "images");
}

void SessionManager::absolutizeAssetPaths(juce::XmlElement& xml, const juce::File& dir) const
{
    auto handle = [&dir](juce::XmlElement* e, const juce::String& attr)
    {
        if (e == nullptr) return;
        const juce::String stored = e->getStringAttribute(attr);
        if (stored.isEmpty() || juce::File::isAbsolutePath(stored))
            return;
        const juce::File resolved = dir.getChildFile(stored);
        // Resolve even when missing: the consumer's own warning names the
        // (absolute) path it could not open, which is the useful message.
        e->setAttribute(attr, resolved.getFullPathName());
    };

    handle(&xml, "scoreWavPath");
    handle(&xml, "luxgrainSamplePath");
    handle(xml.getChildByName("LUXSTRAL_WAVETABLE"), "sourcePath");
    if (auto* ms = xml.getChildByName("MEDIA_SOURCES"))
        for (int s = 0; s < 8; ++s)
        {
            const juce::String sfx = s == 0 ? juce::String() : juce::String(s);
            handle(ms, "imagePath" + sfx);
            handle(ms, "videoPath" + sfx);
        }
    if (auto* slots = xml.getChildByName("SAMPLER_SLOTS"))
        for (auto* eng : slots->getChildIterator())
            for (auto* slot : eng->getChildIterator())
                handle(slot, "srcImagePath");
}

//== Persistence ==============================================================
bool SessionManager::writeStateTo(const juce::File& dir)
{
    // project.sp3ctra carries the full state WITHOUT embedded banks (they live
    // as sidecar files in banks/). Referenced media are copied into assets/
    // and stored session-relative so the folder is portable. Atomic replace:
    // never truncate the previous project file before the new one is written.
    auto state = proc_.captureFullState(/*embedBanks*/ false);
    relativizeAssetPaths(state, dir);
    auto xml   = state.createXml();
    if (xml == nullptr) return false;

    const juce::File target = dir.getChildFile("project.sp3ctra");
    juce::TemporaryFile tmp(target);
    if (! xml->writeTo(tmp.getFile()))
    {
        log_error("VST", "Session save FAILED: could not write %s",
                  target.getFullPathName().toRawUTF8());
        return false;
    }
    if (! tmp.overwriteTargetFileWithTemporary())
    {
        log_error("VST", "Session save FAILED: could not replace %s",
                  target.getFullPathName().toRawUTF8());
        return false;
    }
    return true;
}

bool SessionManager::writeBanksTo(const juce::File& dir)
{
    const juce::File banks = dir.getChildFile("banks");
    banks.createDirectory();
    bool ok = true;
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
    {
        auto* smp = proc_.getSampler(e);
        if (smp == nullptr) continue;
        const juce::File f = banks.getChildFile("engine" + juce::String(e) + ".fsmp");
        juce::TemporaryFile tmp(f);
        if (smp->saveToFile(tmp.getFile()))
            ok &= tmp.overwriteTargetFileWithTemporary();
        else
            ok = false;
    }
    return ok;
}

bool SessionManager::loadBanksFrom(const juce::File& dir)
{
    const juce::File banks = dir.getChildFile("banks");
    if (! banks.isDirectory()) return false;
    bool any = false;
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
    {
        auto* smp = proc_.getSampler(e);
        if (smp == nullptr) continue;
        const juce::File f = banks.getChildFile("engine" + juce::String(e) + ".fsmp");
        if (f.existsAsFile())
            any |= smp->loadFromFile(f);
    }
    return any;
}

void SessionManager::saveStateNow()
{
    // Demo contract: the session NEVER persists — the app relaunches as the
    // user last saved it while licensed (or virgin). Loading stays allowed.
    // The legacy-blob migration is intentionally NOT behind this gate (it
    // writes project.sp3ctra directly): rescuing pre-session work is not
    // "saving new work".
    if (! LicenseManager::isLicensed()) return;
    if (saving_) return;
    const juce::ScopedValueSetter<bool> guard(saving_, true);
    ensureSkeleton(sessionDir_);
    if (writeStateTo(sessionDir_))
    {
        writeMeta(sessionDir_);
        stateDirty_.store(false);
        if (onSessionChanged) onSessionChanged();
    }
}

void SessionManager::saveBanksNow()
{
    if (! LicenseManager::isLicensed()) return;   // demo — see saveStateNow
    if (saving_) return;
    const juce::ScopedValueSetter<bool> guard(saving_, true);
    ensureSkeleton(sessionDir_);
    if (writeBanksTo(sessionDir_))
    {
        banksDirty_.store(false);
        if (onSessionChanged) onSessionChanged();
    }
}

bool SessionManager::onStateRestored()
{
    // Only reached when the restored state carried NO embedded SAMPLER_BANKS
    // (Standalone). Load the sidecar banks from the active session directory.
    return loadBanksFrom(sessionDir_);
}

//== Launch wiring ============================================================
void SessionManager::openOnLaunch(const juce::File& dir)
{
    juce::File target = dir;
    if (target == juce::File() || ! target.getChildFile("project.sp3ctra").existsAsFile())
        target = globalSessionDir();

    // No project yet (very first launch): make Global active and persist a seed.
    if (! target.getChildFile("project.sp3ctra").existsAsFile())
    {
        setActive(globalSessionDir(),
                  target == globalSessionDir() ? globalName() : target.getFileName());
        saveStateNow();
        saveBanksNow();
        return;
    }

    setActive(target, target == globalSessionDir() ? globalName() : target.getFileName());
    if (auto xml = juce::XmlDocument::parse(target.getChildFile("project.sp3ctra")))
    {
        // "assets/…" references → absolute paths (the live state only ever
        // holds absolute paths; the relative form exists on disk only).
        absolutizeAssetPaths(*xml, target);
        proc_.applyStateXml(std::move(xml));   // → applyRestoredStateOnMessageThread → onStateRestored (banks)
    }
    else
        log_error("VST", "Session open FAILED: unreadable %s",
                  target.getChildFile("project.sp3ctra").getFullPathName().toRawUTF8());
}

void SessionManager::migrateLegacyBlobIntoGlobal(std::unique_ptr<juce::XmlElement> fullState)
{
    const juce::File global = globalSessionDir();
    setActive(global, globalName());

    const bool hadProject = global.getChildFile("project.sp3ctra").existsAsFile();

    if (fullState != nullptr && ! hadProject)
    {
        // First upgrade: seed Global from the legacy host blob, then apply it.
        if (auto* xml = fullState.get())
            xml->writeTo(global.getChildFile("project.sp3ctra"));
        proc_.applyStateXml(std::move(fullState));
        writeMeta(global);
        saveBanksNow();
        log_info("VST", "Migrated legacy state blob into the Global session");
    }
    else
    {
        // Already migrated (or nothing to migrate) → open whatever Global holds.
        if (fullState != nullptr && hadProject)
            log_warning("VST", "Legacy state blob ignored: the Global session "
                               "already exists (%s) — its content wins",
                        global.getFullPathName().toRawUTF8());
        openOnLaunch(global);
    }
}

juce::ValueTree SessionManager::makeStandaloneRefState()
{
    // Flush current work, then hand the host a tiny pointer to the session dir.
    saveStateNow();
    saveBanksNow();
    juce::ValueTree ref("Sp3ctraStandaloneRef");
    ref.setProperty("activeSessionDir", sessionDir_.getFullPathName(), nullptr);
    ref.setProperty("sessionName",      sessionName_,                  nullptr);
    return ref;
}

//== User actions =============================================================
bool SessionManager::newSession(const juce::File& parentDir, const juce::String& name)
{
    if (! LicenseManager::isLicensed()) return false;   // demo — UI gates first
    const juce::File dir = parentDir.getChildFile(sanitizeName(name));
    if (! dir.createDirectory())
    {
        log_error("VST", "New session FAILED: cannot create %s",
                  dir.getFullPathName().toRawUTF8());
        return false;
    }
    // Start the new project from the CURRENT live state (nothing is lost).
    setActive(dir, dir.getFileName());
    saveStateNow();
    saveBanksNow();
    return true;
}

bool SessionManager::openSession(const juce::File& dir)
{
    if (! dir.getChildFile("project.sp3ctra").existsAsFile())
    {
        log_error("VST", "Open session FAILED: no project.sp3ctra in %s",
                  dir.getFullPathName().toRawUTF8());
        return false;
    }
    openOnLaunch(dir);
    return true;
}

bool SessionManager::saveAs(const juce::File& parentDir, const juce::String& name)
{
    if (! LicenseManager::isLicensed()) return false;   // demo — UI gates first
    // Duplicate the active session's content into a new folder, then switch.
    const juce::File dir = parentDir.getChildFile(sanitizeName(name));
    if (! dir.createDirectory())
    {
        log_error("VST", "Save-as FAILED: cannot create %s",
                  dir.getFullPathName().toRawUTF8());
        return false;
    }
    setActive(dir, dir.getFileName());
    saveStateNow();
    saveBanksNow();
    return true;
}

void SessionManager::closeSession()
{
    if (isGlobal()) return;   // already on Global
    openOnLaunch(globalSessionDir());
}

//== Chooser directories ======================================================
juce::File SessionManager::startDirFor(const char* key, const juce::File& osFallback,
                                       bool isExport) const
{
    // Exports flow into the working session (Standalone). The user can still
    // navigate elsewhere in the chooser — rememberDirFor then records it for
    // the non-session (DAW) contexts.
    if (isExport && isStandalone())
    {
        exportsDir().createDirectory();
        return exportsDir();
    }
    return Sp3ctraPaths::lastDir(key, osFallback);
}

void SessionManager::rememberDirFor(const char* key, const juce::File& chosenFileOrDir)
{
    Sp3ctraPaths::setLastDir(key, chosenFileOrDir);
}

//== Autosave =================================================================
void SessionManager::markStateDirty()
{
    // Autosave is a Standalone-only mechanism: in a DAW the host project is the
    // source of truth (getStateInformation), not the session folder.
    if (! isStandalone() || suppressAutosave_ || saving_) return;
    if (! stateDirty_.exchange(true))
        firstStateDirtyMs_ = juce::Time::getMillisecondCounter();
    lastStateDirtyMs_ = juce::Time::getMillisecondCounter();
}

void SessionManager::markBanksDirty()
{
    if (! isStandalone() || suppressAutosave_ || saving_) return;
    banksDirty_.store(true);
    lastBanksDirtyMs_ = juce::Time::getMillisecondCounter();
}

void SessionManager::autosaveTick(juce::int64 /*nowMsIgnored*/)
{
    if (! isStandalone() || suppressAutosave_ || saving_) return;
    if (! LicenseManager::isLicensed()) return;   // demo — see saveStateNow
    const juce::int64 nowMs = (juce::int64) juce::Time::getMillisecondCounter();

    // Falling edge of "any slot recording" = a capture just finished → the
    // banks changed. Catches every record path (UI, MIDI-mapped, sequencer)
    // without instrumenting the engine.
    const bool recording = proc_.anySamplerRecording();
    if (wasRecording_ && ! recording)
        markBanksDirty();
    wasRecording_ = recording;

    // project.sp3ctra: trailing-edge ~1.5 s of quiescence, forced every ~10 s
    // while continuously dirty so slow automation still persists.
    if (stateDirty_.load())
    {
        const bool quiet  = (nowMs - lastStateDirtyMs_)  >= 1500;
        const bool capHit = (nowMs - firstStateDirtyMs_) >= 10000;
        if (quiet || capHit)
            saveStateNow();
    }
    // Safety net: non-APVTS edits (per-slot sampler params, sequencer steps,
    // SCORE settings…) have no dirty hook — a slow unconditional state save
    // caps their loss window at ~60 s. Cheap: atomic snapshot + ~100 KB write.
    else if (nowMs - lastSafetySaveMs_ >= 60000)
    {
        lastSafetySaveMs_ = nowMs;
        saveStateNow();
    }
    if (lastSafetySaveMs_ == 0)
        lastSafetySaveMs_ = nowMs;   // arm relative to the first tick

    // banks: larger ~5 s debounce (heavier writes), and NEVER while a slot is
    // recording — LuxSampler::saveToFile reads the frames being captured.
    if (banksDirty_.load() && (nowMs - lastBanksDirtyMs_) >= 5000
        && ! proc_.anySamplerRecording())
        saveBanksNow();
}
