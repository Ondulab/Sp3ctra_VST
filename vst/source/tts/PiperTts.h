/**
 * @file PiperTts.h
 * @brief Offline neural text-to-speech for the VOICE module (Piper VITS voices
 *        run by a statically linked sherpa-onnx engine).
 *
 * pImpl facade — no sherpa-onnx header leaks past this file. When the project
 * is configured with SP3CTRA_ENABLE_TTS=OFF the same class compiles as a stub
 * (isEngineAvailable() == false, loadVoice/synthesize fail with a message) so
 * call sites never need the compile flag.
 *
 * Threading contract:
 *  - static helpers (isEngineAvailable, voicesDirectory, listVoices,
 *    writeWavFile) — any thread;
 *  - everything else (loadVoice / unload / synthesize) — ONE worker thread at
 *    a time (the VOICE generate job). The engine is treated as single-threaded;
 *    never call these from the message or audio thread (model load ~1 s,
 *    synthesis ~0.1-1 s per sentence, both allocate).
 */
#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <vector>

/** One installed Piper voice: an extracted sherpa-onnx `vits-piper-*` bundle
 *  (<id>.onnx + tokens.txt + espeak-ng-data/) under voicesDirectory(). */
struct PiperVoiceInfo
{
    juce::String id;          ///< bundle directory name, e.g. "vits-piper-fr_FR-siwis-medium"
    juce::String lang;        ///< ISO 639-1 ("fr", "en"), empty if the name didn't parse
    juce::String region;      ///< "FR", "US", "GB", …
    juce::String name;        ///< speaker name ("siwis", "lessac", …)
    juce::String quality;     ///< "low" (16 kHz) / "medium" / "high" (22.05 kHz)
    juce::File   modelOnnx, tokensTxt, espeakDataDir;

    /** "siwis (fr-FR, medium)" — for the voice combo. */
    juce::String displayName() const;
};

class PiperTts
{
public:
    /** Synthesis options. noiseScale/noiseScaleW are load-time VITS graph
     *  inputs (changing them reloads the engine); lengthScale and
     *  sentenceSilenceScale apply per generate. */
    struct Options
    {
        float lengthScale          = 1.0f;    ///< >1 = slower speech
        float noiseScale           = 0.667f;  ///< VITS expressiveness (piper default)
        float noiseScaleW          = 0.8f;    ///< VITS duration noise (piper default)
        float sentenceSilenceScale = 1.0f;    ///< scales inter-sentence silence
    };

    struct Result
    {
        std::vector<float> samples;      ///< mono, [-1, 1]
        int                sampleRate = 0;
        juce::String       error;       ///< empty on success

        bool ok() const { return error.isEmpty() && ! samples.empty() && sampleRate > 0; }
    };

    PiperTts();
    ~PiperTts();

    /** False when the project was built with SP3CTRA_ENABLE_TTS=OFF. */
    static bool isEngineAvailable();

    /** ~/Library/Application Support/Sp3ctra/piper_voices (not created here). */
    static juce::File voicesDirectory();

    /** Scans voicesDirectory() for valid voice bundles, sorted by lang then name. */
    static juce::Array<PiperVoiceInfo> listVoices();

    /** Writes a Result as a 16-bit mono WAV at its native rate. */
    static bool writeWavFile (const Result& r, const juce::File& outFile);

    /** Creates (or reuses) the engine for this voice. One engine resident at a
     *  time — loading a different voice frees the previous one. No-op when the
     *  (voice, noiseScale, noiseScaleW) key is already loaded. ~0.5-1.5 s and
     *  ~150-250 MB RAM for a medium voice. */
    bool loadVoice (const PiperVoiceInfo& voice, const Options& opts, juce::String& error);

    /** Frees the resident engine (call when the VOICE module leaves the model). */
    void unload();

    bool isLoaded() const;
    juce::String loadedVoiceId() const;

    /** Blocking synthesis on the calling (worker) thread. onProgress gets
     *  p in [0,1] per generated sentence chunk; shouldCancel() == true aborts
     *  at the next chunk boundary (Result.error = "cancelled"). */
    Result synthesize (const juce::String& text, const Options& opts,
                       std::function<void (float)> onProgress,
                       std::function<bool()>       shouldCancel);

    /** Startup diagnostic: when the SP3CTRA_TTS_SMOKE env var is set, loads the
     *  first installed voice, synthesizes one sentence (the env var's value if
     *  it is longer than 1 char), writes /tmp/sp3ctra_tts_smoke.wav and logs
     *  timings to stderr. No-op otherwise — safe to call unconditionally. */
    static void runSmokeTestIfRequested();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE (PiperTts)
};
