#include "Sp3ctraSharedCore.h"

#include <juce_core/juce_core.h>

extern "C"
{
    #include "core/context.h"
    #include "config/config_loader.h"
    #include "utils/logger.h"
    #include "luxstral/synth_luxstral.h"          // synth_IfftInit / synth_luxstral_cleanup
    #include "luxstral/synth_luxstral_threading.h" // synth_shutdown_thread_pool
    #include "luxstral/synth_luxstral_runtime.h"   // synth_runtime_free_buffers
    #include "luxstral/vst_adapters.h"             // luxstral_init_audio_buffers / luxstral_init_callback_sync
    #include "luxstral/wave_generation.h"          // request_frequency_reinit / reset_frequency_reinit_state
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

    // ── 6. Queue wavetable init BEFORE audio thread starts ──────────────────
    // synth_IfftInit() calloc's waves[] → all start_ptr fields are NULL.
    // init_waves() (called by check_and_process_frequency_reinit inside
    // synth_IfftMode) must run before synth_precompute_wave_data — otherwise
    // the thread would dereference NULL → SIGSEGV.
    reset_frequency_reinit_state();
    request_frequency_reinit(); // PENDING is set BEFORE the thread starts → safe

    // ── 7. Audio processing thread ───────────────────────────────────────────
    audioThread = std::make_unique<AudioProcessingThread>(core.get());
    audioThread->startThread(juce::Thread::Priority::highest);
    log_info("SHARED", "AudioProcessingThread started");

    ready.store(true);
    log_info("SHARED", "startWithConfig() — pipeline up and running");
    return true;
}

void Sp3ctraSharedCore::stopThreads()
{
    if (!ready.load() && !audioThread && !udpThread)
    {
        log_info("SHARED", "stopThreads() — nothing to stop");
        return;
    }

    log_info("SHARED", "stopThreads() — stopping all shared threads");

    // ── AudioProcessingThread first ──────────────────────────────────────────
    // Must stop before calling synth_luxstral_cleanup() to avoid use-after-free.
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
        }
        else
        {
            audioThread.reset();
            log_info("SHARED", "AudioProcessingThread stopped");
        }
    }

    // ── LuxStral cleanup ─────────────────────────────────────────────────────
    // Only safe once the audio thread is confirmed stopped.
    log_info("SHARED", "Cleaning up LuxStral engine...");
    synth_shutdown_thread_pool();
    synth_runtime_free_buffers();
    synth_luxstral_cleanup();
    log_info("SHARED", "LuxStral cleanup complete");

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
        log_info("SHARED", "Shutting down core...");
        core->shutdown();
        // Do NOT reset core — it is referenced by the threads via raw pointer.
        // The unique_ptr destructs naturally with this object.
        log_info("SHARED", "Core shutdown complete");
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
