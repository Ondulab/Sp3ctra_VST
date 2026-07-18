#include "Sp3ctraSharedCore.h"
#include "LuxSynthProcessingThread.h"

#include <juce_core/juce_core.h>

extern "C"
{
    #include "core/context.h"
    #include "config/config_loader.h"
    #include "utils/logger.h"
    #include "synthesis/luxstral/synth_luxstral.h"          // synth_IfftInit / synth_luxstral_cleanup
    #include "synthesis/luxstral/synth_luxstral_threading.h" // synth_shutdown_thread_pool
    #include "synthesis/luxstral/synth_luxstral_runtime.h"   // synth_runtime_free_buffers
    #include "synthesis/luxstral/vst_adapters.h"             // luxstral_init_audio_buffers / luxstral_init_callback_sync
    #include "synthesis/luxstral/wave_generation.h"          // request_frequency_reinit / reset_frequency_reinit_state
    #include "synthesis/luxsynth/luxsynth_vst_adapter.h"     // luxsynth_init_audio_buffers / luxsynth_engine_init / luxsynth_free_audio_buffers
    #include "synthesis/luxwave/luxwave_vst_adapter.h"       // g_luxwave_engine / luxwave_engine_init
}

// ============================================================================
// Static members
// ============================================================================
std::weak_ptr<Sp3ctraSharedCore> Sp3ctraSharedCore::s_instance;
std::mutex                       Sp3ctraSharedCore::s_mutex;

// ============================================================================
// Constructor / Destructor
// ============================================================================

Sp3ctraSharedCore::Sp3ctraSharedCore()
{
    log_info("SHARED", "Sp3ctraSharedCore: Constructor — creating shared resource owner");
    core = std::make_unique<Sp3ctraCore>();
}

Sp3ctraSharedCore::~Sp3ctraSharedCore()
{
    log_info("SHARED", "Sp3ctraSharedCore: Destructor — last instance released, tearing down");
    stopThreads();
    log_info("SHARED", "Sp3ctraSharedCore: Destructor complete");
}

// ============================================================================
// Singleton acquire
// ============================================================================

std::shared_ptr<Sp3ctraSharedCore> Sp3ctraSharedCore::acquire()
{
    std::lock_guard<std::mutex> lock(s_mutex);

    // Try to promote the weak pointer to a shared pointer.
    if (auto existing = s_instance.lock())
    {
        log_info("SHARED", "acquire() — reusing existing singleton (ref-count: %ld)",
                 existing.use_count() + 1 /* +1 for the copy we're about to return */);
        return existing;
    }

    // Weak pointer expired → no live instance, create a new one.
    // We use a custom deleter so the destructor can stay private if desired,
    // but here it is public so a plain shared_ptr works fine.
    auto instance = std::shared_ptr<Sp3ctraSharedCore>(new Sp3ctraSharedCore());
    s_instance = instance;

    log_info("SHARED", "acquire() — new singleton created");
    return instance;
}

// ============================================================================
// Lifecycle
// ============================================================================

bool Sp3ctraSharedCore::startWithConfig(const Sp3ctraCore::ActiveConfig& config,
                                        int   luxstralPixelsPerNote,
                                        double sampleRate,
                                        int    samplesPerBlock)
{
    // ── Idempotency guard ────────────────────────────────────────────────────
    if (ready.load())
    {
        log_info("SHARED", "startWithConfig() — already running, skipping (second instance)");
        return true;
    }

    log_info("SHARED",
             "startWithConfig() — first instance, starting pipeline "
             "(SR=%.0f Hz, BS=%d, ppn=%d)",
             sampleRate, samplesPerBlock, luxstralPixelsPerNote);

    // ── 1. Core init: UDP socket + AudioImageBuffers + DoubleBuffer ──────────
    if (!core->initialize(config))
    {
        log_error("SHARED", "startWithConfig() — core->initialize() failed");
        return false;
    }

    // ── 2. UDP receiver thread ───────────────────────────────────────────────
    udpThread = std::make_unique<UdpReceiverThread>(core.get());
    udpThread->startThread();
    log_info("SHARED", "UdpReceiverThread started");

    // ── 3. Audio-side globals ────────────────────────────────────────────────
    // Expose sample-rate / buffer-size to the global C config so that
    // LuxStral computes the correct Nyquist-clamped frequency table.
    extern sp3ctra_config_t g_sp3ctra_config;
    g_sp3ctra_config.sampling_frequency = static_cast<int>(sampleRate);
    g_sp3ctra_config.audio_buffer_size  = samplesPerBlock;

    // ── 4. LuxStral audio output double-buffer ───────────────────────────────
    if (luxstral_init_audio_buffers(samplesPerBlock) != 0)
    {
        log_error("SHARED", "startWithConfig() — luxstral_init_audio_buffers() failed");
        stopThreads();
        return false;
    }

    // ── 5. LuxStral synthesis engine ─────────────────────────────────────────
    luxstral_init_callback_sync();

    int synthResult = synth_IfftInit();
    if (synthResult != 0)
    {
        log_error("SHARED", "startWithConfig() — synth_IfftInit() failed (rc=%d)", synthResult);
        stopThreads();
        return false;
    }

    log_info("SHARED", "LuxStral initialized (pixels_per_note=%d, notes=%d)",
             luxstralPixelsPerNote,
             get_cis_pixels_nb() / (luxstralPixelsPerNote > 0 ? luxstralPixelsPerNote : 1));

    // ── 6. Clear any stale frequency-reinit BEFORE the audio thread starts ──
    // synth_IfftInit() just built the wavetables (init_waves + gap-limiter
    // coefficients + phase randomization) from the current — already restored —
    // config: queuing a reinit here would only regenerate the same table a
    // second time. A request left pending from before the pipeline was up is
    // stale; clearing it also restores the global fade that request had sent
    // toward 0 with nobody to complete it.
    reset_frequency_reinit_state();

    // ── 7. Audio processing thread (LuxStral) ────────────────────────────────
    audioThread = std::make_unique<AudioProcessingThread>(core.get());
    audioThread->startThread(juce::Thread::Priority::highest);
    log_info("SHARED", "AudioProcessingThread started");

    // ── 8. LuxSynth additive synthesis engine ────────────────────────────────
    if (luxsynth_init_audio_buffers(samplesPerBlock) != 0)
    {
        log_error("SHARED", "startWithConfig() — luxsynth_init_audio_buffers() failed");
        // Non-fatal: LuxStral still works, LuxSynth just won't produce audio
    }
    else
    {
        int lsResult = luxsynth_engine_init(&g_luxsynth_engine,
                                            static_cast<float>(sampleRate),
                                            samplesPerBlock);
        if (lsResult != 0)
        {
            log_error("SHARED", "startWithConfig() — luxsynth_engine_init() failed (rc=%d)", lsResult);
        }
        else
        {
            // LuxSynth engine is now called inline from processBlock (RT-safe).
            // No dedicated thread needed — eliminates double-buffer sync issues.
            log_info("SHARED", "LuxSynth engine initialized inline (SR=%.0f, BS=%d)",
                     sampleRate, samplesPerBlock);
        }
    }

    // ── 9. LuxWave wavetable synthesis engine ─────────────────────────────────
    {
        int lwResult = luxwave_engine_init(&g_luxwave_engine,
                                           static_cast<float>(sampleRate),
                                           samplesPerBlock);
        if (lwResult != 0)
        {
            log_error("SHARED", "startWithConfig() — luxwave_engine_init() failed (rc=%d)", lwResult);
            // Non-fatal: other engines still work
        }
        else
        {
            log_info("SHARED", "LuxWave engine initialized inline (SR=%.0f, BS=%d)",
                     sampleRate, samplesPerBlock);
        }
    }

    ready.store(true);
    log_info("SHARED", "startWithConfig() — pipeline up and running");
    return true;
}

bool Sp3ctraSharedCore::ensureAudioBufferSize(int samplesPerBlock)
{
    if (!ready.load())
        return true;    // startWithConfig() will size the buffers itself

    if (luxstral_get_audio_buffer_size() == samplesPerBlock)
        return true;    // already the right size

    // Another live plugin instance may be reading the shared output buffers
    // from ITS processBlock right now — reallocating would free memory under
    // it. Only the sole owner may resize; other callers keep the old size and
    // clamp their reads.
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_instance.use_count() > 1)
        {
            log_warning("SHARED",
                        "ensureAudioBufferSize(%d) — %ld instances share the core, "
                        "keeping current size %d (reads are clamped)",
                        samplesPerBlock,
                        static_cast<long>(s_instance.use_count()),
                        luxstral_get_audio_buffer_size());
            return false;
        }
    }

    log_info("SHARED", "ensureAudioBufferSize() — host buffer size changed %d → %d, "
                       "reallocating output buffers",
             luxstral_get_audio_buffer_size(), samplesPerBlock);

    // Stop the producer while the buffers it writes to are reallocated.
    bool restartAudioThread = false;
    if (audioThread)
    {
        audioThread->requestStop();
        if (!audioThread->stopThread(2000))
        {
            log_error("SHARED", "ensureAudioBufferSize() — synthesis thread did not "
                                "stop, ABORTING reallocation (old size kept)");
            return false;   // never free buffers under a live producer
        }
        audioThread.reset();
        restartAudioThread = true;
    }

    // Reallocate FIRST, then commit the new size to the global config — the
    // engine sizes its writes on g_sp3ctra_config.audio_buffer_size, so
    // committing before a failed (OOM) realloc would make the producer write
    // the new size into freed/NULL buffers.
    extern sp3ctra_config_t g_sp3ctra_config;
    const int  oldSize = luxstral_get_audio_buffer_size();
    bool ok = (luxstral_init_audio_buffers(samplesPerBlock) == 0);
    if (ok)
        g_sp3ctra_config.audio_buffer_size = samplesPerBlock;
    else
    {
        log_error("SHARED", "ensureAudioBufferSize() — luxstral_init_audio_buffers(%d) "
                            "failed, trying to restore the previous size (%d)",
                  samplesPerBlock, oldSize);
        if (oldSize > 0 && luxstral_init_audio_buffers(oldSize) == 0)
            g_sp3ctra_config.audio_buffer_size = oldSize;
    }

    if (restartAudioThread)
    {
        if (luxstral_are_audio_buffers_ready())
        {
            audioThread = std::make_unique<AudioProcessingThread>(core.get());
            audioThread->startThread(juce::Thread::Priority::highest);
            log_info("SHARED", "ensureAudioBufferSize() — synthesis thread restarted");
        }
        else
            // Both reallocations failed (OOM): leave the producer stopped —
            // silence, but no writes into NULL output buffers.
            log_error("SHARED", "ensureAudioBufferSize() — output buffers unavailable, "
                                "synthesis thread NOT restarted");
    }
    return ok;
}

void Sp3ctraSharedCore::stopThreads()
{
    if (!ready.load() && !audioThread && !luxSynthThread && !udpThread)
    {
        log_info("SHARED", "stopThreads() — nothing to stop");
        return;
    }

    log_info("SHARED", "stopThreads() — stopping all shared threads");

    // ── LuxSynthProcessingThread ─────────────────────────────────────────────
    // Stop before AudioProcessingThread to ensure clean shutdown order.
    if (luxSynthThread)
    {
        log_info("SHARED", "Stopping LuxSynthProcessingThread...");
        luxSynthThread->requestStop();
        bool stopped = luxSynthThread->stopThread(2000);

        if (!stopped)
        {
            log_error("SHARED",
                      "LuxSynthProcessingThread did NOT exit within timeout — leaking to avoid crash");
            (void)luxSynthThread.release(); // NOLINT: intentional leak
        }
        else
        {
            luxSynthThread.reset();
            log_info("SHARED", "LuxSynthProcessingThread stopped");
        }
    }

    // ── LuxSynth cleanup ─────────────────────────────────────────────────────
    luxsynth_free_audio_buffers();
    log_info("SHARED", "LuxSynth buffers freed");

    // ── AudioProcessingThread (LuxStral) ─────────────────────────────────────
    // Must stop before calling synth_luxstral_cleanup() to avoid use-after-free.
    bool audioThreadLeaked = false;
    if (audioThread)
    {
        log_info("SHARED", "Stopping AudioProcessingThread...");
        audioThread->requestStop();
        bool stopped = audioThread->stopThread(2000);

        if (!stopped)
        {
            // Timeout: leak the thread object rather than risk a PAC crash
            // on ARM64 by destroying a still-running thread.
            log_error("SHARED",
                      "AudioProcessingThread did NOT exit within timeout — leaking to avoid crash");
            (void)audioThread.release(); // NOLINT: intentional leak
            audioThreadLeaked = true;
        }
        else
        {
            audioThread.reset();
            log_info("SHARED", "AudioProcessingThread stopped");
        }
    }

    // ── LuxStral cleanup ─────────────────────────────────────────────────────
    // Only safe once the audio thread is confirmed stopped. If the thread was
    // leaked above it may still be running inside the synthesis code — freeing
    // the pool/buffers it uses would turn the intentional leak back into a
    // use-after-free crash, so leak those too.
    if (!audioThreadLeaked)
    {
        log_info("SHARED", "Cleaning up LuxStral engine...");
        synth_shutdown_thread_pool();
        synth_runtime_free_buffers();
        synth_luxstral_cleanup();
        log_info("SHARED", "LuxStral cleanup complete");
    }
    else
        log_error("SHARED", "Skipping LuxStral cleanup — leaked synthesis thread may "
                            "still dereference its pool/buffers");

    // ── UDP receiver thread ───────────────────────────────────────────────────
    if (udpThread)
    {
        log_info("SHARED", "Stopping UdpReceiverThread...");
        udpThread->requestStop();
        udpThread->stopThread(2000);
        udpThread.reset();
        log_info("SHARED", "UdpReceiverThread stopped");
    }

    // ── Core shutdown (closes socket, frees AudioImageBuffers + DoubleBuffer) ─
    if (core)
    {
        if (!audioThreadLeaked)
        {
            log_info("SHARED", "Shutting down core...");
            core->shutdown();
            // Do NOT reset core — it is referenced by the threads via raw pointer.
            // The unique_ptr destructs naturally with this object.
            log_info("SHARED", "Core shutdown complete");
        }
        else
        {
            // The leaked synthesis thread still reads AudioImageBuffers and the
            // DoubleBuffer owned by the core — close the socket but leak the
            // buffers (bounded, process is unloading the plugin anyway).
            log_error("SHARED", "Skipping core buffer teardown (leaked synthesis "
                                "thread) — closing UDP socket only");
            core->closeUdpSocket();
        }
    }

    ready.store(false);
    log_info("SHARED", "stopThreads() — all shared threads stopped");
}

// ============================================================================
// State queries
// ============================================================================

bool Sp3ctraSharedCore::isUdpRunning() const
{
    if (core)
        return core->isUdpRunning();
    return false;
}

// ============================================================================
// UDP hot-reload
// ============================================================================

bool Sp3ctraSharedCore::restartUdp(int port,
                                    const std::string& address,
                                    const std::string& iface)
{
    if (!ready.load() || !core)
    {
        log_error("SHARED", "restartUdp() — shared core not ready");
        return false;
    }

    log_info("SHARED", "restartUdp() — restarting UDP on %s:%d", address.c_str(), port);

    // Stop receiver thread FIRST so recvfrom() is not blocked on the old socket.
    if (udpThread)
    {
        udpThread->requestStop();
        core->closeUdpSocket(); // unblocks recvfrom()
        udpThread->stopThread(1500);
        udpThread.reset();
    }

    // Restart socket via Sp3ctraCore.
    if (!core->restartUdp(port, address, iface))
    {
        log_error("SHARED", "restartUdp() — core->restartUdp() failed");
        return false;
    }

    // Restart receiver thread with the new socket.
    udpThread = std::make_unique<UdpReceiverThread>(core.get());
    udpThread->startThread();

    log_info("SHARED", "restartUdp() — done (%s:%d)", address.c_str(), port);
    return true;
}
