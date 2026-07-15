#include "SamplerPageComponent.h"
#include "../PluginProcessor.h"
#include "../Sp3ctraDialog.h"
#include "../UITheme.h"
#include "../framesequencer/FrameSequencer.h"

// ============================================================================
// Session file format  (.sp3s)
//   [4]  magic    0x53503353  "SP3S"
//   [2]  version  0x0001
//   [4]  xmlLen   (bytes)
//   [*]  UTF-8 XML  (SlotParams + Sequencer state)
//   [4]  fsmpLen  (bytes)
//   [*]  binary .fsmp data  (slot audio — existing LuxSampler format)
//   [4]  EOF marker  0xDEADBEEF
// ============================================================================
namespace
{
    constexpr uint32_t kSessionMagic   = 0x53503353u; // "SP3S"
    // v2: per-engine SlotParams blocks (engine attr) + TWO .fsmp banks (A, B).
    // v1 (single engine) is still readable; new saves are always v2.
    constexpr uint16_t kSessionVersion = 0x0002u;
    constexpr uint32_t kSessionEof     = 0xDEADBEEFu;

} // namespace

// =============================================================================
// Constructor
// =============================================================================

SamplerPageComponent::SamplerPageComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc),
      slotGrid  (proc),
      slotEditor(proc)
{
    slotGrid.onSlotSelected = [this](int idx) { onSlotSelected(idx); };
    slotGrid  .setSelectedSlot(0);
    slotEditor.setSelectedSlot(0);

    addAndMakeVisible(slotGrid);
    addAndMakeVisible(slotEditor);

    // ── Style helper ─────────────────────────────────────────────────────────
    auto styleBtn = [](juce::TextButton& btn, juce::Colour bg, juce::Colour fg)
    {
        btn.setColour(juce::TextButton::buttonColourId,  bg);
        btn.setColour(juce::TextButton::textColourOffId, fg);
    };

    // ── Session toolbar ───────────────────────────────────────────────────────
    styleBtn(newSessionBtn,  juce::Colour(0xff2a1a1a), juce::Colour(0xffff8866));
    styleBtn(saveSessionBtn, juce::Colour(0xff1e2a1e), juce::Colour(0xff88ffaa));
    styleBtn(loadSessionBtn, juce::Colour(0xff1e1e2a), juce::Colour(0xff88aaff));

    // NEW SESSION — ask for a session name (default = ISO timestamp), then
    // stop + clear all slots and immediately create a new session file with
    // the chosen name. Cancel = no-op.
    newSessionBtn.onClick = [this]
    {
        const juce::String defaultName =
            juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");

        Sp3ctraDialog::showInput(
            this,
            "New session",
            "Session name:",
            defaultName,
            "Create",
            "Cancel",
            [this](const juce::String& enteredName)
            {
                const juce::String name = enteredName.trim();
                if (name.isEmpty()) return;

                // Stop the sequencer first (message thread → atomic command)
                if (auto* seq = processor.getFrameSequencer())
                {
                    seq->uiStop();
                    // Reset all step assignments to empty
                    for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
                        seq->setStep(i, FrameSequencer::STEP_EMPTY);
                }

                // Clear all recorded slots and reset play params to defaults —
                // on BOTH engines: SAVE SESSION always writes banks A AND B, so
                // leaving the other engine's takes in RAM would silently embed
                // them into the brand-new session file.
                for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
                {
                    auto* fs = processor.getSampler(e);
                    if (fs == nullptr) continue;
                    fs->clearAllSlots();
                    for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
                        fs->resetSlotPlayParams(i);
                }

                // Also stop transport in APVTS (visual feedback)
                processor.getAPVTS()
                    .getParameterAsValue("samplerFreezeMode").setValue(2);

                // Resolve the target directory: configured output dir or
                // the user's Documents folder as a fallback.
                const juce::String outDir = processor.getSamplerOutputDir();
                const juce::File dir = outDir.isNotEmpty()
                    ? juce::File(outDir)
                    : juce::File::getSpecialLocation(
                          juce::File::userDocumentsDirectory);

                if (! dir.isDirectory())
                    dir.createDirectory();

                // Bind the session to a fresh file. The actual write is
                // deferred until the user presses SAVE SESSION — at that
                // point doSaveSession() will overwrite currentSessionFile
                // directly without a file dialog.
                currentSessionFile = dir.getChildFile(name + ".sp3s");
                processor.setLastSessionPath(
                    currentSessionFile.getFullPathName());

                // Refresh UI
                slotGrid  .repaint();
                slotEditor.setSelectedSlot(0);
            });
    };

    saveSessionBtn.onClick = [this]
    {
        // If a session path is already known (set by NEW SESSION, by a
        // previous SAVE/LOAD), save directly without a file chooser.
        // We only require a non-empty path — the file itself may not exist
        // yet right after NEW SESSION.
        if (currentSessionFile.getFullPathName().isNotEmpty())
        {
            // Make sure the destination directory exists.
            const auto parent = currentSessionFile.getParentDirectory();
            if (! parent.isDirectory())
                parent.createDirectory();

            doSaveSession(currentSessionFile);
            return;
        }

        // If the user has set a default output directory in
        // Sp3ctra Configuration => LuxSampler, use it directly with an
        // auto-generated timestamped filename (no file chooser needed).
        const juce::String outDir = processor.getSamplerOutputDir();
        if (outDir.isNotEmpty())
        {
            const juce::File dir(outDir);
            if (dir.isDirectory() || dir.createDirectory().wasOk())
            {
                const juce::String stamp =
                    juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
                const juce::File target =
                    dir.getChildFile("Session_" + stamp + ".sp3s");
                doSaveSession(target);
                currentSessionFile = target;
                return;
            }
        }

        // No known path yet and no default output dir → show file dialog.
        // Use the configured output dir (if any) as the starting folder.
        const juce::File startDir = outDir.isNotEmpty()
            ? juce::File(outDir)
            : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        fileChooser = std::make_unique<juce::FileChooser>(
            "Save Complete Session",
            startDir,
            "*.sp3s");

        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc)
            {
                const auto result = fc.getResult();
                if (result.getFullPathName().isNotEmpty())
                {
                    const auto target = result.withFileExtension("sp3s");
                    doSaveSession(target);
                    currentSessionFile = target; // remember for next save
                }
            });
    };

    loadSessionBtn.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Load Complete Session",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.sp3s");
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                const auto result = fc.getResult();
                if (result.getFullPathName().isNotEmpty())
                    doLoadSession(result);
            });
    };

    addAndMakeVisible(newSessionBtn);
    addAndMakeVisible(saveSessionBtn);
    addAndMakeVisible(loadSessionBtn);

    // ── Auto-restore last session on startup ──────────────────────────────────
    // lastSessionPath is restored from DAW state (setStateInformation) before
    // the editor is created, so it is already available here. The pending flag
    // is one-shot: re-opening the editor later must NOT reload the session
    // over live (unsaved) in-RAM edits of the banks / slots / pattern.
    // Deferred via callAsync so the component is fully laid out and the
    // LuxSampler player thread is started (prepareToPlay) before file I/O.
    if (processor.consumeSamplerAutoLoadPending())
    {
        const juce::File f(processor.getLastSessionPath());
        if (f.existsAsFile())
        {
            juce::MessageManager::callAsync([this, f]
            {
                doLoadSession(f, /*isAutoRestore*/ true);
            });
        }
        else if (processor.getLastSessionPath().isNotEmpty())
        {
            // The banks live ONLY in the .sp3s (absolute path in the DAW
            // state) — a moved/renamed project used to fail 100% silently,
            // leaving 24 empty slots with no clue which file is missing.
            const juce::String missing = processor.getLastSessionPath();
            juce::MessageManager::callAsync([this, missing]
            {
                Sp3ctraDialog::showWarning(
                    this,
                    "Load Session",
                    ("Saved session not found:\n" + missing
                     + "\n\nSample banks were NOT restored. Use LOAD SESSION "
                       "to locate the .sp3s file.").toRawUTF8());
            });
        }
    }
    else
    {
        // No auto-load, but keep SAVE SESSION pointing at the known session.
        const juce::File f(processor.getLastSessionPath());
        if (f.existsAsFile())
            currentSessionFile = f;
    }
}

SamplerPageComponent::~SamplerPageComponent() = default;

// =============================================================================
// Slot selection
// =============================================================================

void SamplerPageComponent::onSlotSelected(int idx)
{
    slotGrid  .setSelectedSlot(idx);
    slotEditor.setSelectedSlot(idx);
}

// =============================================================================
// paint / resized
// =============================================================================

void SamplerPageComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1e1e));
}

void SamplerPageComponent::resized()
{
    const int w      = getWidth();
    constexpr int pad    = Sp3ctraTheme::kPad;
    constexpr int gap    = Sp3ctraTheme::kGap;
    constexpr int gridH  = kGridH;
    // Editor: 2 param columns + image editor + a SCORE-style EQ panel underneath.
    constexpr int editH  = kEditH;
    constexpr int bankH  = Sp3ctraTheme::kControlH;

    // ── Zone 1: sample bank ───────────────────────────────────────────────────
    slotGrid.setBounds(pad, pad, w - 2 * pad, gridH);

    // ── Zone 2: slot editor ───────────────────────────────────────────────────
    const int editY = pad + gridH + gap;
    slotEditor.setBounds(pad, editY, w - 2 * pad, editH);

    // ── Session toolbar (NEW SESSION / SAVE SESSION / LOAD SESSION) ───────────
    // The step sequencer + its transport bar now live in the SEQUENCER module.
    const int sessY  = editY + editH + gap;
    const int btnGap = 4;
    const int bW3    = (w - 2 * pad - 2 * btnGap) / 3;
    newSessionBtn .setBounds(pad,                      sessY, bW3, bankH);
    saveSessionBtn.setBounds(pad + bW3 + btnGap,       sessY, bW3, bankH);
    loadSessionBtn.setBounds(pad + 2 * (bW3 + btnGap), sessY, bW3, bankH);
}

// =============================================================================
// Session Save / Load helpers
// Non-RT: runs on the message thread only — alloc/I/O allowed.
// =============================================================================

void SamplerPageComponent::doSaveSession(const juce::File& sessionFile)
{
    // v2: the session carries BOTH sampler engines (A + B) — params and banks.
    // v1 only stored the engine this page happened to be bound to, silently
    // dropping the other engine's data on every save.
    // TODO(P6-M2): v3 — N engines (P6 raised the pool to 8; v2 still saves
    // engines 0/1 only, so takes recorded on engines 2..7 do NOT survive a
    // session save until M2 lands).
    LuxSampler* engines[2] = { processor.getSampler(0), processor.getSampler(1) };
    auto* seq = processor.getFrameSequencer();
    if (!engines[0] || !engines[1] || !seq) return;

    // ── Build XML ─────────────────────────────────────────────────────────────
    juce::XmlElement root("Sp3ctraSession");
    root.setAttribute("version", 2);

    // Per-slot play parameters, one block per engine (shared serialisation —
    // same fields as .fslot / DAW state, incl. fade curves
    // and frequency curves that v1 silently dropped).
    for (int e = 0; e < 2; ++e)
    {
        auto* slotsXml = root.createNewChildElement("SlotParams");
        slotsXml->setAttribute("engine",  e);
        slotsXml->setAttribute("overdub", engines[e]->getOverdubMode() ? 1 : 0);
        for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
        {
            auto* s = slotsXml->createNewChildElement("Slot");
            engines[e]->slotParamsToXml(i, *s);
        }
    }

    // Sequencer state (uses FrameSequencer's own serialisation)
    auto* seqXml = root.createNewChildElement("Sequencer");
    seq->saveToXml(*seqXml);

    // ── Save audio frames to temporary .fsmp files (one per engine) ──────────
    juce::MemoryBlock fsmpBlobs[2];
    for (int e = 0; e < 2; ++e)
    {
        juce::TemporaryFile tmpFsmp(".fsmp");
        if (!engines[e]->saveToFile(tmpFsmp.getFile()))
        {
            Sp3ctraDialog::showWarning(
                this,
                "Save Session",
                "Failed to write sample bank to temporary file.");
            return;
        }
        if (!tmpFsmp.getFile().loadFileAsData(fsmpBlobs[e]) || fsmpBlobs[e].isEmpty())
        {
            Sp3ctraDialog::showWarning(
                this,
                "Save Session",
                "Could not read temporary sample bank.");
            return;
        }
    }

    // ── Serialise XML to UTF-8 ────────────────────────────────────────────────
    const juce::String xmlStr = root.toString();
    const juce::MemoryBlock xmlBlock(xmlStr.toRawUTF8(),
                                     static_cast<size_t>(xmlStr.getNumBytesAsUTF8()));

    // ── Write combined .sp3s (ATOMIC replace) ─────────────────────────────────
    // Never delete the previous session before the new one is fully written: a
    // failure mid-write (disk full, USB drive unplugged) would destroy the only
    // copy of the sample banks — the frames live in the .sp3s, not in the DAW
    // state blob. Write a sibling temp file, then swap it in.
    juce::TemporaryFile tmpSession(sessionFile);
    {
        juce::FileOutputStream out(tmpSession.getFile());
        if (out.failedToOpen())
        {
            const juce::String msg =
                "Cannot open output file:\n" + sessionFile.getFullPathName();
            Sp3ctraDialog::showWarning(this, "Save Session", msg.toRawUTF8());
            return;
        }

        out.writeInt  (static_cast<int>(kSessionMagic));
        out.writeShort(static_cast<short>(kSessionVersion));
        out.writeInt  (static_cast<int>(xmlBlock.getSize()));
        out.write     (xmlBlock.getData(), xmlBlock.getSize());
        for (const auto& blob : fsmpBlobs)
        {
            out.writeInt(static_cast<int>(blob.getSize()));
            out.write   (blob.getData(), blob.getSize());
        }
        out.writeInt  (static_cast<int>(kSessionEof));
        out.flush();

        if (!out.getStatus().wasOk())
        {
            Sp3ctraDialog::showWarning(
                this,
                "Save Session",
                "Write error - previous session file left untouched.");
            return;   // tmpSession dtor removes the partial temp file
        }
    }
    if (!tmpSession.overwriteTargetFileWithTemporary())
    {
        Sp3ctraDialog::showWarning(
            this,
            "Save Session",
            "Could not replace the session file - previous session left untouched.");
        return;
    }
    // Persist path so the DAW project and Standalone reload this session on
    // the next launch (stored in getStateInformation via processor.lastSessionPath).
    processor.setLastSessionPath(sessionFile.getFullPathName());

    // The banks in RAM are now exactly what the file holds — drop any stale
    // one-shot auto-load flag so a later editor open cannot reload an older
    // .sp3s on top of this freshly saved session.
    (void) processor.consumeSamplerAutoLoadPending();

    // ── Optional image export ────────────────────────────────────────────────
    // When the user enabled "Export Images on Save Session" in
    // Sp3ctra Configuration => LuxSampler, write every non-empty slot as a
    // PNG or JPEG file next to the .sp3s session.
    auto& apvts = processor.getAPVTS();
    const bool exportImages =
        apvts.getRawParameterValue("luxSamplerExportImages") != nullptr
        && apvts.getRawParameterValue("luxSamplerExportImages")->load() > 0.5f;

    if (exportImages)
    {
        // Format choice: 0 = PNG, 1 = JPEG
        int formatChoice = 0;
        if (auto* p = apvts.getRawParameterValue("luxSamplerExportFormat"))
            formatChoice = static_cast<int>(p->load());
        const bool asPng = (formatChoice == 0);

        // Destination: <session>_images/  next to the .sp3s file
        const juce::String baseName =
            sessionFile.getFileNameWithoutExtension();
        const juce::File destDir =
            sessionFile.getParentDirectory().getChildFile(baseName + "_images");

        int n = engines[0]->exportAllSlotsImages(destDir, baseName, asPng);
        n    += engines[1]->exportAllSlotsImages(destDir, baseName + "_B", asPng);

        if (n <= 0)
        {
            Sp3ctraDialog::showInfo(
                this,
                "Save Session",
                "Session saved, but no slot contained image data to export.");
        }
    }
}


// -----------------------------------------------------------------------------

void SamplerPageComponent::doLoadSession(const juce::File& sessionFile, bool isAutoRestore)
{
    LuxSampler* engines[2] = { processor.getSampler(0), processor.getSampler(1) };
    auto* seq = processor.getFrameSequencer();
    if (!engines[0] || !engines[1] || !seq) return;

    juce::FileInputStream in(sessionFile);
    if (!in.openedOk())
    {
        const juce::String msg =
            "Cannot open session file:\n" + sessionFile.getFullPathName();
        Sp3ctraDialog::showWarning(this, "Load Session", msg.toRawUTF8());
        return;
    }

    // ── Header ────────────────────────────────────────────────────────────────
    const auto magic = static_cast<uint32_t>(in.readInt());
    if (magic != kSessionMagic)
    {
        Sp3ctraDialog::showWarning(
            this,
            "Load Session",
            "Not a valid Sp3ctra session file (.sp3s).");
        return;
    }
    const int version = static_cast<int>(in.readShort()); // 1 = single engine (A), 2 = A+B
    if (version > kSessionVersion)
    {
        Sp3ctraDialog::showWarning(
            this,
            "Load Session",
            "This session was saved by a newer Sp3ctra version and cannot be "
            "loaded - please update the plugin.");
        return;
    }

    // On startup auto-restore, the DAW state carries slot params / sequencer
    // pattern captured at the last close — NEWER than the session's copies
    // (written at the last explicit SAVE SESSION). Keep the state versions.
    const bool skipSlotParams = isAutoRestore && processor.hasStateSamplerParams();
    const bool skipSequencer  = isAutoRestore && processor.wasSeqRestoredFromState();

    // ── XML section ───────────────────────────────────────────────────────────
    const int xmlLen = in.readInt();
    if (xmlLen <= 0 || xmlLen > 10 * 1024 * 1024)
    {
        Sp3ctraDialog::showWarning(
            this,
            "Load Session",
            "Session file is corrupt (invalid XML size).");
        return;
    }

    juce::MemoryBlock xmlBlock;
    xmlBlock.setSize(static_cast<size_t>(xmlLen));
    if (in.read(xmlBlock.getData(), xmlLen) != xmlLen)
    {
        Sp3ctraDialog::showWarning(
            this,
            "Load Session",
            "Session file is truncated (XML section).");
        return;
    }

    const juce::String xmlStr(static_cast<const char*>(xmlBlock.getData()),
                               static_cast<size_t>(xmlLen));
    auto xmlDoc = juce::parseXML(xmlStr);
    if (!xmlDoc || xmlDoc->getTagName() != "Sp3ctraSession")
    {
        Sp3ctraDialog::showWarning(
            this,
            "Load Session",
            "Session file has invalid or unrecognised XML.");
        return;
    }

    // ── Read + validate the binary .fsmp bank sections BEFORE applying ───────
    // anything (v2: engine A then B; v1: A only). Applying params/pattern first
    // and rejecting the banks afterwards left a hybrid, non-undoable state:
    // the rejected file's params on top of the old banks.
    const int numBanks = (version >= 2) ? 2 : 1;
    juce::MemoryBlock fsmpBlobs[2];
    for (int e = 0; e < numBanks; ++e)
    {
        const int fsmpLen = in.readInt();
        if (fsmpLen <= 0 || fsmpLen > 2'000'000'000)
        {
            Sp3ctraDialog::showWarning(
                this,
                "Load Session",
                "Session file is corrupt (invalid sample bank size). Nothing was loaded.");
            return;
        }
        fsmpBlobs[e].setSize(static_cast<size_t>(fsmpLen));
        if (in.read(fsmpBlobs[e].getData(), fsmpLen) != fsmpLen)
        {
            Sp3ctraDialog::showWarning(
                this,
                "Load Session",
                "Session file is truncated (sample bank section). Nothing was loaded.");
            return;
        }
    }

    // ── Apply per-slot parameters ─────────────────────────────────────────────
    // v2: one <SlotParams engine="e"> block per engine. v1: a single block,
    // no engine attribute → engine A. Shared slotParamsFromXml handles every
    // field (incl. fade curves and frequency curves).
    if (! skipSlotParams)
    {
        for (auto* slotsXml : xmlDoc->getChildWithTagNameIterator("SlotParams"))
        {
            const int e = slotsXml->getIntAttribute("engine", 0);
            if (e < 0 || e > 1) continue;
            engines[e]->setOverdubMode(slotsXml->getIntAttribute("overdub", 0) != 0);
            for (auto* s : slotsXml->getChildIterator())
            {
                const int i = s->getIntAttribute("idx", -1);
                if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) continue;
                engines[e]->slotParamsFromXml(i, *s);
            }
        }
    }

    // ── Apply sequencer state ─────────────────────────────────────────────────
    if (! skipSequencer)
    {
        if (auto* seqXml = xmlDoc->getChildByName("Sequencer"))
        {
            seq->loadFromXml(*seqXml);
            // Keep the APVTS transport params (UI + host source of truth) in
            // sync with the values the sequencer just adopted — otherwise the
            // attached controls display stale values that silently reassert
            // themselves on the next parameter edit.
            auto& apvts = processor.getAPVTS();
            auto syncParam = [&apvts](const char* id, float denorm)
            {
                if (auto* p = apvts.getParameter(id))
                    p->setValueNotifyingHost(p->convertTo0to1(denorm));
            };
            syncParam("seqBpm",          seq->getBpm());
            syncParam("seqNumSteps",     (float) seq->getNumSteps());
            syncParam("seqLoop",         seq->isLooping() ? 1.0f : 0.0f);
            syncParam("seqDawSync",      seq->isDawSync() ? 1.0f : 0.0f);
            syncParam("seqBeatsPerStep", (float) seq->getBeatsPerStep());
        }
    }

    // ── Load the pre-read .fsmp banks into the engines ────────────────────────
    for (int e = 0; e < numBanks; ++e)
    {
        // Write to temporary file and load via LuxSampler
        juce::TemporaryFile tmpFsmp(".fsmp");
        {
            juce::FileOutputStream fsmpOut(tmpFsmp.getFile());
            if (fsmpOut.failedToOpen())
            {
                Sp3ctraDialog::showWarning(
                    this,
                    "Load Session",
                    "Cannot create temporary file for sample bank.");
                return;
            }
            fsmpOut.write(fsmpBlobs[e].getData(), fsmpBlobs[e].getSize());
        }

        if (!engines[e]->loadFromFile(tmpFsmp.getFile()))
        {
            Sp3ctraDialog::showWarning(
                this,
                "Load Session",
                "Failed to load sample bank from session.");
            return;
        }
    }

    // v1 sessions carry no engine-B bank: clear B so the loaded session is
    // exactly what the file describes — otherwise the previous session's B
    // takes kept playing under the new session (and the next v2 SAVE embedded
    // them into the new file).
    if (version < 2 && engines[1] != nullptr)
    {
        engines[1]->clearAllSlots();
        for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
            engines[1]->resetSlotPlayParams(i);
        engines[1]->setOverdubMode(false);
    }

    // Bank load restored labels from the .fsmp headers — on auto-restore the
    // DAW state carries the newer per-slot values, so re-apply them on top.
    if (skipSlotParams)
        processor.applySamplerParamsFromState();

    // ── Memorise path — auto-save + DAW/Standalone auto-reload ───────────────
    currentSessionFile = sessionFile;
    processor.setLastSessionPath(sessionFile.getFullPathName());

    // A session is now explicitly loaded — drop any stale one-shot auto-load
    // still armed by an earlier setStateInformation: consumed later (next
    // editor open) it would reload an OLD .sp3s over these banks.
    (void) processor.consumeSamplerAutoLoadPending();

    // ── Refresh UI ────────────────────────────────────────────────────────────
    slotGrid  .repaint();
    slotEditor.setSelectedSlot(0);
}
