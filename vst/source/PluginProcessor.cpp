#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "session/SessionManager.h"   // working-session (project) persistence
#include "session/Sp3sImporter.h"     // one-shot legacy .sp3s migration
#include <juce_audio_formats/juce_audio_formats.h>   // SCORE source-audio preview
#include <iterator>                                  // std::size (insert bank tables)
#include "sources/MediaSourceEngines.h"              // M9 — IMAGE/VIDEO/CAMERA engines
#include "sources/MediaSourceService.h"              // M9 — source service thread
#include "sampler/SamplerMidiTargets.h"              // MIDI-Learn virtual targets (sampler play params)
#include "tts/PiperTts.h"                            // VOICE — offline TTS (startup smoke test)
#include "recorder/VideoRecorder.h"                  // VIDEO MIX + master-audio recorder

// C headers still used directly by this file
extern "C" {
    #include "core/context.h"
    #include "config/config_loader.h"
    #include "utils/logger.h"
    #include "utils/rt_profiler.h"
    #include "synthesis/luxstral/synth_luxstral_algorithms.h" // update_gap_limiter_coefficients()
    #include "synthesis/luxstral/vst_adapters.h"              // luxstral_are_audio_buffers_ready(), buffers
    #include "synthesis/luxstral/wave_generation.h"           // request_frequency_reinit() hot-reload
    #include "synthesis/luxstral/synth_luxstral_threading.h"  // synth_request_pool_restart() hot-reload
    #include "synthesis/luxstral/luxstral_wavetable.h"        // user timbre wavetable (tuned grains)
    #include "processing/lux_pitch.h"                         // LuxPitch engine (g_lux_pitch_proc)
    #include "processing/lux_mask.h"                          // LuxMask engine (g_lux_mask_proc)
    #include "processing/lux_reverb.h"                        // LuxReverb FX (g_lux_reverb_proc)
    #include "processing/lux_echo.h"                          // LuxEcho FX (g_lux_echo_proc)
    #include "processing/lux_eq.h"                            // LuxEq FX (g_lux_eq_proc)
    #include "processing/lux_harmo.h"                         // LuxHarmo/SCALE FX (g_lux_harmo_proc)
    #include "processing/video_scroll.h"                      // VideoScroll capture-ring pool
    #include "processing/image_chain.h"                       // Insert chain executor (order + taps)
    #include "processing/chain_plan.h"                         // M6 Phase 2 — RT chain descriptor
    #include "processing/synth_staging.h"                      // deferred staging resets (M3)
}
#include "ui/ChainPresetIO.h"                                  // J4 — .sp3chain presets
extern "C" {
    #include "audio/buffers/audio_image_buffers.h"             // selection tap (contextual zone 1)
    #include "synthesis/luxsynth/luxsynth_vst_adapter.h"      // luxsynth_push_midi_event(), buffers, engine
    #include "synthesis/luxwave/luxwave_vst_adapter.h"        // luxwave_push_midi_event(), g_luxwave_engine
    #include "synthesis/luxgrain/luxgrain_vst_adapter.h"      // g_luxgrain_engine + render scratch
    #include "processing/luxsynth_feed.h"                      // dropout diag counters (silence/spec pushes)

}
#include <atomic>
// Note: synth_luxstral_threading.h / synth_luxstral_runtime.h / AudioProcessingThread.h
// are now included transitively via Sp3ctraSharedCore.h and handled by Sp3ctraSharedCore.

// Global RT Profiler accessible from C threads (audioProcessingThread)
// This must be declared here (not in header) to avoid multiple definition errors
RTProfiler g_vst_rt_profiler = {};

// AUDIO MIX render gates — DEFINED in multithreading.c (C). Declared here with
// C linkage so the in-function extern below (processBlock) binds to the C
// symbol: MSVC mangles namespace-scope C++ variables, the Itanium ABI does not,
// so without this the Windows link fails on a name mismatch.
extern "C" volatile uint32_t g_engine_render_gates;

// LuxSynth dropout diagnostics — bumped in processBlock (RT), drained by the
// message-thread timer next to the [STAGING] drain. File-scope on purpose:
// shared across instances, diagnostics only.
std::atomic<uint64_t> g_lxDiagGapsVoiced{0}, g_lxDiagGapsUnvoiced{0};
std::atomic<int>      g_lxDiagLastVoices{0}, g_lxDiagLastBins{0};
std::atomic<float>    g_lxDiagLastMaxMag{0.0f};
// Click detector (single-sample discontinuities — the gap detector only sees
// full collapses). Context captured at the worst click: was a spectral latch
// applied this block, did the volume fader step, how many voices.
std::atomic<uint64_t> g_lxDiagClicks{0};
std::atomic<float>    g_lxDiagClickDelta{0.0f}, g_lxDiagClickVolStep{0.0f};
std::atomic<int>      g_lxDiagClickLatched{0}, g_lxDiagClickVoices{0};

//==============================================================================
// Pooled-insert bank suffixes — single source of truth for the per-instance
// param banks (luxpitch{N}_* / luxmask{N}_* / luxreverb{N}_* / luxecho{N}_*).
// Consumed by: listener registration, the per-chain settings memory
// (snapshot / restore / reset-to-defaults) and the legacy-session migration
// (legacy id = type prefix + suffix, bank id = prefix + N + "_" + suffix).
// MUST match the ids created in createParameterLayout().
namespace
{
    // (J1: the per-type suffix tables and InsertBankDesc moved to the single
    // manifest — ui/ModuleParamManifest.h. Call sites use moduleParamManifest()
    // directly.)
    bool isPooledInsertType(ModuleType t)
    {
        return t == ModuleType::Pitch  || t == ModuleType::Mask
            || t == ModuleType::Reverb || t == ModuleType::Echo
            || t == ModuleType::Equalizer || t == ModuleType::Harmonize;
    }
}

//==============================================================================
// Per-module-type score transport param IDs. Each SCORE-family module drives
// ONLY its own player slot, so VOICE / MIDI SCORE / TIMBRE / SCORE have
// INDEPENDENT speed / loop / reverse (moving one no longer drags the others).
// SCORE keeps the legacy "score*" ids so old sessions restore unchanged.
namespace {
struct ScoreXportIds { const char* play; const char* speed; const char* loop; const char* reverse; };

ScoreXportIds scoreXportIds(ModuleType t)
{
    switch (t)
    {
        case ModuleType::Timbre:    return { "timbrePlaying",    "timbreSpeed",    "timbreLoop",    "timbreReverse"    };
        case ModuleType::MidiScore: return { "midiScorePlaying", "midiScoreSpeed", "midiScoreLoop", "midiScoreReverse" };
        case ModuleType::Voice:     return { "voicePlaying",     "voiceSpeed",     "voiceLoop",     "voiceReverse"     };
        default:                    return { "scorePlaying",     "scoreSpeed",     "scoreLoop",     "scoreReverse"     };
    }
}

// Reverse always loops (the engine has no one-shot reverse), mirroring the
// generator pictograms: reverse → INVERSE, else loop → LOOP, else NONE.
LoopMode loopModeFromIds(juce::AudioProcessorValueTreeState& apvts,
                         const char* loopId, const char* reverseId)
{
    if (apvts.getRawParameterValue(reverseId)->load() > 0.5f) return LoopMode::INVERSE;
    if (apvts.getRawParameterValue(loopId)->load()    > 0.5f) return LoopMode::LOOP;
    return LoopMode::NONE;
}
} // namespace

//==============================================================================
// Create parameter layout (called once during construction)
juce::AudioProcessorValueTreeState::ParameterLayout Sp3ctraAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Attribute helpers: infrastructure params hidden from DAW automation lanes.
    // They remain fully functional (saved in presets, accessible via APVTS).
    const auto kHiddenInt    = juce::AudioParameterIntAttributes   {}.withAutomatable(false);
    const auto kHiddenFloat  = juce::AudioParameterFloatAttributes {}.withAutomatable(false);
    const auto kHiddenBool   = juce::AudioParameterBoolAttributes  {}.withAutomatable(false);
    const auto kHiddenChoice = juce::AudioParameterChoiceAttributes{}.withAutomatable(false);

    // ── Infrastructure — UDP ──────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_UDP_PORT, 1}, "UDP Port",
        1024, 65535, Sp3ctraConstants::DEFAULT_UDP_PORT, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_UDP_BYTE1, 1}, "UDP Byte 1", 0, 255, 192, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_UDP_BYTE2, 1}, "UDP Byte 2", 0, 255, 168, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_UDP_BYTE3, 1}, "UDP Byte 3", 0, 255, 100, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_UDP_BYTE4, 1}, "UDP Byte 4", 0, 255, 10, kHiddenInt));

    // ── Infrastructure — Device HTTP host (config.html) ──────────────────────
    // Persistent transport param: where Sp3ctraDeviceClient reaches the device's
    // embedded web server. Default 192.168.100.1 (the config page host).
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_DEVICE_IP_BYTE1, 1}, "Device IP 1", 0, 255, 192, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_DEVICE_IP_BYTE2, 1}, "Device IP 2", 0, 255, 168, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_DEVICE_IP_BYTE3, 1}, "Device IP 3", 0, 255, 100, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PARAM_DEVICE_IP_BYTE4, 1}, "Device IP 4", 0, 255, 1, kHiddenInt));

    // ── Infrastructure — Sensor / Log / Visualizer ───────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PARAM_SENSOR_DPI, 1}, "Sensor DPI",
        juce::StringArray{"200 DPI", "400 DPI"}, 1, kHiddenChoice));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PARAM_LOG_LEVEL, 1}, "Log Level",
        juce::StringArray{"Error", "Warning", "Info", "Debug"},
        Sp3ctraConstants::DEFAULT_LOG_LEVEL, kHiddenChoice));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PARAM_VISUALIZER_MODE, 1}, "Visualizer Mode",
        juce::StringArray{"Image", "Waveform", "Inverted Waveform"}, 2, kHiddenChoice));

    // ── Infrastructure — Musical scale (Settings window) ─────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralTuning", 1}, "Tuning (A4)",
        juce::NormalisableRange<float>(415.0f, 466.0f, 0.1f), 440.0f,
        kHiddenFloat.withLabel("Hz")));

    juce::StringArray noteNames;
    const char* noteLetters[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    for (int octave = 1; octave <= 6; ++octave)
        for (int note = 0; note < 12; ++note)
            noteNames.add(juce::String(noteLetters[note]) + juce::String(octave));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxstralRootNote", 1}, "Root Note",
        noteNames, 12, kHiddenChoice));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"luxstralNumOctaves", 1}, "Octaves", 1, 10, 8, kHiddenInt));

    // ── Gameplay — Envelope (ms) ─────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralAttackMs", 1}, "Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 15.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralReleaseMs", 1}, "Release",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 60.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // (Purge 2026-07-12: the legacy pre-split conditioning params —
    // luxstralInvertIntensity/GammaEnable/GammaValue/ContrastMin,
    // luxstral/luxsynth Inversion/AcRemoval/Source, luxsynthGammaValue,
    // luxpitch/luxmask Source, luxstralFidelityMode/RangeDb and the
    // VolumeWeighting/SummationResponse exponents — are DELETED. The per-OUT
    // banks are the only conditioning authority; the synthSplitVersion
    // migration still reads the OLD ids from the raw session XML, which does
    // not require the params to exist.)

    // ── Gameplay — Stereo enable (PLAY-page badge toggle) ────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralStereoEnable", 1}, "Stereo Enable",
        true));

    // ── Gameplay — Stereo temperature ────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralStereoTempAmp", 1}, "Stereo Temp.",
        juce::NormalisableRange<float>(0.0f, 5.0f, 0.01f), 2.5f));

    // ── Gameplay — Noise gate ────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralNoiseGateThreshold", 1}, "Noise Gate",
        juce::NormalisableRange<float>(0.0f, 0.1f, 0.001f), 0.005f));

    // ── Infrastructure — Worker Threads (Settings window only) ────────────────
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"luxstralNumWorkers", 1}, "Workers", 1, 16, 8, kHiddenInt));

    // ── Infrastructure — Physiological filter ────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralPhysiologicalFilter", 1}, "Equal-Loudness",
        false, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralPhysiologicalDepth", 1}, "Equal-Loudness Depth",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f, kHiddenFloat));

    // ── Gameplay — Volume Controls ────────────────────────────────────────────
    // Master output gain applied after all synthesis consumers.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"masterVolume", 1}, "Volume",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    // Per-synth volume: independent gain for LuxStral and LuxSynth consumers.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralVolume", 1}, "LuxStral Vol.",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthVolume", 1}, "LuxSynth Vol.",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainVolume", 1}, "LuxGrain Vol.",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    // LuxGrain engine enable (AUDIO MIX strip LED, like luxwaveEnabled).
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxgrainEnabled", 1}, "LuxGrain On", true));

    // ── LuxGrain engine (cloud) params — M4. Defaults mirror
    // luxgrain_config_default() so a fresh session sounds like the harness. ──
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainDensity", 1}, "LuxGrain Density",
        juce::NormalisableRange<float>(0.1f, 50.0f, 0.01f, 0.35f), 6.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("g/s")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainDensityShape", 1}, "LuxGrain Dens. Shape",
        juce::NormalisableRange<float>(0.25f, 4.0f, 0.01f, 0.5f), 1.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainSpread", 1}, "LuxGrain Spread",
        juce::NormalisableRange<float>(1.0f, 512.0f, 1.0f, 0.35f), 1.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("lines")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainSizeMin", 1}, "LuxGrain Size Min",
        juce::NormalisableRange<float>(2.0f, 100.0f, 0.1f, 0.5f), 8.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainSizeMax", 1}, "LuxGrain Size Max",
        juce::NormalisableRange<float>(20.0f, 2000.0f, 1.0f, 0.4f), 220.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainTexture", 1}, "LuxGrain Texture>Size",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.7f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainJitter", 1}, "LuxGrain Jitter",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.15f,
        juce::AudioParameterFloatAttributes{}.withLabel("st")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainWidth", 1}, "LuxGrain Width",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.6f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainAmpFollow", 1}, "LuxGrain Amp Follow",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxgrainEnvShape", 1}, "LuxGrain Envelope",
        juce::StringArray{"Hann", "Tukey", "Expodec", "Rexpodec"}, 0));
    // Lot 2 — colour pan / edge bursts / band folding / sample material.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainColorPan", 1}, "LuxGrain Color Pan",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainEdge", 1}, "LuxGrain Edge Burst",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"luxgrainBands", 1}, "LuxGrain Bands",
        16, 192, 128, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxgrainMaterial", 1}, "LuxGrain Material",
        juce::StringArray{"Sine", "Sample"}, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxgrainScrub", 1}, "LuxGrain Scrub",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f));

    // ── Gameplay — Device On ─────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"deviceEnabled", 1}, "Device On", true));

    // ── Setup — Soft limiter (LuxStral) ──────────────────────────────────────
    // These IDs were referenced by LuxStralSetupPanel but never created — the
    // sliders were silently inert (JUCE skips attachments on unknown IDs).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralSoftLimitThreshold", 1}, "Soft Limit Thr.",
        juce::NormalisableRange<float>(0.1f, 1.0f, 0.01f), 0.8f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralSoftLimitKnee", 1}, "Soft Limit Knee",
        juce::NormalisableRange<float>(0.01f, 1.0f, 0.01f), 0.2f, kHiddenFloat));

    // (Purge 2026-07-12: luxstralFidelityMode/RangeDb deleted — the inverse-dB
    // decode law is ALWAYS ON with its dB window PER OUT SEND, from the
    // luxstralOut{N}_rangeDb bank; the migration seeds the banks from the old
    // shared knob read out of the raw session XML.)

    // ── Gameplay — Phase management: physical onset modes ───────────────────
    // When a note attacks (strong volume jump crossing the gate from silence),
    // its oscillator's phase is set by the mode's physical initial conditions:
    //   Strike = struck string (velocity → ±sine)  Pluck = plucked (±cosine)
    //   Bell   = fixed per-note impact hash (repeatable, never combs)
    //   Breath = fresh random per attack (reed/flute turbulence)
    // Free = legacy free-running phases. Silent oscillators idle on random
    // phases → a mode change never inherits the previous organization.
    // Order must match LUXSTRAL_PHASE_MODE_* (config_loader.h).
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxstralPhaseMode", 1}, "Phase Mode",
        juce::StringArray{"Free", "Strike", "Pluck", "Bell", "Breath"}, 0));

    // Onset sensitivity — RELATIVE to the material: the engine tracks a
    // rolling max of note volumes and gates at fraction × ref, so selecting
    // a mode always works without tuning an absolute threshold. 1 = catch
    // even soft onsets (3 % of recent peak), 0 = only the hardest attacks.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralPhaseSensitivity", 1}, "Phase Sensitivity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.7f));

    // Strike/Pluck: position along the string (sign-alternation period of the
    // partials' phases; 0 = whole band aligned). Bell: impact point (16
    // distinct, each a stable phase fingerprint). Breath: unused.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralPhasePosition", 1}, "Phase Position",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));

    // Phase drift — companion of Phase Reset: each onset also redraws a random
    // micro-detune of ±cents for that note. Melts the flanger-like comb of
    // phase-aligned onsets into ensemble texture while keeping the coherent
    // attack. 0 = off (grid-exact pitch, full flanger character).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralPhaseDriftCents", 1}, "Phase Drift",
        juce::NormalisableRange<float>(0.0f, 3.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ct")));

    // Timbre wavetable mix — blend between the analytic sine/square bank and
    // the user-sample wavetable (tuned-grain timbre, luxstral_wavetable.h).
    // Inert while no sample is loaded; default 1.0 so loading a sample is
    // immediately audible.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralTimbreMix", 1}, "Timbre Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    // Timbre scan position — WHERE in the retained sample the cycle is
    // extracted (0 = start, 1 = end). Each move re-extracts and renormalizes
    // at the new spot (quiet passages play as loud as strong ones — only the
    // color changes). Automatable/MIDI-mappable; coalesced on the 30 ms drain.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralTimbrePos", 1}, "Timbre Position",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.5f));

    // Timbre master switch — the badge toggle of the TIMBRE section: OFF
    // bypasses the whole sample-timbre feature (pure analytic SIN bank,
    // mix AND formant inert), ON restores it. MIDI-mappable A/B gesture.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralTimbreEnable", 1}, "Timbre Enable",
        true));

    // Formant follower — depth of the vocoder-like per-note weighting by the
    // sample's spectral envelope at the scan position. Independent of the
    // mix (also colors the pure sine bank). Inert without a loaded sample.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralTimbreFormant", 1}, "Timbre Formant",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    // Timbre playhead — when ON, the scan position advances through the file
    // on its own (looping), at Rate × real time. The timbre then follows the
    // sample's evolution — the link between source and sound becomes obvious.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxstralTimbreScanPlay", 1}, "Timbre Scan Play",
        false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxstralTimbreScanRate", 1}, "Timbre Scan Rate",
        juce::NormalisableRange<float>(0.05f, 4.0f, 0.01f, 0.5f), 1.0f));

    // ── Synth-split P1 — per-OUT conditioning banks (pool slots 0..7) ────────
    // One bank per OUT-module instance: the OUT conditions its chain's flux
    // before sending it to the global engine. Gamma has NO enable toggle
    // (1.0 = off); Intensity is the pre-engine mix weight of the send.
    // Defaults mirror the legacy conditioning defaults so a fresh session is
    // bit-identical. (The legacy pre-split globals were deleted from the
    // layout on 2026-07-12 — the migration reads their OLD ids from the raw
    // session XML, see setStateInformation.)
    for (int s = 0; s < 8; ++s)
    {
        const juce::String n(s);

        // LuxStral OUT
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lsOutParam(s, "negative"), 1},
            "LS OUT" + n + " Negative", true));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lsOutParam(s, "dcBlocking"), 1},
            "LS OUT" + n + " DC Blocking", true));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lsOutParam(s, "gamma"), 1},
            "LS OUT" + n + " Gamma",
            juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lsOutParam(s, "contrastMin"), 1},
            "LS OUT" + n + " Contrast Min",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.21f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lsOutParam(s, "rangeDb"), 1},
            "LS OUT" + n + " Range",
            juce::NormalisableRange<float>(20.0f, 80.0f, 0.5f), 50.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("dB")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lsOutParam(s, "intensity"), 1},
            "LS OUT" + n + " Intensity",
            juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 1.0f));
        // Per-send power (rack LED) — the ENGINE enable stays deviceEnabled
        // (AUDIO MIX strip LED). A disabled send contributes silence to the mix.
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lsOutParam(s, "enabled"), 1},
            "LS OUT" + n + " On", true));

        // LuxSynth OUT
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lxOutParam(s, "negative"), 1},
            "LX OUT" + n + " Negative", true));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lxOutParam(s, "dcBlocking"), 1},
            "LX OUT" + n + " DC Blocking", true));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lxOutParam(s, "gamma"), 1},
            "LX OUT" + n + " Gamma",
            juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lxOutParam(s, "intensity"), 1},
            "LX OUT" + n + " Intensity",
            juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lxOutParam(s, "enabled"), 1},
            "LX OUT" + n + " On", true));

        // LuxWave OUT — autonomous conditioning (no longer inherits LuxSynth's)
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lwOutParam(s, "negative"), 1},
            "LW OUT" + n + " Negative", true));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lwOutParam(s, "dcBlocking"), 1},
            "LW OUT" + n + " DC Blocking", true));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lwOutParam(s, "gamma"), 1},
            "LW OUT" + n + " Gamma",
            juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lwOutParam(s, "intensity"), 1},
            "LW OUT" + n + " Intensity",
            juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lwOutParam(s, "enabled"), 1},
            "LW OUT" + n + " On", true));

        // LuxGrain OUT — autonomous conditioning (granular engine, M2)
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lgOutParam(s, "negative"), 1},
            "LG OUT" + n + " Negative", true));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lgOutParam(s, "dcBlocking"), 1},
            "LG OUT" + n + " DC Blocking", true));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lgOutParam(s, "gamma"), 1},
            "LG OUT" + n + " Gamma",
            juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{lgOutParam(s, "intensity"), 1},
            "LG OUT" + n + " Intensity",
            juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{lgOutParam(s, "enabled"), 1},
            "LG OUT" + n + " On", true));
    }

    // ── Gameplay — StrokeForge enable ────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sfEnabled", 1}, "SF Active", false));

    // ── Infrastructure — SF internal parameters ───────────────────────────────
    // sfBlobBaseThreshold / sfBlobMinWidth / sfBlobMergeGap removed:
    // these are now exclusively controlled by spctrBlob* (IMAGE LUXSTRAL tab).
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sfBlobContrastAdaptive", 1}, "SF Contrast Adaptive",
        true, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfBlobContrastSensitivity", 1}, "SF Contrast Sensitivity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f, kHiddenFloat));

    // ── Infrastructure — SF advanced ─────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"sfMaxHarmonics", 1}, "SF Max Harmonics",
        1, 16, 8, kHiddenInt));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfHarmonicAmpFloor", 1}, "SF Harmonic Floor",
        juce::NormalisableRange<float>(0.001f, 0.1f, 0.001f), 0.01f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfVolumeCenterSigma", 1}, "SF Volume Sigma",
        juce::NormalisableRange<float>(0.1f, 2.0f, 0.01f), 0.4f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sfPhaseCoherence", 1}, "SF Phase Coherence",
        true, kHiddenBool));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfPhaseSmoothAlpha", 1}, "SF Phase Alpha",
        juce::NormalisableRange<float>(0.01f, 0.5f, 0.01f), 0.05f, kHiddenFloat));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfMorphWidthScale", 1}, "SF Morph Scale",
        juce::NormalisableRange<float>(2.0f, 500.0f, 1.0f), 400.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"sfWavetableMinWidth", 1}, "SF WT Min Width",
        1, 200, 50, kHiddenInt));

    // ── Gameplay — Focus Sigma / Spectral Threshold ───────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfBlobFocusSigma", 1}, "Focus Sigma",
        juce::NormalisableRange<float>(0.5f, 100.0f, 0.5f, 0.4f), 20.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("notes")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sfSpectralWidthThreshold", 1}, "Spectral Thr.",
        juce::NormalisableRange<float>(0.0f, 3456.0f, 1.0f), 200.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("notes")));

    // ── Gameplay — Focus Only ─────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"sfFocusOnly", 1}, "Focus Only", false));

    // ── Image Pipeline ────────────────────────────────────────────────────────
    // imageFreezeMode: 0 = PLAY (live frames flow)
    //                  1 = HOLD (freeze last captured frame)
    //                  2 = WHITE (force all pixels → 255, silences synthesis)
    // Stored as int [0..2].  Backend effect is in CisVisualizerComponent::updateCisData().
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"imageFreezeMode", 1}, "Freeze Mode", 0, 2, 0));

    // Live stream opacity [0..1] — darken-blend weight applied to the live CIS frame.
    // Blend mode: min(live_pixel, sampler_pixel) per channel (ImageChops.darker equivalent).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imageLiveOpacity", 1}, "Live Opacity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f, kHiddenFloat));

    // Sampler stream opacity [0..1] — driven by imageMixBalance crossfader.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imageSamplerOpacity", 1}, "Sampler Opacity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f, kHiddenFloat));

    // Mix balance crossfader: 0.0=full Sampler, 0.5=equal, 1.0=full Live.
    // Drives imageLiveOpacity and imageSamplerOpacity via parameterChanged.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imageMixBalance", 1}, "Mix Balance",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f, kHiddenFloat));

    // (Purge 2026-07-12: the per-path routing/conditioning params —
    // luxstral/luxsynth Source/Inversion/AcRemoval, luxsynthGammaValue — are
    // deleted: the ChainPlan routes, the per-OUT banks condition.)

    // ── LuxSynth blob detection — independent of StrokeForge/LuxStral ────────
    // These parameters are owned exclusively by the SYNTH_BLOB visualizer
    // (CisVisualizerComponent::detectSynthBlobs). They have NO effect on the
    // LuxStral audio synthesis path (which uses the sf* parameters).
    // Amplitude threshold: normalised CIS brightness [0..1].
    // Wide range [0.001..1.0] to accommodate both high-contrast and low-contrast sources.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"lxBlobThreshold", 1}, "LX Blob Thr.",
        juce::NormalisableRange<float>(0.001f, 1.0f, 0.001f, 0.35f), 0.05f,
        juce::AudioParameterFloatAttributes{}.withLabel("")));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"lxBlobMinWidth", 1}, "LX Blob Min W",
        1, 200, 10));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"lxBlobMergeGap", 1}, "LX Merge Gap",
        0, 100, 3));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"lxBlobColorSplit", 1}, "LX Color Split",
        juce::NormalisableRange<float>(0.01f, 1.0f, 0.01f), 0.20f,
        juce::AudioParameterFloatAttributes{}.withLabel("")));

    // ── SPCTR (LuxStral) blob detection — identical ranges to lxBlob* ────────
    // These params drive both the IMAGE LUXSTRAL visualizer (detectSpctrBlobs)
    // and the StrokeForge audio synthesis engine (via g_sp3ctra_config).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"spctrBlobThreshold", 1}, "SPCTR Blob Thr.",
        juce::NormalisableRange<float>(0.001f, 1.0f, 0.001f, 0.35f), 0.05f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"spctrBlobMinWidth", 1}, "SPCTR Blob Min W",
        1, 200, 10));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"spctrBlobMergeGap", 1}, "SPCTR Merge Gap",
        0, 100, 3));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"spctrBlobColorSplit", 1}, "SPCTR Color Split",
        juce::NormalisableRange<float>(0.01f, 1.0f, 0.01f), 0.20f));

    // ── LuxSynth FFT quality / synthesis-data parameters ─────────────────────
    // lxFftBins: number of harmonics extracted from the spatial FFT.
    // Each bin maps to one oscillator in the LuxSynth additive synthesis engine.
    // 32 = fast / low quality, 256 = slow / high quality (default 128).
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"lxFftBins", 1}, "LX FFT Bins",
        juce::StringArray{"32", "64", "128", "256"}, 2));

    // lxFftSmoothing: temporal smoothing for FFT magnitudes [0..1].
    // 0 = very fast / reactive (alpha_attack≈0.80, alpha_release≈0.50).
    // 1 = very slow / smooth   (alpha_attack≈0.05, alpha_release≈0.02).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"lxFftSmoothing", 1}, "LX FFT Smoothing",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f));

    // ── LuxSynth Engine Parameters ────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxsynthEnabled", 1}, "LuxSynth Active", false));

    // Volume ADSR
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthAttackMs", 1}, "LS Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 10.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthDecayMs", 1}, "LS Decay",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthSustainLevel", 1}, "LS Sustain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.7f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthReleaseMs", 1}, "LS Release",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 200.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // Filter ADSR
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterAttackMs", 1}, "LS Flt Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 20.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterDecayMs", 1}, "LS Flt Decay",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 150.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterSustain", 1}, "LS Flt Sustain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterReleaseMs", 1}, "LS Flt Release",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 300.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterCutoff", 1}, "LS Flt Cutoff",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterEnvDepth", 1}, "LS Flt Depth",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));

    // ADSR segment curvature ([-1,1], 0 = linear) — volume + filter envelopes
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthAttackCurve", 1}, "LS Attack Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthDecayCurve", 1}, "LS Decay Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthReleaseCurve", 1}, "LS Release Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterAttackCurve", 1}, "LS Flt Attack Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterDecayCurve", 1}, "LS Flt Decay Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthFilterReleaseCurve", 1}, "LS Flt Release Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));

    // Spectral (per-OUT bank gamma is the only gamma — the engine-side
    // "luxsynthGamma" param was never read and is gone, purge 2026-07-12)
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"luxsynthNumOscillators", 1}, "LS Oscillators",
        1, 128, 64));

    // LFO
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthLfoRate", 1}, "LS LFO Rate",
        juce::NormalisableRange<float>(0.0f, 20.0f, 0.01f), 5.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxsynthLfoDepth", 1}, "LS LFO Depth",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.1f,
        juce::AudioParameterFloatAttributes{}.withLabel("st")));

    // LuxSynth MIDI Channel (Channel 1-16)
    {
        juce::StringArray lxMidiChNames;
        for (int i = 1; i <= 16; ++i)
            lxMidiChNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxsynthMidiChannel", 1}, "LuxSynth MIDI Channel",
            lxMidiChNames, 0, kHiddenChoice));  // default = Channel 1 (index 0)
    }

    // LuxSynth Octave Offset (-2 .. +2)
    {
        juce::StringArray octaveNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxsynthOctaveOffset", 1}, "LuxSynth Octave Offset",
            octaveNames, 2, kHiddenChoice));  // default index 2 = 0
    }

    // ── LuxWave Engine Parameters ─────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxwaveEnabled", 1}, "LuxWave Active", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveVolume", 1}, "LuxWave Vol.",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    // LuxWave Volume ADSR
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveAttackMs", 1}, "LW Attack",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 10.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveDecayMs", 1}, "LW Decay",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveSustainLevel", 1}, "LW Sustain",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.7f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveReleaseMs", 1}, "LW Release",
        juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 200.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    // ADSR segment curvature ([-1,1], 0 = linear)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveAttackCurve", 1}, "LW Attack Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveDecayCurve", 1}, "LW Decay Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveReleaseCurve", 1}, "LW Release Curve",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));

    // LuxWave Filter ADSR
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveFilterCutoff", 1}, "LW Flt Cutoff",
        juce::NormalisableRange<float>(100.0f, 20000.0f, 1.0f, 0.3f), 8000.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveFilterEnvDepth", 1}, "LW Flt Depth",
        juce::NormalisableRange<float>(0.0f, 20000.0f, 1.0f, 0.3f), 4000.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));

    // LuxWave LFO
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveLfoRate", 1}, "LW LFO Rate",
        juce::NormalisableRange<float>(0.0f, 20.0f, 0.01f), 5.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveLfoDepth", 1}, "LW LFO Depth",
        juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.1f,
        juce::AudioParameterFloatAttributes{}.withLabel("st")));

    // LuxWave Scan Mode
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"luxwaveScanMode", 1}, "LW Scan Mode",
        juce::StringArray{juce::String::fromUTF8("Left→Right"),
                          juce::String::fromUTF8("Right→Left"), "Dual"}, 0));

    // LuxWave Amplitude
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxwaveAmplitude", 1}, "LW Amplitude",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));

    // LuxWave MIDI Channel (Channel 1-16)
    {
        juce::StringArray lwMidiChNames;
        for (int i = 1; i <= 16; ++i)
            lwMidiChNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxwaveMidiChannel", 1}, "LuxWave MIDI Channel",
            lwMidiChNames, 0, kHiddenChoice));
    }

    // LuxWave Octave Offset (-2 .. +2)
    {
        juce::StringArray lwOctNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxwaveOctaveOffset", 1}, "LuxWave Octave Offset",
            lwOctNames, 2, kHiddenChoice));
    }

    // (P4-M5: "chainInsertOrder" deleted — per-chain insert order comes from
    // each chain's own recipe; the global order died with the modulated bus.)

    // ── PITCH / MASK — per-instance automatable banks (×8) ───────────────────
    // Each pooled insert instance owns a state-pool slot 0..7 (modulePoolSlots_,
    // keyed by the ModuleInstance UUID) that keys both its RT pool instance
    // (lux_pitch_instance / lux_mask_instance) and this APVTS bank — the same
    // pattern as the VideoScroll banks below. Same ranges/defaults as the
    // former per-type params; the zone-3 pages rebind luxpitch{N}_* /
    // luxmask{N}_* when a block is selected.
    {
        juce::StringArray midiChNames;
        for (int i = 1; i <= 16; ++i)
            midiChNames.add("Channel " + juce::String(i));
        const juce::StringArray octNames { "-2", "-1", " 0", "+1", "+2" };
        juce::StringArray noteNames;   // C1..B6 (72 items), default A3 = index 33
        {
            const char* noteLetters[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            for (int oct = 1; oct <= 6; ++oct)
                for (int n = 0; n < 12; ++n)
                    noteNames.add(juce::String(noteLetters[n]) + juce::String(oct));
        }

        for (int n = 0; n < 8; ++n)
        {
            const juce::String tag = "P" + juce::String(n) + " ";
            auto id = [n](const char* sfx) { return juce::ParameterID{lpParam(n, sfx), 1}; };

            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("Enabled"), tag + "Enabled", false));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("Polyphony"), tag + "Polyphony", true));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("BackgroundMode"), tag + "Background",
                juce::StringArray{"Black", "White"}, 1));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("CouplingMode"), tag + "Coupling",
                juce::StringArray{"LuxStral", "Free"}, 0, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("FreePixelsPerST"), tag + "px/semitone",
                juce::NormalisableRange<float>(1.0f, 200.0f, 0.5f), 36.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("PitchBendRange"), tag + "PB Range",
                juce::NormalisableRange<float>(0.0f, 24.0f, 0.5f), 2.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("st")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("AttackMs"), tag + "Attack",
                juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 10.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("DecayMs"), tag + "Decay",
                juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 50.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("SustainLevel"), tag + "Sustain",
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("ReleaseMs"), tag + "Release",
                juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 100.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            // Per-segment curvature [-1,1] (0 = linear). Set visually by bending
            // each envelope segment; MIDI-mappable like every other param.
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("AttackCurve"), tag + "Attack Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("DecayCurve"), tag + "Decay Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("ReleaseCurve"), tag + "Release Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.5f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("GlideMs"), tag + "Glide",
                juce::NormalisableRange<float>(0.0f, 5000.0f, 1.0f, 0.3f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("LfoRate"), tag + "LFO Rate",
                juce::NormalisableRange<float>(0.0f, 20.0f, 0.01f), 5.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("LfoDepth"), tag + "LFO Depth",
                juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("st")));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("VelocityCoupling"), tag + "Velocity", false));
            // MIDI infrastructure (SETUP face)
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("MidiChannel"), tag + "MIDI Channel", midiChNames, 0, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("OctaveOffset"), tag + "Octave Offset", octNames, 2, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("ReferenceNote"), tag + "Reference Note",
                noteNames, 33, kHiddenChoice));  // A3 = index 33
        }

        // ── MASK banks — spatial bandpass filter driven by the ADSR ──────────
        // Always a bandpass centred on the played note (keyboard tracking). The
        // ADSR output is the openness (0 = closed to nothing, 1 = full width).
        //   Width : band width at full open, % of image.
        //   Offset: band-centre offset from the note, % of image.
        //   Slope : edge steepness (1 = sharp, 0 = soft).
        for (int n = 0; n < 8; ++n)
        {
            const juce::String tag = "M" + juce::String(n) + " ";
            auto id = [n](const char* sfx) { return juce::ParameterID{lmParam(n, sfx), 1}; };

            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("Enabled"), tag + "Enabled", false));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("Polyphony"), tag + "Polyphony", true));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("BackgroundMode"), tag + "Background",
                juce::StringArray{"Black", "White"}, 1));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("CouplingMode"), tag + "Coupling",
                juce::StringArray{"LuxStral", "Free"}, 0, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("FreePixelsPerST"), tag + "px/semitone",
                juce::NormalisableRange<float>(1.0f, 200.0f, 0.5f), 36.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("PitchBendRange"), tag + "PB Range",
                juce::NormalisableRange<float>(0.0f, 24.0f, 0.5f), 2.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("st")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("FilterWidth"), tag + "Filter Width",
                juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 30.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("%")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("FilterOffset"), tag + "Filter Offset",
                juce::NormalisableRange<float>(-100.0f, 100.0f, 0.1f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("%")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("FilterSlope"), tag + "Filter Slope",
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));
            // ADSR (drives the filter cutoff/openness)
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("AttackMs"), tag + "Attack",
                juce::NormalisableRange<float>(0.5f, 5000.0f, 0.1f, 0.3f), 20.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("DecayMs"), tag + "Decay",
                juce::NormalisableRange<float>(0.5f, 10000.0f, 0.1f, 0.3f), 120.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("SustainLevel"), tag + "Sustain",
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("ReleaseMs"), tag + "Release",
                juce::NormalisableRange<float>(0.5f, 10000.0f, 0.1f, 0.3f), 200.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("AttackCurve"), tag + "Attack Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("DecayCurve"), tag + "Decay Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("ReleaseCurve"), tag + "Release Curve",
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.5f));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("GlideMs"), tag + "Glide",
                juce::NormalisableRange<float>(0.0f, 5000.0f, 1.0f, 0.3f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("ms")));
            // Position LFO only (vibrato) — the width LFO never sounded musical.
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("LfoPosRate"), tag + "LFO Pos Rate",
                juce::NormalisableRange<float>(0.0f, 30.0f, 0.01f), 5.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("Hz")));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                id("LfoPosDepth"), tag + "LFO Pos Depth",
                juce::NormalisableRange<float>(0.0f, 2.0f, 0.01f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("st")));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                id("VelocityCoupling"), tag + "Velocity", false));
            // MIDI infrastructure (SETUP face)
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("MidiChannel"), tag + "MIDI Channel", midiChNames, 0, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("OctaveOffset"), tag + "Octave Offset", octNames, 2, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                id("ReferenceNote"), tag + "Reference Note",
                noteNames, 33, kHiddenChoice));  // A3 = index 33
        }
    }

    // ── REVERB / ECHO FX — per-instance automatable banks (×8) ───────────────
    for (int n = 0; n < 8; ++n)
    {
        const juce::String tag = "RV" + juce::String(n) + " ";
        auto id = [n](const char* sfx) { return juce::ParameterID{rvParam(n, sfx), 1}; };

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id("Enabled"), tag + "Enabled", false));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Decay"), tag + "Decay",
            juce::NormalisableRange<float>(0.1f, 20.0f, 0.01f, 0.4f), 3.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("s")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Diffusion"), tag + "Diffusion",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 30.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("%")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Mix"), tag + "Mix",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 60.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("%")));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id("BackgroundMode"), tag + "Background",
            juce::StringArray{"Auto", "Black", "White"}, 2));
    }
    for (int n = 0; n < 8; ++n)
    {
        const juce::String tag = "EC" + juce::String(n) + " ";
        auto id = [n](const char* sfx) { return juce::ParameterID{ecParam(n, sfx), 1}; };

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id("Enabled"), tag + "Enabled", false));
        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id("Delay"), tag + "Delay",
            1, LUX_ECHO_MAX_DELAY, 48,
            juce::AudioParameterIntAttributes{}.withLabel("lines")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Feedback"), tag + "Feedback",
            juce::NormalisableRange<float>(0.0f, 95.0f, 0.1f), 35.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("%")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Mix"), tag + "Mix",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 60.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("%")));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id("BackgroundMode"), tag + "Background",
            juce::StringArray{"Auto", "Black", "White"}, 2));
    }
    for (int n = 0; n < 8; ++n)
    {
        const juce::String tag = "EQ" + juce::String(n) + " ";
        auto id = [n](const char* sfx) { return juce::ParameterID{eqParam(n, sfx), 1}; };

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id("Enabled"), tag + "Enabled", false));
        // 9 gain nodes on the octave boundaries of the pixel/frequency axis
        // (LUX_EQ_NUM_BANDS) — 0 dB default = flat curve = pass-through.
        for (int b = 0; b < LUX_EQ_NUM_BANDS; ++b)
        {
            const auto sfx = "Band" + juce::String(b);
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{eqParam(n, sfx.toRawUTF8()), 1},
                tag + sfx,
                juce::NormalisableRange<float>(-LUX_EQ_GAIN_DB_MAX,
                                               LUX_EQ_GAIN_DB_MAX, 0.1f), 0.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("dB")));
        }
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id("BackgroundMode"), tag + "Background",
            juce::StringArray{"Auto", "Black", "White"}, 2));
    }
    for (int n = 0; n < 8; ++n)
    {
        const juce::String tag = "SC" + juce::String(n) + " ";
        auto id = [n](const char* sfx) { return juce::ParameterID{hmParam(n, sfx), 1}; };

        params.push_back(std::make_unique<juce::AudioParameterBool>(
            id("Enabled"), tag + "Enabled", false));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id("Mode"), tag + "Mode",
            juce::StringArray{"Mask", "Warp"}, LUX_HARMO_MODE_WARP));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id("Root"), tag + "Root",
            juce::StringArray{"C", "C#", "D", "D#", "E", "F",
                              "F#", "G", "G#", "A", "A#", "B"}, 0));
        // Order MUST match the LUX_HARMO_SCALE_* preset indices (lux_harmo.h).
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id("Scale"), tag + "Scale",
            juce::StringArray{"Chromatic", "Major", "Minor", "Harm Minor",
                              "Penta Maj", "Penta Min", "Blues", "Whole Tone",
                              "Dorian", "Phrygian", "Lydian", "Mixolydian",
                              "Fifths", "Octaves"}, LUX_HARMO_SCALE_MAJOR));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Strength"), tag + "Strength",
            juce::NormalisableRange<float>(0.0f, 100.0f, 0.1f), 100.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("%")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Width"), tag + "Width",
            juce::NormalisableRange<float>(0.05f, 1.0f, 0.01f), 0.35f,
            juce::AudioParameterFloatAttributes{}.withLabel("st")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            id("Slope"), tag + "Slope",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));
        params.push_back(std::make_unique<juce::AudioParameterInt>(
            id("Glide"), tag + "Glide",
            0, 1000, 64,
            juce::AudioParameterIntAttributes{}.withLabel("lines")));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            id("BackgroundMode"), tag + "Background",
            juce::StringArray{"Auto", "Black", "White"}, 2));
    }

    // Fade-in duration [ms] — applied when restarting the live stream after Stop.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imageFadeInMs", 1}, "Fade-In",
        juce::NormalisableRange<float>(0.0f, 2000.0f, 10.0f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));

    // ── Sampler stream preprocessing (mirroring live params independently) ────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"samplerGamma", 1}, "Sampler Gamma",
        juce::NormalisableRange<float>(0.01f, 10.0f, 0.01f, 0.30f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"samplerContrastMin", 1}, "Sampler Contrast Min",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.0f));
    // samplerFreezeMode: 0=PLAY, 1=HOLD (freeze last sampler frame), 2=STOP (silence)
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"samplerFreezeMode", 1}, "Sampler Freeze Mode",
        0, 2, 0));
    // (P4 2026-07-14: samplerFadeInMs + rawFadeInMs deleted — the sampler
    // transport is instant and the RAW gate carries no fade; the only
    // transport fade is imageFadeInMs, on the SP3CTRA source transport.)

    // rawFreezeMode: 0=PLAY, 1=HOLD (freeze last raw frame), 2=STOP (white)
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"rawFreezeMode", 1}, "RAW Freeze Mode",
        0, 2, 0));

    // ── LuxSampler ──────────────────────────────────────────────────────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxSamplerEnabled", 1}, "LuxSampler Enabled",
        false));

    {
        juce::StringArray midiChannelNames;
        for (int i = 1; i <= 16; ++i)
            midiChannelNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerMidiChannel", 1}, "LuxSampler MIDI Channel",
            midiChannelNames, 0, kHiddenChoice));  // default = Channel 1 (index 0)
    }

    {
        juce::StringArray octaveNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerOctaveOffset", 1}, "LuxSampler Octave Offset",
            octaveNames, 2, kHiddenChoice));  // default index 2 = 0
    }

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"luxSamplerMaxDuration", 1}, "LuxSampler Max Duration",
        juce::NormalisableRange<float>(1.0f, 60.0f, 0.1f), 10.0f, kHiddenFloat));

    // REC / PLAY transport-button mode (engine A). Toggle = bistable click
    // (press flips record / play on↔off). Momentary = press-and-hold (start on
    // press, stop on release) — applies to BOTH the UI buttons and MIDI-mapped
    // keys. Default Toggle preserves the historical click behaviour.
    {
        juce::StringArray modeNames { "Toggle", "Momentary" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerRecMode", 1}, "LuxSampler REC Mode",
            modeNames, 0, kHiddenChoice));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerPlayMode", 1}, "LuxSampler PLAY Mode",
            modeNames, 0, kHiddenChoice));
    }

    // Number of banks shown in the SAMPLER page (1..6). A choice param (index
    // 0..5 → 1..6 banks) so the SETUP combo binds mechanically; the engine
    // keeps its NUM_SLOTS internal slots for session compatibility.
    {
        juce::StringArray bankCounts { "1", "2", "3", "4", "5", "6" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerNumBanks", 1}, "LuxSampler Banks",
            bankCounts, 3, kHiddenChoice));  // default = 4 banks
    }

    // ── LuxSampler engine B — its own play-param bank ("Part B") ─────────────
    // Same suffixes as A's legacy ids so the UI rebinds mechanically
    // (fsEngineParam). MIDI channel defaults to 2, preserving the value that
    // used to be hard-coded in the constructor sync; sessions saved before this
    // bank pick up the defaults (B used to mirror A anyway). REC/PLAY/SAVE are
    // now mapped through the unified MIDI-Learn engine (right-click the buttons),
    // so the old bespoke REC/PLAY/SAVE bind params were removed for both engines.
    {
        // Per-engine enable (P6) — engine B owns its own rack LED / on-off, so a
        // Sampler in one chain toggles independently from a Sampler in another.
        // (Engine A keeps the legacy "luxSamplerEnabled" above.)
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{"luxSamplerBEnabled", 1}, "LuxSampler B Enabled",
            false));

        juce::StringArray bMidiChNames;
        for (int i = 1; i <= 16; ++i)
            bMidiChNames.add("Channel " + juce::String(i));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerBMidiChannel", 1},
            "LuxSampler B MIDI Channel", bMidiChNames, 1, kHiddenChoice));

        juce::StringArray bOctaveNames { "-2", "-1", " 0", "+1", "+2" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerBOctaveOffset", 1},
            "LuxSampler B Octave Offset", bOctaveNames, 2, kHiddenChoice));

        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"luxSamplerBMaxDuration", 1},
            "LuxSampler B Max Duration",
            juce::NormalisableRange<float>(1.0f, 60.0f, 0.1f), 10.0f, kHiddenFloat));

        // REC / PLAY transport-button mode (engine B) — mirrors engine A.
        juce::StringArray bModeNames { "Toggle", "Momentary" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerBRecMode", 1},
            "LuxSampler B REC Mode", bModeNames, 0, kHiddenChoice));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerBPlayMode", 1},
            "LuxSampler B PLAY Mode", bModeNames, 0, kHiddenChoice));

        // Number of banks (engine B) — mirrors luxSamplerNumBanks.
        juce::StringArray bBankCounts { "1", "2", "3", "4", "5", "6" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerBNumBanks", 1}, "LuxSampler B Banks",
            bBankCounts, 3, kHiddenChoice));  // default = 4 banks

        // P6 — engines 2..7: same six play params, generated ids
        // ("luxSampler{N}_*", fsEngineParam). MIDI channel defaults to N+1.
        for (int e = 2; e < LuxSampler::kMaxEngines; ++e)
        {
            const juce::String nm = "LuxSampler " + juce::String(e + 1) + " ";
            // Per-engine enable (P6) — each engine's rack LED is independent.
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                juce::ParameterID{fsEngineParam(e, "Enabled"), 1},
                nm + "Enabled", false));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                juce::ParameterID{fsEngineParam(e, "MidiChannel"), 1},
                nm + "MIDI Channel", bMidiChNames,
                juce::jmin(e, 15), kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                juce::ParameterID{fsEngineParam(e, "OctaveOffset"), 1},
                nm + "Octave Offset", bOctaveNames, 2, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{fsEngineParam(e, "MaxDuration"), 1},
                nm + "Max Duration",
                juce::NormalisableRange<float>(1.0f, 60.0f, 0.1f), 10.0f,
                kHiddenFloat));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                juce::ParameterID{fsEngineParam(e, "RecMode"), 1},
                nm + "REC Mode", bModeNames, 0, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                juce::ParameterID{fsEngineParam(e, "PlayMode"), 1},
                nm + "PLAY Mode", bModeNames, 0, kHiddenChoice));
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                juce::ParameterID{fsEngineParam(e, "NumBanks"), 1},
                nm + "Banks", bBankCounts, 3, kHiddenChoice));
        }
    }

    // Image export on Save Session: bool toggle + format choice (PNG / JPEG)
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"luxSamplerExportImages", 1},
        "LuxSampler Export Images On Save",
        false, kHiddenBool));

    {
        juce::StringArray formats { "PNG", "JPEG" };
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{"luxSamplerExportFormat", 1},
            "LuxSampler Export Format",
            formats, 0, kHiddenChoice));
    }

    // ── FrameSequencer parameters — one bank PER sampler engine ──────────────
    // The sequencer is internal to its sampler (no more global SEQUENCER
    // module): each engine owns its own timing + transport bank, addressed
    // like every other per-engine sampler param (fsEngineParam ids). The
    // retired global ids ("seqBpm"…) are migrated from old state blobs via
    // the legacy SEQ pattern tree in setStateInformation.
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
    {
        const juce::String nm = (e == 0) ? juce::String("LuxSampler ")
                              : (e == 1) ? juce::String("LuxSampler B ")
                              : "LuxSampler " + juce::String(e + 1) + " ";

        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{fsEngineParam(e, "SeqBpm"), 1}, nm + "Seq BPM",
            juce::NormalisableRange<float>(40.0f, 240.0f, 0.5f), 120.0f));
        // 2..16: the sequencer grid displays at most 16 cells (8×2) and a single
        // step is not a sequence — the whole span is MIDI/automation-addressable.
        // Default 8 = one full row (the grid only shows the active steps).
        params.push_back(std::make_unique<juce::AudioParameterInt>(
            juce::ParameterID{fsEngineParam(e, "SeqNumSteps"), 1},
            nm + "Seq Steps", 2, 16, 8));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{fsEngineParam(e, "SeqLoop"), 1},
            nm + "Seq Loop", true));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{fsEngineParam(e, "SeqDawSync"), 1},
            nm + "Seq DAW Sync", true));
        params.push_back(std::make_unique<juce::AudioParameterInt>(
            juce::ParameterID{fsEngineParam(e, "SeqBeatsPerStep"), 1},
            nm + "Seq Beats/Step", 1, 8, 1));
        // Transport as an automatable param (0=Stop, 1=Play, 2=Hold) so the DAW
        // can drive / MIDI-map the PLAY-HOLD-STOP buttons. parameterChanged()
        // maps it to FrameSequencer::uiStop/uiPlay(uiResume)/uiHold (all RT-safe
        // atomics). Forced back to Stop on session restore — never auto-run.
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{fsEngineParam(e, "SeqTransport"), 1},
            nm + "Seq Transport", juce::StringArray{"Stop", "Play", "Hold"}, 0));
    }

    // ── SCORE playback transport (LuxSampler score slot) ─────────────────────
    // The SCORE generator itself stays out of the APVTS (offline settings), but
    // its playback transport is a live performance control: expose it so the
    // DAW can automate / MIDI-map it. parameterChanged() relays to LuxSampler.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"scorePlaying", 1}, "Score Play", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"scoreLoop", 1}, "Score Loop", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"scoreReverse", 1}, "Score Reverse", false));
    {
        // Same feel as the SCORE page knob: 0.1×–6× with 1× at the centre.
        juce::NormalisableRange<float> spd(0.1f, 6.0f, 0.01f);
        spd.setSkewForCentre(1.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"scoreSpeed", 1}, "Score Speed", spd, 1.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("x")));
    }

    // Per-module transport for the other SCORE-family generators — same range /
    // defaults as SCORE above, but independent so VOICE's speed no longer drags
    // MIDI SCORE's (and vice-versa). Routed to each type's own player slot.
    {
        auto addScoreXport = [&params](const char* playId, const char* speedId,
                                       const char* loopId, const char* revId,
                                       const char* pretty)
        {
            // Play is a command transport — forced back to Stop on restore
            // (never auto-run), like scorePlaying.
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                juce::ParameterID{playId, 1}, juce::String(pretty) + " Play", false));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                juce::ParameterID{loopId, 1}, juce::String(pretty) + " Loop", true));
            params.push_back(std::make_unique<juce::AudioParameterBool>(
                juce::ParameterID{revId, 1}, juce::String(pretty) + " Reverse", false));
            juce::NormalisableRange<float> s(0.1f, 6.0f, 0.01f);
            s.setSkewForCentre(1.0f);
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{speedId, 1}, juce::String(pretty) + " Speed", s, 1.0f,
                juce::AudioParameterFloatAttributes{}.withLabel("x")));
        };
        addScoreXport("voicePlaying",     "voiceSpeed",     "voiceLoop",     "voiceReverse",     "Voice");
        addScoreXport("midiScorePlaying", "midiScoreSpeed", "midiScoreLoop", "midiScoreReverse", "MIDI Score");
        addScoreXport("timbrePlaying",    "timbreSpeed",    "timbreLoop",    "timbreReverse",    "Timbre");
    }

    // Per-slot ACTIVE (module enable) for the shared score-player pool. The
    // rack LED toggles this — DECOUPLED from the play transport above: PLAY
    // auto-activates the module, stopping PLAY leaves it active, and a
    // deactivate/re-activate pauses/resumes the reading. Default ON and, unlike
    // the play commands, PERSISTED (never forced off on restore).
    for (int s = 0; s < ScorePlayerService::kMaxSlots; ++s)
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{scoreActiveParam(s), 1},
            "Score Active " + juce::String(s), true));

    // ── M9: IMAGE / VIDEO / CAMERA source modules ────────────────────────────
    // The line position IS the musical control of these sources — automatable,
    // like the SCORE transport above. Loop modes mirror the sampler's:
    // Once / Loop / Reverse / Ping-Pong.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imgSrcPos", 1}, "Image Src Line",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.5f));
    {
        // Full-image scan time (one traversal top→bottom), log feel around 5 s.
        juce::NormalisableRange<float> dur(0.1f, 120.0f, 0.01f);
        dur.setSkewForCentre(5.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"imgSrcDuration", 1}, "Image Src Scan Time", dur, 5.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("s")));
    }
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"imgSrcLoop", 1}, "Image Src Loop",
        juce::StringArray{"Once", "Loop", "Reverse", "Ping-Pong"}, 1));
    // Scan bounds — the transport reads only [start, end] of the image
    // (crossed values are normalised by the engine); the manual LINE cursor
    // stays free. Defaults = full image.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imgSrcScanStart", 1}, "Image Src Scan Start",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"imgSrcScanEnd", 1}, "Image Src Scan End",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"imgSrcPlay", 1}, "Image Src Play", false));
    // ACTIVE: off = the source feeds NOTHING (its chain streams blank paper);
    // media/params are kept, on resumes instantly. Automatable/MIDI-learnable.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"imgSrcEnabled", 1}, "Image Src Active", true));
    // Orientation (quarter turns CW) — lets the scan read the image in any
    // direction; strips + preview are rebuilt by the engine.
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"imgSrcRotate", 1}, "Image Src Rotate",
        juce::StringArray{"0", "90", "180", "270"}, 0));

    // P5-M3 — IMAGE instance banks, slots 1..7 (slot 0 = the legacy ids
    // above, so old sessions/automation load unchanged).
    for (int s = 1; s < 8; ++s)
    {
        const juce::String nm = "Image Src " + juce::String(s + 1) + " ";
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{imgSrcParam(s, "Pos"), 1}, nm + "Line",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.5f));
        juce::NormalisableRange<float> dur(0.1f, 120.0f, 0.01f);
        dur.setSkewForCentre(5.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{imgSrcParam(s, "Duration"), 1}, nm + "Scan Time",
            dur, 5.0f, juce::AudioParameterFloatAttributes{}.withLabel("s")));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{imgSrcParam(s, "Loop"), 1}, nm + "Loop",
            juce::StringArray{"Once", "Loop", "Reverse", "Ping-Pong"}, 1));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{imgSrcParam(s, "ScanStart"), 1}, nm + "Scan Start",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{imgSrcParam(s, "ScanEnd"), 1}, nm + "Scan End",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{imgSrcParam(s, "Play"), 1}, nm + "Play", false));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{imgSrcParam(s, "Enabled"), 1}, nm + "Active", true));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{imgSrcParam(s, "Rotate"), 1}, nm + "Rotate",
            juce::StringArray{"0", "90", "180", "270"}, 0));
    }

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"vidSrcLine", 1}, "Video Src Line",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.5f));
    {
        juce::NormalisableRange<float> spd(0.1f, 4.0f, 0.01f);
        spd.setSkewForCentre(1.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"vidSrcSpeed", 1}, "Video Src Speed", spd, 1.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("x")));
    }
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"vidSrcLoop", 1}, "Video Src Loop",
        juce::StringArray{"Once", "Loop", "Reverse", "Ping-Pong"}, 1));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"vidSrcPlay", 1}, "Video Src Play", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"vidSrcEnabled", 1}, "Video Src Active", true));

    // P5-M3 — VIDEO instance banks, slots 1..7 (slot 0 = legacy ids above).
    for (int s = 1; s < 8; ++s)
    {
        const juce::String nm = "Video Src " + juce::String(s + 1) + " ";
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{vidSrcParam(s, "Line"), 1}, nm + "Line",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.5f));
        juce::NormalisableRange<float> spd(0.1f, 4.0f, 0.01f);
        spd.setSkewForCentre(1.0f);
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{vidSrcParam(s, "Speed"), 1}, nm + "Speed",
            spd, 1.0f, juce::AudioParameterFloatAttributes{}.withLabel("x")));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{vidSrcParam(s, "Loop"), 1}, nm + "Loop",
            juce::StringArray{"Once", "Loop", "Reverse", "Ping-Pong"}, 1));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{vidSrcParam(s, "Play"), 1}, nm + "Play", false));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{vidSrcParam(s, "Enabled"), 1}, nm + "Active", true));
    }

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"camSrcLine", 1}, "Camera Src Line",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"camSrcEnabled", 1}, "Camera Src Active", true));

    // P5-M3 — CAMERA instance banks, slots 1..7 (slot 0 = legacy ids above).
    for (int s = 1; s < 8; ++s)
    {
        const juce::String nm = "Camera Src " + juce::String(s + 1) + " ";
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{camSrcParam(s, "Line"), 1}, nm + "Line",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.0001f), 0.5f));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{camSrcParam(s, "Enabled"), 1}, nm + "Active", true));
    }

    // ── Video Scroll — live controls (hidden from DAW automation) ─────────────
    // (Purge 2026-07-12: the dead global singletons videoScrollEnabled/
    // Direction/Exposure/BlendMode/Bpm/MidiSync are gone — the per-instance
    // videoScroll{N}_* banks carry the module params.)

    // Transport: when true the waterfall is frozen in place (Play/Pause); the
    // renderer drains its capture ring but performs no scroll/stamp.  "Stop"
    // sets this true AND clears the image via requestVideoScrollClear().
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoScrollPaused", 1}, "Video Scroll Paused",
        true, kHiddenBool));   // start paused — the user presses Play to run

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollMode", 1}, "Video Scroll Mode",
        juce::StringArray{
            "0 deg", "90 deg", "180 deg", "270 deg"
        }, 0, kHiddenChoice));

    // Bipolar scroll speed (legacy birth-line model): negative = reverse,
    // 0 = frozen, positive = forward.  Magnitude maps exponentially in the
    // renderer (px = sign(s) * (2^(3*|s|) - 1)), giving ±7 px/frame at the ends.
    // This single control replaces the old Speed slider + Direction dropdown.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollSpeed", 1}, "Video Scroll Speed",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f),
        0.33f, kHiddenFloat));

    // ── Video Scroll — display configuration (Settings window) ────────────────
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollZoom", 1}, "Video Scroll Zoom",
        juce::NormalisableRange<float>(0.5f, 4.0f, 0.05f),
        1.0f, kHiddenFloat.withLabel("x")));

    // Birth-line position: -1 = far edge (top), 0 = centre (symmetric
    // bidirectional scroll), +1 = near edge (bottom → classic upward waterfall).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollLinePos", 1}, "Video Line Position",
        juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f),
        1.0f, kHiddenFloat));

    // Thickness of each freshly-drawn scanline: 0 = single pixel,
    // 1 = full viewport ("barcode" mode).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollLineThickness", 1}, "Video Line Thickness",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f),
        0.0f, kHiddenFloat));

    // Progressive aging with distance from the birth line: the farther a
    // scanline has scrolled, the more it desaturates and dims.  Applied at
    // display time in paint() (NOT baked into the history buffer), so it is
    // instant and never affects the scroll.  0 = none, 1 = strong.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollFade", 1}, "Video Fade",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f),
        0.0f, kHiddenFloat));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoInvertColor", 1}, "Video Invert Color",
        false, kHiddenBool));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"videoColorMode", 1}, "Video Color Mode (RGB)",
        false, kHiddenBool));

    // ── Video Scroll — window geometry (saved across sessions) ────────────────
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"videoWindowWidth", 1}, "Video Window Width",
        320, 2560, 800, kHiddenInt));

    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{"videoWindowHeight", 1}, "Video Window Height",
        240, 1440, 600, kHiddenInt));

    // ── MIDI follow — when a mapped MIDI controller moves a parameter (played
    // notes excluded), jump the editor to that module's page. Toggled in the
    // gear-wheel System Settings. Non-automatable (a UI preference). ──────────
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{"midiFollowParam", 1}, "MIDI Follow Selection",
        true, kHiddenBool));

    // ── Video: live performance params ────────────────────────────────────────
    // Source = which synth engine's input image we visualize — the engine
    // taps published by the chain executors (AUDIO_IMAGE_ENGINE_TAP_*), so
    // the waterfall always matches what the audio engine actually sees.
    //   0 = LuxStral          (LuxStral engine tap)
    //   1 = LuxSynth/LuxWave  (Path-B tap — LuxWave shares it)
    //   2 = AllSynth          (50/50 blend of the two streams above)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"videoScrollSource", 1}, "Video Scroll Source",
        juce::StringArray{"LuxStral", "LuxSynth/LuxWave", "AllSynth"}, 0));

    // Non-linear temporal compression: applied at display time in paint() as a
    // progressive vertical squish with distance from the birth line — content
    // is full-scale at the source and packed ever tighter as it ages (the rows
    // it swallows are averaged, giving a soft blur).  1 = linear (no squish),
    // higher = more history squeezed on screen.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"videoScrollMaxDuration", 1}, "Video Compression (time squish)",
        juce::NormalisableRange<float>(1.0f, 64.0f, 1.0f), 1.0f, kHiddenFloat));

    // ── SP3CTRA source — acquisition speed (frame-advance brake) ──────────────
    // Brakes how often the live CIS line advances the active frame (audio +
    // visual).  See AcquisitionGate.h.  Off = full-rate (no behaviour change).
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"acqGateMode", 1}, "Acquisition Gate",
        juce::StringArray{"Off", "Internal (LFO)", "DAW Sync"}, 0));
    // Internal-mode period in ms (skewed for finer control at short periods).
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"acqGateRateMs", 1}, "Acquisition Rate",
        juce::NormalisableRange<float>(1.0f, 5000.0f, 0.1f, 0.3f), 100.0f,
        juce::AudioParameterFloatAttributes{}.withLabel("ms")));
    // DAW-sync musical division (period per advance).  Index → beats in the
    // kSyncDivBeats table consumed in processBlock.
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"acqGateSyncDiv", 1}, "Acquisition Division",
        juce::StringArray{"1/1", "1/2", "1/4", "1/8", "1/16", "1/32"}, 2));
    // Common refresh-rate multiplier/divider (stretches the period for very slow
    // updates).  Index → factor in the kRefreshFactor table consumed in processBlock.
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"acqGateMultDiv", 1}, "Acquisition Mult/Div",
        juce::StringArray{"/32", "/16", "/8", "/4", "/2", "x1", "x2", "x4"}, 5));

    // ── VIDEO SCROLL output modules — per-instance automatable banks (×8) ──────
    // Each VideoScroll module instance owns a slot 0..7 (ModuleInstance.slot) that
    // keys both its RT capture ring (video_scroll_instance) and this APVTS bank.
    // Same ranges/defaults as the legacy global controls; the contextual zone-3
    // panel binds videoScroll{N}_* and the right-band mixer binds videoMix{N}_*.
    for (int n = 0; n < 8; ++n)
    {
        const juce::String p  = "videoScroll" + juce::String(n) + "_";
        const juce::String mx = "videoMix"    + juce::String(n) + "_";
        const juce::String tag = "VS" + juce::String(n) + " ";

        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{p + "mode", 1}, tag + "Mode",
            juce::StringArray{"0 deg", "90 deg", "180 deg", "270 deg"}, 0));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "speed", 1}, tag + "Speed",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.33f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "linePos", 1}, tag + "Line Pos",
            juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "thickness", 1}, tag + "Thickness",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "zoom", 1}, tag + "Zoom",
            juce::NormalisableRange<float>(0.5f, 4.0f, 0.05f), 1.0f,
            juce::AudioParameterFloatAttributes{}.withLabel("x")));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "fade", 1}, tag + "Fade",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "compress", 1}, tag + "Compression",
            juce::NormalisableRange<float>(1.0f, 64.0f, 1.0f), 1.0f));
        // Legacy per-instance invert toggle (RGB negative). KEPT so old sessions
        // still restore it; migrated to the "invertMode" choice below in
        // setStateInformation. Not shown in the UI any more.
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{p + "invert", 1}, tag + "Invert Color", false));
        // 3-way inversion selector (supersedes "invert"):
        //   Off       — no inversion
        //   Negative  — 255 - RGB per channel (the old invert)
        //   Luminance — invert HSL lightness only; hue + saturation preserved
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{p + "invertMode", 1}, tag + "Invert",
            juce::StringArray{"Off", "Negative", "Luminance"}, 0));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{p + "colorMode", 1}, tag + "Color (RGB)", true));
        // Background/frame colour: painted where the zoomed/rotated image no
        // longer covers the viewport (the negative-zoom border). Default white =
        // the previous hard-coded behaviour. Set per instance in the SETUP face.
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "bgR", 1}, tag + "BG R",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "bgG", 1}, tag + "BG G",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p + "bgB", 1}, tag + "BG B",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{p + "paused", 1}, tag + "Paused", false));
        // Per-instance output enable (the rack block's LED toggles this). Default
        // ON so a freshly placed VideoScroll produces output immediately; the mixer
        // skips this output while off.
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{p + "enabled", 1}, tag + "Enabled", true));

        // Right-band mixer voice for this output.
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{mx + "level", 1}, "Mix " + tag + "Level",
            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{mx + "blend", 1}, "Mix " + tag + "Blend",
            juce::StringArray{"Mix", "Add", "Screen"}, 0));
    }

    return { params.begin(), params.end() };
}

//==============================================================================
Sp3ctraAudioProcessor::Sp3ctraAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
       apvts(*this, nullptr, "Parameters", createParameterLayout())
#else
     : apvts(*this, nullptr, "Parameters", createParameterLayout())
#endif
{
    log_info("VST", "=============================================================");
    log_info("VST", "Sp3ctraAudioProcessor: Constructor - Initializing VST plugin");
    log_info("VST", "=============================================================");

    // VOICE module TTS diagnostic — no-op unless SP3CTRA_TTS_SMOKE is set.
    PiperTts::runSmokeTestIfRequested();

    // VIDEO MIX + master-audio recorder (idle until the REC button starts it).
    videoRecorder_ = std::make_unique<VideoRecorder>();


    // Cache parameter pointers for fast access
    udpPortParam = apvts.getRawParameterValue(PARAM_UDP_PORT);
    udpByte1Param = apvts.getRawParameterValue(PARAM_UDP_BYTE1);
    udpByte2Param = apvts.getRawParameterValue(PARAM_UDP_BYTE2);
    udpByte3Param = apvts.getRawParameterValue(PARAM_UDP_BYTE3);
    udpByte4Param = apvts.getRawParameterValue(PARAM_UDP_BYTE4);
    sensorDpiParam = apvts.getRawParameterValue(PARAM_SENSOR_DPI);
    logLevelParam = apvts.getRawParameterValue(PARAM_LOG_LEVEL);
    deviceEnabledParam  = apvts.getRawParameterValue(PARAM_DEVICE_ENABLED);
    visualizerModeParam = apvts.getRawParameterValue(PARAM_VISUALIZER_MODE);
    masterVolumeParam   = apvts.getRawParameterValue("masterVolume");

    // Route the MIDI-mapping engine's NON-APVTS targets (sampler play params /
    // action buttons) back to this processor. Set before any state restore so
    // restored virtual mappings resolve.
    midiMap_.setVirtualSink(this);
    // Warm up the sampler-target skewed ranges here (message thread) so their
    // first use never triggers a lazy static init on the audio thread.
    (void) SamplerMidiTargets::speedRange();
    (void) SamplerMidiTargets::powerRange();
    // Seed the MIDI EQ-band pending latches to the "no pending" sentinel (-1);
    // atomic<float> default-inits to 0.0f which is a valid gain value.
    for (auto& engine : smpEqPending)
        for (auto& slot : engine)
            for (auto& band : slot)
                band.store(-1.0f, std::memory_order_relaxed);

    // Cache raw-parameter pointers read by processBlock (audio thread) —
    // getRawParameterValue("literal") allocates a juce::String per call.
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)   // sampler engines ×8
        luxSamplerMidiChannelParam[e] = apvts.getRawParameterValue(fsEngineParam(e, "MidiChannel"));
    for (int s = 0; s < ChainModel::kMaxChains; ++s)
    {
        luxpitchMidiChannelParam [s] = apvts.getRawParameterValue(lpParam(s, "MidiChannel"));
        luxpitchOctaveOffsetParam[s] = apvts.getRawParameterValue(lpParam(s, "OctaveOffset"));
        luxmaskMidiChannelParam  [s] = apvts.getRawParameterValue(lmParam(s, "MidiChannel"));
        luxmaskOctaveOffsetParam [s] = apvts.getRawParameterValue(lmParam(s, "OctaveOffset"));
    }
    luxsynthEnabledParam        = apvts.getRawParameterValue("luxsynthEnabled");
    luxsynthMidiChannelParam    = apvts.getRawParameterValue("luxsynthMidiChannel");
    luxsynthOctaveOffsetParam   = apvts.getRawParameterValue("luxsynthOctaveOffset");
    luxsynthVolumeParam         = apvts.getRawParameterValue("luxsynthVolume");
    luxgrainEnabledParam        = apvts.getRawParameterValue("luxgrainEnabled");
    luxgrainVolumeParam         = apvts.getRawParameterValue("luxgrainVolume");
    luxgrainDensityParam        = apvts.getRawParameterValue("luxgrainDensity");
    luxgrainDensityShapeParam   = apvts.getRawParameterValue("luxgrainDensityShape");
    luxgrainSpreadParam         = apvts.getRawParameterValue("luxgrainSpread");
    luxgrainSizeMinParam        = apvts.getRawParameterValue("luxgrainSizeMin");
    luxgrainSizeMaxParam        = apvts.getRawParameterValue("luxgrainSizeMax");
    luxgrainTextureParam        = apvts.getRawParameterValue("luxgrainTexture");
    luxgrainJitterParam         = apvts.getRawParameterValue("luxgrainJitter");
    luxgrainWidthParam          = apvts.getRawParameterValue("luxgrainWidth");
    luxgrainAmpFollowParam      = apvts.getRawParameterValue("luxgrainAmpFollow");
    luxgrainEnvShapeParam       = apvts.getRawParameterValue("luxgrainEnvShape");
    luxgrainColorPanParam       = apvts.getRawParameterValue("luxgrainColorPan");
    luxgrainEdgeParam           = apvts.getRawParameterValue("luxgrainEdge");
    luxgrainBandsParam          = apvts.getRawParameterValue("luxgrainBands");
    luxgrainMaterialParam       = apvts.getRawParameterValue("luxgrainMaterial");
    luxgrainScrubParam          = apvts.getRawParameterValue("luxgrainScrub");
    luxwaveEnabledParam         = apvts.getRawParameterValue("luxwaveEnabled");
    luxwaveMidiChannelParam     = apvts.getRawParameterValue("luxwaveMidiChannel");
    luxwaveOctaveOffsetParam    = apvts.getRawParameterValue("luxwaveOctaveOffset");
    luxwaveVolumeParam          = apvts.getRawParameterValue("luxwaveVolume");
    luxwaveAttackMsParam        = apvts.getRawParameterValue("luxwaveAttackMs");
    luxwaveDecayMsParam         = apvts.getRawParameterValue("luxwaveDecayMs");
    luxwaveSustainLevelParam    = apvts.getRawParameterValue("luxwaveSustainLevel");
    luxwaveReleaseMsParam       = apvts.getRawParameterValue("luxwaveReleaseMs");
    luxwaveAttackCurveParam     = apvts.getRawParameterValue("luxwaveAttackCurve");
    luxwaveDecayCurveParam      = apvts.getRawParameterValue("luxwaveDecayCurve");
    luxwaveReleaseCurveParam    = apvts.getRawParameterValue("luxwaveReleaseCurve");
    luxwaveFilterCutoffParam    = apvts.getRawParameterValue("luxwaveFilterCutoff");
    luxwaveFilterEnvDepthParam  = apvts.getRawParameterValue("luxwaveFilterEnvDepth");
    luxwaveLfoRateParam         = apvts.getRawParameterValue("luxwaveLfoRate");
    luxwaveLfoDepthParam        = apvts.getRawParameterValue("luxwaveLfoDepth");
    luxwaveScanModeParam        = apvts.getRawParameterValue("luxwaveScanMode");
    luxwaveAmplitudeParam       = apvts.getRawParameterValue("luxwaveAmplitude");
    acqGateModeParam            = apvts.getRawParameterValue("acqGateMode");
    acqGateRateMsParam          = apvts.getRawParameterValue("acqGateRateMs");
    acqGateSyncDivParam         = apvts.getRawParameterValue("acqGateSyncDiv");
    acqGateMultDivParam         = apvts.getRawParameterValue("acqGateMultDiv");
    luxstralVolumeParam         = apvts.getRawParameterValue("luxstralVolume");

    // CC1 mod-wheel targets driven from processBlock (setValueNotifyingHost)
    for (int s = 0; s < ChainModel::kMaxChains; ++s)
    {
        luxpitchLfoDepthParam  [s] = apvts.getParameter(lpParam(s, "LfoDepth"));
        luxmaskLfoPosDepthParam[s] = apvts.getParameter(lmParam(s, "LfoPosDepth"));
    }

    // Register as listener for parameter changes
    apvts.addParameterListener(PARAM_UDP_PORT, this);
    apvts.addParameterListener(PARAM_UDP_BYTE1, this);
    apvts.addParameterListener(PARAM_UDP_BYTE2, this);
    apvts.addParameterListener(PARAM_UDP_BYTE3, this);
    apvts.addParameterListener(PARAM_UDP_BYTE4, this);
    apvts.addParameterListener(PARAM_SENSOR_DPI, this);
    apvts.addParameterListener(PARAM_LOG_LEVEL, this);
    
    // Register LuxStral parameter listeners
    apvts.addParameterListener("luxstralTuning", this);
    apvts.addParameterListener("luxstralRootNote", this);
    apvts.addParameterListener("luxstralNumOctaves", this);
    apvts.addParameterListener("luxstralAttackMs", this);
    apvts.addParameterListener("luxstralReleaseMs", this);
    apvts.addParameterListener("luxstralStereoEnable", this);
    apvts.addParameterListener("luxstralStereoTempAmp", this);
    apvts.addParameterListener("luxstralNoiseGateThreshold", this);
    apvts.addParameterListener("luxstralNumWorkers", this);
    apvts.addParameterListener("luxstralPhysiologicalFilter", this);
    apvts.addParameterListener("luxstralPhysiologicalDepth", this);
    apvts.addParameterListener("luxstralSoftLimitThreshold", this);
    apvts.addParameterListener("luxstralSoftLimitKnee", this);
    // M4 — core-side LuxSynth engine feed (lx_fft_* config mirror)
    apvts.addParameterListener("lxFftBins", this);
    apvts.addParameterListener("lxFftSmoothing", this);
    // Phase management (mode + gate + position + drift)
    apvts.addParameterListener("luxstralPhaseMode", this);
    apvts.addParameterListener("luxstralPhaseSensitivity", this);
    apvts.addParameterListener("luxstralPhasePosition", this);
    apvts.addParameterListener("luxstralPhaseDriftCents", this);
    apvts.addParameterListener("luxstralTimbreMix", this);
    apvts.addParameterListener("luxstralTimbrePos", this);
    apvts.addParameterListener("luxstralTimbreFormant", this);
    apvts.addParameterListener("luxstralTimbreEnable", this);
    // (ScanPlay/ScanRate need no listener: the 30 ms timer polls them.)
    
    // Register StrokeForge parameter listeners
    apvts.addParameterListener("sfEnabled", this);
    apvts.addParameterListener("sfBlobContrastAdaptive", this);
    apvts.addParameterListener("sfBlobContrastSensitivity", this);
    apvts.addParameterListener("sfMaxHarmonics", this);
    apvts.addParameterListener("sfHarmonicAmpFloor", this);
    apvts.addParameterListener("sfVolumeCenterSigma", this);
    apvts.addParameterListener("sfPhaseCoherence", this);
    apvts.addParameterListener("sfPhaseSmoothAlpha", this);
    apvts.addParameterListener("sfMorphWidthScale", this);
    apvts.addParameterListener("sfWavetableMinWidth", this);
    apvts.addParameterListener("sfBlobFocusSigma", this);
    apvts.addParameterListener("sfSpectralWidthThreshold", this);
    apvts.addParameterListener("sfFocusOnly", this);

    // LuxSynth blob detection (independent of StrokeForge)
    apvts.addParameterListener("lxBlobThreshold",  this);
    apvts.addParameterListener("lxBlobMinWidth",   this);
    apvts.addParameterListener("lxBlobMergeGap",   this);
    apvts.addParameterListener("lxBlobColorSplit", this);

    // SPCTR blob detection — IMAGE LUXSTRAL tab (drives both visualizer + StrokeForge audio)
    apvts.addParameterListener("spctrBlobThreshold",  this);
    apvts.addParameterListener("spctrBlobMinWidth",   this);
    apvts.addParameterListener("spctrBlobMergeGap",   this);
    apvts.addParameterListener("spctrBlobColorSplit", this);

    // Image pipeline parameters (live transport + opacity + fade-in)
    apvts.addParameterListener("imageFreezeMode",      this);
    apvts.addParameterListener("imageLiveOpacity",     this);
    apvts.addParameterListener("imageSamplerOpacity",  this);
    apvts.addParameterListener("imageMixBalance",      this);
    apvts.addParameterListener("imageFadeInMs",        this);
    // Sampler-specific preprocessing parameters
    apvts.addParameterListener("samplerGamma",         this);
    apvts.addParameterListener("samplerContrastMin",   this);
    apvts.addParameterListener("samplerFreezeMode",    this);
    apvts.addParameterListener("rawFreezeMode",        this);

    // Pitch/Mask/Reverb/Echo/EQ per-instance banks + per-OUT conditioning
    // banks (J1: iterate the SINGLE manifest): every bank param of every slot
    // funnels into the same applyConfigurationToCore() sync. RULE learned in
    // P2b: creating APVTS params is NOT enough — without addParameterListener
    // a knob move never re-syncs g_sp3ctra_config ("no effect").
    // "luxstralOut…" rides the startsWith("luxstral") branch of
    // applyParameterChange; the others land on the generic fallback — all end
    // in applyConfigurationToCore(false). VideoScroll and the Sampler keep
    // their own dedicated listening mechanisms (excluded here, iso-behaviour).
    for (const auto& bank : kModuleParamManifest)
    {
        if (bank.type == ModuleType::VideoScroll
            || bank.type == ModuleType::Sampler)
            continue;
        for (int s = 0; s < bank.numSlots; ++s)
            for (int i = 0; i < bank.numSuffixes; ++i)
                apvts.addParameterListener(bank.paramId(s, bank.suffixes[i]),
                                           this);
    }
    // Create the sampler engines: A (slot 0) + B (slot 1). Per-engine enable is
    // driven by module presence in deriveChainRouting(); both share the single
    // modulated playback channel (one plays at a time — see the arbiter).
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
        samplers_[(size_t) e] = std::make_unique<LuxSampler>(e);
    log_info("FS", "LuxSampler engines A-%c ready — %d slots, %.1f s/slot max",
             (char) ('A' + LuxSampler::kMaxEngines - 1),
             LuxSamplerConstants::NUM_SLOTS,
             static_cast<double>(LuxSamplerConstants::MAX_DURATION_S));
    scorePlayerService_ = std::make_unique<ScorePlayerService>();

    // SCORE generation defaults (shared by the PLAY page and the SETUP panel).
    score_settings_defaults(&scoreSettings_);
    scoreSettings_.writingSpeed = 2.5;   // page maps to a sensible default duration

    // One FrameSequencer per sampler engine — the sequencer is internal to its
    // sampler and addresses only that engine's banks.
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
    {
        frameSequencers_[(size_t) e] = std::make_unique<FrameSequencer>();
        frameSequencers_[(size_t) e]->setLuxSampler(samplers_[(size_t) e].get());
    }

    // Working-session manager: needs the samplers to exist (bank save/load).
    // Host-specific init (Standalone vs DAW) is resolved lazily at first
    // get/setStateInformation, when JUCE has stamped wrapperType.
    sessions_ = std::make_unique<SessionManager>(*this);
    // MIDI-learn table edits are part of the session — autosave them too.
    midiMap_.onMappingsEdited = [this] { if (sessions_) sessions_->markStateDirty(); };
    // Catch-all dirty hook on the state tree (see SessionDirtyListener doc).
    sessionDirtyListener_ = std::make_unique<SessionDirtyListener>(*this);
    apvts.state.addListener(sessionDirtyListener_.get());

    // ── M9 / P5-M3: IMAGE ×8 + VIDEO / CAMERA source engines + service ──────
    for (int s = 0; s < 8; ++s)
    {
        imageSources_[(size_t) s] = std::make_unique<ImageSourceEngine>();
        auto* eng = imageSources_[(size_t) s].get();
        eng->setSlot(s);
        // ONCE traversals snap THIS instance's play param back off at the end.
        eng->onPlaybackFinished = [this, s]
        {
            if (auto* p = apvts.getParameter(imgSrcParam(s, "Play")))
                p->setValueNotifyingHost(0.0f);
        };
        // Initial param sync (media/presence arrive later: restore + model).
        eng->setPosition (apvts.getRawParameterValue(imgSrcParam(s, "Pos"))->load());
        eng->setDurationS(apvts.getRawParameterValue(imgSrcParam(s, "Duration"))->load());
        eng->setLoopMode ((int) apvts.getRawParameterValue(imgSrcParam(s, "Loop"))->load());
        eng->setScanStart(apvts.getRawParameterValue(imgSrcParam(s, "ScanStart"))->load());
        eng->setScanEnd  (apvts.getRawParameterValue(imgSrcParam(s, "ScanEnd"))->load());
        eng->setEnabled  (apvts.getRawParameterValue(imgSrcParam(s, "Enabled"))->load() > 0.5f);
        eng->setRotation ((int) apvts.getRawParameterValue(imgSrcParam(s, "Rotate"))->load());
    }
    for (int s = 0; s < 8; ++s)
    {
        videoSources_[(size_t) s] = std::make_unique<VideoSourceEngine>();
        auto* v = videoSources_[(size_t) s].get();
        v->setSlot(s);
        v->onPlaybackFinished = [this, s]
        {
            if (auto* p = apvts.getParameter(vidSrcParam(s, "Play")))
                p->setValueNotifyingHost(0.0f);
        };
        v->setLineFrac (apvts.getRawParameterValue(vidSrcParam(s, "Line"))->load());
        v->setSpeed    (apvts.getRawParameterValue(vidSrcParam(s, "Speed"))->load());
        v->setLoopMode ((int) apvts.getRawParameterValue(vidSrcParam(s, "Loop"))->load());
        v->setEnabled  (apvts.getRawParameterValue(vidSrcParam(s, "Enabled"))->load() > 0.5f);

        cameraSources_[(size_t) s] = std::make_unique<CameraSourceEngine>();
        auto* c = cameraSources_[(size_t) s].get();
        c->setSlot(s);
        c->setLineFrac(apvts.getRawParameterValue(camSrcParam(s, "Line"))->load());
        c->setEnabled (apvts.getRawParameterValue(camSrcParam(s, "Enabled"))->load() > 0.5f);
    }
    mediaService_ = std::make_unique<MediaSourceService>(imageSources_,
                                                         videoSources_,
                                                         cameraSources_);

    // Register LuxSampler parameter listeners
    apvts.addParameterListener(PARAM_FS_ENABLED,    this);
    apvts.addParameterListener(PARAM_FS_MIDI_CH,    this);
    apvts.addParameterListener(PARAM_FS_OCT_OFFSET, this);
    apvts.addParameterListener(PARAM_FS_MAX_DUR,    this);
    // Engine B..H banks (same play params, own values). The per-engine enable
    // (P6) makes each Sampler instance's rack LED independent — engine 0 keeps
    // the legacy PARAM_FS_ENABLED listener registered just above.
    for (int e = 1; e < LuxSampler::kMaxEngines; ++e)
    {
        apvts.addParameterListener(fsEngineParam(e, "Enabled"),      this);
        apvts.addParameterListener(fsEngineParam(e, "MidiChannel"),  this);
        apvts.addParameterListener(fsEngineParam(e, "OctaveOffset"), this);
        apvts.addParameterListener(fsEngineParam(e, "MaxDuration"),  this);
    }

    // Per-engine sequencer banks (the sequencer is internal to its sampler).
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
    {
        apvts.addParameterListener(fsEngineParam(e, "SeqBpm"),          this);
        apvts.addParameterListener(fsEngineParam(e, "SeqNumSteps"),     this);
        apvts.addParameterListener(fsEngineParam(e, "SeqLoop"),         this);
        apvts.addParameterListener(fsEngineParam(e, "SeqDawSync"),      this);
        apvts.addParameterListener(fsEngineParam(e, "SeqBeatsPerStep"), this);
        apvts.addParameterListener(fsEngineParam(e, "SeqTransport"),    this);
    }

    // SCORE playback transport (relayed to LuxSampler in parameterChanged)
    apvts.addParameterListener(PARAM_SCORE_PLAYING, this);
    apvts.addParameterListener(PARAM_SCORE_LOOP,    this);
    apvts.addParameterListener(PARAM_SCORE_REVERSE, this);
    apvts.addParameterListener(PARAM_SCORE_SPEED,   this);
    // Per-module transports for VOICE / MIDI SCORE / TIMBRE (independent slots).
    for (ModuleType t : { ModuleType::Voice, ModuleType::MidiScore, ModuleType::Timbre })
    {
        const auto ids = scoreXportIds(t);
        apvts.addParameterListener(ids.play,    this);
        apvts.addParameterListener(ids.speed,   this);
        apvts.addParameterListener(ids.loop,    this);
        apvts.addParameterListener(ids.reverse, this);
    }

    // M9 — IMAGE / VIDEO / CAMERA source params → engines
    // (P5-M3: the imgSrc* listeners — slot 0 legacy ids included — are
    // registered by the manifest loop above; individual adds would double.)
    // (P5-M3: the vidSrc*/camSrc* listeners — slot 0 legacy ids included —
    // are registered by the manifest loop above.)

    // Sync LuxSampler config with initial APVTS values
    samplers_[0]->setEnabled(*apvts.getRawParameterValue(PARAM_FS_ENABLED) > 0.5f);
    samplers_[0]->setMidiChannel(
        static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_MIDI_CH)) + 1);
    samplers_[0]->setOctaveOffset(
        static_cast<int>(*apvts.getRawParameterValue(PARAM_FS_OCT_OFFSET)) - 2);
    samplers_[0]->setMaxDuration(*apvts.getRawParameterValue(PARAM_FS_MAX_DUR));

    // Sync each engine's sequencer timing with the initial APVTS values.
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
        if (auto* fs = frameSequencers_[(size_t) e].get())
        {
            fs->setBpm(apvts.getRawParameterValue(fsEngineParam(e, "SeqBpm"))->load());
            fs->setNumSteps(static_cast<int>(
                apvts.getRawParameterValue(fsEngineParam(e, "SeqNumSteps"))->load()));
            fs->setLooping(
                *apvts.getRawParameterValue(fsEngineParam(e, "SeqLoop")) > 0.5f);
            fs->setDawSync(
                *apvts.getRawParameterValue(fsEngineParam(e, "SeqDawSync")) > 0.5f);
            fs->setBeatsPerStep(static_cast<int>(
                apvts.getRawParameterValue(fsEngineParam(e, "SeqBeatsPerStep"))->load()));
        }

    // Score transport → each family type's own slot (independent per module).
    primeScoreTransports();

    // Engines 1..7: their own APVTS banks — MIDI channel defaults to e+1
    // so direct MIDI doesn't double-trigger out of the box. Per-engine enable
    // is set authoritatively by deriveChainRouting().
    for (int e = 1; e < LuxSampler::kMaxEngines; ++e)
    if (samplers_[(size_t) e])
    {
        samplers_[(size_t) e]->setMidiChannel(
            static_cast<int>(*apvts.getRawParameterValue(fsEngineParam(e, "MidiChannel"))) + 1);
        samplers_[(size_t) e]->setOctaveOffset(
            static_cast<int>(*apvts.getRawParameterValue(fsEngineParam(e, "OctaveOffset"))) - 2);
        samplers_[(size_t) e]->setMaxDuration(*apvts.getRawParameterValue(fsEngineParam(e, "MaxDuration")));
    }

    // ── Acquire the process-wide shared core ─────────────────────────────────
    // If this is the FIRST plugin instance in this DAW process → creates the
    // singleton (UDP socket, image pipeline, synthesis engine not yet started).
    // If a SECOND instance is being created → returns the existing singleton.
    // The shared_ptr keeps the singleton alive for this instance's lifetime.
    sharedCore = Sp3ctraSharedCore::acquire();
    
    // 🔧 LAZY INITIALIZATION: Do NOT start the shared pipeline here.
    // Host ordering DIFFERS per wrapper — both must work:
    //   DAW:        ctor → setStateInformation() → prepareToPlay()
    //   Standalone: ctor → prepareToPlay() → setStateInformation() → editor
    // We defer startWithConfig() to prepareToPlay() so we have the correct
    // sample rate and buffer size when initializing LuxStral; coreNeedsInit
    // makes setStateInformation hot-reload instead when it runs second.
    
    // Initialize the LuxPitch / LuxMask processing instances.
    // Since the single-snapshot refactor (M2) there is ONE simulation per
    // insert: the synthesis-thread instance.  Visualizers read the published
    // the contextual selection tap instead of
    // re-simulating.
    // M6 Phase 2 — init the whole per-chain instance pool (slot 0 == legacy).
    lux_pitch_init_all();
    lux_mask_init_all();
    lux_reverb_init_all();
    lux_echo_init_all();
    lux_eq_init_all();
    lux_harmo_init_all();
    video_scroll_init_all();   // init 8 VideoScroll capture rings (RT pool) before the synth thread starts

    // Just update g_sp3ctra_config with current APVTS defaults (no socket/buffer creation)
    applyConfigurationToCore(false);

    // M6 Phase 2 — build the default chain topology and derive routing. A saved
    // session reloads it later in setStateInformation().
    loadChainModelFromState();

    // Deferred-dispatch table for parameterChanged() (see dispatcher): one
    // dirty flag per parameter, drained by the 30 ms message-thread timer.
    for (auto* p : getParameters())
        if (auto* pw = dynamic_cast<juce::AudioProcessorParameterWithID*>(p))
            if (paramIndexById_.find(pw->paramID) == paramIndexById_.end())
            {
                paramIndexById_[pw->paramID] = deferredParamIds_.size();
                deferredParamIds_.add(pw->paramID);
            }
    paramDirty_ = std::make_unique<std::atomic<bool>[]>(
        (size_t) juce::jmax(1, deferredParamIds_.size()));
    for (int i = 0; i < deferredParamIds_.size(); ++i)
        paramDirty_[(size_t) i].store(false, std::memory_order_relaxed);
    {
        const auto it = paramIndexById_.find(PARAM_SCORE_PLAYING);
        scorePlayingParamIdx_ = (it != paramIndexById_.end()) ? it->second : -1;
    }
    startTimer(30);

    log_info("VST", "Sp3ctraAudioProcessor: Constructor complete (deferred init)");
    log_info("VST", "  - Shared core acquired (ref-count now %ld)",
             sharedCore.use_count());
    log_info("VST", "  - Pipeline start deferred to prepareToPlay()");
    if (const char* logPath = logger_log_file_path())
        log_info("VST", "  - Session log: %s", logPath);
}

void Sp3ctraAudioProcessor::getMusicalFrequencyRange(double& lowHz, double& highHz) const noexcept
{
    // Mirror the LuxStral range driven by Tuning + Root Note + Octaves, kept in
    // sync in g_sp3ctra_config whenever those musical params change.
    extern sp3ctra_config_t g_sp3ctra_config;
    double lo = (double) g_sp3ctra_config.low_frequency;
    double hi = (double) g_sp3ctra_config.high_frequency;
    // Defensive fallback if the config has not been synced yet.
    if (!(lo > 0.0) || !(hi > lo))
    {
        lo = 65.41;       // C2
        hi = 16744.04;    // ~8 octaves above C2
    }
    lowHz  = lo;
    highHz = hi;
}

void Sp3ctraAudioProcessor::getScoreFrequencyRange(double& lowHz, double& highHz) const noexcept
{
    if (!scoreFreq_.manual)
    {
        getMusicalFrequencyRange(lowHz, highHz);
        return;
    }

    // Manual override — same formula LuxStral uses (root MIDI = 24 + index).
    const int    rootMidi = 24 + scoreFreq_.rootIndex;
    double low  = scoreFreq_.tuning * pow(2.0, (double)(rootMidi - 69) / 12.0);
    double high = low * pow(2.0, (double) scoreFreq_.octaves);
    if (high > 20000.0) high = 20000.0;
    if (!(low > 0.0) || !(high > low)) { low = 65.41; high = 16744.04; }
    lowHz  = low;
    highHz = high;
}

Sp3ctraAudioProcessor::~Sp3ctraAudioProcessor()
{
    log_info("VST", "=============================================================");
    log_info("VST", "Sp3ctraAudioProcessor: Destructor - Shutting down");
    log_info("VST", "=============================================================");

    // Detach the session-dirty hook first: teardown below may still mutate the
    // state tree, and the listener must never fire into a dying SessionManager.
    if (sessionDirtyListener_ != nullptr)
        apvts.state.removeListener(sessionDirtyListener_.get());

    // Flush any pending session autosave while the samplers still exist.
    // (Normal Standalone close also flushes via makeStandaloneRefState.)
    if (sessions_ != nullptr && sessions_->isStandalone()
        && sessions_->hasUnsavedChanges())
    {
        sessions_->saveStateNow();
        sessions_->saveBanksNow();
    }

    // Stop the RT audio tap, then finalise any in-progress recording.
    recActive_.store(false, std::memory_order_release);
    if (videoRecorder_) { videoRecorder_->stop(); videoRecorder_.reset(); }

    // ── M9: media source service FIRST (uses Context/buffers owned by sharedCore) ──
    if (mediaService_)
    {
        mediaService_->stopThread(2000);
        mediaService_.reset();
    }
    for (auto& c : cameraSources_)
        if (c) c->closeDevice();   // release the capture devices
    for (auto& eng : imageSources_) eng.reset();
    for (auto& v : videoSources_)  v.reset();
    for (auto& c : cameraSources_) c.reset();

    // ── LuxSampler (uses AudioImageBuffers / DoubleBuffer owned by sharedCore) ──
    // Must stop before releasing sharedCore to avoid use-after-free.
    for (int e = LuxSampler::kMaxEngines - 1; e >= 1; --e)
        if (samplers_[(size_t) e])
        {
            samplers_[(size_t) e]->stopPlayerThread();
            samplers_[(size_t) e].reset();
        }
    if (samplers_[0])
    {
        log_info("VST", "Stopping LuxSampler player thread...");
        samplers_[0]->stopPlayerThread();
        samplers_[0].reset();
        log_info("VST", "LuxSampler stopped");
    }

    // ── Release the shared core ──────────────────────────────────────────────
    // Decrements the ref-count. If this is the last plugin instance in the DAW
    // process (ref-count → 0), Sp3ctraSharedCore::~Sp3ctraSharedCore() fires and:
    //   1. Stops AudioProcessingThread
    //   2. Calls synth_luxstral_cleanup()
    //   3. Stops UdpReceiverThread
    //   4. Calls sp3ctraCore->shutdown() (frees buffers)
    //
    // If another instance is still alive (ref-count ≥ 1), the singleton keeps
    // running — the other instance continues producing audio normally.
    if (sharedCore)
    {
        log_info("VST", "Releasing shared core (remaining ref-count will be %ld)...",
                 static_cast<long>(sharedCore.use_count()) - 1);
        sharedCore.reset();
    }

    log_info("VST", "=============================================================");
    log_info("VST", "Sp3ctraAudioProcessor: Destructor complete");
    log_info("VST", "=============================================================");
}

//==============================================================================
// VIDEO MIX recording — thin bridge to the platform recorder. Audio is tapped
// in processBlock (gated by recActive_); video frames arrive from the mixer's
// render thread via pushRecordVideoFrame().
bool Sp3ctraAudioProcessor::startVideoRecording(const juce::File& out, int w, int h,
                                                double fps, juce::String& err)
{
    if (videoRecorder_ == nullptr) { err = "Recorder unavailable"; return false; }

    const double sr = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    const int    ch = juce::jmax(1, getTotalNumOutputChannels());
    if (! videoRecorder_->start(out, w, h, fps, sr, ch, err))
    {
        log_info("VST", "Video recording FAILED to start: %s", err.toRawUTF8());
        return false;
    }

    recActive_.store(true, std::memory_order_release);   // arm the RT audio tap
    log_info("VST", "Video recording started: %s (%dx%d @ %.0f fps)",
             out.getFullPathName().toRawUTF8(), w, h, fps);
    return true;
}

void Sp3ctraAudioProcessor::stopVideoRecording()
{
    recActive_.store(false, std::memory_order_release);  // stop the RT tap first
    if (videoRecorder_) videoRecorder_->stop();
    log_info("VST", "Video recording stopped");
}

bool Sp3ctraAudioProcessor::isVideoRecording() const noexcept
{
    return videoRecorder_ != nullptr && videoRecorder_->isRecording();
}

void Sp3ctraAudioProcessor::pushRecordVideoFrame(const juce::Image& composite,
                                                 double tSeconds)
{
    if (recActive_.load(std::memory_order_acquire) && videoRecorder_ != nullptr)
        videoRecorder_->pushVideoFrame(composite, tSeconds);
}

//==============================================================================
const juce::String Sp3ctraAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool Sp3ctraAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool Sp3ctraAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool Sp3ctraAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double Sp3ctraAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int Sp3ctraAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int Sp3ctraAudioProcessor::getCurrentProgram()
{
    return 0;
}

void Sp3ctraAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String Sp3ctraAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void Sp3ctraAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void Sp3ctraAudioProcessor::setVisualizerSuspendedSafely (bool suspend)
{
    auto apply = [] (Sp3ctraAudioProcessor& p, bool s)
    {
        if (auto* editor = dynamic_cast<Sp3ctraAudioProcessorEditor*>(p.getActiveEditor()))
        {
            if (s) editor->suspendVisualizer();
            else   editor->resumeVisualizer();
        }
    };

    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread())
    {
        apply(*this, suspend);
        return;
    }

    // Off the message thread (host-dependent prepareToPlay): the editor may be
    // mid-destruction on the message thread — marshal instead of touching it.
    juce::WeakReference<Sp3ctraAudioProcessor> weakThis(this);
    juce::MessageManager::callAsync([weakThis, suspend, apply]
    {
        if (auto* p = weakThis.get())
            apply(*p, suspend);
    });
}

void Sp3ctraAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // 🛡️ PROTECTION: Suspend visualizer to prevent Metal/CoreGraphics race
    setVisualizerSuspendedSafely(true);

    const double prepareStartMs = juce::Time::getMillisecondCounterHiRes();
    log_info("VST", "=============================================================");
    log_info("VST", "prepareToPlay - SR=%.1f Hz, BS=%d samples", sampleRate, samplesPerBlock);

    // ── Update global audio parameters (needed by startWithConfig) ───────────
    extern sp3ctra_config_t g_sp3ctra_config;
    int oldSampleRate = g_sp3ctra_config.sampling_frequency;
    g_sp3ctra_config.sampling_frequency = static_cast<int>(sampleRate);
    // audio_buffer_size drives how many samples the synthesis engine WRITES into
    // the shared output buffers. Once the pipeline runs, it must only change
    // together with a successful buffer reallocation (ensureAudioBufferSize()
    // below) — updating it unconditionally made the engine overflow buffers
    // still allocated for the previous host block size.
    if (!sharedCore || !sharedCore->isReady())
        g_sp3ctra_config.audio_buffer_size = samplesPerBlock;
    g_sp3ctra_config.semitone_per_octave = 12;
    g_sp3ctra_config.comma_per_semitone  = 36;

    // ── Performance budget warning ───────────────────────────────────────────
    double bufferDurationUs = (samplesPerBlock / sampleRate) * 1000000.0;
    const double SYNTHESIS_TIME_ESTIMATE_US = 2200.0;
    double loadRatio = SYNTHESIS_TIME_ESTIMATE_US / bufferDurationUs;
    if (loadRatio > 1.0)
    {
        log_warning("VST", "⚠️  PERFORMANCE WARNING: SR too high for buffer size!");
        log_warning("VST", "    SR=%.0f Hz, BS=%d, budget=%.0f µs, estimate=%.0f µs",
                    sampleRate, samplesPerBlock, bufferDurationUs, SYNTHESIS_TIME_ESTIMATE_US);
    }
    else
    {
        log_info("VST", "✅ Headroom: %.0f µs budget, ~%.0f µs synthesis (%.1f%% load)",
                 bufferDurationUs, SYNTHESIS_TIME_ESTIMATE_US, loadRatio * 100.0);
    }

    // ── RT Profiler ──────────────────────────────────────────────────────────
    rt_profiler_init(&g_vst_rt_profiler, static_cast<int>(sampleRate), samplesPerBlock);
    rt_profiler_set_enabled(&g_vst_rt_profiler, 1);
    log_info("VST", "RT Profiler active (interval=%d frames)", RT_PROFILER_REPORT_INTERVAL_FRAMES);

    // ── Recalculate Nyquist-clamped frequencies if SR changed ────────────────
    if (oldSampleRate != static_cast<int>(sampleRate))
    {
        log_info("VST", "SR changed %d → %d Hz — recalculating frequencies",
                 oldSampleRate, static_cast<int>(sampleRate));
        applyConfigurationToCore(false);

        // Hot SR change: every engine cached the rate at init — re-derive it.
        // LuxStral renders on its own producer threads, so its wavetable
        // (phase increments) and gap-limiter coefficients are rebuilt through
        // the existing atomic reinit consumed on the synth thread; the other
        // three engines render inline in processBlock, which the host
        // guarantees is not running during prepareToPlay.
        if (sharedCore && sharedCore->isReady())
        {
            request_frequency_reinit();
            luxsynth_engine_set_sample_rate(&g_luxsynth_engine,
                                            static_cast<float>(sampleRate));
            luxwave_engine_set_sample_rate(&g_luxwave_engine,
                                           static_cast<float>(sampleRate));
            luxgrain_engine_set_sample_rate(&g_luxgrain_engine,
                                            static_cast<float>(sampleRate));

            // The score preview buffer was resampled to the OLD host rate at
            // decode time — drop it so it cannot resume off-pitch.
            scorePreviewPlaying_.store(false, std::memory_order_release);
            {
                juce::SpinLock::ScopedLockType sl(scorePreviewLock_);
                scorePreviewBuf_.setSize(0, 0);
                scorePreviewPos_  = 0;
                scorePreviewRate_ = sampleRate;
            }
            scorePreviewPosAtomic_.store(0, std::memory_order_release);
        }
    }

    // ── Reset consumer tracking (prevent stale buffer re-output at startup) ──
    lastConsumedReadIdx = -1;
    lastConsumedReadIdxLuxSynth = -1;

    // ── Start the shared pipeline (idempotent: no-op if already running) ─────
    if (coreNeedsInit)
    {
        log_info("VST", "Starting shared pipeline (first call or new plugin)...");

        // Ensure g_sp3ctra_config is fully populated before startWithConfig()
        applyConfigurationToCore(false);

        Sp3ctraCore::ActiveConfig udpCfg;
        udpCfg.udpPort           = static_cast<int>(udpPortParam->load());
        udpCfg.udpAddress        = getUdpAddressString().toStdString();
        udpCfg.multicastInterface = "";
        udpCfg.logLevel          = static_cast<int>(logLevelParam->load());

        if (!sharedCore->startWithConfig(udpCfg,
                                          g_sp3ctra_config.pixels_per_note,
                                          sampleRate, samplesPerBlock))
        {
            log_error("VST", "prepareToPlay — sharedCore->startWithConfig() FAILED");
            setVisualizerSuspendedSafely(false);
            return;
        }

        lastInitPixelsPerNote = g_sp3ctra_config.pixels_per_note;
        coreNeedsInit = false;

        log_info("VST", "Shared pipeline started — UDP on %s:%d",
                 getUdpAddressString().toRawUTF8(),
                 static_cast<int>(udpPortParam->load()));
    }
    else
    {
        // Second instance, or subsequent prepareToPlay on the same instance.
        // The shared pipeline (UDP + synthesis thread) is already running.
        // This instance simply reads from the same luxstral_buffers_L/R globals.
        log_info("VST", "Shared pipeline already running — connecting as additional consumer");

        // Host buffer size may have changed since the buffers were allocated —
        // resize them (stops/restarts the synthesis thread). When refused
        // (multi-instance), the old size stays authoritative and processBlock
        // clamps its reads to luxstral_get_audio_buffer_size().
        if (!sharedCore->ensureAudioBufferSize(samplesPerBlock))
            log_warning("VST", "prepareToPlay — shared output buffers stay at %d "
                               "samples (host asked %d); reads are clamped",
                        luxstral_get_audio_buffer_size(), samplesPerBlock);
    }

    // ── LuxSampler player threads (per-instance, non-RT) ───────────────────
    if (sharedCore && sharedCore->getCore())
    {
        auto* aib = sharedCore->getCore()->getAudioImageBuffers();
        auto* dbf = sharedCore->getCore()->getDoubleBuffer();
        for (auto& fs : samplers_)
            if (fs) fs->startPlayerThread(aib, dbf);

        // P5-M4 — per-instance score players (one thread, 8 slots).
        if (scorePlayerService_)
        {
            scorePlayerService_->setBuffers(aib, dbf);
            if (! scorePlayerService_->isThreadRunning())
                scorePlayerService_->startThread();
        }

        // M9 — media source service (ticks the IMAGE/VIDEO/CAMERA engines and
        // pumps the chains when the device is not streaming).
        if (mediaService_)
        {
            mediaService_->setContext(sharedCore->getCore()->getContext());
            if (! mediaService_->isThreadRunning())
                mediaService_->startThread();
        }
    }

    log_info("VST", "prepareToPlay complete in %.1f ms",
             juce::Time::getMillisecondCounterHiRes() - prepareStartMs);
    log_info("VST", "=============================================================");

    setVisualizerSuspendedSafely(false);
}

void Sp3ctraAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool Sp3ctraAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}
#endif

//==============================================================================
// SCORE source-audio preview
//==============================================================================
void Sp3ctraAudioProcessor::startScorePreview(const juce::File& wav,
                                              double startSec, double lengthSec)
{
    stopScorePreview();
    if (! wav.existsAsFile()) return;

    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(wav));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return;

    const double      fileRate    = reader->sampleRate;
    const int         srcCh       = (int) juce::jmax((juce::uint32) 1, reader->numChannels);
    const juce::int64 startSample = juce::jlimit((juce::int64) 0, reader->lengthInSamples,
                                                 (juce::int64) (startSec * fileRate));
    juce::int64 numFile = reader->lengthInSamples - startSample;
    if (lengthSec > 0.0)
        numFile = juce::jmin(numFile, (juce::int64) (lengthSec * fileRate));
    if (numFile <= 0) return;

    const int srcLen = (int) numFile;
    juce::AudioBuffer<float> src(juce::jmin(srcCh, 2), srcLen);
    if (! reader->read(&src, 0, srcLen, startSample, true, true))
        return;

    // Resample (linear) to the host rate so the preview plays at correct pitch.
    const double hostRate = (getSampleRate() > 0.0) ? getSampleRate() : fileRate;
    const double ratio    = hostRate / fileRate;
    const int    outLen   = juce::jmax(1, (int) std::floor((double) srcLen * ratio));
    const int    sc       = src.getNumChannels();

    juce::AudioBuffer<float> out(2, outLen);
    for (int i = 0; i < outLen; ++i)
    {
        const double sp = (double) i / ratio;
        const int    i0 = juce::jmin((int) sp, srcLen - 1);
        const int    i1 = juce::jmin(i0 + 1, srcLen - 1);
        const float  fr = (float) (sp - (double) i0);
        for (int c = 0; c < 2; ++c)
        {
            const int   cc = juce::jmin(c, sc - 1);
            const float a  = src.getSample(cc, i0);
            const float b  = src.getSample(cc, i1);
            out.setSample(c, i, a + (b - a) * fr);
        }
    }

    {
        juce::SpinLock::ScopedLockType sl(scorePreviewLock_);
        scorePreviewBuf_  = std::move(out);
        scorePreviewPos_  = 0;
        scorePreviewRate_ = hostRate;
    }
    scorePreviewPosAtomic_.store(0, std::memory_order_release);
    scorePreviewPlaying_.store(true, std::memory_order_release);
}

void Sp3ctraAudioProcessor::pauseScorePreview() noexcept
{
    scorePreviewPlaying_.store(false, std::memory_order_release);
}

bool Sp3ctraAudioProcessor::resumeScorePreview() noexcept
{
    juce::SpinLock::ScopedLockType sl(scorePreviewLock_);
    if (scorePreviewBuf_.getNumSamples() <= 0) return false;
    if (scorePreviewPos_ >= scorePreviewBuf_.getNumSamples())
        scorePreviewPos_ = 0;                 // finished → restart from the top
    scorePreviewPosAtomic_.store(scorePreviewPos_, std::memory_order_release);
    scorePreviewPlaying_.store(true, std::memory_order_release);
    return true;
}

void Sp3ctraAudioProcessor::stopScorePreview() noexcept
{
    scorePreviewPlaying_.store(false, std::memory_order_release);
    {
        juce::SpinLock::ScopedLockType sl(scorePreviewLock_);
        scorePreviewPos_ = 0;
    }
    scorePreviewPosAtomic_.store(0, std::memory_order_release);
}

double Sp3ctraAudioProcessor::getScorePreviewPositionSec() const noexcept
{
    const double r = (scorePreviewRate_ > 0.0) ? scorePreviewRate_ : 48000.0;
    return (double) scorePreviewPosAtomic_.load(std::memory_order_acquire) / r;
}

void Sp3ctraAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    rt_profiler_callback_start(&g_vst_rt_profiler);
    
    juce::ScopedNoDenormals noDenormals;
    auto totalNumOutputChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();
    
    // Clear output buffer first
    buffer.clear();

    // (The LuxSampler note-triggered play path was removed 2026-07-13 — banks
    //  are no longer note-addressed. Per-bank PLAY/REC lives in the unified
    //  MIDI-Learn targets handled by midiMap_ below.)

    // ── Generic MIDI mappings: CC/Note → any mapped play parameter ─────────
    // RT-safe (lock-free slot table, no alloc). APVTS params land through the
    // same setValueNotifyingHost path the host's automation uses; NON-APVTS
    // sampler targets (play params + REC/PLAY/SAVE action pulses) route through
    // this processor's IVirtualMidiSink implementation.
    midiMap_.processMidi(midiMessages);


    // ── All Notes Off (panic): release every held/stuck note across engines ───
    // Triggered by the header panic button. RT-safe. LuxSynth/LuxWave receive a
    // note-off for every note via the same lock-free ring the audio thread already
    // feeds (consumed below). lux_pitch/lux_mask get a musical release for LIVE
    // instances, plus a hard reset for disabled/orphaned ones (see below).
    if (panicRequested.exchange(false, std::memory_order_acq_rel))
    {
        for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)   // every per-chain instance
        {
            LuxPitchState* pit = lux_pitch_instance(i);
            LuxMaskState*  msk = lux_mask_instance(i);

            // Clear the held-voice atomics. For a LIVE instance this is a musical
            // release: process_frame() sees the `active` falling edge and runs the
            // RELEASE envelope to zero on the image thread.
            lux_pitch_all_notes_off(pit);
            lux_mask_all_notes_off(msk);

            // ...but a DISABLED / orphaned instance (bypassed, or dragged off every
            // chain) has config.enabled == 0, so process_frame() early-returns and
            // never advances the envelope — the runtime voice state stays frozen at
            // its held level and the note is stuck forever (lit key, shift that
            // resurfaces if the slot is reused). all_notes_off alone cannot fix this.
            // Force the runtime state to IDLE here. Writing voices[] from the audio
            // thread is safe precisely BECAUSE the instance is not enabled: the
            // image/synth thread does not pull a disabled pool, so there is no
            // concurrent writer (same rationale as lux_*_reset for orphaned slots).
            if (!pit->config.enabled) lux_pitch_reset(pit);
            if (!msk->config.enabled) lux_mask_reset(msk);
        }
        if (g_luxsynth_engine.initialized)
            for (int n = 0; n < 128; ++n)
                luxsynth_push_midi_event(0x80, (uint8_t)n, 0);
        if (g_luxwave_engine.initialized)
            for (int n = 0; n < 128; ++n)
                luxwave_push_midi_event(0x80, (uint8_t)n, 0);
    }

    // ── LuxPitch MIDI (RT-safe) — fanned out to every active per-chain instance,
    // each filtering on ITS OWN bank's MIDI channel / octave offset ──
    {
        const uint32_t pitchBits = chainPitchMask_.load(std::memory_order_relaxed);
        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();
            for (uint32_t bits = pitchBits, i = 0; bits != 0; bits >>= 1, ++i)
            {
                if ((bits & 1u) == 0) continue;
                auto* chP  = luxpitchMidiChannelParam [i];
                auto* octP = luxpitchOctaveOffsetParam[i];
                if (chP == nullptr || octP == nullptr) continue;
                if (msg.getChannel() != static_cast<int>(chP->load()) + 1) continue;

                if (msg.isControllerOfType(1)) // CC1 mod wheel → this bank's LFO Depth
                {
                    // The wheel and the on-screen "LFO Depth" slider are a
                    // single control (this instance's bank param).
                    if (auto* p = luxpitchLfoDepthParam[i])
                        p->setValueNotifyingHost((float)msg.getControllerValue() / 127.0f);
                    continue;
                }

                const int shifted = msg.getNoteNumber()
                                  + (static_cast<int>(octP->load()) - 2) * 12;
                LuxPitchState* st = lux_pitch_instance((int) i);
                if (msg.isNoteOn())
                    lux_pitch_note_on(st, shifted, msg.getFloatVelocity());
                else if (msg.isNoteOff())
                    lux_pitch_note_off(st, shifted);
                else if (msg.isPitchWheel())
                    lux_pitch_set_pitch_bend(st, (msg.getPitchWheelValue() - 8192) / 8192.0f);
                else if (msg.isSustainPedalOn() || msg.isSustainPedalOff())
                    lux_pitch_set_sustain(st, msg.isSustainPedalOn() ? 1 : 0);
            }
        }
    }

    // ── LuxMask MIDI (RT-safe) — fanned out to every active per-chain instance,
    // each filtering on ITS OWN bank's MIDI channel / octave offset ──
    {
        const uint32_t maskBits = chainMaskMask_.load(std::memory_order_relaxed);
        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();
            for (uint32_t bits = maskBits, i = 0; bits != 0; bits >>= 1, ++i)
            {
                if ((bits & 1u) == 0) continue;
                auto* chP  = luxmaskMidiChannelParam [i];
                auto* octP = luxmaskOctaveOffsetParam[i];
                if (chP == nullptr || octP == nullptr) continue;
                if (msg.getChannel() != static_cast<int>(chP->load()) + 1) continue;

                if (msg.isControllerOfType(1)) // CC1 mod wheel → this bank's LFO Pos Depth
                {
                    if (auto* p = luxmaskLfoPosDepthParam[i])
                        p->setValueNotifyingHost((float)msg.getControllerValue() / 127.0f);
                    continue;
                }

                const int shifted = msg.getNoteNumber()
                                  + (static_cast<int>(octP->load()) - 2) * 12;
                LuxMaskState* st = lux_mask_instance((int) i);
                if (msg.isNoteOn())
                    lux_mask_note_on(st, shifted, msg.getFloatVelocity());
                else if (msg.isNoteOff())
                    lux_mask_note_off(st, shifted);
                else if (msg.isPitchWheel())
                    lux_mask_set_pitch_bend(st, (msg.getPitchWheelValue() - 8192) / 8192.0f);
                else if (msg.isSustainPedalOn() || msg.isSustainPedalOff())
                    lux_mask_set_sustain(st, msg.isSustainPedalOn() ? 1 : 0);
            }
        }
    }

    // ── Enabled-aware engine gates + zero-CPU drain ──────────────────────────
    // "Fed" = engine globally enabled AND ≥1 OUT send whose bank is ENABLED —
    // a send switched OFF via its rack LED starves the engine exactly like a
    // removed one (OFF = silence, uniformly; freeze is a transport feature,
    // not a side effect of the power button). The gate then stays open
    // kEngineDrainBlocks past the last fed block so the feeds' no-send
    // contract (50-tick debounce + silence push + latch/crossfade) and the
    // released voices complete BEFORE the render collapses to zero CPU —
    // closing instantly froze spectrum+voices, which resurrected as a stale
    // "last sound" on re-enable.
    {
        const auto anyEnabled = [](uint32_t mask,
                                   const lux_out_params_t* banks) noexcept {
            for (int s = 0; mask != 0u && s < LUX_OUT_MAX_SLOTS;
                 ++s, mask >>= 1)
                if ((mask & 1u) && banks[s].enabled)
                    return true;
            return false;
        };
        const bool fed[4] = {
            (deviceEnabledParam == nullptr
             || deviceEnabledParam->load() >= 0.5f)
                && anyEnabled(sendSlotsLuxStral_.load(std::memory_order_relaxed),
                              g_sp3ctra_config.luxstral_out),
            luxsynthEnabledParam->load() > 0.5f
                && anyEnabled(sendSlotsLuxSynth_.load(std::memory_order_relaxed),
                              g_sp3ctra_config.luxsynth_out),
            luxwaveEnabledParam->load() > 0.5f
                && anyEnabled(sendSlotsLuxWave_.load(std::memory_order_relaxed),
                              g_sp3ctra_config.luxwave_out),
            luxgrainEnabledParam != nullptr
                && luxgrainEnabledParam->load() > 0.5f
                && anyEnabled(sendSlotsLuxGrain_.load(std::memory_order_relaxed),
                              g_sp3ctra_config.luxgrain_out),
        };
        for (int e = 0; e < 4; ++e)
        {
            // Falling edge → release every voice: the tails drain (inaudibly —
            // the feed zeroes the spectrum/wavetable meanwhile) inside the
            // drain window, so the gate closes on idle voices and a later
            // re-enable cannot resurrect stale notes.
            if (! fed[e] && engineFed_[e])
            {
                if (e == 1 && g_luxsynth_engine.initialized)
                    luxsynth_engine_all_notes_off(&g_luxsynth_engine);
                else if (e == 2 && g_luxwave_engine.initialized)
                    luxwave_engine_all_notes_off(&g_luxwave_engine);
            }
            engineFed_[e] = fed[e];
            if (fed[e])
                engineDrainBlocks_[e] = kEngineDrainBlocks;
            else if (engineDrainBlocks_[e] > 0)
                --engineDrainBlocks_[e];
            engineGate_[e] = fed[e] || engineDrainBlocks_[e] > 0;
        }
    }

    // ── LuxSynth MIDI (RT-safe: push into lock-free ring buffer) ─────────────
    {
        // Strictly "fed" (no drain): with the send OFF a key press must stay
        // silent — during the drain the spectrum may not be zeroed yet.
        const bool lxEnabled = engineFed_[1];
        if (lxEnabled && g_luxsynth_engine.initialized)
        {
            const int lxCh  = static_cast<int>(luxsynthMidiChannelParam->load()) + 1;
            const int lxOct = static_cast<int>(luxsynthOctaveOffsetParam->load()) - 2;
            for (const auto metadata : midiMessages)
            {
                const auto msg = metadata.getMessage();
                if (msg.getChannel() != lxCh) continue;
                const int shifted = msg.getNoteNumber() + lxOct * 12;
                if (shifted < 0 || shifted > 127) continue;
                if (msg.isNoteOn())
                    luxsynth_push_midi_event(0x90, (uint8_t)shifted, (uint8_t)msg.getVelocity());
                else if (msg.isNoteOff())
                    luxsynth_push_midi_event(0x80, (uint8_t)shifted, 0);
            }
        }
    }

    // ── LuxWave MIDI (RT-safe: push into lock-free ring buffer) ──────────────
    {
        // Strictly "fed" (no drain) — same contract as LuxSynth above.
        const bool lwEnabled = engineFed_[2];
        if (lwEnabled && g_luxwave_engine.initialized)
        {
            const int lwCh  = static_cast<int>(luxwaveMidiChannelParam->load()) + 1;
            const int lwOct = static_cast<int>(luxwaveOctaveOffsetParam->load()) - 2;
            for (const auto metadata : midiMessages)
            {
                const auto msg = metadata.getMessage();
                if (msg.getChannel() != lwCh) continue;
                const int shifted = msg.getNoteNumber() + lwOct * 12;
                if (shifted < 0 || shifted > 127) continue;
                if (msg.isNoteOn())
                    luxwave_push_midi_event(0x90, (uint8_t)shifted, (uint8_t)msg.getVelocity());
                else if (msg.isNoteOff())
                    luxwave_push_midi_event(0x80, (uint8_t)shifted, 0);
            }
        }
    }

    // ── FrameSequencers: advance each engine's sequencer if it is running ────
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
        if (auto* fs = frameSequencers_[(size_t) e].get())
            fs->processBlock(getPlayHead(),
                             buffer.getNumSamples(),
                             getSampleRate());

    // ── Acquisition gate: brake the live frame-advance rate ──────────────────
    // "Vitesse d'acquisition" — drive the gate clock once per block.  When Off
    // it disables the gate (full-rate); otherwise it grants advances at the
    // chosen rate and the UDP thread holds the frame between grants.
    {
        const int   mode    = static_cast<int> (acqGateModeParam->load());
        const float rateMs  = acqGateRateMsParam->load();
        const int   divIdx  = static_cast<int> (acqGateSyncDivParam->load());
        const int   mdIdx   = static_cast<int> (acqGateMultDivParam->load());

        // Index → period-in-beats (1/1 .. 1/32, assuming a quarter-note beat).
        static constexpr double kSyncDivBeats[6]  = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
        // Index → refresh-rate factor (/32 .. x4); period = base / factor.
        static constexpr double kRefreshFactor[8] = { 1.0/32, 1.0/16, 1.0/8, 1.0/4,
                                                       1.0/2,  1.0,    2.0,   4.0 };
        const double divBeats = kSyncDivBeats [juce::jlimit(0, 5, divIdx)];
        const double refresh  = kRefreshFactor[juce::jlimit(0, 7, mdIdx)];

        AudioImageBuffers* aib = nullptr;
        if (auto* core = getSp3ctraCore())
            aib = core->getAudioImageBuffers();

        acqGate_.process(aib, mode, rateMs, divBeats, refresh,
                         buffer.getNumSamples(), getSampleRate(), getPlayHead());
    }

    // ── Sequencer-gated recording — per engine: each sampler's OWN sequencer
    // gates its recording (slot of the current step; sentinels < 0 ungate).
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
    {
        if (! samplers_[(size_t) e]) continue;
        int gate = -1;
        if (auto* fs = frameSequencers_[(size_t) e].get();
            fs != nullptr && fs->isPlaying())
        {
            const int curStep = fs->getCurrentStep();
            if (curStep >= 0)
            {
                const int slot = fs->getStep(curStep);
                if (slot >= 0) gate = slot;
            }
        }
        samplers_[(size_t) e]->setSeqGateSlot(gate);
    }

    // ========================================================================
    // 🎯 LUXSTRAL LOCK-FREE DOUBLE-BUFFER CONSUMER (RT-SAFE)
    //
    // Architecture: AudioProcessingThread (producer) writes audio to a double-
    // buffer. processBlock (consumer) reads from it. The key insight is:
    //
    //   processBlock must NEVER output silence when the producer is mid-write.
    //   Instead, it re-outputs the last successfully read buffer.
    //
    // We track which buffer was last consumed via lastConsumedReadIdx.
    // Only signal "consumed" ONCE per new buffer (avoids double-triggering).
    // DO NOT set ready=0 in the consumer — let the producer manage ready flags.
    //
    // The consume/handshake below runs whenever the core is up — INDEPENDENT of
    // deviceEnabled. It paces audioProcessingThread (the whole synth pipeline
    // renders on this signal): gating it on the output toggle starved the
    // pipeline down to the 50ms wait timeout (each grain replayed ~4-5x =
    // robotic sound). deviceEnabled gates the WRITE into the JUCE buffer here,
    // and (via g_engine_render_gates below) collapses the producer's render to
    // a silence commit — the handshake itself is never gated.
    // ========================================================================
    const bool luxstralEnabled = (deviceEnabledParam == nullptr || deviceEnabledParam->load() >= 0.5f);

    // ── Zero-CPU contract — publish per-engine render gates to the synth
    // thread (multithreading.c): the enabled-aware gates computed above
    // (fed OR draining). A cleared LuxStral bit collapses synth_AudioProcess
    // to a cheap silence commit (the producer/consumer pacing stays intact);
    // a cleared LuxSynth/LuxWave/LuxGrain bit skips that engine's feed tick.
    {
        extern volatile uint32_t g_engine_render_gates;
        const uint32_t gates = (engineGate_[0] ? 1u : 0u)
                             | (engineGate_[1] ? 2u : 0u)
                             | (engineGate_[2] ? 4u : 0u)
                             | (engineGate_[3] ? 8u : 0u);
        __atomic_store_n(&g_engine_render_gates, gates, __ATOMIC_RELEASE);
    }
    if (sharedCore && sharedCore->isReady() && luxstral_are_audio_buffers_ready()) {
        extern AudioImageBuffer luxstral_buffers_L[2];
        extern AudioImageBuffer luxstral_buffers_R[2];
        extern volatile int luxstral_buffer_index;
        extern sp3ctra_config_t g_sp3ctra_config;
        
        // Read current buffer index with ACQUIRE (ARM64 memory ordering)
        int readIdx = 1 - __atomic_load_n(&luxstral_buffer_index, __ATOMIC_ACQUIRE);
        
        // Check if the buffer at readIdx has new data
        int leftReady = __atomic_load_n(&luxstral_buffers_L[readIdx].ready, __ATOMIC_ACQUIRE);
        int rightReady = __atomic_load_n(&luxstral_buffers_R[readIdx].ready, __ATOMIC_ACQUIRE);

        // Clamp to the ALLOCATED size, not g_sp3ctra_config.audio_buffer_size —
        // after a host buffer-size change the reallocation can be refused
        // (multi-instance) and reading numSamples would run past the allocation.
        const int synthBufferSize = luxstral_get_audio_buffer_size();
        const int samplesToRead = (numSamples <= synthBufferSize) ? numSamples : synthBufferSize;
        
        if (leftReady && rightReady && readIdx != lastConsumedReadIdx) {
            // ✅ NEW DATA available — copy to JUCE output and signal producer
            float* leftData = luxstral_buffers_L[readIdx].data;
            float* rightData = luxstral_buffers_R[readIdx].data;

                if (leftData && rightData) {
                if (luxstralEnabled) {
                    const float lsVol = luxstralVolumeParam->load();
                    float pk = lsPkBlock_;   // VU: post-volume block peak
                    if (totalNumOutputChannels >= 1) {
                        float* destLeft = buffer.getWritePointer(0);
                        for (int i = 0; i < samplesToRead; ++i) {
                            const float v = leftData[i] * lsVol;
                            destLeft[i] = v;
                            const float a = v < 0.0f ? -v : v;
                            if (a > pk) pk = a;
                        }
                    }
                    if (totalNumOutputChannels >= 2) {
                        float* destRight = buffer.getWritePointer(1);
                        for (int i = 0; i < samplesToRead; ++i) {
                            const float v = rightData[i] * lsVol;
                            destRight[i] = v;
                            const float a = v < 0.0f ? -v : v;
                            if (a > pk) pk = a;
                        }
                    }
                    lsPkBlock_ = pk;
                }

                // Track which buffer we consumed (don't signal twice for same data)
                lastConsumedReadIdx = readIdx;

                // Signal producer that it can generate the next buffer
                // DO NOT set ready=0 — producer manages ready flags
                luxstral_signal_buffer_consumed();
            }
        } else if (luxstralEnabled && leftReady && rightReady) {
            // ♻️ SAME DATA as last time (producer hasn't finished next buffer yet)
            // Re-output the same audio — much better than silence!
            // Count stale re-outputs for RT Profiler diagnostics (always, not only Debug)
            rt_profiler_report_stale_luxstral(&g_vst_rt_profiler);
            float* leftData = luxstral_buffers_L[readIdx].data;
            float* rightData = luxstral_buffers_R[readIdx].data;
            
            if (leftData && rightData) {
                const float lsVol = luxstralVolumeParam->load();
                float pk = lsPkBlock_;   // VU: post-volume block peak
                if (totalNumOutputChannels >= 1) {
                    float* destLeft = buffer.getWritePointer(0);
                    for (int i = 0; i < samplesToRead; ++i) {
                        const float v = leftData[i] * lsVol;
                        destLeft[i] = v;
                        const float a = v < 0.0f ? -v : v;
                        if (a > pk) pk = a;
                    }
                }
                if (totalNumOutputChannels >= 2) {
                    float* destRight = buffer.getWritePointer(1);
                    for (int i = 0; i < samplesToRead; ++i) {
                        const float v = rightData[i] * lsVol;
                        destRight[i] = v;
                        const float a = v < 0.0f ? -v : v;
                        if (a > pk) pk = a;
                    }
                }
                lsPkBlock_ = pk;
            }
            // DO NOT signal consumed — producer is still working on the next buffer
        } else {
            // 🔇 No data ready at all (startup or after long pause)
            // Buffer already cleared — silence is appropriate here
            rt_profiler_report_buffer_miss_luxstral(&g_vst_rt_profiler);
        }
    }

    // ========================================================================
    // 🎯 LUXSYNTH INLINE SYNTHESIS (RT-SAFE, ADDITIVE)
    //
    // The LuxSynth engine is fully RT-safe (no allocation, no lock, no I/O),
    // so it runs directly in processBlock.  This eliminates all double-buffer
    // synchronisation issues (timing mismatch, stale data replay, temporal
    // discontinuities) that occurred with the former async thread approach.
    //
    // MIDI events are pushed to the lock-free ring buffer above, then drained
    // here before generating audio for exactly `numSamples` samples — always
    // matching the DAW buffer size.
    // ========================================================================
    if (sharedCore && sharedCore->isReady() && g_luxsynth_engine.initialized)
    {
        // Gate incl. drain: the render keeps running after the last enabled
        // send drops so the feed's silence push latches and the released
        // voices fade out before the gate closes.
        const bool lxEnabled = engineGate_[1];
        if (lxEnabled)
        {
            struct timeval lxT0, lxT1;
            gettimeofday(&lxT0, NULL);   // per-family perf attribution

            // Click-diagnostic context: what happened during THIS block.
            const unsigned long long lxTrigBefore =
                g_luxsynth_engine.current_trigger_order;
            const uint32_t lxSeqBefore = g_luxsynth_engine.spec_applied_seq;

            // 1. Drain pending MIDI events into engine voices
            luxsynth_process_pending_midi();

            // 2. Generate audio directly — uses preallocated engine buffers
            luxsynth_engine_process(&g_luxsynth_engine, numSamples,
                                    g_luxsynth_engine.output_left,
                                    g_luxsynth_engine.output_right);

            const int lxNoteOnThisBlock =
                (g_luxsynth_engine.current_trigger_order != lxTrigBefore);
            const int lxLatchedThisBlock =
                (g_luxsynth_engine.spec_applied_seq != lxSeqBefore);

            // 3. Mix into JUCE output buffer (additive)
            const float lxVol = luxsynthVolumeParam->load();

            float pk = lxPkBlock_;   // VU: post-volume block peak
            if (totalNumOutputChannels >= 1)
            {
                float* dest = buffer.getWritePointer(0);
                for (int i = 0; i < numSamples; ++i) {
                    const float v = g_luxsynth_engine.output_left[i] * lxVol;
                    dest[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > pk) pk = a;
                }
            }
            if (totalNumOutputChannels >= 2)
            {
                float* dest = buffer.getWritePointer(1);
                for (int i = 0; i < numSamples; ++i) {
                    const float v = g_luxsynth_engine.output_right[i] * lxVol;
                    dest[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > pk) pk = a;
                }
            }
            lxPkBlock_ = pk;

            // ── Dropout diagnostic (RT-safe: counters only, timer drains) ────
            // A block whose RAW engine output collapses to ~0 right after a
            // loud block is a hard cut. "voiced" gaps (voices still held) point
            // upstream (spectrum zeroed / feed silence); "unvoiced" gaps point
            // at MIDI (note-offs). Pre-volume peak so a fader move can't fake
            // a gap.
            {
                extern std::atomic<uint64_t> g_lxDiagGapsVoiced,
                                             g_lxDiagGapsUnvoiced;
                extern std::atomic<int>      g_lxDiagLastVoices,
                                             g_lxDiagLastBins;
                extern std::atomic<float>    g_lxDiagLastMaxMag;
                static float s_lxPrevRawPk = 0.0f;

                float rawPk = 0.0f;
                for (int i = 0; i < numSamples; ++i) {
                    const float aL = std::abs(g_luxsynth_engine.output_left[i]);
                    const float aR = std::abs(g_luxsynth_engine.output_right[i]);
                    if (aL > rawPk) rawPk = aL;
                    if (aR > rawPk) rawPk = aR;
                }
                const float prevPk = s_lxPrevRawPk;
                s_lxPrevRawPk = rawPk;
                if (prevPk > 0.05f && rawPk < 1.0e-4f)
                {
                    int voiced = 0;
                    for (int v = 0; v < g_luxsynth_engine.num_voices; ++v)
                        if (g_luxsynth_engine.voices[v].volume_env.state
                            != ADSR_STATE_IDLE)
                            ++voiced;
                    float maxMag = 0.0f;
                    const int nb = g_luxsynth_engine.spectral.num_bins;
                    for (int k = 0; k < nb && k < LUXSYNTH_MAX_OSCILLATORS; ++k)
                        if (g_luxsynth_engine.spectral.magnitudes[k] > maxMag)
                            maxMag = g_luxsynth_engine.spectral.magnitudes[k];
                    g_lxDiagLastVoices.store(voiced, std::memory_order_relaxed);
                    g_lxDiagLastBins.store(nb, std::memory_order_relaxed);
                    g_lxDiagLastMaxMag.store(maxMag, std::memory_order_relaxed);
                    (voiced > 0 ? g_lxDiagGapsVoiced : g_lxDiagGapsUnvoiced)
                        .fetch_add(1, std::memory_order_relaxed);
                }
            }

            // ── Click diagnostic: single-sample discontinuities in the RAW
            // engine output (the gap detector above only sees full collapses).
            // A fresh note-on legitimately jumps — those blocks are skipped.
            // Post-volume zipper is reported separately via volStep.
            {
                extern std::atomic<uint64_t> g_lxDiagClicks;
                extern std::atomic<float>    g_lxDiagClickDelta,
                                             g_lxDiagClickVolStep;
                extern std::atomic<int>      g_lxDiagClickLatched,
                                             g_lxDiagClickVoices;
                static float s_lxPrevLastL = 0.0f;   // block-boundary continuity
                static float s_lxPrevVol   = -1.0f;

                const float volStep =
                    (s_lxPrevVol >= 0.0f) ? (lxVol - s_lxPrevVol) : 0.0f;
                s_lxPrevVol = lxVol;

                if (!lxNoteOnThisBlock && numSamples > 0)
                {
                    float maxDelta = 0.0f;
                    float prev = s_lxPrevLastL;
                    for (int i = 0; i < numSamples; ++i) {
                        const float cur = g_luxsynth_engine.output_left[i];
                        const float d = std::abs(cur - prev);
                        if (d > maxDelta) maxDelta = d;
                        prev = cur;
                    }
                    if (maxDelta > 0.35f)
                    {
                        int voiced = 0;
                        for (int v = 0; v < g_luxsynth_engine.num_voices; ++v)
                            if (g_luxsynth_engine.voices[v].volume_env.state
                                != ADSR_STATE_IDLE)
                                ++voiced;
                        g_lxDiagClickDelta.store(maxDelta,
                                                 std::memory_order_relaxed);
                        g_lxDiagClickVolStep.store(volStep,
                                                   std::memory_order_relaxed);
                        g_lxDiagClickLatched.store(lxLatchedThisBlock,
                                                   std::memory_order_relaxed);
                        g_lxDiagClickVoices.store(voiced,
                                                  std::memory_order_relaxed);
                        g_lxDiagClicks.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                s_lxPrevLastL = (numSamples > 0)
                    ? g_luxsynth_engine.output_left[numSamples - 1] : 0.0f;
            }

            gettimeofday(&lxT1, NULL);
            rt_profiler_engine_report(&g_vst_rt_profiler, RT_ENGINE_LUXSYNTH,
                (uint64_t)((lxT1.tv_sec - lxT0.tv_sec) * 1000000LL
                           + (lxT1.tv_usec - lxT0.tv_usec)));
        }
    }

    // ========================================================================
    // 🎯 LUXWAVE INLINE SYNTHESIS (RT-SAFE, WAVETABLE)
    //
    // LuxWave reads the LuxSynth grayscale line as a dynamic wavetable.
    // MIDI pitch controls playback speed through the waveform.
    // Fully RT-safe — runs directly in processBlock.
    // ========================================================================
    if (sharedCore && sharedCore->isReady() && g_luxwave_engine.initialized)
    {
        // Gate incl. drain — see the LuxSynth render gate above.
        const bool lwEnabled = engineGate_[2];
        if (lwEnabled)
        {
            struct timeval lwT0, lwT1;
            gettimeofday(&lwT0, NULL);   // per-family perf attribution

            // 1. Update engine config from APVTS (RT-safe: simple struct copy)
            LuxWaveConfig lwCfg;
            lwCfg.attack_ms           = luxwaveAttackMsParam->load();
            lwCfg.decay_ms            = luxwaveDecayMsParam->load();
            lwCfg.sustain_level       = luxwaveSustainLevelParam->load();
            lwCfg.release_ms          = luxwaveReleaseMsParam->load();
            lwCfg.attack_curve        = luxwaveAttackCurveParam->load();
            lwCfg.decay_curve         = luxwaveDecayCurveParam->load();
            lwCfg.release_curve       = luxwaveReleaseCurveParam->load();
            lwCfg.filter_attack_ms    = 20.0f;
            lwCfg.filter_decay_ms     = 150.0f;
            lwCfg.filter_sustain      = 0.5f;
            lwCfg.filter_release_ms   = 300.0f;
            lwCfg.filter_cutoff_hz    = luxwaveFilterCutoffParam->load();
            lwCfg.filter_env_depth_hz = luxwaveFilterEnvDepthParam->load();
            lwCfg.lfo_rate_hz         = luxwaveLfoRateParam->load();
            lwCfg.lfo_depth_semitones = luxwaveLfoDepthParam->load();
            lwCfg.scan_mode           = (LuxWaveScanMode)static_cast<int>(luxwaveScanModeParam->load());
            lwCfg.amplitude           = luxwaveAmplitudeParam->load();
            lwCfg.sample_rate         = (float)getSampleRate();
            lwCfg.buffer_size         = numSamples;
            lwCfg.enabled             = true;
            luxwave_engine_set_config(&g_luxwave_engine, &lwCfg);

            // 2. Drain pending MIDI events
            luxwave_process_pending_midi();

            // 3. Generate audio
            luxwave_engine_process(&g_luxwave_engine, numSamples,
                                   g_luxwave_engine.output_left,
                                   g_luxwave_engine.output_right);

            // 4. Mix into JUCE output buffer (additive)
            const float lwVol = luxwaveVolumeParam->load();
            float pk = lwPkBlock_;   // VU: post-volume block peak
            if (totalNumOutputChannels >= 1)
            {
                float* dest = buffer.getWritePointer(0);
                for (int i = 0; i < numSamples; ++i) {
                    const float v = g_luxwave_engine.output_left[i] * lwVol;
                    dest[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > pk) pk = a;
                }
            }
            if (totalNumOutputChannels >= 2)
            {
                float* dest = buffer.getWritePointer(1);
                for (int i = 0; i < numSamples; ++i) {
                    const float v = g_luxwave_engine.output_right[i] * lwVol;
                    dest[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > pk) pk = a;
                }
            }
            lwPkBlock_ = pk;

            gettimeofday(&lwT1, NULL);
            rt_profiler_engine_report(&g_vst_rt_profiler, RT_ENGINE_LUXWAVE,
                (uint64_t)((lwT1.tv_sec - lwT0.tv_sec) * 1000000LL
                           + (lwT1.tv_usec - lwT0.tv_usec)));
        }
    }

    // ========================================================================
    // 🎯 LUXGRAIN INLINE SYNTHESIS (RT-SAFE, GRANULAR CLOUD)
    //
    // Stochastic granular engine driven by the "→ LUXGRAIN" image sends
    // (luminance → grain emission density). The feed folds the mixed line
    // into band cells on audioProcessingThread; the engine latches here at
    // block start (internal seqlocks) and renders the cloud inline.
    // ========================================================================
    if (sharedCore && sharedCore->isReady() && g_luxgrain_engine.initialized)
    {
        // Gate incl. drain — see the LuxSynth render gate above.
        const bool lgEnabled = engineGate_[3];
        if (lgEnabled && numSamples <= LUXGRAIN_MAX_BUFFER_SIZE)
        {
            struct timeval lgT0, lgT1;
            gettimeofday(&lgT0, NULL);   // per-family perf attribution

            // 1. Engine config from APVTS (RT-safe: staged struct copy, M4).
            LuxGrainConfig lgCfg = luxgrain_config_default();
            lgCfg.enabled       = 1;
            lgCfg.master_volume = 0.35f;   // M5 calibration may retune this
            lgCfg.density_hz      = luxgrainDensityParam->load();
            lgCfg.density_shape   = luxgrainDensityShapeParam->load();
            lgCfg.spread_lines    = luxgrainSpreadParam->load();
            lgCfg.dur_min_ms      = luxgrainSizeMinParam->load();
            lgCfg.dur_max_ms      = juce::jmax(lgCfg.dur_min_ms,
                                               luxgrainSizeMaxParam->load());
            lgCfg.contrast_amount = luxgrainTextureParam->load();
            lgCfg.env_shape       = (int) luxgrainEnvShapeParam->load();
            lgCfg.pitch_jitter_st = luxgrainJitterParam->load();
            lgCfg.stereo_width    = luxgrainWidthParam->load();
            lgCfg.amp_follow      = luxgrainAmpFollowParam->load();
            lgCfg.color_pan       = luxgrainColorPanParam->load();
            lgCfg.edge_amount     = luxgrainEdgeParam->load();
            lgCfg.num_bands       = (int) luxgrainBandsParam->load();
            lgCfg.material        = (int) luxgrainMaterialParam->load();
            lgCfg.scrub           = luxgrainScrubParam->load();
            if (g_sp3ctra_config.low_frequency > 0.0f)
                lgCfg.axis_low_hz = g_sp3ctra_config.low_frequency;
            if (g_sp3ctra_config.num_octaves > 0)
                lgCfg.num_octaves = g_sp3ctra_config.num_octaves;
            luxgrain_engine_set_config(&g_luxgrain_engine, &lgCfg);

            // 2. Render the cloud, then mix (additive, like LuxWave)
            luxgrain_engine_process(&g_luxgrain_engine,
                                    g_luxgrain_out_l, g_luxgrain_out_r,
                                    numSamples);
            const float lgVol = luxgrainVolumeParam
                                ? luxgrainVolumeParam->load() : 1.0f;
            float pk = lgPkBlock_;   // VU: post-volume block peak
            if (totalNumOutputChannels >= 1)
            {
                float* dest = buffer.getWritePointer(0);
                for (int i = 0; i < numSamples; ++i) {
                    const float v = g_luxgrain_out_l[i] * lgVol;
                    dest[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > pk) pk = a;
                }
            }
            if (totalNumOutputChannels >= 2)
            {
                float* dest = buffer.getWritePointer(1);
                for (int i = 0; i < numSamples; ++i) {
                    const float v = g_luxgrain_out_r[i] * lgVol;
                    dest[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > pk) pk = a;
                }
            }
            lgPkBlock_ = pk;

            gettimeofday(&lgT1, NULL);
            rt_profiler_engine_report(&g_vst_rt_profiler, RT_ENGINE_LUXGRAIN,
                (uint64_t)((lgT1.tv_sec - lgT0.tv_sec) * 1000000LL
                           + (lgT1.tv_usec - lgT0.tv_usec)));
        }
    }

    // ── SCORE source preview: mix the auditioned WAV region into the output ──
    // RT-safe: try-lock (skip this block on contention), no I/O, no allocation.
    if (scorePreviewPlaying_.load(std::memory_order_acquire))
    {
        const juce::SpinLock::ScopedTryLockType sl(scorePreviewLock_);
        if (sl.isLocked())
        {
            const int n  = scorePreviewBuf_.getNumSamples();
            const int pc = scorePreviewBuf_.getNumChannels();
            int pos = scorePreviewPos_;
            constexpr float kPreviewGain = 0.9f;
            for (int i = 0; i < numSamples && pos < n; ++i, ++pos)
                for (int ch = 0; ch < totalNumOutputChannels; ++ch)
                    buffer.addSample(ch, i,
                        scorePreviewBuf_.getSample(juce::jmin(ch, pc - 1), pos) * kPreviewGain);
            scorePreviewPos_ = pos;
            scorePreviewPosAtomic_.store(pos, std::memory_order_release);
            if (pos >= n)
                scorePreviewPlaying_.store(false, std::memory_order_release);
        }
    }

    // Apply master volume — RT-safe: atomic read, O(N) multiply, no lock, no allocation
    if (masterVolumeParam != nullptr)
    {
        const float gain = masterVolumeParam->load();
        if (gain < 0.9999f)
            buffer.applyGain(gain);
    }

    // ── VIDEO MIX recording: tap the finished master stereo → recorder FIFO.
    // RT-safe: lock-free write, no alloc/lock; gated so it's a single atomic
    // load when idle. `buffer` here IS the final mastered mix.
    if (recActive_.load(std::memory_order_acquire) && videoRecorder_ != nullptr
        && totalNumOutputChannels > 0)
        videoRecorder_->pushAudio(buffer.getArrayOfReadPointers(),
                                  totalNumOutputChannels, numSamples);

    // ── UI VU meters: fold this block's per-engine peaks into the atomics
    // with an exponential release, then reset the block accumulators.
    // Single RT writer; the AUDIO MIX panel reads at UI rate (relaxed).
    {
        auto fold = [](std::atomic<float>& m, float pk)
        {
            const float rel = m.load(std::memory_order_relaxed) * 0.82f;
            m.store(pk > rel ? pk : rel, std::memory_order_relaxed);
        };
        float mpk = 0.0f;
        for (int ch = 0; ch < totalNumOutputChannels; ++ch)
            mpk = juce::jmax(mpk, buffer.getMagnitude(ch, 0, numSamples));
        fold(meterMaster_,   mpk);
        fold(meterLuxStral_, lsPkBlock_);
        fold(meterLuxSynth_, lxPkBlock_);
        fold(meterLuxWave_,  lwPkBlock_);
        fold(meterLuxGrain_, lgPkBlock_);
        lsPkBlock_ = lxPkBlock_ = lwPkBlock_ = lgPkBlock_ = 0.0f;
    }

    rt_profiler_callback_end(&g_vst_rt_profiler);
}

//==============================================================================
bool Sp3ctraAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* Sp3ctraAudioProcessor::createEditor()
{
    return new Sp3ctraAudioProcessorEditor (*this);
}

//==============================================================================
// APVTS State Management (automatic save/restore in DAW projects)
//
// captureFullState() builds the COMPLETE plugin state (APVTS + every non-APVTS
// child tree). It is the single source of truth shared by getStateInformation
// (the host/DAW blob) and by the SessionManager, which writes the same tree to
// <session>/project.sp3ctra. Message-thread safe: read-only, atomic snapshots.
juce::ValueTree Sp3ctraAudioProcessor::captureFullState(bool embedBanks)
{
    auto state = apvts.copyState();
    // Persist the session paths so they survive DAW project reloads and
    // Standalone restarts. ALWAYS written (even when empty): the copied
    // state may still carry the value restored at load time, and skipping
    // the write would resurrect a path the user has since cleared.
    state.setProperty("lastSessionPath",  lastSessionPath,  nullptr);
    state.setProperty("scoreWavPath",     scoreWavPath,     nullptr);
    state.setProperty("luxgrainSamplePath", luxgrainSamplePath_, nullptr);
    // Synth-split state version — gates the staged migrations in
    // setStateInformation (absent = pre-split blob; 1 = pre per-send enable;
    // 2 = pre per-sampler-engine enable).
    state.setProperty("synthSplitVersion", 3, nullptr);

    // Non-APVTS module state → child trees. Replace (never append next to) any
    // stale copy restored at load time.
    auto replaceChild = [&state](juce::ValueTree child)
    {
        if (! child.isValid())
            return;
        auto existing = state.getChildWithName(child.getType());
        if (existing.isValid())
            state.removeChild(existing, nullptr);
        state.appendChild(child, nullptr);
    };
    replaceChild(scoreStateToTree());        // SCORE settings + freq override
    replaceChild(luxstralWavetableToTree()); // user timbre wavetable (harmonics)
    // A cleared timbre writes no child — drop any stale copy so it doesn't
    // resurrect on the next load.
    if (! luxstral_wavetable_is_loaded())
    {
        auto stale = state.getChildWithName("LUXSTRAL_WAVETABLE");
        if (stale.isValid())
            state.removeChild(stale, nullptr);
    }
    replaceChild(seqStateToTree());          // per-engine sequencer patterns
    // Drop any stale legacy GLOBAL pattern ("SEQ") restored from an old blob —
    // new saves only write the per-engine "SEQS" tree.
    {
        auto staleSeq = state.getChildWithName("SEQ");
        if (staleSeq.isValid())
            state.removeChild(staleSeq, nullptr);
    }
    replaceChild(samplerSlotsStateToTree()); // per-slot params, engines A + B
    replaceChild(mediaSourcesStateToTree()); // M9 — IMAGE/VIDEO paths + camera
    // CHAINS — snapshot the LIVE model, never trust the copy restored/edited
    // earlier: rack edits persist via a deferred callAsync (persistChainModel),
    // so a save taken before that dispatch would write a stale topology; and a
    // fresh session has no CHAINS child at all until the first rack edit.
    // J2 — the chain owns its modules' settings: refresh each module's VALUES
    // from the runtime banks (atomic reads) before serialising.
    snapshotBankValuesIntoModel();
    replaceChild(chainModel_.toValueTree());
    // UUID → pool-slot bindings: the per-instance param banks are keyed by the
    // slot, so the binding must reload identically (else settings would swap).
    replaceChild(poolBindingsToTree());
    // (J3: the per-chain settings memory now lives IN the CHAINS tree —
    // Chain::typeMemory, serialized as CHAIN/MEMORY children. The legacy
    // INSERT_MEMORY blob is no longer written; it is still read once as a
    // migration on load.) J6 — drop the stale copy a migrated session still
    // carries, so old blobs resave as clean v3.
    {
        auto legacy = state.getChildWithName("INSERT_MEMORY");
        if (legacy.isValid())
            state.removeChild(legacy, nullptr);
    }
    replaceChild(midiMap_.toValueTree());    // MIDI CC/Note → param mappings

    // DAW self-containment: embed the recorded sampler audio so the host project
    // needs no external files. Standalone sessions keep banks as sidecar files
    // (embedBanks=false) — see SessionManager.
    if (embedBanks)
        replaceChild(samplerBanksToTree());

    return state;
}

// Recorded sampler audio → a SAMPLER_BANKS tree, one binary child per engine.
// Reuses LuxSampler::saveToFile via a temp file (same primitive as the retired
// .sp3s), then folds the bytes into the ValueTree as a MemoryBlock property.
juce::ValueTree Sp3ctraAudioProcessor::samplerBanksToTree()
{
    juce::ValueTree banks("SAMPLER_BANKS");
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
    {
        auto* smp = getSampler(e);
        if (smp == nullptr) continue;
        juce::TemporaryFile tmp(".fsmp");
        if (! smp->saveToFile(tmp.getFile())) continue;
        juce::MemoryBlock blob;
        if (! tmp.getFile().loadFileAsData(blob) || blob.isEmpty()) continue;
        juce::ValueTree eng("Engine");
        eng.setProperty("index", e, nullptr);
        eng.setProperty("fsmp",  blob, nullptr);   // stored base64 in the XML
        banks.appendChild(eng, nullptr);
    }
    return banks;
}

void Sp3ctraAudioProcessor::restoreSamplerBanksFromTree(const juce::ValueTree& banks)
{
    if (! banks.isValid()) return;
    for (int c = 0; c < banks.getNumChildren(); ++c)
    {
        auto eng = banks.getChild(c);
        const int e = (int) eng.getProperty("index", -1);
        if (e < 0 || e >= LuxSampler::kMaxEngines) continue;
        auto* smp = getSampler(e);
        if (smp == nullptr) continue;
        if (auto* blob = eng.getProperty("fsmp").getBinaryData())
        {
            juce::TemporaryFile tmp(".fsmp");
            tmp.getFile().replaceWithData(blob->getData(), blob->getSize());
            smp->loadFromFile(tmp.getFile());
        }
    }
}

void Sp3ctraAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Standalone: the SessionManager owns persistence — the host blob is only a
    // pointer to the active session directory (the real state auto-saves to the
    // folder). DAW: a full, self-contained blob with the sampler banks embedded.
    if (sessions_ != nullptr && sessions_->isStandalone())
    {
        auto ref = sessions_->makeStandaloneRefState();
        std::unique_ptr<juce::XmlElement> xml(ref.createXml());
        copyXmlToBinary(*xml, destData);
        return;
    }

    auto state = captureFullState(/*embedBanks*/ true);
    std::unique_ptr<juce::XmlElement> xml(state.createXml());

    copyXmlToBinary(*xml, destData);
    log_info("VST", "State saved to DAW project (%d KB)",
             (int) ((destData.getSize() + 1023) / 1024));
}

void Sp3ctraAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));

    // Standalone: the host blob is only a pointer to the active session dir
    // (Sp3ctraStandaloneRef) — open it. On the FIRST launch after upgrading
    // from the pre-session build, the blob is instead a legacy full state →
    // migrate it once into the Global session. The SessionManager reuses
    // applyStateXml for the real restore.
    if (sessions_ != nullptr && sessions_->isStandalone())
    {
        if (xml != nullptr && xml->hasTagName("Sp3ctraStandaloneRef"))
            sessions_->openOnLaunch(
                juce::File(xml->getStringAttribute("activeSessionDir")));
        else
            sessions_->migrateLegacyBlobIntoGlobal(std::move(xml));
        return;
    }

    // DAW / other hosts: full self-contained restore (banks embedded). Delegate
    // to applyStateXml so the SessionManager and the host share one code path.
    applyStateXml(std::move(xml));
}

void Sp3ctraAudioProcessor::applyStateXml(std::unique_ptr<juce::XmlElement> xmlState)
{
    // A failed restore silently keeps every default (topology, SCORE, SEQ,
    // sampler params, session paths) — make that loudly visible in the log.
    if (xmlState == nullptr)
        log_error("VST", "State restore FAILED: corrupt/unreadable state blob "
                         "— session resets to defaults");
    else if (! xmlState->hasTagName(apvts.state.getType()))
        log_error("VST", "State restore FAILED: unexpected root tag '%s' "
                         "(expected '%s') — session resets to defaults",
                  xmlState->getTagName().toRawUTF8(),
                  apvts.state.getType().toString().toRawUTF8());

    if (xmlState.get() != nullptr) {
        if (xmlState->hasTagName(apvts.state.getType())) {
            // Transport open-state is patched into the tree BEFORE replaceState
            // so it IS the restored value — the previous setValueNotifyingHost()
            // push after the restore marked host automation lanes as overridden
            // on every project open (and could write a point in Latch/Write
            // modes). A missing PARAM entry needs no patch: the defaults already
            // match the open-state below.
            //   AUDIO transports (SEQ/SCORE/media sources) open STOPPED so a
            //   project never blasts sound on load (never-auto-run).
            //   The VIDEO SCROLL is a visual output, not audio: it opens RUNNING
            //   so the right-band waterfall scrolls immediately on launch — the
            //   play/pause button already defaults to "running", so button and
            //   param now agree (previously it opened frozen-but-labelled-play,
            //   requiring a pause/play dance to start).
            auto forceRestoredParam = [&xmlState](const juce::String& id, double v)
            {
                for (auto* e : xmlState->getChildWithTagNameIterator("PARAM"))
                    if (e->getStringAttribute("id") == id)
                    { e->setAttribute("value", v); return; }
            };
            forceRestoredParam("videoScrollPaused", 0.0);       // legacy global
            for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
                forceRestoredParam(vsParam(s, "paused"), 0.0);  // per-instance: run

            // Migration — the per-instance VideoScroll "invert" bool became the
            // 3-way "invertMode" choice (Off/Negative/Luminance). A saved
            // invert==1 meant RGB negative → seed invertMode = 1. Only when the
            // session predates the choice (no invertMode entry) so new sessions,
            // which always write invertMode, win untouched.
            for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
            {
                const juce::String invId  = vsParam(s, "invert");
                const juce::String modeId = vsParam(s, "invertMode");
                juce::XmlElement* invEl = nullptr;
                bool haveMode = false;
                for (auto* e : xmlState->getChildWithTagNameIterator("PARAM"))
                {
                    const auto pid = e->getStringAttribute("id");
                    if      (pid == invId)  invEl = e;
                    else if (pid == modeId) haveMode = true;
                }
                if (! haveMode && invEl != nullptr
                    && invEl->getDoubleAttribute("value") >= 0.5)
                {
                    auto* e = xmlState->createNewChildElement("PARAM");
                    e->setAttribute("id", modeId);
                    e->setAttribute("value", 1.0);   // Negative
                }
            }
            for (int e = 0; e < LuxSampler::kMaxEngines; ++e)   // Stop
                forceRestoredParam(fsEngineParam(e, "SeqTransport"), 0.0);
            forceRestoredParam(PARAM_SCORE_PLAYING, 0.0);
            for (int s = 0; s < 8; ++s)                          // M9 sources
            {
                forceRestoredParam(imgSrcParam(s, "Play"), 0.0); // (P5-M3 ×8)
                forceRestoredParam(vidSrcParam(s, "Play"), 0.0);
            }

            // Migration — sessions saved before the per-instance insert banks
            // carry single per-type values ("luxpitchAttackMs"). All instances
            // shared them back then, so seed every bank slot with the legacy
            // value. Patch the tree BEFORE replaceState; banked entries already
            // present (new sessions) win — the legacy value is only a fallback.
            {
                std::map<juce::String, double> restored;   // id → value
                for (auto* e : xmlState->getChildWithTagNameIterator("PARAM"))
                    restored[e->getStringAttribute("id")] =
                        e->getDoubleAttribute("value");

                for (const auto& bank : kModuleParamManifest)
                {
                    if (! isPooledInsertType(bank.type))
                        continue;   // legacy per-type ids existed for the
                                    // 5 pooled inserts only (iso-behaviour)
                    for (int i = 0; i < bank.numSuffixes; ++i)
                    {
                        const auto legacyIt = restored.find(
                            juce::String(bank.bankPrefix) + bank.suffixes[i]);
                        if (legacyIt == restored.end())
                            continue;
                        for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
                        {
                            const juce::String bankId =
                                bank.paramId(s, bank.suffixes[i]);
                            if (restored.count(bankId) != 0)
                                continue;   // already saved per instance
                            auto* e = xmlState->createNewChildElement("PARAM");
                            e->setAttribute("id", bankId);
                            e->setAttribute("value", legacyIt->second);
                        }
                    }
                }
            }

            // Migration — synth-split P1: blobs saved before the per-OUT
            // conditioning banks carry the legacy global conditioning params
            // (luxstral*/luxsynth*). Seed the OUT banks from them
            // so the reloaded session sounds identical. Detected by the
            // absence of the synthSplitVersion root attribute (stamped by
            // getStateInformation since P1). The legacy params stay restored
            // (state compat) but the pipeline no longer reads them. Gamma
            // collapses enable+value → single knob (1.0 = off); Intensity
            // keeps its default (1.0 = unity send).
            const int splitVer = xmlState->getIntAttribute("synthSplitVersion", 0);
            {
                std::map<juce::String, double> restored;   // id → value
                for (auto* e : xmlState->getChildWithTagNameIterator("PARAM"))
                    restored[e->getStringAttribute("id")] =
                        e->getDoubleAttribute("value");

                auto legacy = [&restored](const char* id, double def)
                {
                    const auto it = restored.find(juce::String(id));
                    return it != restored.end() ? it->second : def;
                };
                auto seedBank = [&restored, &xmlState](const juce::String& bankId,
                                                       double v)
                {
                    if (restored.count(bankId) != 0)
                        return;   // an already-banked value always wins
                    auto* e = xmlState->createNewChildElement("PARAM");
                    e->setAttribute("id", bankId);
                    e->setAttribute("value", v);
                };

                // v3 — per-sampler-engine enable: engines B..H used to share the
                // single "luxSamplerEnabled" param (one rack LED gated every
                // present engine). Seed each engine's own enable from it so a
                // reloaded session keeps the same engines audible. Engine 0
                // already reads the legacy id, so only 1..7 need seeding.
                if (splitVer < 3)
                    for (int e = 1; e < LuxSampler::kMaxEngines; ++e)
                        seedBank(fsEngineParam(e, "Enabled"),
                                 legacy("luxSamplerEnabled", 0.0));

                if (splitVer < 1)
                {

                const double lsGamma = legacy("luxstralGammaEnable", 1.0) >= 0.5
                                     ? legacy("luxstralGammaValue", 1.0) : 1.0;
                const double rangeDb = legacy("luxstralFidelityRangeDb", 50.0);

                // LuxStral OUT slot 0 — carries the legacy global conditioning.
                seedBank(lsOutParam(0, "negative"),    legacy("luxstralInversion",   1.0));
                seedBank(lsOutParam(0, "dcBlocking"),  legacy("luxstralAcRemoval",   1.0));
                seedBank(lsOutParam(0, "gamma"),       lsGamma);
                seedBank(lsOutParam(0, "contrastMin"), legacy("luxstralContrastMin", 0.21));
                seedBank(lsOutParam(0, "rangeDb"),     rangeDb);

                // LuxSynth OUT slot 0 — and LuxWave OUT slot 0 seeds from the
                // SAME values: before its own bank, LuxWave inherited the
                // LuxSynth-conditioned line (parity of the inheritance).
                const double xInv = legacy("luxsynthInversion", 1.0);
                const double xDc  = legacy("luxsynthAcRemoval", 1.0);
                const double xGam = legacy("luxsynthGammaValue", 1.0);
                seedBank(lxOutParam(0, "negative"),   xInv);
                seedBank(lxOutParam(0, "dcBlocking"), xDc);
                seedBank(lxOutParam(0, "gamma"),      xGam);
                seedBank(lwOutParam(0, "negative"),   xInv);
                seedBank(lwOutParam(0, "dcBlocking"), xDc);
                seedBank(lwOutParam(0, "gamma"),      xGam);

                log_info("VST", "Synth-split migration: legacy conditioning "
                                "params seeded into the per-OUT banks");
                }
            }

            // replaceState fires the APVTS listeners synchronously. On the
            // message thread each restored parameter runs applyParameterChange
            // inline — silence the per-parameter log storm (the coalesced config
            // work is drained once in applyRestoredStateOnMessageThread). On a
            // loader thread the listeners defer instead, so bulkParamApply_ is
            // never read there; only touch the flag on the message thread to
            // avoid a cross-thread write.
            auto* mm = juce::MessageManager::getInstanceWithoutCreating();
            const bool onMessageThread = (mm == nullptr || mm->isThisTheMessageThread());
            // Silence the session autosave across replaceState too: its listener
            // storm (message-thread restore) would otherwise mark the session
            // dirty and re-save the very state we are loading. The guard in
            // applyRestoredStateOnMessageThread (always called right after) resets it.
            if (sessions_) sessions_->setSuppressAutosave(true);
            if (onMessageThread) bulkParamApply_ = true;
            apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
            if (onMessageThread) bulkParamApply_ = false;
            // replaceState swapped the underlying tree object — re-attach the
            // session-dirty listener to the NEW tree (old one is orphaned).
            if (sessionDirtyListener_ != nullptr)
            {
                apvts.state.removeListener(sessionDirtyListener_.get());
                apvts.state.addListener(sessionDirtyListener_.get());
            }

            // Everything below mutates non-APVTS state that UI timers iterate
            // concurrently (chainModel_, SCORE/SEQ trees, engines). Some hosts
            // call setStateInformation from a project-loading thread — apply
            // on the message thread only (replaceState above is thread-safe).
            if (mm == nullptr || mm->isThisTheMessageThread())
            {
                applyRestoredStateOnMessageThread();
            }
            else
            {
                juce::WeakReference<Sp3ctraAudioProcessor> weakThis(this);
                juce::MessageManager::callAsync([weakThis]
                {
                    if (auto* p = weakThis.get())
                        p->applyRestoredStateOnMessageThread();
                });
            }
        }
    }
}

void Sp3ctraAudioProcessor::applyRestoredStateOnMessageThread()
{
    // Silence the SessionManager autosave for the whole restore: the flood of
    // setValueNotifyingHost / model pushes below would otherwise immediately
    // re-dirty and re-save exactly what we just loaded. RAII-reset on exit.
    struct SuppressGuard {
        SessionManager* s;
        ~SuppressGuard() { if (s) s->setSuppressAutosave(false); }
    } _suppress { sessions_.get() };
    if (sessions_) sessions_->setSuppressAutosave(true);

    {
        {
            // Transport open-state is patched into the tree BEFORE replaceState
            // (see setStateInformation) — no host-visible pushes on the nominal
            // path. Clearing blanks the waterfall history so the video scroll
            // starts from a fresh (empty) waterfall and scrolls in live data.
            requestVideoScrollClear();

            // Belt-and-suspenders for blobs saved by OLDER plugin versions:
            // replaceState KEEPS the current value of any parameter absent
            // from the restored tree, so the pre-replace patch cannot reach
            // those. Fold each transport to its open-state — host-visible only
            // in this corner case (old preset loaded while something differs).
            {
                auto forceTo = [this](const juce::String& id, float openNorm)
                {
                    if (auto* p = apvts.getParameter(id))
                        if (std::abs(p->getValue() - openNorm) > 1.0e-4f)
                            p->setValueNotifyingHost(openNorm);
                };
                // VIDEO SCROLL opens RUNNING (paused = 0); audio transports STOPPED.
                forceTo("videoScrollPaused", 0.0f);
                for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
                    forceTo(vsParam(s, "paused"), 0.0f);
                for (int s = 0; s < LuxSampler::kMaxEngines; ++s)
                    forceTo(fsEngineParam(s, "SeqTransport"), 0.0f);
                forceTo(PARAM_SCORE_PLAYING, 0.0f);
                forceTo("voicePlaying",     0.0f);
                forceTo("midiScorePlaying", 0.0f);
                forceTo("timbrePlaying",    0.0f);
                for (int s = 0; s < 8; ++s)
                {
                    forceTo(imgSrcParam(s, "Play"), 0.0f);
                    forceTo(vidSrcParam(s, "Play"), 0.0f);
                }
            }
            // Push the restored transport into each family type's own slot (the
            // listener does not fire for values equal to the pre-restore state).
            // deriveChainRouting() re-primes once slots are (re)cached; this
            // covers the case where slots were already mapped before restore.
            primeScoreTransports();
            // LEGACY .sp3s pointer — read once for the one-shot bank import
            // below, then cleared forever. (samplerOutputDir was retired with
            // the project-session model: exports land in the session folder.)
            lastSessionPath = apvts.state
                .getProperty("lastSessionPath", "").toString();
            scoreWavPath = apvts.state
                .getProperty("scoreWavPath", "").toString();
            log_info("VST", "State restored from DAW project — applying to engines...");

            // On state restore, just update g_sp3ctra_config.
            // The actual pipeline start (if needed) happens in prepareToPlay().
            applyConfigurationToCore(false);
            // That was a FULL resync — drop the coalesced dirty flag raised by
            // the bulk param apply so drainPendingConfig() doesn't redo it.
            configResyncPending_ = false;

            // M9 — restore media paths + camera device BEFORE the chain model:
            // updateMediaSourcePresence() (inside deriveChainRouting) reopens
            // the camera only when a CAMERA module is placed, and needs the
            // persisted device name to be known by then.
            restoreMediaSourcesFromTree(apvts.state.getChildWithName("MEDIA_SOURCES"));
            // Push the restored source params into the engines (the listener
            // does not fire for values equal to the pre-restore state).
            for (int s = 0; s < 8; ++s)
                if (auto* eng = imageSources_[(size_t) s].get())
                {
                    eng->setPosition (apvts.getRawParameterValue(imgSrcParam(s, "Pos"))->load());
                    eng->setDurationS(apvts.getRawParameterValue(imgSrcParam(s, "Duration"))->load());
                    eng->setLoopMode ((int) apvts.getRawParameterValue(imgSrcParam(s, "Loop"))->load());
                    eng->setScanStart(apvts.getRawParameterValue(imgSrcParam(s, "ScanStart"))->load());
                    eng->setScanEnd  (apvts.getRawParameterValue(imgSrcParam(s, "ScanEnd"))->load());
                    eng->setEnabled  (apvts.getRawParameterValue(imgSrcParam(s, "Enabled"))->load() > 0.5f);
                    eng->setRotation ((int) apvts.getRawParameterValue(imgSrcParam(s, "Rotate"))->load());
                }
            for (int s = 0; s < 8; ++s)
            {
                if (auto* v = videoSources_[(size_t) s].get())
                {
                    v->setLineFrac (apvts.getRawParameterValue(vidSrcParam(s, "Line"))->load());
                    v->setSpeed    (apvts.getRawParameterValue(vidSrcParam(s, "Speed"))->load());
                    v->setLoopMode ((int) apvts.getRawParameterValue(vidSrcParam(s, "Loop"))->load());
                    v->setEnabled  (apvts.getRawParameterValue(vidSrcParam(s, "Enabled"))->load() > 0.5f);
                }
                if (auto* c = cameraSources_[(size_t) s].get())
                {
                    c->setLineFrac(apvts.getRawParameterValue(camSrcParam(s, "Line"))->load());
                    c->setEnabled (apvts.getRawParameterValue(camSrcParam(s, "Enabled"))->load() > 0.5f);
                }
            }

            // Pool-slot bindings + per-chain settings memory FIRST: the derive
            // inside loadChainModelFromState() must reuse the saved bindings
            // (the per-instance banks are keyed by slot) instead of assigning
            // fresh ones in a different order.
            restorePoolBindingsFromTree(apvts.state.getChildWithName("POOL_SLOTS"));

            // M6 Phase 2 — restore the chain topology and derive per-chain
            // routing (headless-correct; enable params are already restored).
            loadChainModelFromState();

            // J3 — legacy INSERT_MEMORY blob → the chains' own type memory
            // (one-shot migration; v3 blobs restored it with the model and
            // win). Must run AFTER the model is loaded.
            restoreInsertMemoryFromTree(apvts.state.getChildWithName("INSERT_MEMORY"));

            // J2 — chain-owned settings: project each module's VALUES onto
            // its runtime bank. Idempotent for a v3 blob (the flat PARAMs
            // restored the same values — the only-if-different guard makes it
            // a no-op); pre-v3 sessions have no VALUES and skip naturally.
            // This is the nominal path of the chain presets (J4).
            projectChainValuesToBanks();

            // SCORE settings + frequency override (processor members, not APVTS).
            restoreScoreStateFromTree(apvts.state.getChildWithName("SCORE"));

            // User timbre wavetable — rebuilds the mip tables from the
            // persisted harmonics (absent child ⇒ clears any loaded table).
            restoreLuxstralWavetableFromTree(
                apvts.state.getChildWithName("LUXSTRAL_WAVETABLE"));

            // LuxGrain grain material — re-retain the persisted file
            // (moved/deleted file ⇒ the cloud falls back to SINE material).
            {
                const auto path =
                    apvts.state.getProperty("luxgrainSamplePath", "").toString();
                luxgrainSamplePath_.clear();
                luxgrain_engine_clear_sample(&g_luxgrain_engine);
                if (path.isNotEmpty())
                {
                    juce::File f(path);
                    juce::String err;
                    if (! (f.existsAsFile() && loadLuxGrainSampleFile(f, err)))
                        log_warning("VST", "LuxGrain material '%s' unavailable "
                                           "— cloud falls back to SINE",
                                    path.toRawUTF8());
                }
            }

            // Sequencer patterns — steps are not APVTS params, only their
            // transport/timing is. Timing attrs in the tree were captured
            // together with the APVTS values, so applying both is consistent.
            // New blobs: per-engine <SEQS>. Old blobs: one GLOBAL <SEQ> whose
            // steps encoded (engine, slot) — split it across the per-engine
            // sequencers and push the migrated timing into the new per-engine
            // params (absent from the old blob, they hold defaults otherwise).
            seqRestoredFromState_ = false;
            if (auto seqsTree = apvts.state.getChildWithName("SEQS");
                seqsTree.isValid())
            {
                if (auto seqsXml = seqsTree.createXml())
                {
                    for (auto* seqXml : seqsXml->getChildWithTagNameIterator("SEQ"))
                    {
                        const int e = seqXml->getIntAttribute("idx", -1);
                        if (e < 0 || e >= LuxSampler::kMaxEngines) continue;
                        if (auto* fs = frameSequencers_[(size_t) e].get())
                        {
                            fs->loadFromXml(*seqXml);
                            seqRestoredFromState_ = true;
                        }
                    }
                }
            }
            else if (auto seqTree = apvts.state.getChildWithName("SEQ");
                     seqTree.isValid())
            {
                if (auto seqXml = seqTree.createXml())
                {
                    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
                    {
                        auto* fs = frameSequencers_[(size_t) e].get();
                        if (fs == nullptr) continue;
                        fs->loadFromLegacyGlobalXml(*seqXml, e);
                        // Keep the new per-engine timing params in sync with
                        // the migrated values (UI + host source of truth).
                        auto syncParam = [this](const juce::String& id, float denorm)
                        {
                            if (auto* p = apvts.getParameter(id))
                                p->setValueNotifyingHost(p->convertTo0to1(denorm));
                        };
                        syncParam(fsEngineParam(e, "SeqBpm"),      fs->getBpm());
                        syncParam(fsEngineParam(e, "SeqNumSteps"), (float) fs->getNumSteps());
                        syncParam(fsEngineParam(e, "SeqLoop"),     fs->isLooping() ? 1.0f : 0.0f);
                        syncParam(fsEngineParam(e, "SeqDawSync"),  fs->isDawSync() ? 1.0f : 0.0f);
                        syncParam(fsEngineParam(e, "SeqBeatsPerStep"),
                                  (float) fs->getBeatsPerStep());
                    }
                    seqRestoredFromState_ = true;
                }
            }

            // Per-slot sampler params (both engines) — the .sp3s auto-load
            // may later re-apply these on top of the freshly loaded banks.
            samplerParamsInState_ =
                apvts.state.getChildWithName("SAMPLER_SLOTS").isValid();
            applySamplerParamsFromState();

            // MIDI CC/Note → param mappings (unknown ids dropped silently).
            midiMap_.restoreFromValueTree(
                apvts.state.getChildWithName("MIDI_MAPPINGS"));

            // Recorded sampler audio. A DAW blob embeds it (SAMPLER_BANKS child);
            // a Standalone session keeps it as sidecar banks/engineN.fsmp loaded
            // by the SessionManager.
            bool banksRestored = false;
            if (auto banksTree = apvts.state.getChildWithName("SAMPLER_BANKS");
                banksTree.isValid())
            {
                restoreSamplerBanksFromTree(banksTree);
                banksRestored = true;
            }
            else if (sessions_ != nullptr && sessions_->isStandalone())
                banksRestored = sessions_->onStateRestored();

            // One-shot migration of the retired .sp3s sampler sessions: legacy
            // blobs carried the recorded audio ONLY in an external .sp3s at
            // lastSessionPath. If nothing restored the banks, import it now —
            // INSIDE this suppressed-autosave scope, so the first autosave can
            // never write an empty banks/ folder over audio still referenced
            // by the old file. Then clear the path forever: the audio now
            // lives in the session (Standalone) or the host blob (DAW).
            if (! banksRestored && lastSessionPath.isNotEmpty())
            {
                const juce::File legacy(lastSessionPath);
                if (legacy.existsAsFile()
                    && Sp3sImporter::importFile(*this, legacy))
                {
                    lastSessionPath.clear();
                    if (sessions_ != nullptr && sessions_->isStandalone())
                        sessions_->saveBanksNow();   // persist the import at once
                }
                else if (! legacy.existsAsFile())
                    log_error("VST", "Legacy .sp3s not found (%s) — sample banks "
                                     "were NOT restored", lastSessionPath.toRawUTF8());
            }
            else if (banksRestored)
                lastSessionPath.clear();   // migration done in a previous run

            if (!coreNeedsInit && sharedCore && sharedCore->isReady())
            {
                // Pipeline already running: hot-reload UDP with restored config.
                log_info("VST", "Restarting UDP with restored settings...");
                if (!sharedCore->restartUdp(
                        static_cast<int>(udpPortParam->load()),
                        getUdpAddressString().toStdString(), ""))
                {
                    log_error("VST", "Failed to restart UDP with restored config!");
                }
                log_info("VST", "UDP restarted → %s:%d",
                         getUdpAddressString().toRawUTF8(),
                         static_cast<int>(udpPortParam->load()));
            }
            // else: coreNeedsInit == true → prepareToPlay() will call startWithConfig()

            // Let an OPEN editor rebuild its rack/panels from the new model.
            if (onStateRestoredUi)
                onStateRestoredUi();

            int restoredModules = 0;
            for (const auto& ch : chainModel_.chains)
                restoredModules += (int) ch.modules.size();
            log_info("VST", "Restore complete — %d chains, %d modules, "
                            "sampler session auto-load %s",
                     (int) chainModel_.chains.size(), restoredModules,
                     lastSessionPath.isNotEmpty() ? "armed" : "none");
        }
    }

    // Apply any config resync / wavetable reinit / envelope coeff rebuild the
    // restored parameters raised (now on the message thread, core coming up).
    drainPendingConfig();
}

//==============================================================================
// Helper to build UDP address string from 4 bytes
juce::String Sp3ctraAudioProcessor::getUdpAddressString() const
{
    int b1 = (int)udpByte1Param->load();
    int b2 = (int)udpByte2Param->load();
    int b3 = (int)udpByte3Param->load();
    int b4 = (int)udpByte4Param->load();
    
    return juce::String::formatted("%d.%d.%d.%d", b1, b2, b3, b4);
}

//==============================================================================
// Parameter Change Listener — DISPATCHER.
// APVTS listeners fire synchronously on whatever thread set the value: the
// audio thread for host automation / MIDI-mapped params (CC1 → LFO depth), a
// loading thread for some hosts. The real handler below does message-thread
// work (applyConfigurationToCore ≈ 60 String lookups + logs, source routing
// under db->mutex, UDP restart with joins/sleeps) — running it on the audio
// thread caused xruns up to guaranteed dropouts. Defer via per-param dirty
// flags drained by the 30 ms timer; values are re-read at apply time.
void Sp3ctraAudioProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread())
    {
        applyParameterChange(parameterID, newValue);
        if (sessions_) sessions_->markStateDirty();   // UI-driven change → autosave
        return;
    }

    // RT path: atomics only (map lookup = String compares, no alloc/lock).
    const auto it = paramIndexById_.find(parameterID);
    if (it == paramIndexById_.end())
        return;
    paramDirty_[(size_t) it->second].store(true, std::memory_order_relaxed);
    anyParamDirty_.store(true, std::memory_order_release);
}

void Sp3ctraAudioProcessor::timerCallback()
{
    // ── Deferred parameter changes (audio/loader thread → here) ──────────────
    // A DAW project restore delivers its parameters through this drain (the
    // loader thread defers instead of applying inline). Suppress the noisy
    // per-parameter logs for the batch; the coalesced config work runs once via
    // drainPendingConfig() below.
    if (anyParamDirty_.exchange(false, std::memory_order_acq_rel))
    {
        bulkParamApply_ = true;
        int applied = 0;
        for (int i = 0; i < deferredParamIds_.size(); ++i)
            if (paramDirty_[(size_t) i].exchange(false, std::memory_order_acq_rel))
                if (auto* raw = apvts.getRawParameterValue(deferredParamIds_[i]))
                { applyParameterChange(deferredParamIds_[i], raw->load()); ++applied; }
        bulkParamApply_ = false;
        if (applied > 0)
        {
            log_debug("VST", "Applied %d deferred parameter change(s)", applied);
            if (sessions_) sessions_->markStateDirty();   // session autosave
        }
    }

    // ── Coalesced config resync / wavetable reinit / envelope coeff rebuild ──
    drainPendingConfig();

    // ── Timbre scan position (coalesced — latest value wins) ─────────────────
    if (timbreScanPending_.exchange(false, std::memory_order_acq_rel))
        luxstral_wavetable_set_position(
            timbreScanPos_.load(std::memory_order_relaxed));

    // ── Timbre playhead: advance the scan through the file (looping) ─────────
    // Runs AFTER the manual drain above, so a drag scrubs the playhead and
    // playback continues from there. Position lives module-side (not pushed
    // back into luxstralTimbrePos — the host isn't spammed with automation).
    {
        const bool play =
            apvts.getRawParameterValue("luxstralTimbreScanPlay")->load() > 0.5f;
        const float dur = luxstral_wavetable_get_duration_s();
        if (play && dur > 0.05f)
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (timbreScanLastMs_ > 0.0)
            {
                const float rate =
                    apvts.getRawParameterValue("luxstralTimbreScanRate")->load();
                float pos = luxstral_wavetable_get_position()
                          + (float)((now - timbreScanLastMs_) * 0.001)
                                * rate / dur;
                pos -= std::floor(pos);   // loop over the whole file
                luxstral_wavetable_set_position(pos);
            }
            timbreScanLastMs_ = now;
        }
        else
            timbreScanLastMs_ = 0.0;
    }

    // ── SCORE transport mirror ────────────────────────────────────────────────
    // The SCORE page is a view: it no longer force-stops the score in its
    // destructor, so with no page open somebody must still fold the engine's
    // one-shot natural end back onto the automatable scorePlaying param (the
    // param listener then runs uiStopScore — idempotent). Guard: never fold
    // while a scorePlaying change is still pending in the deferred queue —
    // an automation Play marked dirty between the drain above and this
    // mirror would otherwise be swallowed (param 1, engine not started yet).
    if (! (scorePlayingParamIdx_ >= 0
           && paramDirty_[(size_t) scorePlayingParamIdx_].load(std::memory_order_acquire)))
        if (auto* p = apvts.getParameter(PARAM_SCORE_PLAYING))
        {
            // P5-M4: the param mirrors the SCORE module's own slot (absent
            // module ⇒ nothing can play ⇒ fold to 0 too).
            auto* sc = getScoreChannel(ModuleType::Score);
            if (p->getValue() >= 0.5f && (sc == nullptr || ! sc->isScorePlaying()))
                p->setValueNotifyingHost(0.0f);
        }

    // ── Deferred Pitch/Mask/Reverb/Echo/VideoScroll pool resets (see header) ──
    if ((pendingPitchResets_ | pendingMaskResets_ | pendingReverbResets_
         | pendingEchoResets_ | pendingEqResets_ | pendingHarmoResets_
         | pendingVideoScrollInits_
         | pendingStagingResets_) != 0
        && juce::Time::getMillisecondCounter() - poolResetArmedMs_ >= 40)
    {
        for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        {
            if ((pendingPitchResets_ >> i) & 1u) lux_pitch_reset(lux_pitch_instance(i));
            if ((pendingMaskResets_  >> i) & 1u) lux_mask_reset(lux_mask_instance(i));
            if ((pendingReverbResets_ >> i) & 1u) lux_reverb_reset(lux_reverb_instance(i));
            if ((pendingEchoResets_   >> i) & 1u) lux_echo_reset(lux_echo_instance(i));
            if ((pendingEqResets_     >> i) & 1u) lux_eq_reset(lux_eq_instance(i));
            if ((pendingHarmoResets_  >> i) & 1u) lux_harmo_reset(lux_harmo_instance(i));
            if ((pendingVideoScrollInits_ >> i) & 1u)
                video_scroll_init(video_scroll_instance(i));
            if ((pendingStagingResets_ >> i) & 1u)
            {
                synth_staging_set_inactive(i);
                synth_staging_luxsynth_set_inactive(i);
                synth_staging_luxwave_set_inactive(i);
                synth_staging_luxgrain_set_inactive(i);
            }
        }
        pendingPitchResets_ = pendingMaskResets_ = pendingVideoScrollInits_ = 0;
        pendingReverbResets_ = pendingEchoResets_ = pendingEqResets_ = 0;
        pendingHarmoResets_ = 0;
        pendingStagingResets_ = 0;
    }

    // ── RT profiler: drain deferred logs (RT threads never log directly) ─────
    rt_profiler_flush_logs(&g_vst_rt_profiler);

    // ── Staging seqlock contention (mixer held instead of silencing) ─────────
    {
        static uint64_t s_holdsLogged = 0;
        static uint32_t s_holdsLogMs  = 0;
        const uint64_t holds = synth_staging_contention_holds();
        const uint32_t nowMs = juce::Time::getMillisecondCounter();
        if (holds != s_holdsLogged && nowMs - s_holdsLogMs >= 10000)
        {
            log_info("STAGING",
                     "contention holds: %llu total (+%llu) — mixer kept "
                     "previous output on a torn slot (was audible dropout)",
                     (unsigned long long) holds,
                     (unsigned long long) (holds - s_holdsLogged));
            s_holdsLogged = holds;
            s_holdsLogMs  = nowMs;
        }
    }

    // ── LuxSynth dropout diagnostic (gap + click detectors in processBlock) ──
    {
        static uint64_t s_gapsV = 0, s_gapsU = 0, s_sil = 0, s_clicks = 0;
        static uint32_t s_lastMs = 0;
        const uint64_t gapsV = g_lxDiagGapsVoiced.load(std::memory_order_relaxed);
        const uint64_t gapsU = g_lxDiagGapsUnvoiced.load(std::memory_order_relaxed);
        const uint64_t clicks = g_lxDiagClicks.load(std::memory_order_relaxed);
        const uint64_t sil   = luxsynth_feed_silence_pushes();
        const uint32_t nowMs = juce::Time::getMillisecondCounter();
        // Trigger on the GAP/CLICK counters only — silencePushes ticks steadily
        // at idle (feeder pushing silence is nominal) and made this line fire
        // every 3 s with all-zero gap fields. sil/spec stay in the payload.
        if ((gapsV != s_gapsV || gapsU != s_gapsU || clicks != s_clicks)
            && nowMs - s_lastMs >= 3000)
        {
            log_info("LXDIAG",
                     "gaps voiced=%llu (+%llu) unvoiced=%llu (+%llu) | clicks="
                     "%llu (+%llu) [last: delta=%.3f latched=%d volStep=%+.4f "
                     "voices=%d] | feed silencePushes=%llu (+%llu) specPushes="
                     "%llu | lastGap: voices=%d bins=%d maxMag=%.4f",
                     (unsigned long long) gapsV,
                     (unsigned long long) (gapsV - s_gapsV),
                     (unsigned long long) gapsU,
                     (unsigned long long) (gapsU - s_gapsU),
                     (unsigned long long) clicks,
                     (unsigned long long) (clicks - s_clicks),
                     (double) g_lxDiagClickDelta.load(std::memory_order_relaxed),
                     g_lxDiagClickLatched.load(std::memory_order_relaxed),
                     (double) g_lxDiagClickVolStep.load(std::memory_order_relaxed),
                     g_lxDiagClickVoices.load(std::memory_order_relaxed),
                     (unsigned long long) sil,
                     (unsigned long long) (sil - s_sil),
                     (unsigned long long) luxsynth_feed_spec_pushes(),
                     g_lxDiagLastVoices.load(std::memory_order_relaxed),
                     g_lxDiagLastBins.load(std::memory_order_relaxed),
                     (double) g_lxDiagLastMaxMag.load(std::memory_order_relaxed));
            s_gapsV  = gapsV;
            s_gapsU  = gapsU;
            s_sil    = sil;
            s_clicks = clicks;
            s_lastMs = nowMs;
        }
    }

    // Session autosave (Standalone only): debounced trailing-edge writes of
    // project.sp3ctra + sidecar banks. No-op in a DAW or with no dirty state.
    if (sessions_) sessions_->autosaveTick(0);
}

// Real parameter handler — message thread only (see dispatcher above).
void Sp3ctraAudioProcessor::applyParameterChange(const juce::String& parameterID, float newValue)
{
    if (!bulkParamApply_)
        log_debug("VST", "Parameter '%s' changed to %.2f", parameterID.toRawUTF8(), newValue);

    // ── PLAY transports — DAW-automatable commands relayed to the engines ────
    // Host automation may deliver these on the audio thread; every engine call
    // below is a lock-free atomic write (uiPlay/uiHold/uiStop, score setters).
    // (The per-engine sequencer transports route through the "luxSampler"
    // branch below — the sequencer is internal to its sampler.)
    // ── Per-module-type score transport (play / speed / loop / reverse) ──────
    // Route each SCORE-family module's transport to ITS OWN player slot so the
    // four generators stay independent (SCORE keeps the legacy "score*" ids).
    // Per-slot module ACTIVE (rack LED enable), decoupled from the transport.
    if (parameterID.startsWith("scoreActive"))
    {
        if (scorePlayerService_ != nullptr)
            if (auto* sc = scorePlayerService_->channel(parameterID.getTrailingIntValue()))
                sc->setScoreActive(newValue > 0.5f);
        return;
    }
    for (ModuleType t : kScoreFamily)
    {
        const auto ids = scoreXportIds(t);
        if (parameterID == ids.play)
        {
            if (auto* sc = getScoreChannel(t))
            {
                const bool wantPlay = newValue > 0.5f;
                if (wantPlay != sc->isScorePlaying())
                {
                    if (wantPlay)
                    {
                        // PLAY activates the module (never plays deactivated):
                        // flip its ACTIVE param on for persistence/host, and
                        // apply it now — the param listener runs deferred. Skip
                        // during a bulk restore: the saved ACTIVE value is being
                        // applied in the same pass and transports are forced off.
                        if (! bulkParamApply_)
                        {
                            if (auto* ap = apvts.getParameter(scoreActiveParam(sc->slot())))
                                if (ap->getValue() < 0.5f)
                                {
                                    ap->beginChangeGesture();
                                    ap->setValueNotifyingHost(1.0f);
                                    ap->endChangeGesture();
                                }
                            sc->setScoreActive(true);
                        }
                        // Same as the page button: push transport settings first.
                        sc->setScoreSpeed(apvts.getRawParameterValue(ids.speed)->load());
                        sc->setScoreLoopMode(loopModeFromIds(apvts, ids.loop, ids.reverse));
                        sc->uiPlayScore();
                    }
                    else
                        sc->uiStopScore();
                }
            }
            return;
        }
        if (parameterID == ids.speed)
        {
            if (auto* sc = getScoreChannel(t)) sc->setScoreSpeed(newValue);
            return;
        }
        if (parameterID == ids.loop || parameterID == ids.reverse)
        {
            if (auto* sc = getScoreChannel(t))
                sc->setScoreLoopMode(loopModeFromIds(apvts, ids.loop, ids.reverse));
            return;
        }
    }

    // ── M9 / P5-M3: IMAGE ×8 / VIDEO / CAMERA — every engine call is atomic ──
    if (parameterID.startsWith("imgSrc"))
    {
        for (int s = 0; s < 8; ++s)
        {
            auto* eng = imageSources_[(size_t) s].get();
            if (eng == nullptr) continue;
            if (parameterID == imgSrcParam(s, "Pos"))
            { eng->setPosition(newValue); return; }
            if (parameterID == imgSrcParam(s, "Duration"))
            { eng->setDurationS(newValue); return; }
            if (parameterID == imgSrcParam(s, "Loop"))
            { eng->setLoopMode((int) (newValue + 0.5f)); return; }
            if (parameterID == imgSrcParam(s, "ScanStart"))
            { eng->setScanStart(newValue); return; }
            if (parameterID == imgSrcParam(s, "ScanEnd"))
            { eng->setScanEnd(newValue); return; }
            if (parameterID == imgSrcParam(s, "Play"))
            { eng->setPlaying(newValue > 0.5f); return; }
            if (parameterID == imgSrcParam(s, "Enabled"))
            { eng->setEnabled(newValue > 0.5f); return; }
            if (parameterID == imgSrcParam(s, "Rotate"))
            { eng->setRotation((int) (newValue + 0.5f)); return; }
        }
        return;
    }
    if (parameterID.startsWith("vidSrc"))
    {
        for (int s = 0; s < 8; ++s)
        {
            auto* v = videoSources_[(size_t) s].get();
            if (v == nullptr) continue;
            if (parameterID == vidSrcParam(s, "Line"))
            { v->setLineFrac(newValue); return; }
            if (parameterID == vidSrcParam(s, "Speed"))
            { v->setSpeed(newValue); return; }
            if (parameterID == vidSrcParam(s, "Loop"))
            { v->setLoopMode((int) (newValue + 0.5f)); return; }
            if (parameterID == vidSrcParam(s, "Play"))
            { v->setPlaying(newValue > 0.5f); return; }
            if (parameterID == vidSrcParam(s, "Enabled"))
            { v->setEnabled(newValue > 0.5f); return; }
        }
        return;
    }
    if (parameterID.startsWith("camSrc"))
    {
        for (int s = 0; s < 8; ++s)
        {
            auto* c = cameraSources_[(size_t) s].get();
            if (c == nullptr) continue;
            if (parameterID == camSrcParam(s, "Line"))
            { c->setLineFrac(newValue); return; }
            if (parameterID == camSrcParam(s, "Enabled"))
            { c->setEnabled(newValue > 0.5f); return; }
        }
        return;
    }

    // 🔧 CRITICAL: LuxStral parameters are automatically synced to g_sp3ctra_config
    // They are read directly by the synthesis engine, NO restart needed!
    // StrokeForge parameters — same hot-reload pattern as LuxStral
    // LuxSampler parameters — update atomic config on LuxSampler
    if (parameterID.startsWith("luxSampler"))
    {
        // Per-engine banks (P6 ×8): "luxSamplerB*" = engine 1,
        // "luxSampler{N}_*" = engines 2..7, the legacy "luxSampler*" ids =
        // engine 0 (fsEngineParam). The export prefs and output dir stay
        // shared (session-level, not play params).
        int e = 0;
        if (parameterID.startsWith("luxSamplerB"))
            e = 1;
        else if (parameterID.length() > 10
                 && juce::CharacterFunctions::isDigit(parameterID[10]))
            e = juce::jlimit(0, LuxSampler::kMaxEngines - 1,
                             parameterID.substring(10).getIntValue());
        // Per-engine sequencer bank — relay to THIS engine's FrameSequencer
        // (internal to the sampler). Transport first: Play/Hold/Stop commands,
        // then the timing params as one grouped refresh.
        if (auto* fs = frameSequencers_[(size_t) e].get();
            fs != nullptr && parameterID.contains("Seq"))
        {
            if (parameterID == fsEngineParam(e, "SeqTransport"))
            {
                const int mode = static_cast<int>(newValue + 0.5f); // 0=Stop 1=Play 2=Hold
                if (mode == 1)
                {
                    if (fs->isHeld()) fs->uiResume();
                    else              fs->uiPlay();
                }
                else if (mode == 2) fs->uiHold();
                else                fs->uiStop();
                return;
            }
            fs->setBpm(
                apvts.getRawParameterValue(fsEngineParam(e, "SeqBpm"))->load());
            fs->setNumSteps(static_cast<int>(
                apvts.getRawParameterValue(fsEngineParam(e, "SeqNumSteps"))->load()));
            fs->setLooping(
                *apvts.getRawParameterValue(fsEngineParam(e, "SeqLoop")) > 0.5f);
            fs->setDawSync(
                *apvts.getRawParameterValue(fsEngineParam(e, "SeqDawSync")) > 0.5f);
            fs->setBeatsPerStep(static_cast<int>(
                apvts.getRawParameterValue(fsEngineParam(e, "SeqBeatsPerStep"))->load()));
            return;
        }
        LuxSampler* engine = samplers_[(size_t) e].get();
        if (engine != nullptr)
        {
            // Per-engine enable = model presence AND this engine's OWN enable
            // param (its rack LED / host automation). Each Sampler instance
            // toggles independently — previously all engines shared one param,
            // so a Sampler in chain 2 flipped the one in chain 1.
            engine->setEnabled(samplerPresent_[(size_t) e]
                && apvts.getRawParameterValue(fsEngineParam(e, "Enabled"))->load() > 0.5f);
            engine->setMidiChannel(
                static_cast<int>(*apvts.getRawParameterValue(fsEngineParam(e, "MidiChannel"))) + 1);
            engine->setOctaveOffset(
                static_cast<int>(*apvts.getRawParameterValue(fsEngineParam(e, "OctaveOffset"))) - 2);
            engine->setMaxDuration(*apvts.getRawParameterValue(fsEngineParam(e, "MaxDuration")));
        }
        return;
    }

    // ── "Silent config update" branches ─────────────────────────────────────
    // These only refresh g_sp3ctra_config (no socket restart, no engine call).
    // Coalesced: raise the dirty flag and let drainPendingConfig() run ONE
    // applyConfigurationToCore() per timer tick / restore, instead of one full
    // ~60-lookup resync per parameter (see header). Hot-reload triggers below
    // are likewise deferred to a single coalesced reinit / coefficient rebuild.
    bool isStrokeForgeParam = parameterID.startsWith("sf");
    if (isStrokeForgeParam) {
        configResyncPending_ = true;
        return;
    }

    // LuxSynth blob detection — independent of StrokeForge (visualizer-only params)
    if (parameterID.startsWith("lxBlob")) {
        configResyncPending_ = true;
        return;
    }

    // SPCTR blob detection — IMAGE LUXSTRAL tab (drives visualizer + StrokeForge audio)
    if (parameterID.startsWith("spctrBlob")) {
        configResyncPending_ = true;
        return;
    }

    // Timbre scan position: NOT a config value — it drives a message-thread
    // re-extraction of the timbre wavetable. Must be caught BEFORE the generic
    // startsWith("luxstral") branch below, which only marks a config resync
    // and returns (it would swallow the scan). Coalesced: the 30 ms drain
    // applies the LATEST value, one extraction per tick max.
    if (parameterID == "luxstralTimbrePos") {
        timbreScanPos_.store(newValue, std::memory_order_relaxed);
        timbreScanPending_.store(true, std::memory_order_release);
        return;
    }

    bool isLuxStralParam = parameterID.startsWith("luxstral");
    if (isLuxStralParam) {
        // Just update g_sp3ctra_config silently (no restart) — coalesced.
        configResyncPending_ = true;

        // 🔧 HOT-RELOAD: Musical parameters (tuning, root note, octaves) change frequency range
        // This triggers fade-out → regenerate → fade-in for smooth transition
        if (parameterID == "luxstralTuning" ||
            parameterID == "luxstralRootNote" ||
            parameterID == "luxstralNumOctaves") {
            if (!bulkParamApply_)
                log_info("VST", "Musical parameter changed - requesting hot-reload");
            freqReinitPending_ = true;
        }

        // 🔧 HOT-RELOAD: Physiological filter toggle requires wavetable regeneration
        // Waveform amplitudes change when equal-loudness compensation is enabled/disabled
        if (parameterID == "luxstralPhysiologicalFilter") {
            if (!bulkParamApply_)
                log_info("VST", "Physiological filter %s - requesting wavetable regeneration",
                         newValue > 0.5f ? "ENABLED" : "DISABLED");
            freqReinitPending_ = true;
        }

        // 🔧 HOT-RELOAD: Depth change requires re-weighting all wavetable gains
        if (parameterID == "luxstralPhysiologicalDepth") {
            if (!bulkParamApply_)
                log_info("VST", "Physiological depth changed to %.2f - requesting wavetable regeneration",
                         newValue);
            freqReinitPending_ = true;
        }

        // 🔧 HOT-RELOAD: Envelope parameters (Attack/Release) require coefficient update
        // Recalculates alpha_up and alpha_down_weighted for all oscillators.
        if (parameterID == "luxstralAttackMs" || parameterID == "luxstralReleaseMs") {
            if (!bulkParamApply_)
                log_info("VST", "Envelope parameter changed - updating coefficients");
            coeffUpdatePending_ = true;
        }

        // 🔧 HOT-RELOAD: Worker-thread count → rebuild the synthesis pool at the
        // next pass boundary (no app restart). The producer consumes the request
        // after the coalesced config resync has refreshed num_workers.
        if (parameterID == "luxstralNumWorkers") {
            if (!bulkParamApply_)
                log_info("VST", "Worker threads set to %d - pool rebuild requested",
                         (int) newValue);
            workerPoolRestartPending_ = true;
        }

        return;  // Done - synthesis engine will pick up changes automatically
    }
    
    // Check if UDP parameters changed (need to restart thread)
    bool needsUdpRestart = (parameterID == PARAM_UDP_PORT || 
                           parameterID == PARAM_UDP_BYTE1 ||
                           parameterID == PARAM_UDP_BYTE2 ||
                           parameterID == PARAM_UDP_BYTE3 ||
                           parameterID == PARAM_UDP_BYTE4);
    
    if (needsUdpRestart) {
        // 🔧 CRITICAL: Ignore UDP parameter changes if core not yet initialized
        // This prevents errors during APVTS state restoration at startup
        if (coreNeedsInit) {
            log_debug("VST", "UDP parameter changed (init pending) - restart deferred");
            return;  // Don't restart now, will init properly in setStateInformation/prepareToPlay
        }
        
        // 🔧 BATCH UPDATE: If we're in a batch update, just mark that restart is needed
        // The actual restart will happen once in endUdpBatchUpdate()
        if (udpBatchUpdateActive.load()) {
            udpNeedsRestart.store(true);
            log_debug("VST", "UDP parameter changed (batch mode) - restart deferred");
            return;  // Don't restart now, wait for batch completion
        }
        
        log_info("VST", "UDP parameter changed — restarting shared socket...");
        applyConfigurationToCore(false);  // update g_sp3ctra_config first

        if (sharedCore && sharedCore->isReady())
        {
            if (!sharedCore->restartUdp(
                    static_cast<int>(udpPortParam->load()),
                    getUdpAddressString().toStdString(), ""))
            {
                log_error("VST", "Failed to restart UDP with new config!");
            }
            else
            {
                log_info("VST", "UDP restarted → %s:%d",
                         getUdpAddressString().toRawUTF8(),
                         static_cast<int>(udpPortParam->load()));
            }
        }
    }
    // ── Mix balance crossfader: push derived opacities into APVTS ─────────────
    // This ensures CisVisualizerComponent (which reads APVTS directly) and the
    // C pipeline (which reads g_sp3ctra_config) both stay in sync.
    else if (parameterID == "imageMixBalance")
    {
        const float bal = newValue;
        const float liveOp = std::min(1.0f, 2.0f * bal);
        const float smpOp  = std::min(1.0f, 2.0f * (1.0f - bal));

        if (auto* p = apvts.getParameter("imageLiveOpacity"))
            p->setValueNotifyingHost(p->convertTo0to1(liveOp));
        if (auto* p = apvts.getParameter("imageSamplerOpacity"))
            p->setValueNotifyingHost(p->convertTo0to1(smpOp));

        configResyncPending_ = true;   // coalesced (see header)
        return;
    }
    else
    {
        // For other non-UDP, non-LuxStral parameters (sensor DPI, log level, visualizer mode)
        configResyncPending_ = true;   // coalesced — applied on the next drain

        // RT Profiler stays ALWAYS enabled — profiler output uses log_info (not log_debug)
        // so it is visible regardless of log level. Changing log level only affects
        // the verbose per-metric breakdown (which uses log_debug).
        if (parameterID == PARAM_LOG_LEVEL && !bulkParamApply_) {
            log_info("VST", "Log level changed - RT Profiler summary remains visible at INFO");
        }
    }
}

// Apply coalesced config work once, on the message thread. Called from the
// 30 ms timer and at the end of a state restore. Idempotent: flags are cleared
// as they are consumed, so repeat calls with nothing pending are ~free.
void Sp3ctraAudioProcessor::drainPendingConfig()
{
    if (configResyncPending_) {
        configResyncPending_ = false;
        applyConfigurationToCore(false);   // one full g_sp3ctra_config resync
    }
    if (freqReinitPending_) {
        freqReinitPending_ = false;
        // Skip while the core is still coming up: synth_IfftInit() builds the
        // wavetables from the (already restored) config at startWithConfig()
        // time — a request now would only go stale (no synth thread to process
        // it, and the fade-out it starts would never fade back in).
        if (!coreNeedsInit)
            request_frequency_reinit();    // flips an atomic; regen on synth thread
    }
    if (coeffUpdatePending_) {
        coeffUpdatePending_ = false;
        // Skip while the core is still coming up: prepareToPlay() builds the
        // envelope coefficients from config anyway, and calling this before the
        // wavetables exist only produced the "waves is NULL, skipping" warning.
        if (!coreNeedsInit)
            update_gap_limiter_coefficients();
    }
    if (workerPoolRestartPending_) {
        workerPoolRestartPending_ = false;
        // Runs AFTER the applyConfigurationToCore() above so num_workers is
        // fresh when the producer consumes the request. Skip while the core is
        // coming up: the pool is built lazily on the first synthesis pass and
        // reads num_workers from config at that point anyway.
        if (!coreNeedsInit)
            synth_request_pool_restart();
    }
}

//==============================================================================
// M6 Phase 2 — chain topology ownership (model lives here, not in the editor)
//==============================================================================
void Sp3ctraAudioProcessor::loadChainModelFromState()
{
    auto& state = apvts.state;
    auto t = state.getChildWithName(ChainModel::kChainsTag);
    if (t.isValid())
        chainModel_.fromValueTree(t);
    else
        chainModel_ = ChainModel::makeDefault();
    chainModel_.validateAndRepair();

    // VIDEO SCROLL opens RUNNING (see setStateInformation). "paused" is a
    // chain-owned VALUE, so projectChainValuesToBanks() would otherwise
    // re-apply a saved paused=1 on top of the flat-param patch and re-freeze
    // the waterfall. Normalise the transport bit to "running" in the loaded
    // model so the projection reinforces it instead of fighting it. The
    // artistic settings (speed/zoom/fade/…) are untouched.
    for (auto& ch : chainModel_.chains)
        for (auto& m : ch.modules)
            if (m.type == ModuleType::VideoScroll
                && m.values.isValid()
                && m.values.hasProperty("paused"))
                m.values.setProperty("paused", 0.0, nullptr);

    // (The SEQUENCER rack module was retired — the sequencer is internal to
    // each sampler now. Old models still carrying a "Sequencer" MODULE entry
    // simply drop it at fromValueTree(): its type id no longer resolves.)

    // Presence baseline so the enable bridge only fires on real transitions.
    chainActiveTypes_.clear();
    chainModel_.deriveActiveTypes(chainActiveTypes_);
    videoScrollSlots_.clear();
    videoScrollSlotIds_.clear();
    luxstralSends_.clear();
    for (const auto& ch : chainModel_.chains)
        for (const auto& mod : ch.modules)
        {
            if (mod.type == ModuleType::VideoScroll
                && mod.slot >= 0 && mod.slot < CHAIN_MAX_CHAINS)
            {
                videoScrollSlots_.insert(mod.slot);
                videoScrollSlotIds_[mod.slot] = mod.id;   // identity baseline (no reset on load)
            }
            if (mod.type == ModuleType::LuxStral
                && mod.slot >= 0 && mod.slot < ChainModel::kMaxEngineSends)
                luxstralSends_.insert(mod.slot);
        }

    deriveChainRouting();   // headless-correct per-synth source routing

    // Location baseline for the per-chain settings memory — a session load must
    // never snapshot/reset the freshly restored banks.
    baselineInsertLocations();
}

void Sp3ctraAudioProcessor::deriveChainRouting()
{
    // (M8: the legacy per-synth source routing is gone — the ChainPlan is the
    // single routing authority; see deriveAndPublishChainPlan.)

    // Stable MODULE-INSTANCE → pool-slot binding (keyed by instance UUID). The
    // state belongs to the module: moving it across chains carries its live
    // state; removing / reordering other chains never rebinds it. Slots whose
    // binding just changed carry stale state — reset AFTER the new plan is
    // published at the end of this function.
    const PoolStale staleSlots = updateModulePoolBindings();

    // Active Pitch/Mask/Reverb/Echo instances → MIDI fan-out + config-sync
    // mask, indexed by the INSTANCE'S pool slot (stable across edits and
    // chain moves).
    uint32_t pitchMask = 0, maskMask = 0, reverbMask = 0, echoMask = 0, eqMask = 0,
             harmoMask = 0;
    for (int c = 0; c < chainModel_.numChains(); ++c)
    {
        for (const auto& m : chainModel_.chains[(size_t) c].modules)
        {
            const int slot = poolSlotForInstance(m.id);
            if (m.type == ModuleType::Pitch)     pitchMask  |= (1u << slot);
            if (m.type == ModuleType::Mask)      maskMask   |= (1u << slot);
            if (m.type == ModuleType::Reverb)    reverbMask |= (1u << slot);
            if (m.type == ModuleType::Echo)      echoMask   |= (1u << slot);
            if (m.type == ModuleType::Equalizer) eqMask     |= (1u << slot);
            if (m.type == ModuleType::Harmonize) harmoMask  |= (1u << slot);
        }
    }
    chainPitchMask_.store(pitchMask, std::memory_order_relaxed);
    chainMaskMask_.store(maskMask,  std::memory_order_relaxed);
    chainReverbMask_.store(reverbMask, std::memory_order_relaxed);
    chainEchoMask_.store(echoMask,   std::memory_order_relaxed);
    chainEqMask_.store(eqMask,       std::memory_order_relaxed);
    chainHarmoMask_.store(harmoMask, std::memory_order_relaxed);

    // Per-instance `enabled` sync — must NOT wait for applyConfigurationToCore:
    // a pure topology change (Pitch dragged to another chain, chain removal,
    // session restore) flips no enable param, so no parameterChanged() would
    // refresh the pool configs and the moved module would stay silently
    // bypassed on its new slot until some unrelated param edit.
    {
        auto bankOn = [this](const juce::String& id)
        { return apvts.getRawParameterValue(id)->load() >= 0.5f; };
        for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
        {
            lux_pitch_instance(i)->config.enabled =
                (bankOn(lpParam(i, "Enabled")) && ((pitchMask  >> i) & 1u)) ? 1 : 0;
            lux_mask_instance(i)->config.enabled =
                (bankOn(lmParam(i, "Enabled")) && ((maskMask   >> i) & 1u)) ? 1 : 0;
            lux_reverb_instance(i)->config.enabled =
                (bankOn(rvParam(i, "Enabled")) && ((reverbMask >> i) & 1u)) ? 1 : 0;
            lux_echo_instance(i)->config.enabled =
                (bankOn(ecParam(i, "Enabled")) && ((echoMask   >> i) & 1u)) ? 1 : 0;
            lux_eq_instance(i)->config.enabled =
                (bankOn(eqParam(i, "Enabled")) && ((eqMask     >> i) & 1u)) ? 1 : 0;
            lux_harmo_instance(i)->config.enabled =
                (bankOn(hmParam(i, "Enabled")) && ((harmoMask  >> i) & 1u)) ? 1 : 0;
        }
    }

    // Per-engine sampler enable: a Sampler instance carries its engine index
    // in `slot` (0..7 since P6). An engine is enabled iff its instance is
    // present in the model AND its OWN enable param (rack LED / host automation)
    // is on — each Sampler instance toggles independently, so a Sampler in
    // chain 2 no longer flips the one in chain 1.
    std::array<bool, LuxSampler::kMaxEngines> present {};
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (m.type == ModuleType::Sampler)
                present[(size_t) juce::jlimit(
                    0, LuxSampler::kMaxEngines - 1,
                    m.slot >= 0 ? m.slot : 0)] = true;
    samplerPresent_ = present;
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
        if (samplers_[(size_t) e])
            samplers_[(size_t) e]->setEnabled(present[(size_t) e]
                && apvts.getRawParameterValue(fsEngineParam(e, "Enabled"))->load() > 0.5f);

    // (P4-M5: the "chainInsertOrder" projection is gone with the param —
    // which LuxStral consumes whenever a Sampler sits on its chain — the default
    // topology. (P4-M3: the GLOBAL insert order died with the modulated
    // build — per-chain order comes from each chain's own recipe; the
    // "chainInsertOrder" param remains as a host-visible projection only.)

    // M9 — push IMAGE/VIDEO/CAMERA module presence onto their engines so they
    // publish lines only while placed in a chain.
    updateMediaSourcePresence();

    deriveAndPublishChainPlan();   // RT-safe per-chain recipe for the synth thread

    // Reset the transient state (held voices, LFO phase — config untouched) of
    // every pool slot that just lost its Pitch/Mask instance or changed its
    // chain binding. Without this, a removed instance's held voices would
    // silently resurface when the module (or a new chain) reuses the slot.
    // DEFERRED (not immediate): the publish above only takes effect from the
    // NEXT frame — the UDP/feeder thread may still be running the CURRENT
    // frame with the OLD plan, actively writing these very pool instances.
    // The processor timer executes the reset ≥40 ms later (frames last ~1 ms).
    {
        const uint32_t lostPitch  = (prevPitchSlots_  & ~pitchMask)  | staleSlots.pitch;
        const uint32_t lostMask   = (prevMaskSlots_   & ~maskMask)   | staleSlots.mask;
        const uint32_t lostReverb = (prevReverbSlots_ & ~reverbMask) | staleSlots.reverb;
        const uint32_t lostEcho   = (prevEchoSlots_   & ~echoMask)   | staleSlots.echo;
        const uint32_t lostEq     = (prevEqSlots_     & ~eqMask)     | staleSlots.eq;
        const uint32_t lostHarmo  = (prevHarmoSlots_  & ~harmoMask)  | staleSlots.harmo;
        pendingPitchResets_  |= lostPitch;
        pendingMaskResets_   |= lostMask;
        pendingReverbResets_ |= lostReverb;
        pendingEchoResets_   |= lostEcho;
        pendingEqResets_     |= lostEq;
        pendingHarmoResets_  |= lostHarmo;
        // A slot ACTIVE in the new plan must not be reset by a pending bit
        // armed for a previous removal (remove + re-add within the 40 ms
        // window): the deferred reset would wipe — and race — the freshly
        // active instance. Trade-off: such a fast re-add skips the clean-
        // start reset (held voices may resurface — the old, benign quirk).
        pendingPitchResets_  &= ~pitchMask;
        pendingMaskResets_   &= ~maskMask;
        pendingReverbResets_ &= ~reverbMask;
        pendingEchoResets_   &= ~echoMask;
        pendingEqResets_     &= ~eqMask;
        pendingHarmoResets_  &= ~harmoMask;
        if ((lostPitch | lostMask | lostReverb | lostEcho | lostEq | lostHarmo) != 0)
            poolResetArmedMs_ = juce::Time::getMillisecondCounter();
        prevPitchSlots_  = pitchMask;
        prevMaskSlots_   = maskMask;
        prevReverbSlots_ = reverbMask;
        prevEchoSlots_   = echoMask;
        prevEqSlots_     = eqMask;
        prevHarmoSlots_  = harmoMask;
    }
}

//==============================================================================
// Stable MODULE-INSTANCE → pool-slot binding (message thread).
// Instances keep their existing slot (keyed by ModuleInstance UUID) — the
// state belongs to the MODULE, so moving it to another chain carries its live
// state along. Vanished instances release their slot; new instances take the
// lowest free slot of THEIR TYPE'S pool. Returns per-type masks of slots whose
// binding changed (released or freshly assigned) — their pool state is stale.
//==============================================================================
Sp3ctraAudioProcessor::PoolStale Sp3ctraAudioProcessor::updateModulePoolBindings()
{
    PoolStale stale;
    auto staleFor = [&stale](ModuleType t) -> uint32_t&
    {
        switch (t)
        {
            case ModuleType::Pitch:     return stale.pitch;
            case ModuleType::Mask:      return stale.mask;
            case ModuleType::Reverb:    return stale.reverb;
            case ModuleType::Equalizer: return stale.eq;
            case ModuleType::Harmonize: return stale.harmo;
            case ModuleType::Echo:
            default:                    return stale.echo;
        }
    };
    auto isPooled = [](ModuleType t)
    {
        return t == ModuleType::Pitch  || t == ModuleType::Mask
            || t == ModuleType::Reverb || t == ModuleType::Echo
            || t == ModuleType::Equalizer || t == ModuleType::Harmonize;
    };

    std::map<juce::Uuid, ModuleType> live;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (isPooled(m.type))
                live[m.id] = m.type;

    for (auto it = modulePoolSlots_.begin(); it != modulePoolSlots_.end();)
    {
        const auto liveIt = live.find(it->first);
        // Type mismatch only happens on a corrupt/hand-edited POOL_SLOTS seed —
        // drop the binding and let the instance take a fresh slot of its type.
        if (liveIt == live.end() || liveIt->second != it->second.type)
        {
            staleFor(it->second.type) |= (1u << it->second.slot); // module gone → stale
            it = modulePoolSlots_.erase(it);
        }
        else
            ++it;
    }

    // Per-type used masks over the surviving bindings.
    std::map<ModuleType, uint32_t> used;
    for (const auto& binding : modulePoolSlots_)
        used[binding.second.type] |= (1u << binding.second.slot);

    for (const auto& [id, type] : live)
    {
        if (modulePoolSlots_.count(id) != 0)
            continue;
        uint32_t& u = used[type];
        for (int s = 0; s < CHAIN_MAX_CHAINS; ++s)
            if (((u >> s) & 1u) == 0)
            {
                modulePoolSlots_[id] = { s, type };
                u                |= (1u << s);
                staleFor(type)   |= (1u << s);    // fresh binding → start clean
                break;
            }
        // The model allows at most one instance of a pooled type per chain and
        // caps chains at kMaxChains == CHAIN_MAX_CHAINS, so a free slot always
        // exists for a legal model.
        jassert(modulePoolSlots_.count(id) != 0);
    }
    return stale;
}

int Sp3ctraAudioProcessor::poolSlotForInstance(const juce::Uuid& moduleId) const noexcept
{
    const auto it = modulePoolSlots_.find(moduleId);
    return it != modulePoolSlots_.end() ? it->second.slot : 0;
}

//==============================================================================
// MIDI-follow — decode a mapped parameter id back to the module instance that
// owns it (inverse of the per-instance bank id helpers in PluginProcessor.h).
//==============================================================================
Sp3ctraAudioProcessor::ParamNavTarget
Sp3ctraAudioProcessor::navTargetForParam(const juce::String& id) const
{
    ParamNavTarget t;

    // Pooled inserts: bank slot IS the POOL slot (keyed by UUID). Inverse lookup.
    auto poolInstance = [this](ModuleType type, int slot) -> juce::Uuid
    {
        for (const auto& [uuid, b] : modulePoolSlots_)
            if (b.type == type && b.slot == slot)
                return uuid;
        return juce::Uuid::null();
    };
    // Slotted / singleton types live in the chain model; slot < 0 = first of type.
    auto chainInstance = [this](ModuleType type, int slot) -> juce::Uuid
    {
        for (const auto& ch : chainModel_.chains)
            for (const auto& mi : ch.modules)
                if (mi.type == type && (slot < 0 || mi.slot == slot))
                    return mi.id;
        return juce::Uuid::null();
    };
    // Match a banked id "<prefix><digits>_..." and pull out the slot.
    auto banked = [&id](const char* prefix, int& outSlot) -> bool
    {
        const juce::String p(prefix);
        if (! id.startsWith(p))
            return false;
        const juce::String rest = id.substring(p.length());
        const int us = rest.indexOfChar('_');
        if (us <= 0)
            return false;
        const juce::String num = rest.substring(0, us);
        if (! num.containsOnly("0123456789"))
            return false;
        outSlot = num.getIntValue();
        return true;
    };

    int slot = -1;
    // Order matters: the banked "...Out"/"luxSamplerB" families must be tested
    // before their generic "luxstral"/"luxSampler" prefixes.
    if      (banked("luxpitch",  slot)) { t.type = ModuleType::Pitch;      t.instanceId = poolInstance (t.type, slot); }
    else if (banked("luxmask",   slot)) { t.type = ModuleType::Mask;       t.instanceId = poolInstance (t.type, slot); }
    else if (banked("luxreverb", slot)) { t.type = ModuleType::Reverb;     t.instanceId = poolInstance (t.type, slot); }
    else if (banked("luxecho",   slot)) { t.type = ModuleType::Echo;       t.instanceId = poolInstance (t.type, slot); }
    else if (banked("luxeq",     slot)) { t.type = ModuleType::Equalizer;  t.instanceId = poolInstance (t.type, slot); }
    else if (banked("luxharmo",  slot)) { t.type = ModuleType::Harmonize;  t.instanceId = poolInstance (t.type, slot); }
    else if (banked("videoScroll", slot) || banked("videoMix", slot))
                                        { t.type = ModuleType::VideoScroll; t.instanceId = chainInstance(t.type, slot); }
    else if (banked("luxstralOut", slot)) { t.type = ModuleType::LuxStral; t.instanceId = chainInstance(t.type, slot); }
    else if (banked("luxsynthOut", slot)) { t.type = ModuleType::LuxSynth; t.instanceId = chainInstance(t.type, -1); }
    else if (banked("luxwaveOut",  slot)) { t.type = ModuleType::LuxWave;  t.instanceId = chainInstance(t.type, -1); }
    else if (banked("luxgrainOut", slot)) { t.type = ModuleType::LuxGrain; t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("luxSamplerB")) { t.type = ModuleType::Sampler; t.instanceId = chainInstance(t.type, 1); }
    else if (id.startsWith("luxSampler") && id.length() > 10
             && juce::CharacterFunctions::isDigit(id[10]))
                                           { t.type = ModuleType::Sampler;
                                             t.instanceId = chainInstance(t.type,
                                                 juce::jlimit(0, LuxSampler::kMaxEngines - 1,
                                                              id.substring(10).getIntValue())); }
    else if (id.startsWith("luxSampler"))  { t.type = ModuleType::Sampler; t.instanceId = chainInstance(t.type, 0); }
    // Virtual (non-APVTS) sampler targets — REC/PLAY actions and per-slot value
    // params. Their synthetic id encodes the engine as "smp:e{E}:…" (E = 0/1),
    // and the model stores that engine index in the module's slot. So a mapped
    // key that records/plays a slot follows to that sampler's page.
    else if (id.startsWith("smp:e"))       { t.type = ModuleType::Sampler;
                                             t.instanceId = chainInstance(t.type,
                                                 juce::jlimit(0, LuxSampler::kMaxEngines - 1,
                                                              id.substring(5).getIntValue())); }
    // Synth ENGINE params (own page). StrokeForge (sf*) / blob (spctr*) belong
    // to LuxStral.
    else if (id.startsWith("luxstral") || id.startsWith("sf") || id.startsWith("spctr"))
                                        { t.type = ModuleType::LuxStral; t.engineView = true; t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("luxsynth")) { t.type = ModuleType::LuxSynth; t.engineView = true; t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("luxwave"))  { t.type = ModuleType::LuxWave;  t.engineView = true; t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("luxgrain")) { t.type = ModuleType::LuxGrain; t.engineView = true; t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("score"))    { t.type = ModuleType::Score;    t.instanceId = chainInstance(t.type, -1); }
    else if (id.startsWith("timbre"))   { t.type = ModuleType::Timbre;   t.instanceId = chainInstance(t.type, -1); }
    else
        return t;   // global / source / master param — no module to navigate to

    t.valid = ! t.instanceId.isNull();
    return t;
}

//==============================================================================
// IVirtualMidiSink — NON-APVTS mapping targets: the LuxSampler per-slot play
// params (Speed/Loop/EQ floor/fades…) and the REC/PLAY/SAVE action buttons.
// virtualResolve runs on the message thread; the rest run on the audio thread
// and only touch atomics (LuxSampler setters + the per-slot action pulses).
//==============================================================================
int Sp3ctraAudioProcessor::virtualResolve(const juce::String& paramId) const
{
    return SamplerMidiTargets::resolve(paramId);
}

int Sp3ctraAudioProcessor::virtualSteps(int targetId) const noexcept
{
    return SamplerMidiTargets::steps(SamplerMidiTargets::tKind(targetId));
}

float Sp3ctraAudioProcessor::virtualRead(int targetId) const noexcept
{
    const int  e = SamplerMidiTargets::tEngine(targetId);
    LuxSampler* fs = getSampler(e);
    if (fs == nullptr) return 0.0f;
    return SamplerMidiTargets::read(*fs, SamplerMidiTargets::tSlot(targetId),
                                    SamplerMidiTargets::tKind(targetId));
}

void Sp3ctraAudioProcessor::virtualApply(int targetId, float norm01) noexcept
{
    const auto kind = SamplerMidiTargets::tKind(targetId);
    const int  e    = juce::jlimit(0, LuxSampler::kMaxEngines - 1,
                                   SamplerMidiTargets::tEngine(targetId));
    const int  s    = SamplerMidiTargets::tSlot(targetId) % kSmpSlots;

    if (SamplerMidiTargets::isAction(kind))
    {
        // Action "press" — latch a pulse for the open SlotEditor to run on the
        // message thread (uiToggleRecord / uiPlaySlot / save are non-RT).
        switch (kind)
        {
            case SamplerMidiTargets::Kind::Rec:
                if (! smpRecHeld[e][s].exchange(true, std::memory_order_acq_rel))
                    smpRecPressed[e][s].store(true, std::memory_order_release);
                break;
            case SamplerMidiTargets::Kind::Play:
                if (! smpPlayHeld[e][s].exchange(true, std::memory_order_acq_rel))
                    smpPlayPressed[e][s].store(true, std::memory_order_release);
                break;
            case SamplerMidiTargets::Kind::Save:
                smpSaveTrigger[e][s].store(true, std::memory_order_release);
                break;
            case SamplerMidiTargets::Kind::Clear:
                smpClearTrigger[e][s].store(true, std::memory_order_release);
                break;
            default: break;
        }
        return;
    }

    // EQ band — non-RT to apply (parse + LUT rebuild), so latch the normalised
    // value; the open SlotEditor drains it on the message thread.
    if (kind == SamplerMidiTargets::Kind::EqBand)
    {
        const int band = SamplerMidiTargets::tBand(targetId) % LuxSampler::kEqBands;
        smpEqPending[e][s][band].store(juce::jlimit(0.0f, 1.0f, norm01),
                                       std::memory_order_release);
        smpValueTouchWhere_.store((e << 8) | s, std::memory_order_relaxed);
        smpValueTouchGen_  .fetch_add(1u, std::memory_order_release);
        return;
    }

    // Value target — apply straight to the engine (atomic store), then flag the
    // touch so the open SlotEditor refreshes its sliders if it shows this slot.
    LuxSampler* fs = getSampler(e);
    if (fs == nullptr) return;
    SamplerMidiTargets::apply(*fs, s, kind, norm01);
    smpValueTouchWhere_.store((e << 8) | s, std::memory_order_relaxed);
    smpValueTouchGen_  .fetch_add(1u, std::memory_order_release);
}

void Sp3ctraAudioProcessor::virtualRelease(int targetId) noexcept
{
    const auto kind = SamplerMidiTargets::tKind(targetId);
    const int  e    = juce::jlimit(0, LuxSampler::kMaxEngines - 1,
                                   SamplerMidiTargets::tEngine(targetId));
    const int  s    = SamplerMidiTargets::tSlot(targetId) % kSmpSlots;

    // Momentary action "release" — mirror the press latch.
    if (kind == SamplerMidiTargets::Kind::Rec)
    {
        if (smpRecHeld[e][s].exchange(false, std::memory_order_acq_rel))
            smpRecReleased[e][s].store(true, std::memory_order_release);
    }
    else if (kind == SamplerMidiTargets::Kind::Play)
    {
        if (smpPlayHeld[e][s].exchange(false, std::memory_order_acq_rel))
            smpPlayReleased[e][s].store(true, std::memory_order_release);
    }
}

//==============================================================================
// REC / PLAY transport-button mode (per engine). Choice index 1 = "Momentary".
//==============================================================================
bool Sp3ctraAudioProcessor::samplerRecMomentary(int engine) const noexcept
{
    if (auto* v = apvts.getRawParameterValue(fsEngineParam(juce::jlimit(0, LuxSampler::kMaxEngines - 1, engine), "RecMode")))
        return v->load() > 0.5f;
    return false;
}

bool Sp3ctraAudioProcessor::samplerPlayMomentary(int engine) const noexcept
{
    if (auto* v = apvts.getRawParameterValue(fsEngineParam(juce::jlimit(0, LuxSampler::kMaxEngines - 1, engine), "PlayMode")))
        return v->load() > 0.5f;
    return false;
}

//==============================================================================
// UUID → pool-slot binding persistence. The binding keys each instance's APVTS
// param bank, so it must survive a reload: rebuilding it in a different order
// would silently swap two same-type instances' settings.
//==============================================================================
juce::ValueTree Sp3ctraAudioProcessor::poolBindingsToTree() const
{
    juce::ValueTree t("POOL_SLOTS");
    for (const auto& [uuid, b] : modulePoolSlots_)
    {
        juce::ValueTree e("BIND");
        e.setProperty("uuid", uuid.toString(), nullptr);
        e.setProperty("type", juce::String(moduleTypeId(b.type)), nullptr);
        e.setProperty("slot", b.slot, nullptr);
        t.appendChild(e, nullptr);
    }
    return t;
}

void Sp3ctraAudioProcessor::restorePoolBindingsFromTree(const juce::ValueTree& t)
{
    modulePoolSlots_.clear();
    if (! t.isValid())
        return;
    std::map<ModuleType, uint32_t> used;   // reject duplicate slots (corrupt blob)
    for (const auto& e : t)
    {
        if (! e.hasType(juce::Identifier("BIND")))
            continue;
        ModuleType type;
        if (! moduleTypeFromId(e.getProperty("type").toString(), type)
            || ! isPooledInsertType(type))
            continue;
        const int slot = (int) e.getProperty("slot", -1);
        if (slot < 0 || slot >= ChainModel::kMaxChains)
            continue;
        if ((used[type] >> slot) & 1u)
            continue;
        const juce::String u = e.getProperty("uuid").toString();
        if (u.isEmpty())
            continue;
        modulePoolSlots_[juce::Uuid(u)] = { slot, type };
        used[type] |= (1u << slot);
    }
    // updateModulePoolBindings() (next derive) prunes entries whose instance is
    // absent from the restored model and assigns slots to any unseeded one.
}

//==============================================================================
// J3 — per-chain settings memory, generalized to EVERY manifest type (pooled
// inserts, VideoScroll, OUT sends, sampler engines) and stored IN THE CHAIN
// (Chain::typeMemory — serialized with the model, carried by presets).
// Diffs the model against the previous instance locations:
//   • instance removed (or moved to another chain) → its LAST chain snapshots
//     the bank values under its type (Enabled excluded);
//   • instance added → its bank is reset to defaults (never inherit a dead
//     instance's values via slot reuse), then the hosting chain's remembered
//     settings are applied (chain inheritance), then the module starts
//     enabled. Moved/kept instances carry their settings (stable slots).
// Runs on every model edit, AFTER updateModulePoolBindings() (fresh slots).
//==============================================================================
namespace
{
    inline bool isEnableSuffix(const char* sfx)
    {
        return std::strcmp(sfx, "Enabled") == 0
            || std::strcmp(sfx, "enabled") == 0;
    }
}

int Sp3ctraAudioProcessor::bankSlotForModule(const ModuleInstance& m) const
{
    const auto* d = moduleParamManifest(m.type);
    if (d == nullptr)
        return 0;
    return isPooledInsertType(m.type)
               ? poolSlotForInstance(m.id)
               : juce::jlimit(0, d->numSlots - 1, m.slot >= 0 ? m.slot : 0);
}

void Sp3ctraAudioProcessor::updateInsertParamMemory()
{
    std::map<juce::Uuid, InsertLoc> now;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (moduleParamManifest(m.type) != nullptr)
                now[m.id] = { ch.id, m.type, bankSlotForModule(m) };

    auto chainByUuid = [this](const juce::Uuid& id) -> Chain*
    {
        for (auto& ch : chainModel_.chains)
            if (ch.id == id)
                return &ch;
        return nullptr;
    };

    // The banks of departed instances still hold their values (pool resets only
    // touch the transient C state) — snapshot them into the chain's memory.
    // (A memory of a REMOVED chain needs no cleanup: it died with the chain.)
    for (const auto& [uuid, prev] : prevInsertLoc_)
    {
        const auto it = now.find(uuid);
        if (it != now.end() && it->second.chain == prev.chain)
            continue;
        Chain* src = chainByUuid(prev.chain);
        const auto* d = moduleParamManifest(prev.type);
        if (src == nullptr || d == nullptr)
            continue;
        juce::ValueTree mem(ChainModel::kValuesTag);
        for (int i = 0; i < d->numSuffixes; ++i)
        {
            const char* sfx = d->suffixes[i];
            if (isEnableSuffix(sfx))
                continue;   // a re-added module always starts enabled
            if (auto* raw = apvts.getRawParameterValue(d->paramId(prev.slot, sfx)))
                mem.setProperty(juce::Identifier(sfx), (double) raw->load(),
                                nullptr);
        }
        src->typeMemory[prev.type] = std::move(mem);
    }

    for (const auto& [uuid, loc] : now)
    {
        if (prevInsertLoc_.count(uuid) != 0)
            continue;   // moved/kept instances carry their settings with them
        const auto* d = moduleParamManifest(loc.type);
        if (d == nullptr)
            continue;

        for (int i = 0; i < d->numSuffixes; ++i)
            if (auto* p = apvts.getParameter(d->paramId(loc.slot, d->suffixes[i])))
                if (p->getValue() != p->getDefaultValue())
                    p->setValueNotifyingHost(p->getDefaultValue());

        // Chain inheritance: the hosting chain's remembered settings.
        if (Chain* host = chainByUuid(loc.chain))
        {
            const auto memIt = host->typeMemory.find(loc.type);
            if (memIt != host->typeMemory.end() && memIt->second.isValid())
                for (int i = 0; i < d->numSuffixes; ++i)
                {
                    const juce::Identifier sfx(d->suffixes[i]);
                    if (! memIt->second.hasProperty(sfx))
                        continue;
                    if (auto* p = apvts.getParameter(
                            d->paramId(loc.slot, d->suffixes[i])))
                        p->setValueNotifyingHost(p->convertTo0to1(
                            (float) (double) memIt->second.getProperty(sfx)));
                }
        }

        // Newly placed ⇒ enabled (same semantics as the former type-level
        // enable bridge and the VideoScroll slot forcing).
        for (const char* en : { "Enabled", "enabled" })
            if (auto* p = apvts.getParameter(d->paramId(loc.slot, en)))
            {
                if (p->getValue() < 0.5f)
                    p->setValueNotifyingHost(1.0f);
                break;
            }
    }

    prevInsertLoc_ = std::move(now);
}

void Sp3ctraAudioProcessor::baselineInsertLocations()
{
    // Session load: the banks were restored WITH their instances — record the
    // locations without snapshotting or resetting anything.
    prevInsertLoc_.clear();
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (moduleParamManifest(m.type) != nullptr)
                prevInsertLoc_[m.id] = { ch.id, m.type, bankSlotForModule(m) };
}

void Sp3ctraAudioProcessor::restoreInsertMemoryFromTree(const juce::ValueTree& t)
{
    // J3 — one-shot MIGRATION of the legacy INSERT_MEMORY blob into the
    // chains' own type memory (Chain::typeMemory). Only fills chains that
    // carry no memory of that type yet (a v3+ blob already restored it via
    // the CHAIN/MEMORY children). Call AFTER loadChainModelFromState().
    if (! t.isValid())
        return;
    for (const auto& e : t)
    {
        if (! e.hasType(juce::Identifier("MEM")))
            continue;
        ModuleType type;
        if (! moduleTypeFromId(e.getProperty("type").toString(), type))
            continue;
        const auto* d = moduleParamManifest(type);
        const juce::String chainId = e.getProperty("chain").toString();
        if (d == nullptr || chainId.isEmpty())
            continue;
        for (auto& ch : chainModel_.chains)
        {
            if (ch.id.toString() != chainId)
                continue;
            if (ch.typeMemory.count(type) != 0)
                break;   // v3 memory wins
            juce::ValueTree mem(ChainModel::kValuesTag);
            for (int i = 0; i < d->numSuffixes; ++i)
            {
                const juce::Identifier sfx(d->suffixes[i]);
                if (e.hasProperty(sfx))
                    mem.setProperty(sfx, e.getProperty(sfx), nullptr);
            }
            if (mem.getNumProperties() > 0)
                ch.typeMemory[type] = std::move(mem);
            break;
        }
    }
}

//==============================================================================
// Contextual visualizer — the selected module drives the plan's selection tap.
//==============================================================================
void Sp3ctraAudioProcessor::setVisualizerTapModule(const juce::Uuid& moduleId)
{
    if (vizTapModuleId_ == moduleId)
        return;
    vizTapModuleId_ = moduleId;

    // Blank the tap first: if the new target's chain is silent/unfed, the view
    // must show white — never the PREVIOUS selection's last frame.
    if (auto* core = getSp3ctraCore())
        if (auto* ab = core->getAudioImageBuffers())
            audio_image_buffers_clear_selection_tap(ab);

    deriveAndPublishChainPlan();   // republish with the new viz-tap position
}

//==============================================================================
// Derive the per-synth chain recipe from the model and publish it RT-safely.
// Each synth (≤1 chain) gets: its source kind, the ordered Pitch/Mask inserts
// upstream of it (each bound to its chain's per-instance state pool slot), and
// whether a sampler/score sits upstream. Consumed by the synthesis thread.
//==============================================================================
void Sp3ctraAudioProcessor::deriveAndPublishChainPlan()
{
    ChainPlan plan;
    memset(&plan, 0, sizeof(plan));

    // Only modules UPSTREAM of the synth count as its source: "order is
    // significant" (ChainModel.h) — a source dragged BELOW the synth used to
    // feed it anyway, contradicting how Pitch/Mask below the synth are ignored.
    // P5-M1: kind + the source INSTANCE's slot (media pools). SP3CTRA/none → 0.
    auto sourceKind = [](const Chain& ch, int limit, int* slot_out) -> int
    {
        *slot_out = 0;
        for (int i = 0; i < limit && i < (int) ch.modules.size(); ++i)
        {
            const auto& m = ch.modules[(size_t) i];
            if (m.type == ModuleType::Sp3ctra) return CHAIN_SRC_LIVE;
            if (m.type == ModuleType::Image || m.type == ModuleType::Video
                || m.type == ModuleType::Camera)
            {
                *slot_out = juce::jlimit(0, ChainModel::kMaxMediaSlots - 1,
                                         m.slot >= 0 ? m.slot : 0);
                return m.type == ModuleType::Image ? CHAIN_SRC_IMAGE
                     : m.type == ModuleType::Video ? CHAIN_SRC_VIDEO
                                                   : CHAIN_SRC_CAMERA;
            }
        }
        return CHAIN_SRC_NONE;
    };

    // Shared recipe builder: source + ordered inserts of ONE chain, up to (not
    // including) `limitIdx`. Used both for synth chains (limit = synth index)
    // and probe-only chains (limit = whole chain).
    auto fillFromChain = [&](SynthChainPlan& sp, int chainIdx, int limitIdx)
    {
        const auto& ch = chainModel_.chains[(size_t) chainIdx];
        sp.present        = 1;
        sp.source_kind    = sourceKind(ch, limitIdx, &sp.source_slot);
        sp.viz_tap_insert = -1;   // set below when this chain hosts the selection

        for (int i = 0; i < limitIdx && i < (int) ch.modules.size(); ++i)
        {
            const ModuleInstance& mi = ch.modules[(size_t) i];
            const ModuleType t = mi.type;
            if ((t == ModuleType::Pitch || t == ModuleType::Mask
                 || t == ModuleType::Reverb || t == ModuleType::Echo
                 || t == ModuleType::Equalizer || t == ModuleType::Harmonize)
                && sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
            {
                sp.insert_id[sp.num_inserts] =
                      (t == ModuleType::Pitch) ? IMAGE_CHAIN_INSERT_LUXPITCH
                    : (t == ModuleType::Mask)  ? IMAGE_CHAIN_INSERT_LUXMASK
                    : (t == ModuleType::Reverb)? IMAGE_CHAIN_INSERT_LUXREVERB
                    : (t == ModuleType::Echo)  ? IMAGE_CHAIN_INSERT_LUXECHO
                    : (t == ModuleType::Harmonize) ? IMAGE_CHAIN_INSERT_LUXHARMO
                    :                            IMAGE_CHAIN_INSERT_LUXEQ;
                // Pool slot bound to THIS INSTANCE's UUID — stable across edits
                // and chain moves (must match deriveChainRouting's masks).
                sp.insert_state_idx[sp.num_inserts] = poolSlotForInstance(mi.id);
                sp.num_inserts++;
            }
            else if (t == ModuleType::VideoScroll
                     && mi.slot >= 0 && mi.slot < CHAIN_MAX_CHAINS
                     && sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
            {
                // PASS-THROUGH PROBE: insert_state_idx is the PER-INSTANCE slot (0..7).
                sp.insert_id[sp.num_inserts]        = IMAGE_CHAIN_INSERT_VIDEOSCROLL;
                sp.insert_state_idx[sp.num_inserts] = mi.slot;
                sp.num_inserts++;
            }
            else if ((t == ModuleType::LuxStral || t == ModuleType::LuxSynth
                      || t == ModuleType::LuxWave || t == ModuleType::LuxGrain)
                     && sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
            {
                // OUT SEND MARKER (M3/M6) — pass-through; locates the send so
                // the chain executor taps the stream at its position.
                // insert_state_idx = the send's conditioning-bank slot
                // (ModuleInstance.slot, per-type pools).
                sp.insert_id[sp.num_inserts] =
                      (t == ModuleType::LuxStral) ? IMAGE_CHAIN_INSERT_OUT_LUXSTRAL
                    : (t == ModuleType::LuxSynth) ? IMAGE_CHAIN_INSERT_OUT_LUXSYNTH
                    : (t == ModuleType::LuxWave)  ? IMAGE_CHAIN_INSERT_OUT_LUXWAVE
                    :                               IMAGE_CHAIN_INSERT_OUT_LUXGRAIN;
                sp.insert_state_idx[sp.num_inserts] =
                    juce::jlimit(0, CHAIN_MAX_CHAINS - 1,
                                 mi.slot >= 0 ? mi.slot : 0);
                sp.num_inserts++;
            }
            else if (t == ModuleType::Sampler)
            {
                sp.has_sampler = 1;
                // Record the sampler's POSITION in the insert list so the executor
                // can feed VideoScroll probes pre- vs post-sampler correctly.
                // insert_state_idx = the instance's ENGINE slot (A=0/B=1) — the
                // per-chain feed records the chain's stream into THAT engine.
                if (sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
                {
                    sp.insert_id[sp.num_inserts]        = IMAGE_CHAIN_INSERT_SAMPLER;
                    sp.insert_state_idx[sp.num_inserts] =
                        juce::jlimit(0, ChainModel::kMaxSamplerEngines - 1,
                                     mi.slot >= 0 ? mi.slot : 0);
                    sp.num_inserts++;
                }
            }
            else if (isScoreFamily(t))
            {
                // P5-M4: one marker PER INSTANCE (the shared score channel is
                // gone) — each SCORE-family module records its POSITION (like
                // the sampler marker) and ITS pool slot in insert_state_idx,
                // so the executor gates + split point resolve per slot
                // (chain_player_owned / chain_hosts_driving_score). A chain
                // may host up to one module of each family type → up to four
                // markers, each an independent player.
                sp.has_score = 1;
                if (sp.num_inserts < CHAIN_PLAN_MAX_INSERTS)
                {
                    sp.insert_id[sp.num_inserts]        = IMAGE_CHAIN_INSERT_SCORE;
                    sp.insert_state_idx[sp.num_inserts] =
                        juce::jlimit(0, ChainModel::kMaxScorePlayers - 1,
                                     mi.slot >= 0 ? mi.slot : 0);
                    sp.num_inserts++;
                }
            }

            // Contextual visualizer: the SELECTED module's output position in
            // this chain — "after the inserts pushed so far" (a source module
            // or a pass-through marker maps to the frame at its position).
            if (mi.id == vizTapModuleId_)
                sp.viz_tap_insert = sp.num_inserts;
        }
    };

    // (P4-M4) plan.synth[] is gone — chain[] and ls_send[] are the only
    // recipes; every consumer (executors, players, gates) is plan-driven.

    // Synth-split P3 — LuxStral SENDS: every "→ LUXSTRAL" OUT across all
    // chains becomes one ls_send entry (recipe compiled up to the OUT's
    // position, bank = the instance's slot). The audio-thread mixer blends
    // every staged send into the single engine feed; the legacy synth[A]
    // entry above stays filled for visualizer compatibility only.
    for (int c = 0; c < chainModel_.numChains()
                    && plan.num_ls_sends < CHAIN_MAX_CHAINS; ++c)
    {
        const auto& mods = chainModel_.chains[(size_t) c].modules;
        for (int i = 0; i < (int) mods.size()
                        && plan.num_ls_sends < CHAIN_MAX_CHAINS; ++i)
        {
            const auto& mi = mods[(size_t) i];
            if (mi.type != ModuleType::LuxStral)
                continue;
            LsSendPlan& snd = plan.ls_send[plan.num_ls_sends];
            snd.chain_idx = c;
            snd.bank_slot = juce::jlimit(0, CHAIN_MAX_CHAINS - 1,
                                         mi.slot >= 0 ? mi.slot : 0);
            fillFromChain(snd.recipe, c, i);
            if (mi.id == vizTapModuleId_)
                snd.recipe.viz_tap_insert = snd.recipe.num_inserts;
            ++plan.num_ls_sends;
        }
    }

    // M3 — uniform per-chain recipes: chain[i] mirrors model chain i. A chain
    // is executed (present=1) when something observes its stream: an OUT send
    // (LuxStral staging / Path-B feed), a VideoScroll probe, or the SELECTED
    // module (so zone 1 can show ANY module's stream, even in an otherwise
    // inert chain). Probes capture and OUT markers tap AT THEIR POSITION.
    plan.num_chains = chainModel_.numChains();
    for (int c = 0; c < plan.num_chains && c < CHAIN_MAX_CHAINS; ++c)
    {
        const auto& ch = chainModel_.chains[(size_t) c];
        bool hasOut = false, hasProbe = false, hasVizTarget = false,
             hasPlayer = false;
        for (const auto& m : ch.modules)
        {
            if (m.type == ModuleType::LuxStral || m.type == ModuleType::LuxSynth
                || m.type == ModuleType::LuxWave || m.type == ModuleType::LuxGrain)
                hasOut = true;
            if (m.type == ModuleType::VideoScroll
                && m.slot >= 0 && m.slot < CHAIN_MAX_CHAINS)
                hasProbe = true;
            if (m.type == ModuleType::Sampler || isScoreFamily(m.type))
                hasPlayer = true;   // mod-bus owner candidate (REC/relay hooks)
            if (m.id == vizTapModuleId_)
                hasVizTarget = true;   // selection tap lives in this chain
        }
        if (! (hasOut || hasProbe || hasVizTarget || hasPlayer))
            continue;   // present stays 0 — nothing observes this chain

        fillFromChain(plan.chain[c], c, (int) ch.modules.size());
    }

    // Deferred staging reset: a chain slot that LOST its "→ LUXSTRAL" or
    // "→ LUXSYNTH" send (module removed / chain deleted / reorder) must stop
    // contributing to the mixes — the producers no longer iterate it, so its
    // last staged frame would linger (the plan-gated mixers already ignore
    // it; the reset guards against chain-index reuse). Reset ≥40 ms later
    // (pool-reset pattern: the in-flight frame may still write under the OLD
    // plan).
    {
        uint32_t sendChains = 0;
        for (int k = 0; k < plan.num_ls_sends; ++k)
            sendChains |= (1u << plan.ls_send[k].chain_idx);
        for (int c = 0; c < plan.num_chains && c < CHAIN_MAX_CHAINS; ++c)
            for (int i = 0; i < plan.chain[c].num_inserts; ++i)
                if (plan.chain[c].insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXSYNTH
                    || plan.chain[c].insert_id[i] == IMAGE_CHAIN_INSERT_OUT_LUXWAVE)
                    sendChains |= (1u << c);
        pendingStagingResets_ |= (prevLsSendChains_ & ~sendChains);
        prevLsSendChains_ = sendChains;
        if (pendingStagingResets_ != 0)
            poolResetArmedMs_ = juce::Time::getMillisecondCounter();
    }

    // Multi-chain split (per PAIR since P6): publish, for every engine, the
    // set of engines sharing at least one chain with it. Gates the playback
    // arbiter — on split chains engines play independently; on a shared
    // chain (one stream) starting one still evicts the sharing ones.
    {
        uint8_t masks[LuxSampler::kMaxEngines] = {};
        for (int c = 0; c < plan.num_chains; ++c)
        {
            const SynthChainPlan& sp = plan.chain[c];
            if (!sp.present || !sp.has_sampler) continue;
            uint8_t here = 0;
            for (int i = 0; i < sp.num_inserts; ++i)
                if (sp.insert_id[i] == IMAGE_CHAIN_INSERT_SAMPLER)
                    here |= (uint8_t) (1u << juce::jlimit(
                        0, LuxSampler::kMaxEngines - 1, sp.insert_state_idx[i]));
            for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
                if ((here >> e) & 1u)
                    masks[e] |= (uint8_t) (here & ~(1u << e));
        }
        LuxSampler::setEngineShareMasks(masks);
    }

    // ── P5-M4 — score-family instance map ────────────────────────────────────
    // Cache the FIRST placed pool slot per family type (getScoreChannel: each
    // generator tab drives its own type's player) and diff the present slots:
    // a removed instance's frames are discarded HERE, per slot (replaces the
    // old "free the shared channel when the LAST family member leaves").
    {
        int famSlot[4] = { -1, -1, -1, -1 };
        uint8_t present = 0;
        for (const auto& ch : chainModel_.chains)
            for (const auto& m : ch.modules)
            {
                if (! isScoreFamily(m.type)) continue;
                const int slot = juce::jlimit(0, ChainModel::kMaxScorePlayers - 1,
                                              m.slot >= 0 ? m.slot : 0);
                present |= (uint8_t) (1u << slot);
                for (int f = 0; f < 4; ++f)
                    if (kScoreFamily[f] == m.type && famSlot[f] < 0)
                        famSlot[f] = slot;
            }
        for (int f = 0; f < 4; ++f)
            scoreFamilySlot_[f].store(famSlot[f], std::memory_order_release);

        const uint8_t gone = (uint8_t) (scoreSlotsPresentMask_ & ~present);
        scoreSlotsPresentMask_ = present;
        if (gone != 0 && scorePlayerService_ != nullptr)
            for (int s = 0; s < ScorePlayerService::kMaxSlots; ++s)
                if ((gone >> s) & 1u)
                    scorePlayerService_->discard(s);

        // Slots just (re)mapped: push each family type's transport params into
        // its own slot (the atomic loopMode default is NONE but the params
        // default to Loop=on — a freshly placed generator needs this).
        primeScoreTransports();
    }

    // ── AUDIO MIX / zero-CPU contract — per-engine OUT send counts ──────────
    // 0 sends → processBlock skips that engine's render entirely and the
    // AUDIO MIX panel hides its strip. (LuxStral: matches plan.num_ls_sends.)
    // The slot masks feed the enabled-aware render gates: presence (counts,
    // UI-facing) stays distinct from "actually feeding" (mask ∧ bank enabled).
    {
        int      n[4]    = { 0, 0, 0, 0 };
        uint32_t mask[4] = { 0, 0, 0, 0 };
        for (const auto& ch : chainModel_.chains)
            for (const auto& m : ch.modules)
            {
                int e = -1;
                if      (m.type == ModuleType::LuxStral) e = 0;
                else if (m.type == ModuleType::LuxSynth) e = 1;
                else if (m.type == ModuleType::LuxWave)  e = 2;
                else if (m.type == ModuleType::LuxGrain) e = 3;
                if (e < 0) continue;
                ++n[e];
                if (m.slot >= 0 && m.slot < ChainModel::kMaxEngineSends)
                    mask[e] |= 1u << m.slot;
            }
        sendCountLuxStral_.store(n[0], std::memory_order_relaxed);
        sendCountLuxSynth_.store(n[1], std::memory_order_relaxed);
        sendCountLuxWave_ .store(n[2], std::memory_order_relaxed);
        sendCountLuxGrain_.store(n[3], std::memory_order_relaxed);
        sendSlotsLuxStral_.store(mask[0], std::memory_order_relaxed);
        sendSlotsLuxSynth_.store(mask[1], std::memory_order_relaxed);
        sendSlotsLuxWave_ .store(mask[2], std::memory_order_relaxed);
        sendSlotsLuxGrain_.store(mask[3], std::memory_order_relaxed);
    }

    chain_plan_publish(&plan);
}

void Sp3ctraAudioProcessor::primeScoreTransports()
{
    if (scorePlayerService_ == nullptr)
        return;
    for (ModuleType t : kScoreFamily)
        if (auto* sc = getScoreChannel(t))
        {
            const auto ids = scoreXportIds(t);
            sc->setScoreSpeed(apvts.getRawParameterValue(ids.speed)->load());
            sc->setScoreLoopMode(loopModeFromIds(apvts, ids.loop, ids.reverse));
        }
    // Per-slot module ACTIVE state persists (unlike the play transports): push
    // each restored enable into its pool slot so a saved-deactivated module
    // opens deactivated. Transports are already forced STOPPED, so no resume
    // is armed here.
    for (int s = 0; s < ScorePlayerService::kMaxSlots; ++s)
        if (auto* sc = scorePlayerService_->channel(s))
            sc->setScoreActive(apvts.getRawParameterValue(scoreActiveParam(s))->load() >= 0.5f);
}

ScoreChannel* Sp3ctraAudioProcessor::getScoreChannel(ModuleType t) noexcept
{
    if (scorePlayerService_ == nullptr)
        return nullptr;
    for (int f = 0; f < 4; ++f)
        if (kScoreFamily[f] == t)
        {
            const int slot = scoreFamilySlot_[f].load(std::memory_order_acquire);
            return slot >= 0 ? scorePlayerService_->channel(slot) : nullptr;
        }
    return nullptr;
}

std::vector<int> Sp3ctraAudioProcessor::activeVideoSlots() const
{
    std::vector<int> out;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (m.type == ModuleType::VideoScroll && m.slot >= 0)
                out.push_back(m.slot);
    std::sort(out.begin(), out.end());
    return out;
}

//==============================================================================
// J2 — the chain OWNS its modules' settings (chantier « chain porteuse »).
// snapshotBankValuesIntoModel: runtime banks → each ModuleInstance.values
// (called at save time; atomic reads, any thread). projectChainValuesToBanks:
// ModuleInstance.values → runtime banks (load/preset time; MESSAGE THREAD
// ONLY, guarded "only if different" so reopening a project never marks host
// automation lanes as touched).
//==============================================================================
void Sp3ctraAudioProcessor::snapshotBankValuesIntoModel()
{
    for (auto& ch : chainModel_.chains)
        for (auto& m : ch.modules)
        {
            const auto* d = moduleParamManifest(m.type);
            if (d == nullptr)
                continue;
            const int slot = isPooledInsertType(m.type)
                                 ? poolSlotForInstance(m.id)
                                 : juce::jlimit(0, d->numSlots - 1,
                                                m.slot >= 0 ? m.slot : 0);
            juce::ValueTree values(ChainModel::kValuesTag);
            for (int i = 0; i < d->numSuffixes; ++i)
            {
                if (auto* raw = apvts.getRawParameterValue(
                        d->paramId(slot, d->suffixes[i])))
                    values.setProperty(juce::Identifier(d->suffixes[i]),
                                       (double) raw->load(), nullptr);
            }
            m.values = std::move(values);
        }
}

void Sp3ctraAudioProcessor::projectChainValuesToBanks()
{
    JUCE_ASSERT_MESSAGE_THREAD
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
        {
            if (! m.values.isValid())
                continue;
            const auto* d = moduleParamManifest(m.type);
            if (d == nullptr)
                continue;
            const int slot = isPooledInsertType(m.type)
                                 ? poolSlotForInstance(m.id)
                                 : juce::jlimit(0, d->numSlots - 1,
                                                m.slot >= 0 ? m.slot : 0);
            for (int i = 0; i < d->numSuffixes; ++i)
            {
                const juce::Identifier key(d->suffixes[i]);
                if (! m.values.hasProperty(key))
                    continue;
                const float target = (float) (double) m.values.getProperty(key);
                const juce::String id = d->paramId(slot, d->suffixes[i]);
                auto* param = apvts.getParameter(id);
                auto* raw   = apvts.getRawParameterValue(id);
                if (param == nullptr || raw == nullptr)
                    continue;
                if (std::abs(raw->load() - target) < 1.0e-6f)
                    continue;   // identical → never touch the host lane
                param->setValueNotifyingHost(
                    param->convertTo0to1(target));
            }
        }
}

void Sp3ctraAudioProcessor::persistChainModel()
{
    auto& state = apvts.state;
    auto existing = state.getChildWithName(ChainModel::kChainsTag);
    if (existing.isValid())
        state.removeChild(existing, nullptr);
    state.appendChild(chainModel_.toValueTree(), nullptr);
    // Rack topology edits are session content — schedule an autosave.
    if (sessions_) sessions_->markStateDirty();
}

//==============================================================================
// Non-APVTS module state ↔ session blob (SCORE / SEQ / SAMPLER_SLOTS)
//==============================================================================
juce::ValueTree Sp3ctraAudioProcessor::scoreStateToTree() const
{
    juce::ValueTree t("SCORE");
    const auto& s = scoreSettings_;
    t.setProperty("dynamicRangeDB",      s.dynamicRangeDB,      nullptr);
    t.setProperty("gammaCorrection",     s.gammaCorrection,     nullptr);
    t.setProperty("contrastFactor",      s.contrastFactor,      nullptr);
    t.setProperty("enableDithering",     s.enableDithering,     nullptr);
    t.setProperty("binsPerSecond",       s.binsPerSecond,       nullptr);
    t.setProperty("overlapPreset",       s.overlapPreset,       nullptr);
    t.setProperty("printerDpi",          s.printerDpi,          nullptr);
    t.setProperty("pageFormat",          s.pageFormat,          nullptr);
    t.setProperty("writingSpeed",        s.writingSpeed,        nullptr);
    t.setProperty("spectroHeightMM",     s.spectroHeightMM,     nullptr);
    t.setProperty("spectroHeightManual", s.spectroHeightManual, nullptr);
    t.setProperty("bottomMarginMM",      s.bottomMarginMM,      nullptr);
    t.setProperty("enableHighBoost",     s.enableHighBoost,     nullptr);
    t.setProperty("highBoostAlpha",      s.highBoostAlpha,      nullptr);
    t.setProperty("enableNoiseGate",     s.enableNoiseGate,     nullptr);
    t.setProperty("noiseGateThreshold",  s.noiseGateThreshold,  nullptr);
    t.setProperty("enableHighPassFilter",s.enableHighPassFilter,nullptr);
    t.setProperty("highPassCutoffFreq",  s.highPassCutoffFreq,  nullptr);
    t.setProperty("highPassFilterOrder", s.highPassFilterOrder, nullptr);
    t.setProperty("enableNormalization", s.enableNormalization, nullptr);
    t.setProperty("fftSize",             s.fftSize,             nullptr);
    t.setProperty("startTimeSec",        s.startTimeSec,        nullptr);
    t.setProperty("selectionSec",        s.selectionSec,        nullptr);
    t.setProperty("enableStereoMode",    s.enableStereoMode,    nullptr);
    t.setProperty("enableMultiRes",      s.enableMultiRes,      nullptr);
    // minFreq/maxFreq are recomputed at GENERATE time from the musical range —
    // persisting them would only freeze stale values; deliberately omitted.

    t.setProperty("ovManual",  scoreFreq_.manual,    nullptr);
    t.setProperty("ovTuning",  scoreFreq_.tuning,    nullptr);
    t.setProperty("ovRoot",    scoreFreq_.rootIndex, nullptr);
    t.setProperty("ovOctaves", scoreFreq_.octaves,   nullptr);
    return t;
}

void Sp3ctraAudioProcessor::restoreScoreStateFromTree(const juce::ValueTree& t)
{
    if (! t.isValid())
        return;
    auto& s = scoreSettings_;   // defaults (constructor) fill missing props
    s.dynamicRangeDB       = (double) t.getProperty("dynamicRangeDB",      s.dynamicRangeDB);
    s.gammaCorrection      = (double) t.getProperty("gammaCorrection",     s.gammaCorrection);
    s.contrastFactor       = (double) t.getProperty("contrastFactor",      s.contrastFactor);
    s.enableDithering      = (int)    t.getProperty("enableDithering",     s.enableDithering);
    s.binsPerSecond        = (double) t.getProperty("binsPerSecond",       s.binsPerSecond);
    s.overlapPreset        = (int)    t.getProperty("overlapPreset",       s.overlapPreset);
    s.printerDpi           = (double) t.getProperty("printerDpi",          s.printerDpi);
    s.pageFormat           = (int)    t.getProperty("pageFormat",          s.pageFormat);
    s.writingSpeed         = (double) t.getProperty("writingSpeed",        s.writingSpeed);
    s.spectroHeightMM      = (double) t.getProperty("spectroHeightMM",     s.spectroHeightMM);
    s.spectroHeightManual  = (int)    t.getProperty("spectroHeightManual", s.spectroHeightManual);
    s.bottomMarginMM       = (double) t.getProperty("bottomMarginMM",      s.bottomMarginMM);
    s.enableHighBoost      = (int)    t.getProperty("enableHighBoost",     s.enableHighBoost);
    s.highBoostAlpha       = (double) t.getProperty("highBoostAlpha",      s.highBoostAlpha);
    s.enableNoiseGate      = (int)    t.getProperty("enableNoiseGate",     s.enableNoiseGate);
    s.noiseGateThreshold   = (double) t.getProperty("noiseGateThreshold",  s.noiseGateThreshold);
    s.enableHighPassFilter = (int)    t.getProperty("enableHighPassFilter",s.enableHighPassFilter);
    s.highPassCutoffFreq   = (double) t.getProperty("highPassCutoffFreq",  s.highPassCutoffFreq);
    s.highPassFilterOrder  = (int)    t.getProperty("highPassFilterOrder", s.highPassFilterOrder);
    s.enableNormalization  = (int)    t.getProperty("enableNormalization", s.enableNormalization);
    s.fftSize              = (int)    t.getProperty("fftSize",             s.fftSize);
    s.startTimeSec         = (double) t.getProperty("startTimeSec",        s.startTimeSec);
    s.selectionSec         = (double) t.getProperty("selectionSec",        s.selectionSec);
    s.enableStereoMode     = (int)    t.getProperty("enableStereoMode",    s.enableStereoMode);
    s.enableMultiRes       = (int)    t.getProperty("enableMultiRes",      s.enableMultiRes);
    if (! s.spectroHeightManual)
        s.spectroHeightMM = SCORE_CIS_HEIGHT_MM;   // keep the lock invariant

    scoreFreq_.manual    = (bool)   t.getProperty("ovManual",  scoreFreq_.manual);
    scoreFreq_.tuning    = (double) t.getProperty("ovTuning",  scoreFreq_.tuning);
    scoreFreq_.rootIndex = (int)    t.getProperty("ovRoot",    scoreFreq_.rootIndex);
    scoreFreq_.octaves   = (int)    t.getProperty("ovOctaves", scoreFreq_.octaves);
}

// User timbre wavetable (tuned grains): persist the harmonic coefficients the
// mip tables were built from — ~2 KB, and the session survives a moved or
// deleted source WAV (the tables rebuild bit-exact from the harmonics).
juce::ValueTree Sp3ctraAudioProcessor::luxstralWavetableToTree() const
{
    float re[LUXSTRAL_WT_MAX_HARMONICS];
    float im[LUXSTRAL_WT_MAX_HARMONICS];
    float rootHz = 0.0f;
    const int n = luxstral_wavetable_get_harmonics(re, im,
                                                   LUXSTRAL_WT_MAX_HARMONICS,
                                                   &rootHz);
    if (n <= 0)
        return {};   // nothing loaded — no child written (absent = cleared)

    char name[LUXSTRAL_WT_NAME_MAX] = {0};
    luxstral_wavetable_get_info(name, sizeof(name), nullptr, nullptr);

    juce::MemoryBlock blob((size_t) n * 2 * sizeof(float));
    auto* dst = static_cast<float*>(blob.getData());
    for (int k = 0; k < n; ++k)
    {
        dst[2 * k]     = re[k];
        dst[2 * k + 1] = im[k];
    }

    juce::ValueTree t("LUXSTRAL_WAVETABLE");
    t.setProperty("numHarmonics", n,                          nullptr);
    t.setProperty("rootHz",       (double) rootHz,            nullptr);
    t.setProperty("name",         juce::String::fromUTF8(name), nullptr);
    t.setProperty("harmonics",    juce::var(blob),            nullptr);
    // Spectral envelope at the saved position — so the static fallback
    // restore keeps the formant color too.
    {
        float env[LUXSTRAL_WT_ENV_POINTS];
        if (luxstral_wavetable_get_env(env))
        {
            juce::MemoryBlock envBlob(env, sizeof(env));
            t.setProperty("envelope", juce::var(envBlob), nullptr);
        }
    }
    // Full source path: lets the restore re-retain the file so the scan
    // position stays live. The harmonics above are the fallback when the
    // file has moved. (Scan position itself is APVTS: luxstralTimbrePos.)
    t.setProperty("sourcePath",   timbreSamplePath_,          nullptr);
    return t;
}

bool Sp3ctraAudioProcessor::loadTimbreSampleFile(const juce::File& file,
                                                 float rootHzOverride,
                                                 juce::String& errorOut)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples < 512)
    {
        errorOut = "Could not read: " + file.getFileName();
        return false;
    }

    // Retain up to 30 s: enough to scan through, bounded memory (~5.8 MB).
    const auto maxLen = (juce::int64)(reader->sampleRate * 30.0);
    const int  numSamples =
        (int) juce::jmin<juce::int64>(reader->lengthInSamples, maxLen);
    const int  numCh = (int) reader->numChannels;

    juce::AudioBuffer<float> buf(numCh, numSamples);
    reader->read(&buf, 0, numSamples, 0, true, true);

    std::vector<float> mono((size_t) numSamples, 0.0f);
    const float chScale = 1.0f / (float) juce::jmax(1, numCh);
    for (int c = 0; c < numCh; ++c)
    {
        const float* src = buf.getReadPointer(c);
        for (int i = 0; i < numSamples; ++i)
            mono[(size_t) i] += src[i] * chScale;
    }

    if (luxstral_wavetable_load(mono.data(), numSamples,
                                (float) reader->sampleRate, rootHzOverride,
                                file.getFileName().toRawUTF8()) != 0)
    {
        errorOut = "No stable pitch found in " + file.getFileName();
        return false;
    }
    timbreSamplePath_ = file.getFullPathName();
    // Land the extraction on the persisted/current scan position.
    luxstral_wavetable_set_position(
        apvts.getRawParameterValue("luxstralTimbrePos")->load());
    return true;
}

bool Sp3ctraAudioProcessor::loadLuxGrainSampleFile(const juce::File& file,
                                                   juce::String& errorOut)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples < 512)
    {
        errorOut = "Could not read: " + file.getFileName();
        return false;
    }

    // Retain up to the engine bank capacity (10 s at 48 kHz).
    const int numSamples = (int) juce::jmin<juce::int64>(
        reader->lengthInSamples, (juce::int64) LUXGRAIN_SAMPLE_MAX);
    const int numCh = (int) reader->numChannels;

    juce::AudioBuffer<float> buf(numCh, numSamples);
    reader->read(&buf, 0, numSamples, 0, true, true);

    std::vector<float> mono((size_t) numSamples, 0.0f);
    const float chScale = 1.0f / (float) juce::jmax(1, numCh);
    for (int c = 0; c < numCh; ++c)
    {
        const float* src = buf.getReadPointer(c);
        for (int i = 0; i < numSamples; ++i)
            mono[(size_t) i] += src[i] * chScale;
    }

    if (luxgrain_engine_set_sample(&g_luxgrain_engine, mono.data(), numSamples,
                                   (float) reader->sampleRate, 0.0f) != 0)
    {
        errorOut = "No stable pitch found in " + file.getFileName();
        return false;
    }
    luxgrainSamplePath_ = file.getFullPathName();
    return true;
}

void Sp3ctraAudioProcessor::clearLuxGrainSample()
{
    luxgrainSamplePath_.clear();
    luxgrain_engine_clear_sample(&g_luxgrain_engine);
}

void Sp3ctraAudioProcessor::restoreLuxstralWavetableFromTree(const juce::ValueTree& t)
{
    if (! t.isValid())
    {
        timbreSamplePath_.clear();
        luxstral_wavetable_clear();   // session saved without a table
        return;
    }
    const int    n      = (int)    t.getProperty("numHarmonics", 0);
    const double rootHz = (double) t.getProperty("rootHz", 0.0);
    const auto   name   = t.getProperty("name", "(restored)").toString();
    const auto   path   = t.getProperty("sourcePath", juce::String()).toString();
    auto*        blob   = t.getProperty("harmonics").getBinaryData();

    // Preferred path: re-retain the source file so the scan position stays
    // live. The persisted root is forced (no re-detection) so the session
    // sounds identical. loadTimbreSampleFile() also re-applies the restored
    // luxstralTimbrePos.
    if (path.isNotEmpty() && rootHz > 0.0)
    {
        juce::File f(path);
        juce::String err;
        if (f.existsAsFile() && loadTimbreSampleFile(f, (float) rootHz, err))
            return;
        log_warning("VST", "Timbre source '%s' unavailable — restoring static "
                           "timbre from harmonics",
                    path.toRawUTF8());
    }

    if (blob == nullptr || n < 1 || n > LUXSTRAL_WT_MAX_HARMONICS
        || blob->getSize() < (size_t) n * 2 * sizeof(float) || rootHz <= 0.0)
    {
        log_warning("VST", "LUXSTRAL_WAVETABLE blob invalid — timbre not restored");
        return;
    }
    float re[LUXSTRAL_WT_MAX_HARMONICS];
    float im[LUXSTRAL_WT_MAX_HARMONICS];
    const auto* src = static_cast<const float*>(blob->getData());
    for (int k = 0; k < n; ++k)
    {
        re[k] = src[2 * k];
        im[k] = src[2 * k + 1];
    }
    const float* envPtr = nullptr;
    float env[LUXSTRAL_WT_ENV_POINTS];
    if (auto* envBlob = t.getProperty("envelope").getBinaryData())
        if (envBlob->getSize() >= sizeof(env))
        {
            memcpy(env, envBlob->getData(), sizeof(env));
            envPtr = env;
        }
    timbreSamplePath_.clear();   // static fallback — no scanning available
    luxstral_wavetable_load_from_harmonics(re, im, n, (float) rootHz, envPtr,
                                           name.toRawUTF8());
}

juce::ValueTree Sp3ctraAudioProcessor::seqStateToTree() const
{
    // Per-engine patterns: <SEQS><SEQ idx="e" seq_bpm=… seq_step_i=…/>…</SEQS>.
    // (The legacy single-<SEQ> global tree is migrated on load.)
    juce::XmlElement root("SEQS");
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
    {
        const FrameSequencer* fs = frameSequencers_[(size_t) e].get();
        if (fs == nullptr)
            continue;
        auto* xml = root.createNewChildElement("SEQ");
        xml->setAttribute("idx", e);
        fs->saveToXml(*xml);
    }
    return juce::ValueTree::fromXml(root);
}

juce::ValueTree Sp3ctraAudioProcessor::samplerSlotsStateToTree() const
{
    juce::ValueTree root("SAMPLER_SLOTS");
    for (int e = 0; e < LuxSampler::kMaxEngines; ++e)
    {
        const LuxSampler* engine = samplers_[(size_t) e].get();
        if (engine == nullptr)
            continue;
        juce::XmlElement engXml("Engine");
        engXml.setAttribute("idx",     e);
        engXml.setAttribute("overdub", engine->getOverdubMode() ? 1 : 0);
        for (int i = 0; i < LuxSamplerConstants::NUM_SLOTS; ++i)
        {
            auto* slotXml = engXml.createNewChildElement("Slot");
            engine->slotParamsToXml(i, *slotXml);
        }
        root.appendChild(juce::ValueTree::fromXml(engXml), nullptr);
    }
    return root;
}

void Sp3ctraAudioProcessor::applySamplerParamsFromState()
{
    auto root = apvts.state.getChildWithName("SAMPLER_SLOTS");
    if (! root.isValid())
        return;
    for (const auto& eng : root)
    {
        const int e = (int) eng.getProperty("idx", -1);
        if (e < 0 || e >= LuxSampler::kMaxEngines
            || samplers_[(size_t) e] == nullptr)
            continue;
        LuxSampler* engine = samplers_[(size_t) e].get();
        engine->setOverdubMode((int) eng.getProperty("overdub", 0) != 0);
        for (const auto& slot : eng)
        {
            if (auto slotXml = slot.createXml())
            {
                const int i = slotXml->getIntAttribute("idx", -1);
                // Settings follow content: a bank that is EMPTY right now
                // starts from clean defaults instead of resurrecting the
                // previous session's knobs. At restore time recordings are
                // not loaded yet (they live in the .sp3s, not the DAW blob) —
                // the auto-load refills the banks and re-runs this overlay,
                // so content-bearing banks do get their saved settings back.
                // EXCEPTION: an image-bound bank (srcImagePath) IS its
                // settings — keep them even while empty; the rebuild below
                // re-renders the content from the picture.
                if (i >= 0 && i < LuxSamplerConstants::NUM_SLOTS
                    && ! engine->slotHasContent(i)
                    && slotXml->getStringAttribute("srcImagePath", "").isEmpty())
                    engine->resetSlotPlayParams(i);
                else
                    engine->slotParamsFromXml(i, *slotXml);
            }
        }
        // Image-bound banks: the persisted binding (path + rotation) is the
        // truth — re-render the frames from the picture. This makes the last
        // rotation survive a restart even when the .sp3s session frames are
        // stale (or when no session file was ever saved).
        engine->rebuildImageBoundSlots();
    }
}

//==============================================================================
// M9 — IMAGE / VIDEO / CAMERA source engines: presence + state blob
//==============================================================================
void Sp3ctraAudioProcessor::updateMediaSourcePresence()
{
    bool imgSlot[8] = { false }, vidSlot[8] = { false }, camSlot[8] = { false };
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
        {
            if (m.slot < 0 || m.slot >= 8) continue;
            if (m.type == ModuleType::Image)  imgSlot[m.slot] = true;
            if (m.type == ModuleType::Video)  vidSlot[m.slot] = true;
            if (m.type == ModuleType::Camera) camSlot[m.slot] = true;
        }
    for (int s = 0; s < 8; ++s)
    {
        if (auto* eng = imageSources_[(size_t) s].get())
            eng->setModulePresent(imgSlot[s]);
        if (auto* v = videoSources_[(size_t) s].get())
            v->setModulePresent(vidSlot[s]);
        auto* c = cameraSources_[(size_t) s].get();
        if (c == nullptr) continue;
        c->setModulePresent(camSlot[s]);

        // A CAMERA instance placed with a persisted device choice and no open
        // device (fresh restore, module re-added) → reopen it. Message thread.
        if (camSlot[s] && ! c->isOpen()
            && cameraDeviceNames_[(size_t) s].isNotEmpty())
        {
            const auto names = CameraSourceEngine::getDeviceNames();
            const int  idx   = names.indexOf(cameraDeviceNames_[(size_t) s]);
            if (idx >= 0)
            {
                juce::String err;
                if (! c->openDevice(idx, err))
                    log_warning("VST", "Camera %d reopen failed: %s",
                                s, err.toRawUTF8());
            }
        }
        // Instance removed → release its device (camera light off).
        if (! camSlot[s] && c->isOpen())
            c->closeDevice();
    }
}

juce::ValueTree Sp3ctraAudioProcessor::mediaSourcesStateToTree() const
{
    juce::ValueTree t("MEDIA_SOURCES");
    for (int s = 0; s < 8; ++s)
    {
        const juce::String sfx = s == 0 ? juce::String() : juce::String(s);
        if (auto* eng = imageSources_[(size_t) s].get())
            t.setProperty(juce::Identifier("imagePath" + sfx),
                          eng->getFile().getFullPathName(), nullptr);
        if (auto* v = videoSources_[(size_t) s].get())
            t.setProperty(juce::Identifier("videoPath" + sfx),
                          v->getFile().getFullPathName(), nullptr);
        t.setProperty(juce::Identifier("cameraDevice" + sfx),
                      cameraDeviceNames_[(size_t) s], nullptr);
    }
    return t;
}

void Sp3ctraAudioProcessor::restoreMediaSourcesFromTree(const juce::ValueTree& t)
{
    if (! t.isValid())
        return;

    for (int s = 0; s < 8; ++s)
    {
        const juce::String sfx = s == 0 ? juce::String() : juce::String(s);
        const juce::String imgPath =
            t.getProperty(juce::Identifier("imagePath" + sfx), "").toString();
        if (auto* eng = imageSources_[(size_t) s].get(); eng && imgPath.isNotEmpty())
        {
            juce::String err;
            if (! eng->loadFile(juce::File(imgPath), err))
                log_warning("VST", "Image source %d restore failed: %s",
                            s, err.toRawUTF8());
        }
        const juce::String vidPath =
            t.getProperty(juce::Identifier("videoPath" + sfx), "").toString();
        if (auto* v = videoSources_[(size_t) s].get(); v && vidPath.isNotEmpty())
        {
            juce::String err;
            if (! v->loadFile(juce::File(vidPath), err))
                log_warning("VST", "Video source %d restore failed: %s",
                            s, err.toRawUTF8());
        }
        cameraDeviceNames_[(size_t) s] =
            t.getProperty(juce::Identifier("cameraDevice" + sfx), "").toString();
    }
    // The devices are (re)opened by updateMediaSourcePresence() once the
    // chain model restore confirms CAMERA instances are actually placed.
}

void Sp3ctraAudioProcessor::applyChainEnableBridge()
{
    auto setParam = [this](const juce::String& id, bool on)
    {
        if (id.isEmpty())
            return;
        if (auto* param = apvts.getParameter(id))
        {
            const float v = on ? 1.0f : 0.0f;
            if (param->getValue() != v)
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost(v);
                param->endChangeGesture();
            }
        }
    };

    std::set<ModuleType> now;
    chainModel_.deriveActiveTypes(now);

    // LuxStral's engine enable (deviceEnabled) is handled below on the
    // presence of ANY "→ LUXSTRAL" send; the per-send power lives in the
    // luxstralOut{N}_enabled banks.
    // Pitch/Mask/Reverb/Echo are handled PER INSTANCE in
    // updateInsertParamMemory() (their enable lives in the per-slot bank).
    static const ModuleType kEnableTypes[] = {
        ModuleType::Sampler,
        ModuleType::LuxSynth, ModuleType::LuxWave, ModuleType::LuxGrain
    };
    for (auto t : kEnableTypes)
    {
        const bool isNow = now.count(t) > 0;
        const bool was   = chainActiveTypes_.count(t) > 0;
        if (isNow && ! was)
            setParam(moduleEnableParam(t), true);    // newly added ⇒ enable
        else if (! isNow)
            setParam(moduleEnableParam(t), false);   // absent ⇒ force off
    }

    // LuxStral engine enable (same add ⇒ on / absent ⇒ off diff semantics).
    {
        std::set<int> sendsNow;
        for (const auto& ch : chainModel_.chains)
            for (const auto& m : ch.modules)
                if (m.type == ModuleType::LuxStral && m.slot >= 0
                    && m.slot < ChainModel::kMaxEngineSends)
                    sendsNow.insert(m.slot);

        const bool anyNow = ! sendsNow.empty();
        const bool anyWas = ! luxstralSends_.empty();
        if (anyNow && ! anyWas)
            setParam("deviceEnabled", true);
        else if (! anyNow)
            setParam("deviceEnabled", false);
        luxstralSends_ = sendsNow;
    }

    chainActiveTypes_ = now;
}

// Free the state of every module that just disappeared from the topology. The
// enable bridge (next call) silences modules that own an enable param, but some
// modules keep state the bridge cannot reach: SCORE has NO enable param (its
// has_score plan flag only routes the score-player feed), so removal alone
// never stops it; VideoScroll keeps a captured waterfall ring; Pitch/Mask hold live
// MIDI voices. Recorded content (sampler slots, sequencer pattern, synth params)
// is intentionally preserved — removal only tears down transient/live state.
// Runs AFTER deriveChainRouting() published the new plan (the synth thread has
// stopped pulling the removed modules) and BEFORE applyChainEnableBridge()
// overwrites chainActiveTypes_, so chainActiveTypes_ still holds the old set.
void Sp3ctraAudioProcessor::teardownAbsentModules(const std::set<ModuleType>& now)
{
    auto removed = [&](ModuleType t)
    { return chainActiveTypes_.count(t) > 0 && now.count(t) == 0; };

    // P5-M4: score-family frames live PER SLOT in the ScorePlayerService —
    // a removed instance's slot is discarded by the present-mask diff in
    // deriveAndPublishChainPlan (no shared channel left to guard). SCORE's
    // settings reset stays SCORE-only.
    if (removed(ModuleType::Score))
    {
        score_settings_defaults(&scoreSettings_);
        scoreSettings_.writingSpeed = 2.5;   // match the constructor default
    }

    // Pitch / Mask: handled at INSTANCE granularity by deriveChainRouting() —
    // it diffs the per-slot presence masks (and chain→slot rebindings) and
    // resets exactly the pool slots that lost their module, which subsumes the
    // old type-level reset here.

    // VideoScroll: per-instance probe (slot 0..7), may repeat per chain, so diff
    // at slot granularity. Re-init the capture ring of any removed probe so its
    // waterfall image stops being displayed. We are on the message thread — the
    // same thread that consumes the ring — so this cannot race the consumer, and
    // the producer (synth thread) no longer captures to a dropped slot.
    std::set<int> vsNow;
    for (const auto& ch : chainModel_.chains)
        for (const auto& m : ch.modules)
            if (m.type == ModuleType::VideoScroll
                && m.slot >= 0 && m.slot < CHAIN_MAX_CHAINS)
                vsNow.insert(m.slot);
    for (int slot : videoScrollSlots_)
        if (vsNow.count(slot) == 0)
        {
            // Deferred re-init (= clear ring): the UDP/feeder thread may still
            // be capturing into this slot for the in-flight frame taken with
            // the OLD plan — memset'ing 6.3 MB under it would race the writes.
            // The processor timer runs it ≥40 ms after the publish.
            pendingVideoScrollInits_ |= (1u << slot);
            poolResetArmedMs_ = juce::Time::getMillisecondCounter();
        }
    // A slot re-armed before the deferred init fired (remove + re-add within
    // 40 ms) must keep its ring — the pending memset would wipe and race the
    // freshly active probe.
    for (int slot : vsNow)
        pendingVideoScrollInits_ &= ~(1u << slot);
    // A slot that just (re)appeared is a freshly placed output: force its enable
    // ON so a new VideoScroll block starts visible even if that slot was left
    // disabled by a previously removed instance.
    for (int slot : vsNow)
        if (videoScrollSlots_.count(slot) == 0)
            if (auto* p = apvts.getParameter(vsParam(slot, "enabled")))
                p->setValueNotifyingHost(1.0f);

    // A NEW instance (different UUID) claiming a slot must not inherit the
    // removed instance's parameter bank: existing "VS{N} …" automation lanes
    // and settings would silently drive the new module. Reset the whole
    // videoScroll{N}_* / videoMix{N}_* bank to defaults (enable is owned by
    // the forcing above). Same-UUID instances (a plain re-derive) are
    // untouched, and session loads baseline the map without resetting.
    {
        std::map<int, juce::Uuid> vsIdsNow;
        for (const auto& ch : chainModel_.chains)
            for (const auto& m : ch.modules)
                if (m.type == ModuleType::VideoScroll
                    && m.slot >= 0 && m.slot < CHAIN_MAX_CHAINS)
                    vsIdsNow[m.slot] = m.id;
        for (const auto& entry : vsIdsNow)
        {
            const auto prev = videoScrollSlotIds_.find(entry.first);
            if (prev != videoScrollSlotIds_.end() && prev->second == entry.second)
                continue;   // same instance as before
            const juce::String pfx1 = "videoScroll" + juce::String(entry.first) + "_";
            const juce::String pfx2 = "videoMix"    + juce::String(entry.first) + "_";
            for (auto* param : getParameters())
                if (auto* pw = dynamic_cast<juce::RangedAudioParameter*>(param))
                {
                    if (!pw->paramID.startsWith(pfx1) && !pw->paramID.startsWith(pfx2))
                        continue;
                    if (pw->paramID.endsWith("_enabled"))
                        continue;
                    pw->setValueNotifyingHost(pw->getDefaultValue());
                }
        }
        videoScrollSlotIds_ = std::move(vsIdsNow);
    }
    videoScrollSlots_ = vsNow;
}

bool Sp3ctraAudioProcessor::saveChainPreset(int chainIdx, const juce::File& file)
{
    if (chainIdx < 0 || chainIdx >= chainModel_.numChains())
        return false;
    snapshotBankValuesIntoModel();   // the preset carries the CURRENT settings
    const auto preset = ChainPresetIO::makePresetTree(
        chainModel_.chains[(size_t) chainIdx],
        file.getFileNameWithoutExtension());
    return ChainPresetIO::saveToFile(preset, file);
}

Sp3ctraAudioProcessor::ChainPresetLoadResult
Sp3ctraAudioProcessor::loadChainPreset(const juce::ValueTree& preset,
                                       int targetChainIdx)
{
    ChainPresetLoadResult res;
    const auto ct = preset.getChildWithName(ChainModel::kChainTag);
    if (! ct.isValid())
        return res;

    // Refresh every chain's VALUES first: the projection at the end walks the
    // whole model, and the OTHER chains must project as no-ops (their trees
    // would otherwise hold save-time values and revert live knob moves).
    snapshotBankValuesIntoModel();

    int target = targetChainIdx;
    if (target < 0)
    {
        target = chainModel_.addChain();
        if (target < 0)
            return res;   // 8-chain cap
    }
    else if (target >= chainModel_.numChains())
        return res;

    Chain& ch = chainModel_.chains[(size_t) target];

    // J5 — automation/MIDI stability: a same-type module in the preset lands
    // on the bank slot the OLD composition used, so host lanes and MIDI
    // mappings keep driving "the module of this chain".
    std::map<ModuleType, int> oldSlot;
    for (const auto& m : ch.modules)
        if (moduleParamManifest(m.type) != nullptr)
            oldSlot.try_emplace(m.type, bankSlotForModule(m));

    // "Load into": replace the content, the chain identity survives.
    ch.modules.clear();
    ch.typeMemory.clear();

    for (const auto& mt : ct)
    {
        if (mt.hasType(ChainModel::kMemoryTag))
        {
            ModuleType type;
            if (moduleTypeFromId(
                    mt.getProperty(ChainModel::kTypeProp).toString(), type))
            {
                juce::ValueTree mem(ChainModel::kValuesTag);
                mem.copyPropertiesFrom(mt, nullptr);
                mem.removeProperty(ChainModel::kTypeProp, nullptr);
                ch.typeMemory[type] = std::move(mem);
            }
            continue;
        }
        if (! mt.hasType(ChainModel::kModuleTag))
            continue;
        ModuleType type;
        const juce::String typeId =
            mt.getProperty(ChainModel::kTypeProp).toString();
        if (! moduleTypeFromId(typeId, type))
        {
            res.skipped.add(typeId);   // newer/unknown type
            continue;
        }
        const int at = (int) ch.modules.size();
        if (! chainModel_.insert(target, type, at))
        {
            // Singleton placed elsewhere / exhausted pool / duplicate — skip,
            // the rest of the preset still loads.
            res.skipped.add(moduleDisplayName(type));
            continue;
        }

        ModuleInstance& mi = ch.modules[(size_t) at];
        const auto values = mt.getChildWithName(ChainModel::kValuesTag);
        if (values.isValid())
            mi.values = values.createCopy();

        // J5 pre-seed: reuse the old composition's slot for this type.
        const auto it = oldSlot.find(type);
        if (it != oldSlot.end())
        {
            if (isPooledInsertType(type))
                modulePoolSlots_[mi.id] = { it->second, type };
            else if (ChainModel::hasSlot(type))
                mi.slot = it->second;   // collisions healed below
            oldSlot.erase(it);
        }
    }
    chainModel_.validateAndRepair();

    onChainModelEdited();          // bindings + reset/inherit + bridge + plan
    projectChainValuesToBanks();   // preset VALUES → the fresh banks
    res.chainIdx = target;
    return res;
}

int Sp3ctraAudioProcessor::duplicateChain(int chainIdx)
{
    // Fresh VALUES on every module first, so the copies carry the CURRENT
    // settings (VALUES are otherwise only refreshed at save time).
    snapshotBankValuesIntoModel();
    const int newIdx = chainModel_.duplicateChain(chainIdx);
    if (newIdx < 0)
        return -1;
    // Bindings (fresh pool slots) + new-instance reset/inherit + enable
    // bridge + plan republish…
    onChainModelEdited();
    // …then the copied VALUES override the freshly-reset banks: the duplicate
    // plays with the source chain's exact settings, fully independent.
    projectChainValuesToBanks();
    return newIdx;
}

void Sp3ctraAudioProcessor::onChainModelEdited()
{
    // Routing/masks/plan FIRST: the enable bridge below flips APVTS params which
    // trigger applyConfigurationToCore() (per-instance config sync) — that reads
    // the freshly-computed chainPitch/MaskMask_.
    deriveChainRouting();       // per-synth source routing + instance masks + RT plan

    // Per-chain settings memory + per-instance bank hygiene (reset on claim,
    // restore on re-add, enable on placement) — needs the fresh pool bindings.
    updateInsertParamMemory();

    std::set<ModuleType> now;
    chainModel_.deriveActiveTypes(now);
    teardownAbsentModules(now); // free removed modules' state (uses old chainActiveTypes_)

    applyChainEnableBridge();   // enable params + insert order (diff vs baseline)
    persistChainModel();
}

//==============================================================================
// Apply APVTS parameters to Sp3ctraCore and global C config
// needsSocketRestart: true = full reinit (UDP change), false = just update g_sp3ctra_config
void Sp3ctraAudioProcessor::applyConfigurationToCore(bool needsSocketRestart)
{
    // Read current APVTS parameters
    int udpPort = (int)udpPortParam->load();
    int dpiChoice = (int)sensorDpiParam->load();  // 0=200, 1=400
    int logLevel = (int)logLevelParam->load();
    
    // Map DPI choice to actual DPI value
    int sensorDpi = (dpiChoice == 0) ? 200 : 400;
    
    // Build UDP address from 4 bytes
    juce::String udpAddress = getUdpAddressString();
    
    // Update global C config (used by udpThread) - ALWAYS SAFE, no buffer realloc
    extern sp3ctra_config_t g_sp3ctra_config;
    g_sp3ctra_config.udp_port = udpPort;
    strncpy(g_sp3ctra_config.udp_address, udpAddress.toRawUTF8(), 
            sizeof(g_sp3ctra_config.udp_address) - 1);
    g_sp3ctra_config.udp_address[sizeof(g_sp3ctra_config.udp_address) - 1] = '\0';
    g_sp3ctra_config.sensor_dpi = sensorDpi;
    g_sp3ctra_config.log_level = (log_level_t)logLevel;
    
    // Maximum resolution: always 1 pixel = 1 oscillator (3456 oscillators).
    // The former adaptive reduction (calibrated on ~760 KB multi-table design)
    // is no longer needed: the shared sine table (4 KB, L1-resident) eliminates
    // the cache-miss bottleneck that made high oscillator counts CPU-expensive.
    g_sp3ctra_config.pixels_per_note = 1;
    // DEBUG, not INFO: this is a fixed constant (get_cis_pixels_nb()), NOT a
    // per-call oscillator (re)allocation — nothing is built here. Logging it at
    // INFO made every config resync look like heavy work and flooded restores.
    log_debug("VST", "Resolution: pixels_per_note=1 → %d oscillators (max, shared sine table)",
              get_cis_pixels_nb());
    
    // ========================================================================
    // Synchronize LuxStral parameters from APVTS to g_sp3ctra_config
    // ========================================================================
    
    // 🎵 Musical Frequency Calculation from Tuning + Root Note + Num Octaves
    // This eliminates "jumps" caused by dynamic octave recalculation
    float tuning = apvts.getRawParameterValue("luxstralTuning")->load();
    int rootNoteIndex = (int)apvts.getRawParameterValue("luxstralRootNote")->load();
    int numOctaves = (int)apvts.getRawParameterValue("luxstralNumOctaves")->load();
    
    // Convert ComboBox index to MIDI note number
    // Index 0 = C1 = MIDI 24, Index 12 = C2 = MIDI 36, etc.
    int rootNoteMidi = 24 + rootNoteIndex;  // C1 starts at MIDI 24
    
    // Calculate frequency from MIDI note: freq = tuning * 2^((midi - 69) / 12)
    // A4 (MIDI 69) = tuning Hz
    float lowFrequency = tuning * powf(2.0f, (float)(rootNoteMidi - 69) / 12.0f);
    float highFrequency = lowFrequency * powf(2.0f, (float)numOctaves);
    
    // Clamp high frequency to 20 kHz and Nyquist frequency
    if (highFrequency > 20000.0f) {
        highFrequency = 20000.0f;
    }
    
    // Additional safety: clamp to Nyquist frequency if sample rate is known
    extern sp3ctra_config_t g_sp3ctra_config;
    float nyquist = (float)g_sp3ctra_config.sampling_frequency * 0.5f;
    if (nyquist > 0 && highFrequency > nyquist) {
        log_warning("VST", "High frequency %.1f Hz clamped to Nyquist %.1f Hz", highFrequency, nyquist);
        highFrequency = nyquist;
    }
    
    // Store calculated frequencies
    g_sp3ctra_config.low_frequency = lowFrequency;
    g_sp3ctra_config.high_frequency = highFrequency;
    g_sp3ctra_config.start_frequency = lowFrequency;  // Backward compatibility
    
    // Store the fixed number of octaves (no more dynamic calculation!)
    g_sp3ctra_config.num_octaves = numOctaves;
    
    log_debug("VST", "Musical config: tuning=%.1f Hz, root=%d (MIDI %d), octaves=%d -> %.1f - %.1f Hz",
              tuning, rootNoteIndex, rootNoteMidi, numOctaves, lowFrequency, highFrequency);
    
    // Envelope Parameters
    g_sp3ctra_config.tau_up_base_ms = apvts.getRawParameterValue("luxstralAttackMs")->load();
    g_sp3ctra_config.tau_down_base_ms = apvts.getRawParameterValue("luxstralReleaseMs")->load();
    
    // Stereo Processing
    g_sp3ctra_config.stereo_mode_enabled = 
        (int)apvts.getRawParameterValue("luxstralStereoEnable")->load();
    g_sp3ctra_config.stereo_temperature_amplification = 
        apvts.getRawParameterValue("luxstralStereoTempAmp")->load();
    
    // Dynamics Processing
    g_sp3ctra_config.noise_gate_threshold =
        apvts.getRawParameterValue("luxstralNoiseGateThreshold")->load();
    g_sp3ctra_config.soft_limit_threshold =
        apvts.getRawParameterValue("luxstralSoftLimitThreshold")->load();
    g_sp3ctra_config.soft_limit_knee =
        apvts.getRawParameterValue("luxstralSoftLimitKnee")->load();

    // Phase management (mode + sensitivity + position + drift)
    g_sp3ctra_config.luxstral_phase_mode =
        (int)apvts.getRawParameterValue("luxstralPhaseMode")->load();
    g_sp3ctra_config.luxstral_phase_sensitivity =
        apvts.getRawParameterValue("luxstralPhaseSensitivity")->load();
    g_sp3ctra_config.luxstral_phase_position =
        apvts.getRawParameterValue("luxstralPhasePosition")->load();
    g_sp3ctra_config.luxstral_phase_drift_cents =
        apvts.getRawParameterValue("luxstralPhaseDriftCents")->load();

    // Timbre master switch + mix + formant depth (inert while no table)
    g_sp3ctra_config.luxstral_timbre_enable =
        (int)apvts.getRawParameterValue("luxstralTimbreEnable")->load();
    g_sp3ctra_config.luxstral_timbre_mix =
        apvts.getRawParameterValue("luxstralTimbreMix")->load();
    g_sp3ctra_config.luxstral_timbre_formant =
        apvts.getRawParameterValue("luxstralTimbreFormant")->load();

    // Performance
    g_sp3ctra_config.num_workers = 
        (int)apvts.getRawParameterValue("luxstralNumWorkers")->load();
    
    // Physiological Filter (Equal-Loudness Compensation on wavetables)
    g_sp3ctra_config.physiological_filter_enabled = 
        (int)apvts.getRawParameterValue("luxstralPhysiologicalFilter")->load();
    // FIX: this field was never written → depth was 0.0f (zero-init) → all gains = 1.000
    g_sp3ctra_config.physiological_correction_depth =
        apvts.getRawParameterValue("luxstralPhysiologicalDepth")->load();
    
    // ========================================================================
    // StrokeForge — Blob-to-note mapping with waveform morphing
    // Blob detection params (threshold, min width, merge gap) are now set
    // exclusively by spctrBlob* (IMAGE LUXSTRAL tab) in the block below.
    // ========================================================================
    g_sp3ctra_config.strokeforge_enabled =
        (int)apvts.getRawParameterValue("sfEnabled")->load();
    g_sp3ctra_config.strokeforge_morph_width_scale =
        apvts.getRawParameterValue("sfMorphWidthScale")->load();
    g_sp3ctra_config.strokeforge_blob_focus_sigma =
        apvts.getRawParameterValue("sfBlobFocusSigma")->load();
    g_sp3ctra_config.strokeforge_spectral_width_threshold =
        apvts.getRawParameterValue("sfSpectralWidthThreshold")->load();
    g_sp3ctra_config.strokeforge_focus_only =
        (int)apvts.getRawParameterValue("sfFocusOnly")->load();

    /* ── Image Pipeline live controls ──────────────────────────────────────── */
    /* Blob detection now runs only when StrokeForge is in use (enabled or       */
    /* focus-only) — see image_pipeline.c Stage 9 — to save CPU when unused.     */
    // ── Mix balance crossfader → derived live/sampler opacities ───────────────
    // balance=0.0 → smpOp=1.0, liveOp=0.0 (full Sampler)
    // balance=0.5 → smpOp=1.0, liveOp=1.0 (equal, full darken blend)
    // balance=1.0 → smpOp=0.0, liveOp=1.0 (full Live)
    {
        const float bal = apvts.getRawParameterValue("imageMixBalance")->load();
        const float liveOp = std::min(1.0f, 2.0f * bal);
        const float smpOp  = std::min(1.0f, 2.0f * (1.0f - bal));
        g_sp3ctra_config.image_live_opacity    = liveOp;
        g_sp3ctra_config.image_sampler_opacity = smpOp;
    }
    g_sp3ctra_config.image_freeze_mode   =
        static_cast<int>(apvts.getRawParameterValue("imageFreezeMode")->load());
    g_sp3ctra_config.image_fade_in_ms    =
        static_cast<int>(apvts.getRawParameterValue("imageFadeInMs")->load());
    g_sp3ctra_config.sampler_gamma        =
        apvts.getRawParameterValue("samplerGamma")->load();
    g_sp3ctra_config.sampler_contrast_min =
        apvts.getRawParameterValue("samplerContrastMin")->load();
    g_sp3ctra_config.sampler_freeze_mode  =
        static_cast<int>(apvts.getRawParameterValue("samplerFreezeMode")->load());
    g_sp3ctra_config.raw_freeze_mode      =
        static_cast<int>(apvts.getRawParameterValue("rawFreezeMode")->load());

    // ========================================================================
    // Per-path pipeline routing — source selection, inversion, AC removal
    //
    // APVTS choice indices:  0="S - Sampler", 1="M - Mix", 2="L - Live", 3="P - LuxPitch"
    // ImageSourceType enum:  SAMPLER=0, LIVE=1, MIX=2, LUXPITCH=3
    // Mapping table: choice → enum
    // ========================================================================
    {
        // Routing: the ChainPlan is the single authority (M7/M8) — an engine
        // is fed by the OUT modules placed in chains; the per-OUT banks below
        // are the only conditioning.

        // M4 — core-side LuxSynth engine feed (luxsynth_feed_tick): FFT bins
        // choice + temporal smoothing, mirrored from the APVTS params the UI
        // FFT view uses (same values → view matches what the engine hears).
        g_sp3ctra_config.lx_fft_bins_choice =
            static_cast<int>(apvts.getRawParameterValue("lxFftBins")->load());
        g_sp3ctra_config.lx_fft_smoothing =
            apvts.getRawParameterValue("lxFftSmoothing")->load();

        // ── Synth-split P1 — per-OUT conditioning banks → g_sp3ctra_config ──
        // contrast_min / range_db are LuxStral-only.
        for (int s = 0; s < LUX_OUT_MAX_SLOTS; ++s)
        {
            auto rawf = [this](const juce::String& id) {
                auto* v = apvts.getRawParameterValue(id);
                return v ? v->load() : 0.0f;
            };

            lux_out_params_t* ls = &g_sp3ctra_config.luxstral_out[s];
            ls->negative     = (int)rawf(lsOutParam(s, "negative"));
            ls->dc_blocking  = (int)rawf(lsOutParam(s, "dcBlocking"));
            ls->gamma        = rawf(lsOutParam(s, "gamma"));
            ls->contrast_min = rawf(lsOutParam(s, "contrastMin"));
            ls->range_db     = rawf(lsOutParam(s, "rangeDb"));
            ls->intensity    = rawf(lsOutParam(s, "intensity"));
            ls->enabled      = (int)rawf(lsOutParam(s, "enabled"));

            lux_out_params_t* lx = &g_sp3ctra_config.luxsynth_out[s];
            lx->negative    = (int)rawf(lxOutParam(s, "negative"));
            lx->dc_blocking = (int)rawf(lxOutParam(s, "dcBlocking"));
            lx->gamma       = rawf(lxOutParam(s, "gamma"));
            lx->intensity   = rawf(lxOutParam(s, "intensity"));
            lx->enabled     = (int)rawf(lxOutParam(s, "enabled"));

            lux_out_params_t* lw = &g_sp3ctra_config.luxwave_out[s];
            lw->negative    = (int)rawf(lwOutParam(s, "negative"));
            lw->dc_blocking = (int)rawf(lwOutParam(s, "dcBlocking"));
            lw->gamma       = rawf(lwOutParam(s, "gamma"));
            lw->intensity   = rawf(lwOutParam(s, "intensity"));
            lw->enabled     = (int)rawf(lwOutParam(s, "enabled"));

            lux_out_params_t* lg = &g_sp3ctra_config.luxgrain_out[s];
            lg->negative    = (int)rawf(lgOutParam(s, "negative"));
            lg->dc_blocking = (int)rawf(lgOutParam(s, "dcBlocking"));
            lg->gamma       = rawf(lgOutParam(s, "gamma"));
            lg->intensity   = rawf(lgOutParam(s, "intensity"));
            lg->enabled     = (int)rawf(lgOutParam(s, "enabled"));
        }

        // ── Insert chain order (M1 — modular pipeline core) ──
        // (P4-M3) The GLOBAL insert order is gone — per-chain order comes
        // from each chain's own recipe (the model), "chainInsertOrder" is a
        // host-visible projection only.

        // ── Sync LuxPitch configs — one APVTS bank per pool instance ──
        // Slot i reads luxpitch{i}_* : two Pitch modules on two chains carry
        // fully independent settings. `enabled` stays gated on presence
        // (mask bit i) so an unplaced slot never processes a chain's image.
        {
            const uint32_t pmask = chainPitchMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(lpParam(i, sfx))->load(); };

                LuxPitchConfig c;
                c.enabled                  = static_cast<int>(raw("Enabled"));
                c.polyphony_enabled        = static_cast<int>(raw("Polyphony"));
                c.background_mode          = static_cast<int>(raw("BackgroundMode"));
                c.coupling_mode            = static_cast<int>(raw("CouplingMode"));
                c.free_pixels_per_semitone = raw("FreePixelsPerST");
                c.pitch_bend_range         = raw("PitchBendRange");
                c.attack_ms                = raw("AttackMs");
                c.decay_ms                 = raw("DecayMs");
                c.sustain_level            = raw("SustainLevel");
                c.release_ms               = raw("ReleaseMs");
                c.attack_curve             = raw("AttackCurve");
                c.decay_curve              = raw("DecayCurve");
                c.release_curve            = raw("ReleaseCurve");
                c.glide_time_ms            = raw("GlideMs");
                c.lfo_rate_hz              = raw("LfoRate");
                c.lfo_depth_semitones      = raw("LfoDepth");
                c.velocity_coupling        = static_cast<int>(raw("VelocityCoupling"));
                c.reference_note           = 24 + static_cast<int>(raw("ReferenceNote"));
                if (((pmask >> i) & 1u) == 0) c.enabled = 0;
                lux_pitch_instance(i)->config = c;
            }
        }

        // ── Sync LuxMask configs — one APVTS bank per pool instance ──
        {
            const uint32_t mmask = chainMaskMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(lmParam(i, sfx))->load(); };

                LuxMaskConfig c;
                c.enabled                  = static_cast<int>(raw("Enabled"));
                c.polyphony_enabled        = static_cast<int>(raw("Polyphony"));
                c.background_mode          = static_cast<int>(raw("BackgroundMode"));
                c.coupling_mode            = static_cast<int>(raw("CouplingMode"));
                c.free_pixels_per_semitone = raw("FreePixelsPerST");
                c.pitch_bend_range         = raw("PitchBendRange");
                c.filter_width_pct         = raw("FilterWidth");
                c.filter_offset_pct        = raw("FilterOffset");
                c.filter_slope             = raw("FilterSlope");
                c.attack_ms                = raw("AttackMs");
                c.decay_ms                 = raw("DecayMs");
                c.sustain_level            = raw("SustainLevel");
                c.release_ms               = raw("ReleaseMs");
                c.attack_curve             = raw("AttackCurve");
                c.decay_curve              = raw("DecayCurve");
                c.release_curve            = raw("ReleaseCurve");
                c.glide_time_ms            = raw("GlideMs");
                c.lfo_pos_rate_hz          = raw("LfoPosRate");
                c.lfo_pos_depth_semitones  = raw("LfoPosDepth");
                c.velocity_coupling        = static_cast<int>(raw("VelocityCoupling"));
                c.reference_note           = 24 + static_cast<int>(raw("ReferenceNote"));
                if (((mmask >> i) & 1u) == 0) c.enabled = 0;
                lux_mask_instance(i)->config = c;
            }
        }

        // ── Sync LuxReverb configs — one APVTS bank per pool instance ──
        {
            // Choice order is {Auto, Black, White} — map onto the C-side modes.
            static const int kRvBgChoiceToMode[3] =
                { LUX_REVERB_BG_AUTO, LUX_REVERB_BG_BLACK, LUX_REVERB_BG_WHITE };
            const uint32_t rmask = chainReverbMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(rvParam(i, sfx))->load(); };

                int bgChoice = static_cast<int>(raw("BackgroundMode"));
                if (bgChoice < 0 || bgChoice > 2) bgChoice = 0;

                LuxReverbConfig c  = lux_reverb_config_default();
                c.enabled          = static_cast<int>(raw("Enabled"));
                c.background_mode  = kRvBgChoiceToMode[bgChoice];
                c.decay_s          = raw("Decay");
                c.diffusion        = raw("Diffusion") / 100.0f;
                c.mix              = raw("Mix") / 100.0f;
                if (((rmask >> i) & 1u) == 0) c.enabled = 0;
                lux_reverb_instance(i)->config = c;
            }
        }

        // ── Sync LuxEcho configs — one APVTS bank per pool instance ──
        {
            // Choice order is {Auto, Black, White} — map onto the C-side modes.
            static const int kEcBgChoiceToMode[3] =
                { LUX_ECHO_BG_AUTO, LUX_ECHO_BG_BLACK, LUX_ECHO_BG_WHITE };
            const uint32_t emask = chainEchoMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(ecParam(i, sfx))->load(); };

                int bgChoice = static_cast<int>(raw("BackgroundMode"));
                if (bgChoice < 0 || bgChoice > 2) bgChoice = 0;

                LuxEchoConfig c   = lux_echo_config_default();
                c.enabled         = static_cast<int>(raw("Enabled"));
                c.background_mode = kEcBgChoiceToMode[bgChoice];
                c.delay_lines     = static_cast<int>(raw("Delay"));
                c.feedback        = raw("Feedback") / 100.0f;
                c.mix             = raw("Mix") / 100.0f;
                if (((emask >> i) & 1u) == 0) c.enabled = 0;
                lux_echo_instance(i)->config = c;
            }
        }

        // ── Sync LuxEq configs — one APVTS bank per pool instance ──
        {
            // Choice order is {Auto, Black, White} — map onto the C-side modes.
            static const int kEqBgChoiceToMode[3] =
                { LUX_EQ_BG_AUTO, LUX_EQ_BG_BLACK, LUX_EQ_BG_WHITE };
            const uint32_t qmask = chainEqMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(eqParam(i, sfx))->load(); };

                int bgChoice = static_cast<int>(raw("BackgroundMode"));
                if (bgChoice < 0 || bgChoice > 2) bgChoice = 0;

                LuxEqConfig c     = lux_eq_config_default();
                c.enabled         = static_cast<int>(raw("Enabled"));
                c.background_mode = kEqBgChoiceToMode[bgChoice];
                for (int b = 0; b < LUX_EQ_NUM_BANDS; ++b)
                    c.band_gain_db[b] =
                        raw(("Band" + juce::String(b)).toRawUTF8());
                if (((qmask >> i) & 1u) == 0) c.enabled = 0;
                lux_eq_instance(i)->config = c;
            }
        }

        // ── Sync LuxHarmo (SCALE) configs — one APVTS bank per pool instance ──
        {
            // Choice order is {Auto, Black, White} — map onto the C-side modes.
            static const int kHmBgChoiceToMode[3] =
                { LUX_HARMO_BG_AUTO, LUX_HARMO_BG_BLACK, LUX_HARMO_BG_WHITE };
            const uint32_t hmask = chainHarmoMask_.load(std::memory_order_relaxed);
            for (int i = 0; i < CHAIN_MAX_CHAINS; ++i)
            {
                auto raw = [&, i](const char* sfx)
                { return apvts.getRawParameterValue(hmParam(i, sfx))->load(); };

                int bgChoice = static_cast<int>(raw("BackgroundMode"));
                if (bgChoice < 0 || bgChoice > 2) bgChoice = 0;

                LuxHarmoConfig c  = lux_harmo_config_default();
                c.enabled         = static_cast<int>(raw("Enabled"));
                c.mode            = static_cast<int>(raw("Mode"));
                c.root            = static_cast<int>(raw("Root"));
                c.scale           = static_cast<int>(raw("Scale"));
                c.strength        = raw("Strength") / 100.0f;
                c.width_st        = raw("Width");
                c.slope           = raw("Slope");
                c.glide_lines     = static_cast<int>(raw("Glide"));
                c.background_mode = kHmBgChoiceToMode[bgChoice];
                // Anchor the degree grid on the instrument's PHYSICAL axis so
                // the allowed rows line up with its true pitch classes.
                c.axis_low_hz     = g_sp3ctra_config.low_frequency;
                if (((hmask >> i) & 1u) == 0) c.enabled = 0;
                lux_harmo_instance(i)->config = c;
            }
        }

    }

    // ========================================================================
    // LuxSynth blob detection — FULLY ISOLATED from StrokeForge (lxBlob*)
    // These fields feed detectSynthBlobs() in CisVisualizerComponent ONLY.
    // They have zero effect on LuxStral audio synthesis.
    // ========================================================================
    g_sp3ctra_config.luxsynth_blob_threshold  =
        apvts.getRawParameterValue("lxBlobThreshold")->load();
    g_sp3ctra_config.luxsynth_blob_min_width  =
        static_cast<int>(apvts.getRawParameterValue("lxBlobMinWidth")->load());
    g_sp3ctra_config.luxsynth_blob_merge_gap  =
        static_cast<int>(apvts.getRawParameterValue("lxBlobMergeGap")->load());
    g_sp3ctra_config.luxsynth_blob_color_split =
        apvts.getRawParameterValue("lxBlobColorSplit")->load();

    // ========================================================================
    // SPCTR blob detection — IMAGE LUXSTRAL tab (spctrBlob* params)
    // Single source of truth for detectSpctrBlobs() + StrokeForge audio.
    // spctrBlobColorSplit is visualizer-only (no StrokeForge audio equivalent).
    // ========================================================================
    g_sp3ctra_config.strokeforge_blob_base_threshold =
        apvts.getRawParameterValue("spctrBlobThreshold")->load();
    g_sp3ctra_config.strokeforge_blob_min_width =
        static_cast<int>(apvts.getRawParameterValue("spctrBlobMinWidth")->load());
    g_sp3ctra_config.strokeforge_blob_merge_gap =
        static_cast<int>(apvts.getRawParameterValue("spctrBlobMergeGap")->load());

    // Update logger level immediately
    logger_init((log_level_t)logLevel);
    
    // In the shared-core design, the needsSocketRestart path is handled by
    // sharedCore->startWithConfig() (first time) or sharedCore->restartUdp()
    // (hot-reload). applyConfigurationToCore() is now a pure config updater.
    juce::ignoreUnused(needsSocketRestart);
    log_debug("VST", "Config updated — %d DPI, log level %d", sensorDpi, logLevel);
}

//==============================================================================
// UDP Batch Update API Implementation
void Sp3ctraAudioProcessor::beginUdpBatchUpdate()
{
    udpBatchUpdateActive.store(true);
    udpNeedsRestart.store(false);
    log_debug("VST", "UDP batch update started");
}

void Sp3ctraAudioProcessor::endUdpBatchUpdate()
{
    udpBatchUpdateActive.store(false);

    int newPort = static_cast<int>(udpPortParam->load());
    juce::String newAddress = getUdpAddressString();

    log_info("VST", "UDP batch update — restarting shared socket → %s:%d",
             newAddress.toRawUTF8(), newPort);

    applyConfigurationToCore(false);  // update g_sp3ctra_config

    if (sharedCore && sharedCore->isReady())
    {
        if (!sharedCore->restartUdp(newPort, newAddress.toStdString(), ""))
            log_error("VST", "Failed to restart UDP after batch update!");
        else
            log_info("VST", "UDP restarted → %s:%d", newAddress.toRawUTF8(), newPort);
    }

    udpNeedsRestart.store(false);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Sp3ctraAudioProcessor();
}
