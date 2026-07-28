/**
 * @file Sp3sImporter.h
 * @brief One-shot importer for the retired LuxSampler ".sp3s session" files.
 *
 * The pre-session builds stored the recorded sampler audio ONLY in an external
 * .sp3s file referenced by an absolute path (lastSessionPath) in the state
 * blob. The session model replaced it: banks are embedded in DAW blobs
 * (SAMPLER_BANKS) or live as sidecar files in the session folder.
 *
 * This importer reads a legacy .sp3s ONCE at restore time so no recorded audio
 * is lost on upgrade, then the caller clears lastSessionPath forever. Headless
 * (no dialogs — failures are logged): it runs inside
 * applyRestoredStateOnMessageThread, before the first autosave can write an
 * empty banks/ folder over audio still referenced by the old file.
 *
 * Logic lifted from the retired SamplerPageComponent::doLoadSession — same
 * format (v1..v4), same auto-restore semantics: the state blob's SAMPLER_SLOTS
 * params and SEQS pattern are NEWER than the .sp3s copies, so they win.
 */
#pragma once

#include <juce_core/juce_core.h>

class Sp3ctraAudioProcessor;

namespace Sp3sImporter
{
    /** Import a legacy .sp3s into the live engines. Message thread only.
     *  @returns true if the sample banks were loaded. */
    bool importFile(Sp3ctraAudioProcessor& proc, const juce::File& sessionFile);
}
