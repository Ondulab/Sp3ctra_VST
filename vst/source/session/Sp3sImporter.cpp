#include "Sp3sImporter.h"
#include "../PluginProcessor.h"
#include "../framesequencer/FrameSequencer.h"
#include "../utils/logger.h"

// Legacy .sp3s binary layout (see the retired SamplerPageComponent):
//   [4] magic 0x53503353 "SP3S" | [2] version | [4] xmlLen | [*] UTF-8 XML
//   [4+*]×numBanks .fsmp blobs | [4] EOF 0xDEADBEEF
// v1: engine A only. v2: A+B. v3: all 8 banks. v4: per-engine <Sequencer>.
namespace
{
    constexpr uint32_t kSessionMagic   = 0x53503353u; // "SP3S"
    constexpr uint16_t kSessionVersion = 0x0004u;
}

bool Sp3sImporter::importFile(Sp3ctraAudioProcessor& proc, const juce::File& sessionFile)
{
    LuxSampler* engines[LuxSampler::kMaxEngines];
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
        if ((engines[e] = proc.getSampler(e)) == nullptr) return false;

    juce::FileInputStream in(sessionFile);
    if (! in.openedOk())
    {
        log_error("VST", ".sp3s import: cannot open %s",
                  sessionFile.getFullPathName().toRawUTF8());
        return false;
    }

    // ── Header ───────────────────────────────────────────────────────────────
    if (static_cast<uint32_t>(in.readInt()) != kSessionMagic)
    {
        log_error("VST", ".sp3s import: bad magic in %s",
                  sessionFile.getFullPathName().toRawUTF8());
        return false;
    }
    const int version = static_cast<int>(in.readShort());
    if (version > kSessionVersion)
    {
        log_error("VST", ".sp3s import: version %d newer than supported (%d)",
                  version, (int) kSessionVersion);
        return false;
    }

    // The state blob's params/pattern (captured at last close) are NEWER than
    // the .sp3s copies (last explicit SAVE SESSION) — keep the state versions.
    const bool skipSlotParams = proc.hasStateSamplerParams();
    const bool skipSequencer  = proc.wasSeqRestoredFromState();

    // ── XML section ──────────────────────────────────────────────────────────
    const int xmlLen = in.readInt();
    if (xmlLen <= 0 || xmlLen > 10 * 1024 * 1024)
    {
        log_error("VST", ".sp3s import: corrupt XML size (%d)", xmlLen);
        return false;
    }
    juce::MemoryBlock xmlBlock;
    xmlBlock.setSize(static_cast<size_t>(xmlLen));
    if (in.read(xmlBlock.getData(), xmlLen) != xmlLen)
    {
        log_error("VST", ".sp3s import: truncated XML section");
        return false;
    }
    const juce::String xmlStr(static_cast<const char*>(xmlBlock.getData()),
                              static_cast<size_t>(xmlLen));
    auto xmlDoc = juce::parseXML(xmlStr);
    if (! xmlDoc || xmlDoc->getTagName() != "Sp3ctraSession")
    {
        log_error("VST", ".sp3s import: invalid session XML");
        return false;
    }

    // ── Read + validate ALL bank blobs before applying anything ──────────────
    const int numBanks = (version >= 3) ? LuxSampler::kMaxEngines
                       : (version == 2) ? 2 : 1;
    juce::MemoryBlock fsmpBlobs[LuxSampler::kMaxEngines];
    for (int e = 0; e < numBanks; ++e)
    {
        const int fsmpLen = in.readInt();
        if (fsmpLen <= 0 || fsmpLen > 2'000'000'000)
        {
            log_error("VST", ".sp3s import: corrupt bank size (engine %d)", e);
            return false;
        }
        fsmpBlobs[e].setSize(static_cast<size_t>(fsmpLen));
        if (in.read(fsmpBlobs[e].getData(), fsmpLen) != fsmpLen)
        {
            log_error("VST", ".sp3s import: truncated bank section (engine %d)", e);
            return false;
        }
    }

    // ── Apply per-slot parameters (unless the state carries newer ones) ──────
    if (! skipSlotParams)
    {
        for (auto* slotsXml : xmlDoc->getChildWithTagNameIterator("SlotParams"))
        {
            const int e = slotsXml->getIntAttribute("engine", 0);
            if (e < 0 || e >= LuxSampler::kMaxEngines) continue;
            engines[e]->setOverdubMode(slotsXml->getIntAttribute("overdub", 0) != 0);
            for (auto* s : slotsXml->getChildIterator())
            {
                const int i = s->getIntAttribute("idx", -1);
                if (i < 0 || i >= LuxSamplerConstants::NUM_SLOTS) continue;
                engines[e]->slotParamsFromXml(i, *s);
            }
        }
    }

    // ── Apply sequencer state (unless the state carries a newer pattern) ─────
    if (! skipSequencer)
    {
        auto& apvts = proc.getAPVTS();
        auto syncEngineParams = [&apvts](int e, const FrameSequencer& fs)
        {
            auto syncParam = [&apvts](const juce::String& id, float denorm)
            {
                if (auto* p = apvts.getParameter(id))
                    p->setValueNotifyingHost(p->convertTo0to1(denorm));
            };
            syncParam(fsEngineParam(e, "SeqBpm"),          fs.getBpm());
            syncParam(fsEngineParam(e, "SeqNumSteps"),     (float) fs.getNumSteps());
            syncParam(fsEngineParam(e, "SeqLoop"),         fs.isLooping() ? 1.0f : 0.0f);
            syncParam(fsEngineParam(e, "SeqDawSync"),      fs.isDawSync() ? 1.0f : 0.0f);
            syncParam(fsEngineParam(e, "SeqBeatsPerStep"), (float) fs.getBeatsPerStep());
        };

        bool sawPerEngine = false;
        for (auto* seqXml : xmlDoc->getChildWithTagNameIterator("Sequencer"))
        {
            if (! seqXml->hasAttribute("engine")) continue;
            sawPerEngine = true;
            const int e = seqXml->getIntAttribute("engine", -1);
            if (e < 0 || e >= LuxSampler::kMaxEngines) continue;
            if (auto* fs = proc.getFrameSequencer(e))
            {
                fs->loadFromXml(*seqXml);
                syncEngineParams(e, *fs);
            }
        }
        if (! sawPerEngine)
        {
            if (auto* seqXml = xmlDoc->getChildByName("Sequencer"))
                for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
                    if (auto* fs = proc.getFrameSequencer(e))
                    {
                        fs->loadFromLegacyGlobalXml(*seqXml, e);
                        syncEngineParams(e, *fs);
                    }
        }
    }

    // A session is the FULL sampler state: engines beyond the file's bank
    // count revert to empty defaults.
    for (int e = numBanks; e < LuxSampler::kMaxEngines; ++e)
    {
        engines[e]->clearAllSlots();
        for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
            engines[e]->resetSlotPlayParams(i);
    }

    // ── Load the pre-read .fsmp banks into the engines ───────────────────────
    for (int e = 0; e < numBanks; ++e)
    {
        juce::TemporaryFile tmpFsmp(".fsmp");
        {
            juce::FileOutputStream fsmpOut(tmpFsmp.getFile());
            if (fsmpOut.failedToOpen())
            {
                log_error("VST", ".sp3s import: cannot create temp bank file");
                return false;
            }
            fsmpOut.write(fsmpBlobs[e].getData(), fsmpBlobs[e].getSize());
        }
        if (! engines[e]->loadFromFile(tmpFsmp.getFile()))
        {
            log_error("VST", ".sp3s import: bank load failed (engine %d)", e);
            return false;
        }
    }

    // v1 sessions carry no engine-B bank: clear B so the loaded session is
    // exactly what the file describes.
    if (version < 2 && engines[1] != nullptr)
    {
        engines[1]->clearAllSlots();
        for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
            engines[1]->resetSlotPlayParams(i);
        engines[1]->setOverdubMode(false);
    }

    // Bank load restored labels from the .fsmp headers — the state carries the
    // newer per-slot values, so re-apply them on top (also re-renders
    // image-bound banks from their source picture).
    if (skipSlotParams)
        proc.applySamplerParamsFromState();
    else
        for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
            engines[e]->rebuildImageBoundSlots();

    log_info("VST", ".sp3s import OK: %s (v%d, %d bank(s)) — legacy session "
                    "absorbed into the project session",
             sessionFile.getFileName().toRawUTF8(), version, numBanks);
    return true;
}
