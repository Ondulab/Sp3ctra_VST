/**
 * @file VoiceGenJob.h
 * @brief Worker thread of the VOICE module: text → Piper TTS → WAV cache →
 *        scoregen::renderScore() → spectral page, off the message thread.
 *
 * Same shape as ScoreGenJob: onProgress/onDone fire FROM THE WORKER; the owner
 * marshals to the message thread (juce::MessageManager::callAsync). The
 * PiperTts engine lives here so the loaded voice stays resident between
 * generations (~600 ms model load paid only on voice change); all PiperTts
 * calls stay on this thread, honouring its single-thread contract.
 */
#pragma once

#include <juce_core/juce_core.h>
#include "../image/ScoreGenRenderer.h"
#include "PiperTts.h"

class VoiceGenJob : public juce::Thread
{
public:
    struct Request
    {
        juce::String      text;
        PiperVoiceInfo    voice;
        PiperTts::Options opts;
        ScoreSettings     score {};       ///< freq range already set by the caller
        juce::File        wavFile;        ///< synth output / render input cache
        bool              renderOnly = false; ///< skip TTS, encode wavFile as-is
                                              ///< (session-restore replay)
    };

    struct Result
    {
        scoregen::RenderResult render;
        juce::File   wavFile;
        double       audioSeconds = 0.0;
        juce::String voiceId;
        juce::String error;               ///< non-empty = failed before encoding

        bool ok() const { return error.isEmpty() && render.ok; }
    };

    VoiceGenJob() : juce::Thread ("Sp3ctraVoiceGen") {}
    ~VoiceGenJob() override { stopThread (4000); }

    /** Both fire from the worker thread. */
    std::function<void (float)>  onProgress;
    std::function<void (Result)> onDone;

    /** Starts a generation; cancels any in-flight one first. */
    void start (Request r)
    {
        stopThread (4000);
        req_ = std::move (r);
        startThread();
    }

    /** Frees the resident TTS engine (voice model RAM). Message thread, only
     *  meaningful while idle — a no-op when a generation is running. */
    void unloadEngine()
    {
        if (! isThreadRunning())
            tts_.unload();
    }

    bool engineLoaded() const { return tts_.isLoaded(); }

    void run() override
    {
        Result out;
        out.wavFile = req_.wavFile;
        out.voiceId = req_.voice.id;

        auto report = [this] (float p) { if (onProgress) onProgress (p); };
        auto cancelled = [this] { return threadShouldExit(); };

        if (! req_.renderOnly)
        {
            if (! PiperTts::isEngineAvailable())
            {
                out.error = "TTS engine not built (SP3CTRA_ENABLE_TTS=OFF)";
                finish (std::move (out));
                return;
            }

            juce::String loadError;
            if (! tts_.loadVoice (req_.voice, req_.opts, loadError))
            {
                out.error = loadError;
                finish (std::move (out));
                return;
            }
            report (0.10f);
            if (cancelled()) return;

            auto speech = tts_.synthesize (
                req_.text, req_.opts,
                [&] (float p) { report (0.10f + 0.35f * p); },
                cancelled);
            if (cancelled()) return;
            if (! speech.ok())
            {
                out.error = speech.error;
                finish (std::move (out));
                return;
            }
            out.audioSeconds = (double) speech.samples.size() / (double) speech.sampleRate;

            if (! PiperTts::writeWavFile (speech, req_.wavFile))
            {
                out.error = "Cannot write " + req_.wavFile.getFullPathName();
                finish (std::move (out));
                return;
            }
        }
        else
        {
            const auto info = scoregen::probeWav (req_.wavFile);
            if (! info.ok)
            {
                out.error = "Cached render missing (" + req_.wavFile.getFileName()
                          + ") — GENERATE again";
                finish (std::move (out));
                return;
            }
            out.audioSeconds = info.durationSec;
        }
        report (0.50f);

        out.render = scoregen::renderScore (
            req_.wavFile, req_.score,
            [&] (float p) { report (0.50f + 0.50f * p); },
            cancelled);

        if (! cancelled())
            finish (std::move (out));
    }

private:
    void finish (Result&& out)
    {
        if (onDone)
            onDone (std::move (out));
    }

    Request  req_;
    PiperTts tts_;   // engine cache — survives across generations

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceGenJob)
};
