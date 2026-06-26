/**
 * @file Sp3ctraDeviceClient.h
 * @brief HTTP control-plane client for the Sp3ctra hardware device.
 *
 * The Sp3ctra device talks to the host over TWO independent channels:
 *   - UDP (continuous, real-time CIS image + IMU stream) — handled elsewhere.
 *   - HTTP (request/response configuration) — handled HERE.
 *
 * The device runs an embedded web server (the same one that serves
 * config.html). This class is simply a *second client* of that REST-ish API:
 * it GETs every setting to populate the Setup panel and POSTs changes back.
 * No new protocol, no firmware change.
 *
 * Design rules (see SourceSetupPanel):
 *   - The DEVICE is the source of truth for everything here. We never persist
 *     these values in the DAW state (except the transport params the VST needs
 *     to even reach the device: IP / UDP port / DPI — those live in APVTS).
 *   - All network I/O runs on a background ThreadPool; results are marshalled
 *     back to the message thread via MessageManager::callAsync.
 *   - A monotonic "generation" token drops stale callbacks after cancel() /
 *     a new loadAll() / a host change, so a slow reply can never clobber the UI
 *     of a panel that has since been closed or re-pointed at another device.
 *   - HTTP is connectionless: there is nothing to keep alive between requests.
 *     "Connection only while Setup is open" == just stop issuing requests.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <memory>
#include <atomic>

class Sp3ctraDeviceClient
{
public:
    Sp3ctraDeviceClient();
    ~Sp3ctraDeviceClient();

    enum class State { Idle, Connecting, Connected, Failed };

    struct MidiButton { int channel = 0; int command = 0; int param = 0; };

    struct NetworkConfig
    {
        juce::String ip       = "192.168.100.1";
        juce::String mask     = "255.255.255.0";
        juce::String gateway  = "192.168.100.1";
        juce::String destIp   = "192.168.100.2";
        int udpPort           = 0;
        int rtpMidiControlPort = 0;
    };

    /** A full snapshot of every device setting exposed by config.html. */
    struct DeviceConfig
    {
        // CIS
        int   dpi               = 400;   // 200 | 400  (also drives UDP parsing)
        int   oversampling      = 1;     // 1,2,4,8,16,32
        int   handedness        = 1;     // 0 = left, 1 = right
        int   freqLps           = 0;     // read-only (lines per second)
        // IMU
        int   gyroSensitivity   = 0;     // 0..7
        int   accelSensitivity  = 0;     // 0..3
        // GUI / screensaver
        bool  guiShowImu        = false;
        bool  guiInvertCis      = false;
        int   screensaverTimeout = 60;   // seconds
        float motionThresholdAcc = 0.1f; // g
        float motionThresholdGyro = 1.0f;// dps
        // MIDI button mapping (SW1..SW3)
        MidiButton midi[3];
        // Network / MIDI transport
        bool  mdnsEnabled       = false;
        int   rtpMidiMode       = 0;     // 0 = server, 1 = client
        NetworkConfig network;
        juce::String firmwareVersion;

        bool valid = false;              // true only if loadAll() fully succeeded
    };

    //==========================================================================
    /** Set the device HTTP endpoint. Bumps the generation (drops pending). */
    void setHost (juce::String ipAddress, int httpPort = 80);
    juce::String getHost() const;

    /** GET every endpoint on a background thread; deliver the result on the
        message thread. State is Connecting → Connected | Failed. */
    void loadAll (std::function<void (State, DeviceConfig)> onComplete);

    /** POST a form-urlencoded body to an endpoint; onComplete(success) on the
        message thread. */
    void postForm (const juce::String& endpoint,
                   const juce::String& body,
                   std::function<void (bool)> onComplete);

    /** Multipart firmware upload to /upload (field name "firmware").
        onProgress(0..1) is called on the message thread; onComplete(success)
        once finished. */
    void uploadFirmware (const juce::File& binFile,
                         std::function<void (double)> onProgress,
                         std::function<void (bool)> onComplete);

    /** Drop all pending callbacks (call when the Setup panel is hidden). */
    void cancel();

private:
    /** "http://<host>:<port>" snapshot — taken on the message thread, passed to
        the pool job by value so the job never touches `this`. */
    juce::String hostUrl() const;

    /** Supersede any in-flight request; returns the new generation token. */
    int bumpGeneration();

    // Blocking helpers — run ONLY on the pool thread. Full URLs, no shared state.
    static bool httpGet  (const juce::String& url, juce::String& bodyOut);
    static bool httpPost (const juce::String& url, const juce::String& body);

    juce::CriticalSection hostLock;
    juce::String host = "192.168.100.1";
    int port = 80;

    // Shared so queued message-thread callbacks can compare against it even
    // after this client has been destroyed (panel closed mid-request).
    std::shared_ptr<std::atomic<int>> generation = std::make_shared<std::atomic<int>> (0);

    // Declared LAST → destroyed FIRST: its destructor blocks until the running
    // job returns, while the rest of the members are still alive.
    juce::ThreadPool pool { juce::ThreadPool::Options{}
                                .withThreadName ("Sp3ctraDeviceHTTP")
                                .withNumberOfThreads (1) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sp3ctraDeviceClient)
};
