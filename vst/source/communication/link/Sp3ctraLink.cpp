/**
 * @file Sp3ctraLink.cpp
 * @brief Sp3ctra Link (SLP v1) host side — see header.
 */
#include "Sp3ctraLink.h"
#include "slp_rx_state.h"
#include <cstring>

extern "C" {
#include "logger.h"
}

#if __has_include("Sp3ctraVersion.h")
 #include "Sp3ctraVersion.h"
#endif

namespace
{
    constexpr double kDeviceExpiryMs   = 5000.0;
    constexpr double kBusyParkMs       = 4000.0;
    constexpr double kHelloBoundMs     = 3000.0;
    constexpr double kIfaceScanMs      = 10000.0;
    constexpr int    kBindMaxAttempts  = 10;
    constexpr int    kRxBufBytes       = 512;

    double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

    bool isMulticast (const juce::String& ip)
    {
        const int first = ip.upToFirstOccurrenceOf (".", false, false).getIntValue();
        return first >= 224 && first <= 239;
    }

    template <typename T> void readStruct (T& out, const uint8_t* data) { std::memcpy (&out, data, sizeof (T)); }
}

//==============================================================================
Sp3ctraLink::Sp3ctraLink() : juce::Thread ("Sp3ctraLink")
{
#ifdef SP3CTRA_VERSION_MAJOR
    hostVersion_[0] = (uint8_t) SP3CTRA_VERSION_MAJOR;
    hostVersion_[1] = (uint8_t) SP3CTRA_VERSION_MINOR;
    hostVersion_[2] = (uint8_t) SP3CTRA_VERSION_PATCH;
#endif
}

Sp3ctraLink::~Sp3ctraLink()
{
    stop();
}

juce::String Sp3ctraLink::stateName (State s)
{
    switch (s)
    {
        case State::Off:       return "off";
        case State::Searching: return "searching";
        case State::Binding:   return "binding";
        case State::Bound:     return "bound";
    }
    return {};
}

//==============================================================================
void Sp3ctraLink::start (int streamPort, const juce::String& streamAddress)
{
    {
        const juce::ScopedLock sl (lock_);
        streamPort_    = streamPort > 0 ? streamPort : (int) SLP_STREAM_PORT;
        streamAddress_ = streamAddress;
    }
    if (! isThreadRunning())
    {
        state_.store (State::Searching);
        startThread (juce::Thread::Priority::normal);
        log_info ("LINK", "Sp3ctra Link started (stream target port %d%s)", streamPort_,
                  isMulticast (streamAddress) ? ", multicast" : "");
    }
}

void Sp3ctraLink::stop()
{
    if (isThreadRunning())
    {
        signalThreadShouldExit();
        if (socket_) socket_->shutdown();
        stopThread (2000);
    }
    state_.store (State::Off);
}

void Sp3ctraLink::setStreamTarget (int streamPort, const juce::String& streamAddress)
{
    const juce::ScopedLock sl (lock_);
    const int p = streamPort > 0 ? streamPort : (int) SLP_STREAM_PORT;
    if (p != streamPort_ || streamAddress != streamAddress_)
    {
        streamPort_ = p;
        streamAddress_ = streamAddress;
        streamTargetDirty_ = true;
    }
}

void Sp3ctraLink::setManualHosts (const juce::StringArray& ips)
{
    const juce::ScopedLock sl (lock_);
    manualHosts_.clear();
    for (auto& ip : ips)
        if (ip.trim().isNotEmpty() && ip.trim() != "0.0.0.0")
            manualHosts_.addIfNotAlreadyThere (ip.trim());
}

void Sp3ctraLink::setPreferredUid (const juce::String& uid) { const juce::ScopedLock sl (lock_); preferredUid_ = uid; }
void Sp3ctraLink::setAutoBind (bool on)                     { const juce::ScopedLock sl (lock_); autoBind_ = on; }
void Sp3ctraLink::bindTo (const juce::String& uid)          { const juce::ScopedLock sl (lock_); bindRequestUid_ = uid; }
void Sp3ctraLink::unbind()                                  { const juce::ScopedLock sl (lock_); unbindRequest_ = true; }

//==============================================================================
void Sp3ctraLink::setLed (int index, const slp_led_cmd& cmd)
{
    if (index < 0 || index >= (int) SLP_MAX_LEDS) return;
    const juce::ScopedLock sl (lock_);
    pendingLed_[(size_t) index] = cmd;
    pendingLedMask_ |= (uint8_t) (1u << index);
}

void Sp3ctraLink::setOverlay (const std::vector<OverlayItem>& items, int ttlMs)
{
    slp_oled_overlay o {};
    o.ttl_ms = (uint16_t) juce::jlimit (100, 65535, ttlMs);
    o.count  = (uint8_t) juce::jmin ((int) SLP_OVERLAY_MAX_ITEMS, (int) items.size());
    for (int i = 0; i < (int) o.count; ++i)
    {
        const auto& it = items[(size_t) i];
        auto& d = o.item[i];
        const auto label = it.label.toStdString();
        const auto value = it.value.toStdString();
        std::strncpy (d.label, label.c_str(), SLP_OVERLAY_LABEL_LEN);
        std::strncpy (d.value, value.c_str(), SLP_OVERLAY_VALUE_LEN);
        d.norm  = it.norm < 0.0f ? 0xFFFF : (uint16_t) juce::roundToInt (juce::jlimit (0.0f, 1.0f, it.norm) * 65535.0f);
        d.flags = (uint8_t) ((it.bipolar ? SLP_OVL_BIPOLAR : 0) | (it.highlight ? SLP_OVL_HIGHLIGHT : 0));
    }
    const juce::ScopedLock sl (lock_);
    pendingOverlay_ = o;
    overlayPending_ = true;
    overlayClearPending_ = false;
}

void Sp3ctraLink::clearOverlay()
{
    const juce::ScopedLock sl (lock_);
    overlayPending_ = false;
    overlayClearPending_ = true;
}

void Sp3ctraLink::requestCalibration (int slpCalKind)
{
    const juce::ScopedLock sl (lock_);
    calRequest_ = slpCalKind;
}

//==============================================================================
Sp3ctraLink::Status Sp3ctraLink::status() const
{
    const juce::ScopedLock sl (lock_);
    Status s = status_;
    s.state = state_.load();
    return s;
}

std::vector<Sp3ctraLink::DeviceInfo> Sp3ctraLink::devices() const
{
    const juce::ScopedLock sl (lock_);
    return devices_;
}

void Sp3ctraLink::bumpGeneration()
{
    status_.generation++;
    sendChangeMessage();
}

//==============================================================================
// link thread
//==============================================================================
void Sp3ctraLink::openSocket()
{
    socket_ = std::make_unique<juce::DatagramSocket> (true /* broadcast */);
    if (! socket_->bindToPort (0))
        log_error ("LINK", "cannot bind the control socket");
}

void Sp3ctraLink::refreshBroadcastTargets()
{
    broadcastTargets_.clear();
    for (const auto& a : juce::IPAddress::getAllAddresses (false))
    {
        if (a.isNull() || a == juce::IPAddress::local()) continue;
        const auto b = juce::IPAddress::getInterfaceBroadcastAddress (a);
        if (! b.isNull())
            broadcastTargets_.addIfNotAlreadyThere (b.toString());
    }
    if (broadcastTargets_.isEmpty())
        broadcastTargets_.add ("255.255.255.255");
}

void Sp3ctraLink::fillHdr (slp_hdr& h, uint8_t type, uint16_t len)
{
    h.magic = SLP_MAGIC; h.version = SLP_VERSION; h.type = type; h.length = len; h.flags = 0; h.seq = txSeq_++;
}

void Sp3ctraLink::sendTo (const juce::String& ipIn, int port, const void* msg, size_t len)
{
    if (socket_ == nullptr) return;
    juce::String ip = ipIn;
    if (ctrlViaBroadcast_ && ! ip.endsWith (".255") && ip != "255.255.255.255")
    {
        // Test aid: a process without the macOS "Local Network" permission can
        // broadcast but not unicast. Every control datagram goes to the
        // directed broadcast of the device's /24; the device answers unicast.
        const juce::String prefix = ip.upToLastOccurrenceOf (".", true, false);
        juce::String target = "255.255.255.255";
        for (auto& b : broadcastTargets_) if (b.startsWith (prefix)) { target = b; break; }
        ip = target;
    }
    if (socket_->write (ip, port, msg, (int) len) < 0)
    {
        // Point-to-point interfaces (VPN tunnels) report a "broadcast" address
        // that refuses datagrams: drop it until the next interface scan.
        if (broadcastTargets_.contains (ip))
            broadcastTargets_.removeString (ip);
        else
            log_warning_every_ms (5000, "LINK", "send to %s:%d failed", ip.toRawUTF8(), port);
    }
}

void Sp3ctraLink::sendHello()
{
    slp_hello m {};
    fillHdr (m.hdr, SLP_HELLO, sizeof (m));
    std::memcpy (m.host_version, hostVersion_, 3);
    m.proto_min = SLP_VERSION;
    m.want_features = SLP_FEAT_LED_SET | SLP_FEAT_OLED_OVERLAY | SLP_FEAT_CFG | SLP_FEAT_CAL
                    | SLP_FEAT_HID_BUTTONS | SLP_FEAT_HID_ACC | SLP_FEAT_HID_GYRO | SLP_FEAT_HID_TEMP;

    juce::StringArray targets = broadcastTargets_;
    {
        const juce::ScopedLock sl (lock_);
        for (auto& h : manualHosts_) targets.addIfNotAlreadyThere (h);
        if (state_.load() == State::Bound) targets.addIfNotAlreadyThere (boundIp_);
    }
    for (auto& t : targets)
        sendTo (t, SLP_CTRL_PORT, &m, sizeof (m));
}

void Sp3ctraLink::sendBind()
{
    slp_bind m {};
    fillHdr (m.hdr, SLP_BIND, sizeof (m));
    m.session = session_;
    {
        const juce::ScopedLock sl (lock_);
        m.stream_port = (uint16_t) streamPort_;
        if (isMulticast (streamAddress_))
        {
            m.stream_mode = SLP_STREAM_MULTICAST;
            const juce::IPAddress group (streamAddress_);
            std::memcpy (m.mcast_group, group.address, 4);
        }
        else
            m.stream_mode = SLP_STREAM_UNICAST_TO_SENDER;
        streamTargetDirty_ = false;
    }
    m.hid_rate_hz = SLP_DEFAULT_HID_RATE_HZ;
    m.want_features = SLP_FEAT_LED_SET | SLP_FEAT_OLED_OVERLAY | SLP_FEAT_CFG | SLP_FEAT_CAL
                    | SLP_FEAT_HID_BUTTONS | SLP_FEAT_HID_ACC | SLP_FEAT_HID_GYRO | SLP_FEAT_HID_TEMP;
    std::memcpy (m.host_version, hostVersion_, 3);
    sendTo (boundIp_, SLP_CTRL_PORT, &m, sizeof (m));
    lastBindMs_ = nowMs();
    bindAttempts_++;
}

void Sp3ctraLink::sendUnbind()
{
    if (boundIp_.isEmpty() || session_ == 0) return;
    slp_unbind m {};
    fillHdr (m.hdr, SLP_UNBIND, sizeof (m));
    m.session = session_;
    sendTo (boundIp_, SLP_CTRL_PORT, &m, sizeof (m));
}

void Sp3ctraLink::sendPing()
{
    slp_ping m {};
    fillHdr (m.hdr, SLP_PING, sizeof (m));
    m.session = session_;
    m.host_time_ms = (uint32_t) juce::Time::getMillisecondCounter();
    sendTo (boundIp_, SLP_CTRL_PORT, &m, sizeof (m));
    lastPingMs_ = nowMs();
}

void Sp3ctraLink::flushFeedback()
{
    // Take the pending items under the lock, send outside of it.
    uint8_t ledMask = 0; std::array<slp_led_cmd, SLP_MAX_LEDS> leds {};
    bool overlay = false, clear = false; slp_oled_overlay ovl {};
    int cal = -1;
    {
        const juce::ScopedLock sl (lock_);
        ledMask = pendingLedMask_; leds = pendingLed_; pendingLedMask_ = 0;
        overlay = overlayPending_; ovl = pendingOverlay_; overlayPending_ = false;
        clear = overlayClearPending_; overlayClearPending_ = false;
        cal = calRequest_; calRequest_ = -1;
    }
    if (ledMask)
    {
        slp_led_set m {};
        fillHdr (m.hdr, SLP_LED_SET, sizeof (m));
        m.led_mask = ledMask;
        for (size_t i = 0; i < SLP_MAX_LEDS; ++i) m.led[i] = leds[i];
        sendTo (boundIp_, SLP_CTRL_PORT, &m, sizeof (m));
    }
    if (clear)
    {
        slp_oled_clear m {};
        fillHdr (m.hdr, SLP_OLED_CLEAR, sizeof (m));
        sendTo (boundIp_, SLP_CTRL_PORT, &m, sizeof (m));
    }
    if (overlay)
    {
        fillHdr (ovl.hdr, SLP_OLED_OVERLAY, sizeof (ovl));
        sendTo (boundIp_, SLP_CTRL_PORT, &ovl, sizeof (ovl));
    }
    if (cal >= 0)
    {
        slp_cal_start m {};
        fillHdr (m.hdr, SLP_CAL_START, sizeof (m));
        m.kind = (uint8_t) cal;
        sendTo (boundIp_, SLP_CTRL_PORT, &m, sizeof (m));
    }
}

//==============================================================================
juce::String Sp3ctraLink::uidToString (const uint8_t* uid)
{
    return juce::String::toHexString (uid, 12, 0).toUpperCase();
}

juce::String Sp3ctraLink::macToString (const uint8_t* mac)
{
    juce::String s;
    for (int i = 0; i < 6; ++i)
        s << (i ? ":" : "") << juce::String::toHexString (mac[i]).paddedLeft ('0', 2).toUpperCase();
    return s;
}

void Sp3ctraLink::handleDatagram (const uint8_t* data, int len, const juce::String& fromIp, int fromPort)
{
    juce::ignoreUnused (fromPort);
    if (len < (int) sizeof (slp_hdr)) return;
    slp_hdr h; readStruct (h, data);
    if (h.magic != SLP_MAGIC || h.version != SLP_VERSION || h.length != len) return;

    switch (h.type)
    {
        case SLP_ANNOUNCE: if (len >= (int) sizeof (slp_announce)) handleAnnounce (data, fromIp); break;
        case SLP_BIND_ACK: if (len >= (int) sizeof (slp_bind_ack)) handleBindAck (data, fromIp);  break;
        case SLP_PONG:     if (len >= (int) sizeof (slp_pong))     handlePong (data);             break;
        case SLP_ERROR:    if (len >= (int) sizeof (slp_error))    handleError (data, fromIp);    break;
        default: break;   // CFG_REPLY etc.: V5
    }
}

void Sp3ctraLink::handleAnnounce (const uint8_t* data, const juce::String& fromIp)
{
    slp_announce a; readStruct (a, data);
    DeviceInfo d;
    d.uid = uidToString (a.uid);
    d.name = juce::String::fromUTF8 (a.name, (int) strnlen (a.name, SLP_NAME_LEN));
    d.ip = fromIp;
    d.mac = macToString (a.mac);
    d.fw[0] = a.fw_version[0]; d.fw[1] = a.fw_version[1]; d.fw[2] = a.fw_version[2];
    d.hwRevision = a.hw_revision; d.protoMin = a.proto_min;
    d.bound = a.bound != 0;
    d.boundPeerIp = juce::String (a.bound_peer_ip[0]) + "." + juce::String (a.bound_peer_ip[1]) + "."
                  + juce::String (a.bound_peer_ip[2]) + "." + juce::String (a.bound_peer_ip[3]);
    d.features = a.features;
    d.nButtons = a.n_buttons; d.nLeds = a.n_leds; d.ledKind = a.led_kind; d.imuKind = a.imu_kind;
    d.displayW = a.display_w; d.displayH = a.display_h; d.displayBpp = a.display_bpp;
    for (int i = 0; i < juce::jmin (4, (int) a.n_dpi); ++i) { d.dpis.push_back (a.dpi[i]); d.pixelsAtDpi.push_back (a.pixels_at_dpi[i]); }
    d.lineRateMax = a.line_rate_max; d.hidRateMax = a.hid_rate_max;
    d.lastSeenMs = nowMs();

    bool changed = false;
    {
        const juce::ScopedLock sl (lock_);
        auto it = std::find_if (devices_.begin(), devices_.end(), [&] (const DeviceInfo& x) { return x.uid == d.uid; });
        if (it == devices_.end())
        {
            devices_.push_back (d);
            changed = true;
            log_info ("LINK", "device %s at %s (fw %d.%d.%d, %s)", d.name.toRawUTF8(), fromIp.toRawUTF8(),
                      d.fw[0], d.fw[1], d.fw[2], d.bound ? "bound" : "free");
        }
        else
        {
            const bool wasBound = it->bound;
            const auto oldIp = it->ip;
            const auto busy = it->busyUntilMs;
            *it = d;
            it->busyUntilMs = busy;
            changed = (wasBound != d.bound) || (oldIp != d.ip);
        }
    }
    if (changed) bumpGeneration();
}

void Sp3ctraLink::handleBindAck (const uint8_t* data, const juce::String& fromIp)
{
    slp_bind_ack ack; readStruct (ack, data);
    if (ack.session != session_ || fromIp != boundIp_) return;

    if (ack.status == SLP_BIND_OK)
    {
        const bool wasBound = state_.load() == State::Bound;
        {
            const juce::ScopedLock sl (lock_);
            status_.layout.dpi = ack.dpi;
            status_.layout.pixelsPerLine = ack.pixels_per_line;
            status_.layout.fragmentCount = ack.fragment_count;
            status_.layout.fragmentPixels = ack.fragment_pixels;
            status_.layout.linePacketBytes = ack.line_packet_bytes;
            status_.layout.hidRateHz = ack.hid_rate_hz;
            status_.layout.hidValidMask = ack.hid_valid_mask;
            status_.layout.sessionTimeoutMs = ack.session_timeout_ms > 0 ? ack.session_timeout_ms : (int) SLP_SESSION_TIMEOUT_MS;
            status_.deviceIp = boundIp_;
            status_.deviceUid = boundUid_;
            for (auto& d : devices_) if (d.uid == boundUid_) { status_.deviceName = d.name; d.bound = true; }
        }
        state_.store (State::Bound);
        lastPongMs_ = nowMs();
        if (! wasBound)
            log_info ("LINK", "bound to %s (%s): %d DPI, %d px = %d x %d, HID %d Hz",
                      status_.deviceName.toRawUTF8(), boundIp_.toRawUTF8(), ack.dpi, ack.pixels_per_line,
                      ack.fragment_count, ack.fragment_pixels, ack.hid_rate_hz);
        bumpGeneration();
    }
    else
    {
        {
            const juce::ScopedLock sl (lock_);
            for (auto& d : devices_) if (d.uid == boundUid_) { d.busyUntilMs = nowMs() + kBusyParkMs; if (ack.status == SLP_BIND_BUSY) d.bound = true; }
            status_.lastError = ack.status == SLP_BIND_BUSY ? "device in use by another host" : "bind refused";
            status_.errorCount++;
        }
        log_warning ("LINK", "BIND refused by %s (status %d)", boundIp_.toRawUTF8(), (int) ack.status);
        enterSearching ("bind refused");
    }
}

void Sp3ctraLink::handlePong (const uint8_t* data)
{
    slp_pong p; readStruct (p, data);
    if (p.session != session_) return;
    lastPongMs_ = nowMs();
    const juce::ScopedLock sl (lock_);
    status_.uptimeMs = p.uptime_ms;
    status_.linesSent = p.lines_sent;
    status_.lineRateLps = p.line_rate_lps;
    status_.calState = p.cal_state;
    status_.calProgress = p.cal_progress;
    status_.tempC = (float) p.temp_c_x10 / 10.0f;
    status_.deviceStreaming = (p.link_flags & SLP_LINK_STREAMING) != 0;
    status_.calRunning = (p.link_flags & SLP_LINK_CAL_RUNNING) != 0;
    status_.rttMs = (int) ((uint32_t) juce::Time::getMillisecondCounter() - p.host_time_ms);
    status_.pongCount++;
}

void Sp3ctraLink::handleError (const uint8_t* data, const juce::String& fromIp)
{
    slp_error e; readStruct (e, data);
    {
        const juce::ScopedLock sl (lock_);
        status_.errorCount++;
        status_.lastError = "device error " + juce::String ((int) e.code) + " (msg 0x" + juce::String::toHexString ((int) e.in_reply_to) + ")";
    }
    if (e.code == SLP_ERR_NOT_BOUND && state_.load() == State::Bound && fromIp == boundIp_)
        enterSearching ("session lost (NOT_BOUND)");
}

//==============================================================================
void Sp3ctraLink::enterSearching (const juce::String& why)
{
    const bool wasBound = state_.load() == State::Bound;
    if (wasBound) log_info ("LINK", "session with %s closed: %s", boundIp_.toRawUTF8(), why.toRawUTF8());
    state_.store (State::Searching);
    {
        const juce::ScopedLock sl (lock_);
        for (auto& d : devices_) if (d.uid == boundUid_ && wasBound) d.bound = false;
        status_.deviceIp.clear(); status_.deviceName.clear(); status_.deviceUid.clear();
        status_.layout = {};
        status_.deviceStreaming = false;
    }
    session_ = 0; boundIp_.clear(); boundUid_.clear(); bindAttempts_ = 0;
    lastHelloMs_ = 0;   // HELLO right away
    bumpGeneration();
}

void Sp3ctraLink::enterBinding (const DeviceInfo& d)
{
    boundIp_ = d.ip; boundUid_ = d.uid;
    session_ = 0;
    while (session_ == 0) session_ = (uint32_t) juce::Random::getSystemRandom().nextInt64();
    bindAttempts_ = 0;
    lastBindMs_ = 0;
    state_.store (State::Binding);
    {
        const juce::ScopedLock sl (lock_);
        status_.deviceIp = d.ip; status_.deviceUid = d.uid; status_.deviceName = d.name;
    }
    log_info ("LINK", "binding %s (%s)...", d.name.toRawUTF8(), d.ip.toRawUTF8());
    bumpGeneration();
}

void Sp3ctraLink::pickCandidate()
{
    DeviceInfo pick; bool found = false;
    {
        const juce::ScopedLock sl (lock_);
        const double now = nowMs();
        std::vector<const DeviceInfo*> free;
        for (auto& d : devices_)
            if (d.supported() && ! d.bound && d.busyUntilMs <= now)
                free.push_back (&d);

        if (preferredUid_.isNotEmpty())
        {
            for (auto* d : free) if (d->uid == preferredUid_) { pick = *d; found = true; break; }
        }
        else if (autoBind_ && free.size() == 1)
        {
            pick = *free[0]; found = true;
        }
    }
    if (found) enterBinding (pick);
}

void Sp3ctraLink::expireDevices()
{
    bool changed = false;
    {
        const juce::ScopedLock sl (lock_);
        const double now = nowMs();
        const auto before = devices_.size();
        devices_.erase (std::remove_if (devices_.begin(), devices_.end(), [&] (const DeviceInfo& d)
                                        { return d.uid != boundUid_ && (now - d.lastSeenMs) > kDeviceExpiryMs; }),
                        devices_.end());
        changed = devices_.size() != before;
    }
    if (changed) bumpGeneration();
}

//==============================================================================
void Sp3ctraLink::run()
{
    ctrlViaBroadcast_ = juce::SystemStats::getEnvironmentVariable ("SP3CTRA_LINK_BROADCAST_CTRL", "0") == "1";
    if (ctrlViaBroadcast_) log_warning ("LINK", "control datagrams sent via directed broadcast (SP3CTRA_LINK_BROADCAST_CTRL)");
    openSocket();
    refreshBroadcastTargets();
    lastIfaceScanMs_ = nowMs();
    uint8_t buf[kRxBufBytes];

    while (! threadShouldExit())
    {
        // ── receive ──────────────────────────────────────────────────────────
        if (socket_ != nullptr && socket_->waitUntilReady (true, 20) == 1)
        {
            for (int n = 0; n < 32; ++n)   // drain, bounded
            {
                juce::String fromIp; int fromPort = 0;
                const int len = socket_->read (buf, sizeof (buf), false, fromIp, fromPort);
                if (len <= 0) break;
                handleDatagram (buf, len, fromIp, fromPort);
            }
        }
        if (threadShouldExit()) break;

        const double now = nowMs();
        const State st = state_.load();

        // ── UI requests ──────────────────────────────────────────────────────
        juce::String bindUid; bool unbindReq = false; bool retarget = false;
        {
            const juce::ScopedLock sl (lock_);
            bindUid = bindRequestUid_; bindRequestUid_.clear();
            unbindReq = unbindRequest_; unbindRequest_ = false;
            retarget = streamTargetDirty_;
        }
        if (unbindReq && st != State::Searching)
        {
            sendUnbind();
            enterSearching ("released by user");
            continue;
        }
        if (bindUid.isNotEmpty() && bindUid != boundUid_)
        {
            DeviceInfo d; bool ok = false;
            {
                const juce::ScopedLock sl (lock_);
                for (auto& x : devices_) if (x.uid == bindUid) { d = x; ok = true; }
            }
            if (ok)
            {
                if (st == State::Bound) sendUnbind();
                enterBinding (d);
                continue;
            }
        }
        if (retarget && st == State::Bound)
        {
            // Same peer, same session: the device refreshes the stream target.
            state_.store (State::Binding);
            bindAttempts_ = 0; lastBindMs_ = 0;
        }

        // ── periodic ─────────────────────────────────────────────────────────
        if (now - lastIfaceScanMs_ >= kIfaceScanMs) { refreshBroadcastTargets(); lastIfaceScanMs_ = now; }
        expireDevices();

        const double helloPeriod = (st == State::Bound) ? kHelloBoundMs : (double) SLP_HELLO_PERIOD_MS;
        if (now - lastHelloMs_ >= helloPeriod) { sendHello(); lastHelloMs_ = now; }

        switch (state_.load())
        {
            case State::Searching:
                pickCandidate();
                break;

            case State::Binding:
                if (now - lastBindMs_ >= (double) SLP_BIND_RETRY_MS)
                {
                    if (bindAttempts_ >= kBindMaxAttempts)
                    {
                        log_warning ("LINK", "no BIND_ACK from %s", boundIp_.toRawUTF8());
                        { const juce::ScopedLock sl (lock_); for (auto& d : devices_) if (d.uid == boundUid_) d.busyUntilMs = now + kBusyParkMs; }
                        enterSearching ("no answer to BIND");
                    }
                    else
                        sendBind();
                }
                break;

            case State::Bound:
            {
                int timeout = SLP_SESSION_TIMEOUT_MS;
                { const juce::ScopedLock sl (lock_); if (status_.layout.sessionTimeoutMs > 0) timeout = status_.layout.sessionTimeoutMs; }
                if (now - lastPongMs_ > (double) timeout)
                {
                    enterSearching ("no PONG");
                    break;
                }
                if (now - lastPingMs_ >= (double) SLP_PING_PERIOD_MS) sendPing();
                flushFeedback();
                break;
            }

            case State::Off:
                break;
        }
    }

    if (state_.load() == State::Bound) sendUnbind();
    socket_.reset();
}
