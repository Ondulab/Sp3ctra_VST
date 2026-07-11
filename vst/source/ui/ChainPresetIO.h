/**
 * @file ChainPresetIO.h
 * @brief J4 — chain presets (.sp3chain): one chain + its modules' settings.
 *
 * A preset is a readable XML ValueTree:
 *
 *   <SP3CHAIN version="1" chainsSchema="3" name="…">
 *     <CHAIN>
 *       <MODULE type="pitch"> <VALUES AttackMs="12.0" …/> </MODULE>
 *       …
 *       <MEMORY type="echo" Delay="8.0" …/>
 *     </CHAIN>
 *   </SP3CHAIN>
 *
 * Deliberately WITHOUT uuids (regenerated at load — two loads of the same
 * preset are independent instances) and WITHOUT slots (reassigned by the
 * placement rules; the processor pre-seeds them for automation stability).
 *
 * V1 scope: topology + manifest VALUES + the chain's type memory. Excluded
 * (documented): sampler audio slots (.sp3s sessions), SCORE image/settings,
 * media file paths, sequencer pattern, MIDI mappings.
 */
#pragma once

#include "ChainModel.h"

namespace ChainPresetIO
{
    inline const juce::Identifier kRootTag      { "SP3CHAIN" };
    inline const juce::Identifier kVersionProp  { "version" };
    inline const juce::Identifier kSchemaProp   { "chainsSchema" };
    inline const juce::Identifier kNameProp     { "name" };
    constexpr int kPresetVersion = 1;

    /** Preset tree for one chain (no uuids, no slots). */
    inline juce::ValueTree makePresetTree(const Chain& chain,
                                          const juce::String& name)
    {
        juce::ValueTree root(kRootTag);
        root.setProperty(kVersionProp, kPresetVersion, nullptr);
        root.setProperty(kSchemaProp, ChainModel::kSchemaVersion, nullptr);
        root.setProperty(kNameProp, name, nullptr);

        juce::ValueTree ct(ChainModel::kChainTag);
        for (const auto& m : chain.modules)
        {
            juce::ValueTree mt(ChainModel::kModuleTag);
            mt.setProperty(ChainModel::kTypeProp,
                           juce::String(moduleTypeId(m.type)), nullptr);
            if (m.values.isValid())
                mt.appendChild(m.values.createCopy(), nullptr);
            ct.appendChild(mt, nullptr);
        }
        for (const auto& [type, mem] : chain.typeMemory)
        {
            if (! mem.isValid())
                continue;
            juce::ValueTree et(ChainModel::kMemoryTag);
            et.copyPropertiesFrom(mem, nullptr);
            et.setProperty(ChainModel::kTypeProp,
                           juce::String(moduleTypeId(type)), nullptr);
            ct.appendChild(et, nullptr);
        }
        root.appendChild(ct, nullptr);
        return root;
    }

    /** Atomic write (TemporaryFile — the previous file survives any failure).
     *  Returns true on success. */
    inline bool saveToFile(const juce::ValueTree& preset, const juce::File& f)
    {
        if (! preset.hasType(kRootTag))
            return false;
        const auto xml = preset.createXml();
        if (xml == nullptr)
            return false;
        juce::TemporaryFile tmp(f);
        {
            juce::FileOutputStream out(tmp.getFile());
            if (out.failedToOpen())
                return false;
            xml->writeTo(out, {});
            out.flush();
            if (! out.getStatus().wasOk())
                return false;
        }
        return tmp.overwriteTargetFileWithTemporary();
    }

    /** Load a preset file; returns an invalid tree on any error. */
    inline juce::ValueTree loadFromFile(const juce::File& f)
    {
        const auto xml = juce::parseXML(f);
        if (xml == nullptr)
            return {};
        auto tree = juce::ValueTree::fromXml(*xml);
        if (! tree.hasType(kRootTag)
            || ! tree.getChildWithName(ChainModel::kChainTag).isValid())
            return {};
        return tree;
    }
}
