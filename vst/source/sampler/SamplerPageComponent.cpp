#include "SamplerPageComponent.h"
#include "../PluginProcessor.h"
#include "../UITheme.h"
#include "../framesequencer/FrameSequencer.h"

// ============================================================================
// Session file format  (.sp3s)
//   [4]  magic    0x53503353  "SP3S"
//   [2]  version  0x0001
//   [4]  xmlLen   (bytes)
//   [*]  UTF-8 XML  (SlotParams + Sequencer state)
//   [4]  fsmpLen  (bytes)
//   [*]  binary .fsmp data  (slot audio — existing FrameSampler format)
//   [4]  EOF marker  0xDEADBEEF
// ============================================================================
namespace
{
    constexpr uint32_t kSessionMagic   = 0x53503353u; // "SP3S"
    constexpr uint16_t kSessionVersion = 0x0001u;
    constexpr uint32_t kSessionEof     = 0xDEADBEEFu;

    // ── Inline confirmation dialog — JUCE dark theme, small buttons ──────────
    // Used instead of juce::AlertWindow::showAsync to avoid:
    //   • macOS native chrome
    //   • oversized system buttons
    //   • UTF-8 encoding artefacts in juce::String literals
    class ConfirmDialog final : public juce::Component
    {
    public:
        std::function<void(bool confirmed)> onResult;

        ConfirmDialog(const char* title, const char* msg,
                      const char* confirmLabel, const char* cancelLabel)
        {
            confirmBtn_.setButtonText(confirmLabel);
            cancelBtn_ .setButtonText(cancelLabel);
            using B = juce::TextButton;
            confirmBtn_.setColour(B::buttonColourId,  juce::Colour(0xff3a1a1a));
            confirmBtn_.setColour(B::textColourOffId, juce::Colour(0xffff6644));
            cancelBtn_ .setColour(B::buttonColourId,  juce::Colour(0xff242424));
            cancelBtn_ .setColour(B::textColourOffId, juce::Colour(0xff999999));
            confirmBtn_.onClick = [this] { if (onResult) onResult(true);  dismiss(); };
            cancelBtn_ .onClick = [this] { if (onResult) onResult(false); dismiss(); };
            addAndMakeVisible(confirmBtn_);
            addAndMakeVisible(cancelBtn_);
            title_ = title;
            msg_   = msg;
        }

        void paint(juce::Graphics& g) override
        {
            const auto b = getLocalBounds().toFloat();
            // Panel background + border
            g.setColour(juce::Colour(0xf21e1e1e));
            g.fillRoundedRectangle(b, 5.0f);
            g.setColour(juce::Colour(0xff663322));
            g.drawRoundedRectangle(b.reduced(0.5f), 5.0f, 1.0f);

            // Red warning triangle ▲ (hand-drawn — no OS icon)
            constexpr float tw = 18.0f, th = 16.0f;
            constexpr float tx = 14.0f, ty = 12.0f;
            juce::Path tri;
            tri.addTriangle(tx + tw * 0.5f, ty,
                            tx,             ty + th,
                            tx + tw,        ty + th);
            g.setColour(juce::Colour(0xffcc3311));
            g.fillPath(tri);
            g.setColour(juce::Colour(0xffffeeaa));
            g.setFont(juce::FontOptions(9.0f));
            g.drawText("!", (int)tx, (int)(ty + 3), (int)tw, (int)(th - 3),
                       juce::Justification::centred);

            // Title
            g.setColour(juce::Colour(0xffdde3e8));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSettings));
            g.drawText(title_,
                       getLocalBounds().withY(10).withHeight(20),
                       juce::Justification::centredTop);

            // Message
            g.setColour(juce::Colour(0xff8a9aaa));
            g.setFont(juce::FontOptions(Sp3ctraTheme::kFontSmall));
            g.drawText(msg_,
                       getLocalBounds().reduced(12, 0).withY(32).withHeight(20),
                       juce::Justification::centredTop);
        }

        void resized() override
        {
            constexpr int pad  = 12;
            constexpr int gap  = 6;
            constexpr int btnH = Sp3ctraTheme::kControlH; // 22 px
            const int bw  = (getWidth() - 2 * pad - gap) / 2;
            const int by  = getHeight() - pad - btnH;
            confirmBtn_.setBounds(pad,            by, bw, btnH);
            cancelBtn_ .setBounds(pad + bw + gap, by, bw, btnH);
        }

        static void show(juce::Component*          parent,
                         const char*               title,
                         const char*               msg,
                         const char*               confirmLabel,
                         const char*               cancelLabel,
                         std::function<void(bool)> cb)
        {
            constexpr int dw = 310, dh = 110;
            auto* dlg = new ConfirmDialog(title, msg, confirmLabel, cancelLabel);
            dlg->onResult = std::move(cb);
            parent->addAndMakeVisible(dlg);
            dlg->setBounds((parent->getWidth()  - dw) / 2,
                           (parent->getHeight() - dh) / 2,
                           dw, dh);
            dlg->toFront(true);
        }

    private:
        void dismiss()
        {
            // Defer removal so the button's onClick finishes before we delete.
            juce::MessageManager::callAsync(
                [sp = juce::Component::SafePointer<ConfirmDialog>(this)]
                {
                    if (sp != nullptr)
                    {
                        if (auto* p = sp->getParentComponent())
                            p->removeChildComponent(sp.getComponent());
                        delete sp.getComponent();
                    }
                });
        }

        juce::String     title_, msg_;
        juce::TextButton confirmBtn_, cancelBtn_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ConfirmDialog)
    };

} // namespace

// =============================================================================
// Constructor
// =============================================================================

SamplerPageComponent::SamplerPageComponent(Sp3ctraAudioProcessor& proc)
    : processor(proc),
      slotGrid  (proc),
      slotEditor(proc),
      sequencer (proc),
      transport (proc)
{
    slotGrid.onSlotSelected = [this](int idx) { onSlotSelected(idx); };
    slotGrid  .setSelectedSlot(0);
    slotEditor.setSelectedSlot(0);

    addAndMakeVisible(slotGrid);
    addAndMakeVisible(slotEditor);
    addAndMakeVisible(sequencer);
    addAndMakeVisible(transport);

    // ── Style helper ─────────────────────────────────────────────────────────
    auto styleBtn = [](juce::TextButton& btn, juce::Colour bg, juce::Colour fg)
    {
        btn.setColour(juce::TextButton::buttonColourId,  bg);
        btn.setColour(juce::TextButton::textColourOffId, fg);
    };

    // ── Bank toolbar ──────────────────────────────────────────────────────────
    styleBtn(saveBankBtn, juce::Colour(0xff1a3a2a), juce::Colour(0xff55cc88));
    styleBtn(loadBankBtn, juce::Colour(0xff1a2a3a), juce::Colour(0xff5588cc));
    styleBtn(clearAllBtn, juce::Colour(0xff3a1a1a), juce::Colour(0xffcc5555));

    saveBankBtn.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Save Sample Bank",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.fsmp");
        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode |
            juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                const auto result = fc.getResult();
                if (result.getFullPathName().isNotEmpty())
                    if (auto* fs = processor.getFrameSampler())
                        fs->saveToFile(result.withFileExtension("fsmp"));
            });
    };

    loadBankBtn.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Load Sample Bank",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.fsmp");
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                const auto result = fc.getResult();
                if (result.getFullPathName().isNotEmpty())
                {
                    if (auto* fs = processor.getFrameSampler())
                    {
                        fs->loadFromFile(result);
                        slotGrid  .repaint();
                        slotEditor.setSelectedSlot(0);
                    }
                }
            });
    };

    clearAllBtn.onClick = [this]
    {
        if (auto* fs = processor.getFrameSampler())
        {
            fs->clearAllSlots();
            slotGrid  .repaint();
            slotEditor.setSelectedSlot(0);
        }
    };

    addAndMakeVisible(saveBankBtn);
    addAndMakeVisible(loadBankBtn);
    addAndMakeVisible(clearAllBtn);

    // ── Session toolbar ───────────────────────────────────────────────────────
    styleBtn(newSessionBtn,  juce::Colour(0xff2a1a1a), juce::Colour(0xffff8866));
    styleBtn(saveSessionBtn, juce::Colour(0xff1e2a1e), juce::Colour(0xff88ffaa));
    styleBtn(loadSessionBtn, juce::Colour(0xff1e1e2a), juce::Colour(0xff88aaff));

    // NEW SESSION — stop + clear all with confirmation
    newSessionBtn.onClick = [this]
    {
        ConfirmDialog::show(
            this,
            "Nouvelle session",
            "Effacer tous les slots et repartir de zero ?",
            "Oui, tout effacer",
            "Annuler",
            [this](bool confirmed)
            {
                if (!confirmed) return;

                // Stop the sequencer first (message thread → atomic command)
                if (auto* seq = processor.getFrameSequencer())
                {
                    seq->uiStop();
                    // Reset all step assignments to empty
                    for (int i = 0; i < FrameSequencer::MAX_STEPS; ++i)
                        seq->setStep(i, FrameSequencer::STEP_EMPTY);
                }

                // Clear all recorded slots and reset play params to defaults
                if (auto* fs = processor.getFrameSampler())
                {
                    fs->clearAllSlots();
                    for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
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

                // Refresh UI
                slotGrid  .repaint();
                slotEditor.setSelectedSlot(0);
                sequencer .repaint();
            });
    };

    saveSessionBtn.onClick = [this]
    {
        // If a session was previously loaded/saved, overwrite it directly.
        if (currentSessionFile.getFullPathName().isNotEmpty()
            && currentSessionFile.existsAsFile())
        {
            doSaveSession(currentSessionFile);
            return;
        }
        // No known path yet → show file dialog.
        fileChooser = std::make_unique<juce::FileChooser>(
            "Save Complete Session",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
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

    // Subtle separator between bank toolbar and session toolbar
    const int w      = getWidth();
    constexpr int pad    = Sp3ctraTheme::kPad;
    constexpr int gap    = Sp3ctraTheme::kGap;
    constexpr int gridH  = 66;
    constexpr int editH  = 210;
    constexpr int bankH  = Sp3ctraTheme::kControlH;
    const int bankY  = pad + gridH + gap + editH + gap;
    const int sessY  = bankY + bankH + 2;

    g.setColour(juce::Colour(0xff303030));
    g.fillRect(pad, sessY, w - 2 * pad, 1);
}

void SamplerPageComponent::resized()
{
    const int w      = getWidth();
    const int h      = getHeight();
    constexpr int pad    = Sp3ctraTheme::kPad;
    constexpr int gap    = Sp3ctraTheme::kGap;
    constexpr int gridH  = 66;
    constexpr int editH  = 210;
    constexpr int bankH  = Sp3ctraTheme::kControlH;
    constexpr int transH = 44;

    // ── Zone 1: sample bank ───────────────────────────────────────────────────
    slotGrid.setBounds(pad, pad, w - 2 * pad, gridH);

    // ── Zone 2: slot editor ───────────────────────────────────────────────────
    const int editY = pad + gridH + gap;
    slotEditor.setBounds(pad, editY, w - 2 * pad, editH);

    // ── Transport bar (pinned to bottom) ──────────────────────────────────────
    const int transY = h - pad - transH;
    transport.setBounds(pad, transY, w - 2 * pad, transH);

    // ── Bank toolbar (SAVE BANK / LOAD BANK / CLEAR ALL) ──────────────────────
    const int bankY  = editY + editH + gap;
    const int btnGap = 4;
    const int bW3    = (w - 2 * pad - 2 * btnGap) / 3;
    saveBankBtn.setBounds(pad,                     bankY, bW3, bankH);
    loadBankBtn.setBounds(pad + bW3 + btnGap,      bankY, bW3, bankH);
    clearAllBtn.setBounds(pad + 2 * (bW3 + btnGap),bankY, bW3, bankH);

    // ── Session toolbar (NEW SESSION / SAVE SESSION / LOAD SESSION) ───────────
    const int sessY = bankY + bankH + 3;
    // Reuse bW3 / btnGap — same 3-column split as bank toolbar
    newSessionBtn .setBounds(pad,                      sessY, bW3, bankH);
    saveSessionBtn.setBounds(pad + bW3 + btnGap,       sessY, bW3, bankH);
    loadSessionBtn.setBounds(pad + 2 * (bW3 + btnGap), sessY, bW3, bankH);

    // ── Zone 3: step sequencer (fills space between session row and transport)
    const int seqY = sessY + bankH + gap;
    const int seqH = transY - gap - seqY;
    sequencer.setBounds(pad, seqY, w - 2 * pad, juce::jmax(80, seqH));
}

// =============================================================================
// Session Save / Load helpers
// Non-RT: runs on the message thread only — alloc/I/O allowed.
// =============================================================================

void SamplerPageComponent::doSaveSession(const juce::File& sessionFile)
{
    auto* fs  = processor.getFrameSampler();
    auto* seq = processor.getFrameSequencer();
    if (!fs || !seq) return;

    // ── Build XML ─────────────────────────────────────────────────────────────
    juce::XmlElement root("Sp3ctraSession");
    root.setAttribute("version", 1);

    // Per-slot play parameters
    auto* slotsXml = root.createNewChildElement("SlotParams");
    for (int i = 0; i < FrameSamplerConstants::NUM_SLOTS; ++i)
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
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Save Session", "Failed to write sample bank to temporary file.");
        return;
    }

    juce::MemoryBlock fsmpBlob;
    if (!tmpFsmp.getFile().loadFileAsData(fsmpBlob) || fsmpBlob.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Save Session", "Could not read temporary sample bank.");
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
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Save Session", "Cannot open output file:\n" + sessionFile.getFullPathName());
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
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Save Session", "Write error — session may be incomplete.");
    }
}

// -----------------------------------------------------------------------------

void SamplerPageComponent::doLoadSession(const juce::File& sessionFile)
{
    auto* fs  = processor.getFrameSampler();
    auto* seq = processor.getFrameSequencer();
    if (!fs || !seq) return;

    juce::FileInputStream in(sessionFile);
    if (!in.openedOk())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Load Session", "Cannot open session file:\n" + sessionFile.getFullPathName());
        return;
    }

    // ── Header ────────────────────────────────────────────────────────────────
    const auto magic = static_cast<uint32_t>(in.readInt());
    if (magic != kSessionMagic)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Load Session", "Not a valid Sp3ctra session file (.sp3s).");
        return;
    }
    in.readShort(); // version — reserved for future compatibility checks

    // ── XML section ───────────────────────────────────────────────────────────
    const int xmlLen = in.readInt();
    if (xmlLen <= 0 || xmlLen > 10 * 1024 * 1024)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Load Session", "Session file is corrupt (invalid XML size).");
        return;
    }

    juce::MemoryBlock xmlBlock;
    xmlBlock.setSize(static_cast<size_t>(xmlLen));
    if (in.read(xmlBlock.getData(), xmlLen) != xmlLen)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Load Session", "Session file is truncated (XML section).");
        return;
    }

    const juce::String xmlStr(static_cast<const char*>(xmlBlock.getData()),
                               static_cast<size_t>(xmlLen));
    auto xmlDoc = juce::parseXML(xmlStr);
    if (!xmlDoc || xmlDoc->getTagName() != "Sp3ctraSession")
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Load Session", "Session file has invalid or unrecognised XML.");
        return;
    }

    // ── Apply per-slot parameters ─────────────────────────────────────────────
    if (auto* slotsXml = xmlDoc->getChildByName("SlotParams"))
    {
        for (auto* s : slotsXml->getChildIterator())
        {
            const int i = s->getIntAttribute("idx", -1);
            if (i < 0 || i >= FrameSamplerConstants::NUM_SLOTS) continue;
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
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Load Session", "Session file is corrupt (invalid sample bank size).");
        return;
    }

    juce::MemoryBlock fsmpBlob;
    fsmpBlob.setSize(static_cast<size_t>(fsmpLen));
    if (in.read(fsmpBlob.getData(), fsmpLen) != fsmpLen)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Load Session", "Session file is truncated (sample bank section).");
        return;
    }

    // Write to temporary file and load via FrameSampler
    juce::TemporaryFile tmpFsmp(".fsmp");
    {
        juce::FileOutputStream fsmpOut(tmpFsmp.getFile());
        if (fsmpOut.failedToOpen())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Load Session", "Cannot create temporary file for sample bank.");
            return;
        }
        fsmpOut.write(fsmpBlob.getData(), fsmpBlob.getSize());
    }

    if (!fs->loadFromFile(tmpFsmp.getFile()))
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::MessageBoxIconType::WarningIcon,
            "Load Session", "Failed to load sample bank from session.");
        return;
    }

    // ── Memorise path for auto-save (SAVE SESSION without dialog) ─────────────
    currentSessionFile = sessionFile;

    // ── Refresh UI ────────────────────────────────────────────────────────────
    slotGrid  .repaint();
    slotEditor.setSelectedSlot(0);
    sequencer .repaint();
}
