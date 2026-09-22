/**
 * @file Sp3ctraLink.h
 * @brief Sp3ctra Link (SLP v1) host side — discovery, exclusive session and
 *        host → device feedback over the CONTROL channel (UDP 55150).
 *
 * One instance per PROCESS, owned by Sp3ctraSharedCore (the device is a
 * machine resource, like the STREAM socket). A dedicated thread owns the
 * control socket and runs the state machine:
 *
 *   Searching ──ANNOUNCE + policy──▶ Binding ──BIND_ACK──▶ Bound
 *       ▲                              │ 10 × 300 ms         │ 3 s without PONG
 *       └──────────────────────────────┴─────────────────────┘
 *
 *  - HELLO is broadcast on every interface (a.b.c.255) every second while
 *    searching (every 3 s while bound, to keep the device list fresh) plus
 *    unicast to the manual hosts.
 *  - Bind policy: the preferred UID when it is seen, otherwise the only free
 *    device on the network when auto-bind is on. A BUSY answer parks that
 *    device for a few seconds.
 *  - The STREAM target sent in BIND is the plugin's own listen port; the
 *    device streams to the address the BIND came from (or to the multicast
 *    group when the configured listen address is multicast).
 *  - Feedback (LED_SET / OLED_OVERLAY / OLED_CLEAR / CAL_START / CFG_GET /
 *    CFG_SET) is queued by the message thread and flushed by the link thread,
 *    coalesced. Device SETTINGS travel here, not over the web server: an HTTP
 *    POST needs the admin password (random, generated on the device's first
 *    boot and shown once on its screen), while the bound session is already
 *    the proof of ownership. CFG_REPLY values land in a small cache the pages
 *    read back (a change message is broadcast on every reply).
 *
 * Thread-safety: every public method is callable from any thread; readers get
 * copies under a lock; listeners are notified through ChangeBroadcaster
 * (asynchronous, message thread).
 */
#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <vector>
#include "sp3ctra_link.h"

class Sp3ctraLink : public juce::ChangeBroadcaster,
                    private juce::Thread
{
public:
    enum class State { Off, Searching, Binding, Bound };

    struct DeviceInfo
    {
        juce::String uid;          // 24 hex chars
        juce::String name;         // "Sp3ctra-XXXX"
        juce::String ip;
        juce::String mac;          // "02:53:33:xx:xx:xx"
        int          fw[3] { 0, 0, 0 };
        int          hwRevision = 0;
        int          protoMin = 0;
        bool         bound = false;         // a session is active on the device
        juce::String boundPeerIp;
        uint32_t     features = 0;
        int          nButtons = 0, nLeds = 0, ledKind = 0, imuKind = 0;
        int          displayW = 0, displayH = 0, displayBpp = 0;
        std::vector<int> dpis, pixelsAtDpi;
        int          lineRateMax = 0, hidRateMax = 0;
        double       lastSeenMs = 0;        // Time::getMillisecondCounterHiRes()
        double       busyUntilMs = 0;       // do not retry BIND before this

        juce::String fwString() const { return juce::String (fw[0]) + "." + juce::String (fw[1]) + "." + juce::String (fw[2]); }
        bool supported() const { return protoMin <= (int) SLP_VERSION && fw[0] >= 4; }
    };

    struct Layout
    {
        int dpi = 0, pixelsPerLine = 0, fragmentCount = 0, fragmentPixels = 0;
        int linePacketBytes = 0, hidRateHz = 0, hidValidMask = 0, sessionTimeoutMs = 0;
    };

    struct Status
    {
        State        state = State::Off;
        uint32_t     generation = 0;      // +1 on every state / layout / device change
        juce::String deviceUid, deviceName, deviceIp;
        Layout       layout;
        // last PONG
        uint32_t     uptimeMs = 0, linesSent = 0, pongCount = 0;
        int          lineRateLps = 0, calState = 0, calProgress = 0, rttMs = 0;
        float        tempC = 0.0f;
        bool         deviceStreaming = false;
        bool         calRunning = false;
        int          errorCount = 0;
        juce::String lastError;
    };

    struct OverlayItem
    {
        juce::String label, value;
        float norm = -1.0f;                // < 0 = no bar
        bool  bipolar = false, highlight = false;
        bool  tagInvert = false;           // label starts with "Cn " drawn inverted
    };

    Sp3ctraLink();
    ~Sp3ctraLink() override;

    //── lifecycle (Sp3ctraSharedCore) ────────────────────────────────────────
    /** Configure the STREAM target announced in BIND and start the thread. */
    void start (int streamPort, const juce::String& streamAddress);
    void stop();
    /** Re-announce a new STREAM target (UDP restart): re-BINDs the current device. */
    void setStreamTarget (int streamPort, const juce::String& streamAddress);

    //── policy (SETUP page, machine prefs) ───────────────────────────────────
    void setManualHosts (const juce::StringArray& ips);
    void setPreferredUid (const juce::String& uid);
    void setAutoBind (bool on);
    void bindTo (const juce::String& uid);      // explicit USE
    void unbind();                              // release the device

    //── feedback (message thread, coalesced) ─────────────────────────────────
    void setLed (int index, const slp_led_cmd& cmd);
    void setOverlay (const std::vector<OverlayItem>& items, int ttlMs);
    void clearOverlay();
    void requestCalibration (int slpCalKind);

    //── device configuration over the session (SLP_CFG_*) ────────────────────
    /** One value the device holds, as its last CFG_REPLY reported it. */
    struct CfgValue { uint32_t value = 0; uint8_t type = 0, flags = 0; };

    /** Ask the device for these ids (enum slp_cfg_id). The answers land in the
        cache and a change message follows. */
    void requestConfig (const std::vector<uint16_t>& ids);

    /** Write these items (id + type + value). The device answers with the
        STORED value and its flags (REBOOT / READONLY / REJECTED), so the cache
        always ends up holding what the device really has. */
    void writeConfig (const std::vector<slp_cfg_item>& items);

    /** Last value the device reported for `id`; false if it never answered. */
    bool configValue (uint16_t id, CfgValue& out) const;

    //── queries (copies) ─────────────────────────────────────────────────────
    Status status() const;
    std::vector<DeviceInfo> devices() const;
    bool isBound() const { return state_.load() == State::Bound; }
    State state() const  { return state_.load(); }

    static juce::String stateName (State s);

private:
    void run() override;

    // control-channel I/O (link thread)
    void openSocket();
    void refreshBroadcastTargets();
    void sendHello();
    void sendBind();
    void sendUnbind();
    void sendPing();
    void flushFeedback();
    void handleDatagram (const uint8_t* data, int len, const juce::String& fromIp, int fromPort);
    void handleAnnounce (const uint8_t* data, const juce::String& fromIp);
    void handleBindAck  (const uint8_t* data, const juce::String& fromIp);
    void handlePong     (const uint8_t* data);
    void handleError    (const uint8_t* data, const juce::String& fromIp);
    void handleCfgReply (const uint8_t* data);
    void sendTo (const juce::String& ip, int port, const void* msg, size_t len);
    void fillHdr (slp_hdr& h, uint8_t type, uint16_t len);
    void enterSearching (const juce::String& why);
    void enterBinding (const DeviceInfo& d);
    void pickCandidate();
    void expireDevices();
    void bumpGeneration();
    static juce::String uidToString (const uint8_t* uid);
    static juce::String macToString (const uint8_t* mac);

    // ── shared state (lock_) ─────────────────────────────────────────────────
    mutable juce::CriticalSection lock_;
    std::atomic<State> state_ { State::Off };
    Status       status_;
    std::vector<DeviceInfo> devices_;

    // config
    int          streamPort_ = SLP_STREAM_PORT;
    juce::String streamAddress_;            // multicast group when applicable
    juce::StringArray manualHosts_;
    juce::String preferredUid_;
    bool         autoBind_ = true;
    bool         streamTargetDirty_ = false;

    // requests from the UI
    juce::String bindRequestUid_;
    bool         unbindRequest_ = false;

    // feedback queue (coalesced)
    std::array<slp_led_cmd, SLP_MAX_LEDS> pendingLed_ {};
    uint8_t      pendingLedMask_ = 0;
    slp_oled_overlay pendingOverlay_ {};
    bool         overlayPending_ = false, overlayClearPending_ = false;
    int          calRequest_ = -1;
    std::vector<uint16_t>     pendingCfgGet_;
    std::vector<slp_cfg_item> pendingCfgSet_;
    std::map<uint16_t, CfgValue> cfg_;      // last CFG_REPLY per id

    // ── link-thread private state ────────────────────────────────────────────
    std::unique_ptr<juce::DatagramSocket> socket_;
    juce::StringArray broadcastTargets_;
    bool         ctrlViaBroadcast_ = false;   // SP3CTRA_LINK_BROADCAST_CTRL=1: unicast-less test mode (sandboxed shells)
    bool         socketOk_ = false;           // bindToPort succeeded on the current socket
    int          sendFails_ = 0;              // consecutive unicast send failures (socket sickness)
    double       lastSocketRetryMs_ = 0;
    uint32_t     txSeq_ = 0;
    uint32_t     session_ = 0;
    juce::String boundIp_;
    juce::String boundUid_;
    double       lastHelloMs_ = 0, lastBindMs_ = 0, lastPingMs_ = 0, lastPongMs_ = 0, lastIfaceScanMs_ = 0;
    int          bindAttempts_ = 0;
    uint8_t      hostVersion_[3] { 0, 0, 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sp3ctraLink)
};
