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
    constexpr uint16_t kSessionVersion = 0x0001u;
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

                // Clear all recorded slots and reset play params to defaults
                if (auto* fs = processor.getSampler(samplerIndex_))
                {
                    fs->clearAllSlots();
                    for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
                    {
                        fs->setSlotStartFrac   (i, 0.0f);
                        fs->setSlotEndFrac     (i, 1.0f);
                        fs->setSlotSpeed       (i, 1.0f);
                        fs->setSlotLoopMode    (i, LoopMode::LOOP);
                        fs->setSlotResumeMode  (i, false);
                        fs->setSlotBlendAmount (i, 0.0f);
                        fs->setSlotAttackLen   (i, 0.0f);
                        fs->setSlotDecayLen    (i, 0.0f);
                        fs->setSlotBrightnessLift(i, 0.0f);
                        fs->setSlotTrebleCut   (i, 0.0f);
                        fs->setSlotBassCut     (i, 0.0f);
                        fs->setSlotLabel       (i, "");
                    }
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
    // the editor is created, so it is already available here.
    // Deferred via callAsync so the component is fully laid out and the
    // LuxSampler player thread is started (prepareToPlay) before file I/O.
    const auto lastPath = processor.getLastSessionPath();
    if (lastPath.isNotEmpty())
    {
        const juce::File f(lastPath);
        if (f.existsAsFile())
        {
            juce::MessageManager::callAsync([this, f]
            {
                doLoadSession(f);
            });
        }
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
    constexpr int gridH  = 66;
    // Editor grew a full-width spectral-curve band at the bottom (+ Loop XF row).
    constexpr int editH  = 430;
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
    auto* fs  = processor.getSampler(samplerIndex_);
    auto* seq = processor.getFrameSequencer();
    if (!fs || !seq) return;

    // ── Build XML ─────────────────────────────────────────────────────────────
    juce::XmlElement root("Sp3ctraSession");
    root.setAttribute("version", 1);

    // Per-slot play parameters
    auto* slotsXml = root.createNewChildElement("SlotParams");
    for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
    {
        auto* s = slotsXml->createNewChildElement("Slot");
        s->setAttribute("idx",        i);
        s->setAttribute("startFrac",  static_cast<double>(fs->getSlotStartFrac(i)));
        s->setAttribute("endFrac",    static_cast<double>(fs->getSlotEndFrac(i)));
        s->setAttribute("speed",      static_cast<double>(fs->getSlotSpeed(i)));
        s->setAttribute("loopMode",      static_cast<int>(fs->getSlotLoopMode(i)));
        s->setAttribute("resumeMode",    static_cast<int>(fs->getSlotResumeMode(i)));
        s->setAttribute("label",         juce::String(fs->getSlotLabel(i)));
        s->setAttribute("blendAmount",   static_cast<double>(fs->getSlotBlendAmount(i)));
        s->setAttribute("attackLen",     static_cast<double>(fs->getSlotAttackLen(i)));
        s->setAttribute("decayLen",      static_cast<double>(fs->getSlotDecayLen(i)));
        s->setAttribute("brightnessLift",static_cast<double>(fs->getSlotBrightnessLift(i)));
        s->setAttribute("trebleCut",     static_cast<double>(fs->getSlotTrebleCut(i)));
        s->setAttribute("bassCut",       static_cast<double>(fs->getSlotBassCut(i)));
    }

    // Sequencer state (uses FrameSequencer's own serialisation)
    auto* seqXml = root.createNewChildElement("Sequencer");
    seq->saveToXml(*seqXml);

    // ── Save audio frames to a temporary .fsmp file ────────────────────────────
    juce::TemporaryFile tmpFsmp(".fsmp");
    if (!fs->saveToFile(tmpFsmp.getFile()))
    {
        Sp3ctraDialog::showWarning(
            this,
            "Save Session",
            "Failed to write sample bank to temporary file.");
        return;
    }

    juce::MemoryBlock fsmpBlob;
    if (!tmpFsmp.getFile().loadFileAsData(fsmpBlob) || fsmpBlob.isEmpty())
    {
        Sp3ctraDialog::showWarning(
            this,
            "Save Session",
            "Could not read temporary sample bank.");
        return;
    }

    // ── Serialise XML to UTF-8 ────────────────────────────────────────────────
    const juce::String xmlStr = root.toString();
    const juce::MemoryBlock xmlBlock(xmlStr.toRawUTF8(),
                                     static_cast<size_t>(xmlStr.getNumBytesAsUTF8()));

    // ── Write combined .sp3s ──────────────────────────────────────────────────
    sessionFile.deleteFile();
    juce::FileOutputStream out(sessionFile);
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
    out.writeInt  (static_cast<int>(fsmpBlob.getSize()));
    out.write     (fsmpBlob.getData(), fsmpBlob.getSize());
    out.writeInt  (static_cast<int>(kSessionEof));

    if (!out.getStatus().wasOk())
    {
        Sp3ctraDialog::showWarning(
            this,
            "Save Session",
            "Write error - session may be incomplete.");
        return;
    }
    // Persist path so the DAW project and Standalone reload this session on
    // the next launch (stored in getStateInformation via processor.lastSessionPath).
    processor.setLastSessionPath(sessionFile.getFullPathName());

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

        const int n = fs->exportAllSlotsImages(destDir, baseName, asPng);

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

void SamplerPageComponent::doLoadSession(const juce::File& sessionFile)
{
    auto* fs  = processor.getSampler(samplerIndex_);
    auto* seq = processor.getFrameSequencer();
    if (!fs || !seq) return;

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
    in.readShort(); // version — reserved for future compatibility checks

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

    // ── Apply per-slot parameters ─────────────────────────────────────────────
    if (auto* slotsXml = xmlDoc->getChildByName("SlotParams"))
    {
        for (auto* s : slotsXml->getChildIterator())
        {
            const int i = s->getIntAttribute("idx", -1);
            if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) continue;
            fs->setSlotStartFrac (i, static_cast<float>(s->getDoubleAttribute("startFrac",  0.0)));
            fs->setSlotEndFrac   (i, static_cast<float>(s->getDoubleAttribute("endFrac",    1.0)));
            fs->setSlotSpeed     (i, static_cast<float>(s->getDoubleAttribute("speed",      1.0)));
            fs->setSlotLoopMode  (i, static_cast<LoopMode>(s->getIntAttribute("loopMode",   1)));
            fs->setSlotResumeMode(i, s->getIntAttribute("resumeMode", 0) != 0);
            fs->setSlotLabel     (i, s->getStringAttribute("label", "").toRawUTF8());
            fs->setSlotBlendAmount    (i, static_cast<float>(s->getDoubleAttribute("blendAmount",    0.0)));
            fs->setSlotAttackLen      (i, static_cast<float>(s->getDoubleAttribute("attackLen",      0.0)));
            fs->setSlotDecayLen       (i, static_cast<float>(s->getDoubleAttribute("decayLen",       0.0)));
            fs->setSlotBrightnessLift (i, static_cast<float>(s->getDoubleAttribute("brightnessLift", 0.0)));
            fs->setSlotTrebleCut      (i, static_cast<float>(s->getDoubleAttribute("trebleCut",      0.0)));
            fs->setSlotBassCut        (i, static_cast<float>(s->getDoubleAttribute("bassCut",        0.0)));
        }
    }

    // ── Apply sequencer state ─────────────────────────────────────────────────
    if (auto* seqXml = xmlDoc->getChildByName("Sequencer"))
        seq->loadFromXml(*seqXml);

    // ── Binary .fsmp section ──────────────────────────────────────────────────
    const int fsmpLen = in.readInt();
    if (fsmpLen <= 0 || fsmpLen > 2'000'000'000)
    {
        Sp3ctraDialog::showWarning(
            this,
            "Load Session",
            "Session file is corrupt (invalid sample bank size).");
        return;
    }

    juce::MemoryBlock fsmpBlob;
    fsmpBlob.setSize(static_cast<size_t>(fsmpLen));
    if (in.read(fsmpBlob.getData(), fsmpLen) != fsmpLen)
    {
        Sp3ctraDialog::showWarning(
            this,
            "Load Session",
            "Session file is truncated (sample bank section).");
        return;
    }

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
        fsmpOut.write(fsmpBlob.getData(), fsmpBlob.getSize());
    }

    if (!fs->loadFromFile(tmpFsmp.getFile()))
    {
        Sp3ctraDialog::showWarning(
            this,
            "Load Session",
            "Failed to load sample bank from session.");
        return;
    }

    // ── Memorise path — auto-save + DAW/Standalone auto-reload ───────────────
    currentSessionFile = sessionFile;
    processor.setLastSessionPath(sessionFile.getFullPathName());

    // ── Refresh UI ────────────────────────────────────────────────────────────
    slotGrid  .repaint();
    slotEditor.setSelectedSlot(0);
}
