#pragma once

#include <memory>
#include <mutex>
#include <atomic>

#include "Sp3ctraCore.h"
#include "UdpReceiverThread.h"
#include "AudioProcessingThread.h"

// Forward declaration — full include in .cpp to avoid transitive C header issues
class LuxSynthProcessingThread;

/**
 * @file Sp3ctraSharedCore.h
 * @brief Process-wide singleton that owns all shared, expensive resources.
 *
 * MOTIVATION
 * ----------
 * The image-acquisition pipeline (UDP reception + CIS frame decoding) is the
 * heaviest part of Sp3ctra. Running it twice when two plugin instances are
 * loaded in the same DAW session wastes CPU and creates a port-conflict on the
 * UDP socket (only one process can bind a given unicast port).
 *
 * DESIGN
 * ------
 * Sp3ctraSharedCore is a reference-counted singleton. The first plugin instance
 * to call acquire() creates the singleton and starts all shared threads. Every
 * subsequent call returns the same shared_ptr, incrementing the ref-count. When
 * the last owner is destroyed the shared_ptr destructor fires, bringing down
 * threads and freeing buffers exactly once.
 *
 * WHAT IS SHARED (this class)
 * ---------------------------
 *  - UDP socket + UdpReceiverThread
 *  - AudioImageBuffers   (raw RGB ring-buffer from the scanner)
 *  - DoubleBuffer        (preprocessed image, ready for synthesis)
 *  - Context             (shared C state)
 *  - Global C inits      (displayable_synth_buffers, image_preprocess, …)
 *  - AudioProcessingThread + LuxStral engine (one synthesis loop, shared)
 *
 * WHAT STAYS PER-INSTANCE (PluginProcessor)
 * ------------------------------------------
 *  - APVTS parameter tree
 *  - LuxSampler
 *  - processBlock read-pointer tracking (lastConsumedReadIdx)
 *
 * PHASE-2 NOTE
 * ------------
 * Because the C synthesis engine uses global state (g_sp3ctra_config,
 * luxstral_buffers_L/R, …), all instances currently produce identical audio.
 * Phase 2 will make audio output per-instance by heap-allocating those buffers
 * and passing the config by pointer to the synthesis functions.
 *
 * THREAD SAFETY
 * -------------
 *  - acquire() / destructor : protected by s_mutex (main thread only)
 *  - startWithConfig()      : main thread only, called once by first instance
 *  - All accessors          : safe to call from any thread after startWithConfig()
 */
class Sp3ctraSharedCore
{
public:
    // -------------------------------------------------------------------------
    // Public singleton API
    // -------------------------------------------------------------------------

    /**
     * @brief Acquire (or create) the process-wide shared core.
     *
     * Thread-safe with respect to other acquire() / destructor calls.
     * Must be called from the main (message) thread.
     *
     * @return Shared ownership pointer. Releasing the last copy tears everything
     *         down automatically via the destructor.
     */
    static std::shared_ptr<Sp3ctraSharedCore> acquire();

    // -------------------------------------------------------------------------
    // Lifecycle (called by the first plugin instance to acquire)
    // -------------------------------------------------------------------------

    /**
     * @brief Start UDP reception, image preprocessing and synthesis.
     *
     * No-op if already started (second instance acquires an already-running
     * singleton). Must be called from the main thread.
     *
     * @param config  UDP / log-level configuration from the first instance's APVTS.
     * @param luxstralPixelsPerNote  pixels_per_note value from g_sp3ctra_config.
     * @param sampleRate             Current DAW sample rate.
     * @param samplesPerBlock        Current DAW buffer size.
     * @return true on success, false on failure (log printed internally).
     */
    bool startWithConfig(const Sp3ctraCore::ActiveConfig& config,
                         int luxstralPixelsPerNote,
                         double sampleRate,
                         int samplesPerBlock);

    /**
     * @brief Stop and join all threads owned by this singleton.
     *
     * Called automatically from the destructor. May also be called explicitly
     * to drain threads before DAW shutdown. Safe to call multiple times.
     */
    void stopThreads();

    // -------------------------------------------------------------------------
    // State queries
    // -------------------------------------------------------------------------

    /** @return true once startWithConfig() completed successfully. */
    bool isReady() const { return ready.load(); }

    /** @return true if the UDP thread is currently running. */
    bool isUdpRunning() const;

    // -------------------------------------------------------------------------
    // Resource accessors (valid only after isReady() == true)
    // -------------------------------------------------------------------------

    Sp3ctraCore*          getCore()         { return core.get(); }
    UdpReceiverThread*    getUdpThread()    { return udpThread.get(); }
    AudioProcessingThread* getAudioThread() { return audioThread.get(); }

    // -------------------------------------------------------------------------
    // UDP hot-reload (delegates to Sp3ctraCore)
    // -------------------------------------------------------------------------

    /**
     * @brief Restart the UDP socket with new address / port.
     *
     * Stops the receiver thread, closes the old socket, opens a new one, then
     * restarts the receiver. Main thread only.
     */
    bool restartUdp(int port, const std::string& address, const std::string& iface);

    // -------------------------------------------------------------------------
    // Destructor (public for shared_ptr)
    // -------------------------------------------------------------------------
    ~Sp3ctraSharedCore();

private:
    // Private constructor — only acquire() may create instances.
    Sp3ctraSharedCore();

    // Non-copyable / non-movable
    Sp3ctraSharedCore(const Sp3ctraSharedCore&) = delete;
    Sp3ctraSharedCore& operator=(const Sp3ctraSharedCore&) = delete;

    // -------------------------------------------------------------------------
    // Singleton bookkeeping
    // -------------------------------------------------------------------------
    static std::weak_ptr<Sp3ctraSharedCore> s_instance; ///< Weak ref to live singleton
    static std::mutex                       s_mutex;    ///< Guards s_instance access

    // -------------------------------------------------------------------------
    // Owned resources
    // -------------------------------------------------------------------------
    std::unique_ptr<Sp3ctraCore>              core;
    std::unique_ptr<UdpReceiverThread>        udpThread;
    std::unique_ptr<AudioProcessingThread>    audioThread;
    std::unique_ptr<LuxSynthProcessingThread> luxSynthThread;

    std::atomic<bool> ready{false};
};
