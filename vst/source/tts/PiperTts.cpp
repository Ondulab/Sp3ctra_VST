/**
 * @file PiperTts.cpp
 * @brief sherpa-onnx implementation of the PiperTts facade (see PiperTts.h for
 *        the threading contract). Compiles as a stub when SP3CTRA_HAS_TTS is
 *        not defined (SP3CTRA_ENABLE_TTS=OFF).
 */
#include "PiperTts.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdlib>
#include <cstring>
#include <regex>

#if SP3CTRA_HAS_TTS
 #include <sherpa-onnx/c-api/c-api.h>
#endif

//==============================================================================
// Voice bundle enumeration — needs no engine, available in both builds.
//==============================================================================
juce::String PiperVoiceInfo::displayName() const
{
    juce::String s = lang.isEmpty()
        ? id
        : name + " (" + lang + (region.isNotEmpty() ? "-" + region : "")
               + ", " + quality + ")";
    if (builtIn)
        s += " [built-in]";
    return s;
}

juce::File PiperTts::voicesDirectory()
{
#if JUCE_WINDOWS
    // %APPDATA%\Sp3ctra\piper_voices (userApplicationDataDirectory = %APPDATA%)
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Sp3ctra/piper_voices");
#else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Application Support/Sp3ctra/piper_voices");
#endif
}

juce::File PiperTts::bundleVoicesDirectory()
{
    // currentExecutableFile resolves to the binary of THIS module — the plugin
    // dylib/DLL when hosted, the app binary for the Standalone.
#if JUCE_WINDOWS
    // No bundle machinery: the build copies voices to Resources/piper_voices
    // NEXT TO the binary (inside the .vst3 folder for the plugin).
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
               .getParentDirectory()
               .getChildFile ("Resources/piper_voices");
#else
    // macOS bundles: ../.. from Contents/MacOS is the Contents directory.
    return juce::File::getSpecialLocation (juce::File::currentExecutableFile)
               .getParentDirectory()    // Contents/MacOS
               .getParentDirectory()    // Contents
               .getChildFile ("Resources/piper_voices");
#endif
}

static void scanVoicesDir (const juce::File& root, bool builtIn,
                           juce::Array<PiperVoiceInfo>& out)
{
    for (const auto& dir : root.findChildFiles (juce::File::findDirectories, false))
    {
        const auto onnx = dir.findChildFiles (juce::File::findFiles, false, "*.onnx");
        const auto tokens = dir.getChildFile ("tokens.txt");
        const auto espeak = dir.getChildFile ("espeak-ng-data");
        if (onnx.size() != 1 || ! tokens.existsAsFile() || ! espeak.isDirectory())
            continue;

        const juce::String id = dir.getFileName();
        bool seen = false;
        for (const auto& existing : out)
            if (existing.id == id) { seen = true; break; }
        if (seen)
            continue;   // first occurrence wins (external scanned before bundle)

        PiperVoiceInfo v;
        v.id            = id;
        v.modelOnnx     = onnx.getReference (0);
        v.tokensTxt     = tokens;
        v.espeakDataDir = espeak;
        v.builtIn       = builtIn;

        // "vits-piper-<lang>_<REGION>-<name>-<quality>"
        static const std::regex re ("vits-piper-([a-z]{2,3})_([A-Z]{2})-(.+)-(low|medium|high)");
        std::smatch m;
        const std::string s = v.id.toStdString();
        if (std::regex_match (s, m, re))
        {
            v.lang    = juce::String (m[1].str());
            v.region  = juce::String (m[2].str());
            v.name    = juce::String (m[3].str());
            v.quality = juce::String (m[4].str());
        }
        out.add (std::move (v));
    }
}

juce::Array<PiperVoiceInfo> PiperTts::listVoices (const juce::File& externalDir)
{
    juce::Array<PiperVoiceInfo> voices;
    scanVoicesDir (externalDir, /*builtIn*/ false, voices);   // external wins dedupe
    scanVoicesDir (bundleVoicesDirectory(), /*builtIn*/ true, voices);

    std::sort (voices.begin(), voices.end(),
               [] (const PiperVoiceInfo& a, const PiperVoiceInfo& b)
               {
                   if (a.lang != b.lang) return a.lang < b.lang;
                   return a.name < b.name;
               });
    return voices;
}

bool PiperTts::writeWavFile (const Result& r, const juce::File& outFile)
{
    if (! r.ok())
        return false;

    outFile.getParentDirectory().createDirectory();
    outFile.deleteFile();
    auto stream = outFile.createOutputStream();
    if (stream == nullptr)
        return false;

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream.get(), (double) r.sampleRate, 1, 16, {}, 0));
    if (writer == nullptr)
        return false;
    stream.release();   // writer owns it now

    const float* chans[1] = { r.samples.data() };
    return writer->writeFromFloatArrays (chans, 1, (int) r.samples.size());
}

//==============================================================================
#if SP3CTRA_HAS_TTS
//==============================================================================

struct PiperTts::Impl
{
    const SherpaOnnxOfflineTts* tts = nullptr;

    // Engine cache key (noise scales are load-time VITS graph inputs).
    juce::String voiceId;
    float noiseScale = 0.0f, noiseScaleW = 0.0f;

    ~Impl() { destroy(); }

    void destroy()
    {
        if (tts != nullptr)
        {
            SherpaOnnxDestroyOfflineTts (tts);
            tts = nullptr;
        }
        voiceId.clear();
    }
};

PiperTts::PiperTts() : impl (std::make_unique<Impl>()) {}
PiperTts::~PiperTts() = default;

bool PiperTts::isEngineAvailable()          { return true; }
bool PiperTts::isLoaded() const             { return impl->tts != nullptr; }
juce::String PiperTts::loadedVoiceId() const{ return impl->voiceId; }
void PiperTts::unload()                     { impl->destroy(); }

bool PiperTts::loadVoice (const PiperVoiceInfo& voice, const Options& opts, juce::String& error)
{
    if (impl->tts != nullptr && impl->voiceId == voice.id
        && juce::approximatelyEqual (impl->noiseScale,  opts.noiseScale)
        && juce::approximatelyEqual (impl->noiseScaleW, opts.noiseScaleW))
        return true;   // already resident

    impl->destroy();

    // Keep the UTF-8 path strings alive across the create call.
    const std::string model  = voice.modelOnnx.getFullPathName().toStdString();
    const std::string tokens = voice.tokensTxt.getFullPathName().toStdString();
    const std::string data   = voice.espeakDataDir.getFullPathName().toStdString();

    SherpaOnnxOfflineTtsConfig config;
    std::memset (&config, 0, sizeof (config));
    config.model.vits.model         = model.c_str();
    config.model.vits.tokens        = tokens.c_str();
    config.model.vits.data_dir      = data.c_str();
    config.model.vits.noise_scale   = opts.noiseScale;
    config.model.vits.noise_scale_w = opts.noiseScaleW;
    config.model.vits.length_scale  = 1.0f;   // speed is applied per generate
    config.model.num_threads        = 2;
    config.model.provider           = "cpu";
    config.max_num_sentences        = 1;      // chunk per sentence → fine progress/cancel

    impl->tts = SherpaOnnxCreateOfflineTts (&config);
    if (impl->tts == nullptr)
    {
        error = "Failed to load voice \"" + voice.id + "\" (invalid or corrupt bundle?)";
        return false;
    }

    impl->voiceId     = voice.id;
    impl->noiseScale  = opts.noiseScale;
    impl->noiseScaleW = opts.noiseScaleW;
    return true;
}

PiperTts::Result PiperTts::synthesize (const juce::String& text, const Options& opts,
                                       std::function<void (float)> onProgress,
                                       std::function<bool()>       shouldCancel)
{
    Result r;
    if (impl->tts == nullptr)
    {
        r.error = "No voice loaded";
        return r;
    }

    struct CallbackCtx
    {
        std::function<void (float)>* progress;
        std::function<bool()>*       cancel;
        bool cancelled = false;
    } ctx { onProgress ? &onProgress : nullptr,
            shouldCancel ? &shouldCancel : nullptr };

    // Chunks only drive progress/cancellation; the sample data is taken from
    // the final GeneratedAudio (chunks and final buffer overlap).
    const auto chunkCb = [] (const float*, int32_t, float p, void* arg) -> int32_t
    {
        auto* c = static_cast<CallbackCtx*> (arg);
        if (c->progress != nullptr)
            (*c->progress) (juce::jlimit (0.0f, 1.0f, p));
        if (c->cancel != nullptr && (*c->cancel)())
        {
            c->cancelled = true;
            return 0;
        }
        return 1;
    };

    SherpaOnnxGenerationConfig gen;
    std::memset (&gen, 0, sizeof (gen));
    gen.sid           = 0;
    gen.speed         = 1.0f / juce::jlimit (0.25f, 4.0f, opts.lengthScale);
    gen.silence_scale = opts.sentenceSilenceScale;

    const auto* audio = SherpaOnnxOfflineTtsGenerateWithConfig (
        impl->tts, text.toRawUTF8(), &gen, chunkCb, &ctx);

    if (ctx.cancelled)
    {
        if (audio != nullptr)
            SherpaOnnxDestroyOfflineTtsGeneratedAudio (audio);
        r.error = "cancelled";
        return r;
    }
    if (audio == nullptr || audio->samples == nullptr || audio->n <= 0)
    {
        if (audio != nullptr)
            SherpaOnnxDestroyOfflineTtsGeneratedAudio (audio);
        r.error = "Synthesis failed (no audio produced)";
        return r;
    }

    r.samples.assign (audio->samples, audio->samples + audio->n);
    r.sampleRate = (int) audio->sample_rate;
    SherpaOnnxDestroyOfflineTtsGeneratedAudio (audio);

    if (onProgress)
        onProgress (1.0f);
    return r;
}

//==============================================================================
#else // ─── stub build (SP3CTRA_ENABLE_TTS=OFF) ───────────────────────────────
//==============================================================================

struct PiperTts::Impl {};

PiperTts::PiperTts() : impl (std::make_unique<Impl>()) {}
PiperTts::~PiperTts() = default;

bool PiperTts::isEngineAvailable()           { return false; }
bool PiperTts::isLoaded() const              { return false; }
juce::String PiperTts::loadedVoiceId() const { return {}; }
void PiperTts::unload()                      {}

bool PiperTts::loadVoice (const PiperVoiceInfo&, const Options&, juce::String& error)
{
    error = "TTS engine not built (SP3CTRA_ENABLE_TTS=OFF)";
    return false;
}

PiperTts::Result PiperTts::synthesize (const juce::String&, const Options&,
                                       std::function<void (float)>,
                                       std::function<bool()>)
{
    Result r;
    r.error = "TTS engine not built (SP3CTRA_ENABLE_TTS=OFF)";
    return r;
}

#endif // SP3CTRA_HAS_TTS

//==============================================================================
// Startup smoke test (both builds — logs "engine not built" when stubbed).
//==============================================================================
void PiperTts::runSmokeTestIfRequested()
{
    const char* env = std::getenv ("SP3CTRA_TTS_SMOKE");
    if (env == nullptr)
        return;

    std::fprintf (stderr, "[TTS SMOKE] engineAvailable=%d externalDir=%s bundleDir=%s\n",
                  (int) isEngineAvailable(),
                  voicesDirectory().getFullPathName().toRawUTF8(),
                  bundleVoicesDirectory().getFullPathName().toRawUTF8());

    const auto voices = listVoices (voicesDirectory());
    std::fprintf (stderr, "[TTS SMOKE] %d voice(s) installed\n", voices.size());
    for (const auto& v : voices)
        std::fprintf (stderr, "[TTS SMOKE]   %s\n", v.displayName().toRawUTF8());

    if (! isEngineAvailable() || voices.isEmpty())
        return;

    const juce::String text = (std::strlen (env) > 1)
        ? juce::String::fromUTF8 (env)
        : juce::String ("Bonjour, ceci est un test de synthese vocale.");

    PiperTts tts;
    juce::String error;
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    if (! tts.loadVoice (voices.getReference (0), {}, error))
    {
        std::fprintf (stderr, "[TTS SMOKE] loadVoice FAILED: %s\n", error.toRawUTF8());
        return;
    }
    auto t1 = juce::Time::getMillisecondCounterHiRes();

    auto res = tts.synthesize (text, {},
                               [] (float p) { std::fprintf (stderr, "[TTS SMOKE] progress %.2f\n", p); },
                               [] { return false; });
    auto t2 = juce::Time::getMillisecondCounterHiRes();

    if (! res.ok())
    {
        std::fprintf (stderr, "[TTS SMOKE] synthesize FAILED: %s\n", res.error.toRawUTF8());
        return;
    }

    const juce::File out ("/tmp/sp3ctra_tts_smoke.wav");
    const bool wrote = writeWavFile (res, out);
    const double audioSec = (double) res.samples.size() / (double) res.sampleRate;
    std::fprintf (stderr,
                  "[TTS SMOKE] OK — load %.0f ms, synth %.0f ms for %.2f s audio "
                  "(RTF %.3f) @ %d Hz → %s (written=%d)\n",
                  t1 - t0, t2 - t1, audioSec, (t2 - t1) / 1000.0 / audioSec,
                  res.sampleRate, out.getFullPathName().toRawUTF8(), (int) wrote);
}
