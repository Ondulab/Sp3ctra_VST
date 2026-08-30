#include "SourceSetupPanel.h"
#include "SetupHeader.h"
#include "../../Sp3ctraConstants.h"
#include "../../UITheme.h"
#include "../../session/MachinePrefs.h"
#include "../../communication/link/slp_rx_state.h"
#include "../../Sp3ctraDialog.h"

using DC = Sp3ctraDeviceClient;

namespace
{
    constexpr int kOvspVals[] = { 1, 2, 4, 8, 16, 32 };

    int  ovspToId   (int v)  { for (int i = 0; i < 6; ++i) if (kOvspVals[i] == v) return i + 1; return 1; }
    int  idToOvsp   (int id) { return kOvspVals[juce::jlimit (1, 6, id) - 1]; }
    int  binId      (int v)  { return v != 0 ? 2 : 1; }            // 0/1 → combo id 1/2
    int  binVal     (const juce::ComboBox& c) { return c.getSelectedId() == 2 ? 1 : 0; }
    int  zeroBasedVal (const juce::ComboBox& c) { return juce::jmax (0, c.getSelectedId() - 1); }
}

//==============================================================================
// IpBytes helper
//==============================================================================
void SourceSetupPanel::IpBytes::init (juce::Component& parent)
{
    for (auto& b : box)
    {
        b.setMultiLine (false);
        b.setReturnKeyStartsNewLine (false);
        b.setScrollbarsShown (false);
        b.setCaretVisible (true);
        b.setPopupMenuEnabled (true);
        b.setFont (juce::FontOptions (Sp3ctraTheme::kFontSettings));
        b.setJustification (juce::Justification::centred);
        b.setInputRestrictions (3, "0123456789");
        parent.addAndMakeVisible (b);
    }
    for (auto& d : dot)
    {
        d.setText (".", juce::dontSendNotification);
        d.setJustificationType (juce::Justification::centred);
        d.setFont (juce::Font (juce::FontOptions (Sp3ctraTheme::kFontSettings)).boldened());
        parent.addAndMakeVisible (d);
    }
}

void SourceSetupPanel::IpBytes::layout (int x, int y, int w, int h)
{
    constexpr int dotW = 8;
    const int bw = (w - 3 * dotW) / 4;
    int bx = x;
    for (int i = 0; i < 4; ++i)
    {
        box[i].setBounds (bx, y, bw, h);
        bx += bw;
        if (i < 3) { dot[i].setBounds (bx, y, dotW, h); bx += dotW; }
    }
}

void SourceSetupPanel::IpBytes::set (const juce::String& dotted, juce::NotificationType n)
{
    juce::StringArray parts;
    parts.addTokens (dotted, ".", "");
    for (int i = 0; i < 4; ++i)
        box[i].setText (i < parts.size() ? parts[i].trim() : juce::String ("0"),
                        n != juce::dontSendNotification);
}

void SourceSetupPanel::IpBytes::set (int b0, int b1, int b2, int b3)
{
    const int v[4] = { b0, b1, b2, b3 };
    for (int i = 0; i < 4; ++i)
        box[i].setText (juce::String (v[i]), false);
}

juce::String SourceSetupPanel::IpBytes::get() const
{
    return box[0].getText().trim() + "." + box[1].getText().trim() + "."
         + box[2].getText().trim() + "." + box[3].getText().trim();
}

void SourceSetupPanel::IpBytes::onAnyChange (std::function<void()> cb)
{
    for (auto& b : box)
        b.onTextChange = cb;
}

//==============================================================================
// Small init helpers
//==============================================================================
void SourceSetupPanel::initLabel (juce::Label& l, const juce::String& text, juce::Justification j)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (j);
    l.setFont (juce::FontOptions (Sp3ctraTheme::kFontSettings));
    addAndMakeVisible (l);
}

void SourceSetupPanel::initSection (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centredLeft);
    l.setFont (juce::Font (juce::FontOptions (Sp3ctraTheme::kFontBadge)).boldened());
    l.setColour (juce::Label::textColourId, accent);
    addAndMakeVisible (l);
}

void SourceSetupPanel::initEditor (juce::TextEditor& e, int maxLen, const juce::String& allowed)
{
    e.setMultiLine (false);
    e.setReturnKeyStartsNewLine (false);
    e.setScrollbarsShown (false);
    e.setCaretVisible (true);
    e.setPopupMenuEnabled (true);
    e.setFont (juce::FontOptions (Sp3ctraTheme::kFontSettings));
    e.setJustification (juce::Justification::centred);
    if (maxLen > 0)
        e.setInputRestrictions (maxLen, allowed);
    addAndMakeVisible (e);
}

void SourceSetupPanel::initCombo (juce::ComboBox& c, const juce::StringArray& items)
{
    for (int i = 0; i < items.size(); ++i)
        c.addItem (items[i], i + 1);
    addAndMakeVisible (c);
}

//==============================================================================
SourceSetupPanel::SourceSetupPanel (Sp3ctraAudioProcessor& processor, juce::Colour accentColour)
    : audioProcessor (processor), apvts (processor.getAPVTS()), accent (accentColour)
{
    // ── LINK block: Sp3ctra Link devices + APVTS transport params ────────────
    initLabel (devicesLabel, "Devices:");
    for (auto& r : deviceRows)
    {
        r.text.setFont (juce::FontOptions (Sp3ctraTheme::kFontSettings));
        r.text.setJustificationType (juce::Justification::centredLeft);
        addChildComponent (r.text);
        r.button.setButtonText ("USE");
        addChildComponent (r.button);
    }
    linkStatusLabel.setJustificationType (juce::Justification::centredLeft);
    linkStatusLabel.setFont (juce::Font (juce::FontOptions (Sp3ctraTheme::kFontTiny)).italicised());
    linkStatusLabel.setText ("Sp3ctra Link: starting ...", juce::dontSendNotification);
    addAndMakeVisible (linkStatusLabel);

    autoBindToggle.setButtonText ("Connect automatically (preferred device, or the only one found)");
    autoBindToggle.setToggleState (MachinePrefs::linkAutoBind(), juce::dontSendNotification);
    autoBindToggle.onClick = [this]
    {
        MachinePrefs::setLinkAutoBind (autoBindToggle.getToggleState());
        if (auto* l = link()) l->setAutoBind (autoBindToggle.getToggleState());
    };
    addAndMakeVisible (autoBindToggle);

    initLabel (deviceIpLabel, "Device IP:");
    deviceIp.init (*this);
    deviceIp.set ((int) apvts.getRawParameterValue ("deviceIpByte1")->load(),
                  (int) apvts.getRawParameterValue ("deviceIpByte2")->load(),
                  (int) apvts.getRawParameterValue ("deviceIpByte3")->load(),
                  (int) apvts.getRawParameterValue ("deviceIpByte4")->load());

    initLabel (udpPortLabel, "Stream Port:");
    initEditor (udpPortEditor, 5, "0123456789");
    udpPortEditor.setText (juce::String ((int) apvts.getRawParameterValue ("udpPort")->load()), false);

    initLabel (udpAddressLabel, "Multicast:");
    udpAddr.init (*this);
    udpAddr.set ((int) apvts.getRawParameterValue ("udpByte1")->load(),
                 (int) apvts.getRawParameterValue ("udpByte2")->load(),
                 (int) apvts.getRawParameterValue ("udpByte3")->load(),
                 (int) apvts.getRawParameterValue ("udpByte4")->load());

    applyLinkButton.setButtonText ("Apply Link");
    applyLinkButton.onClick = [this] { applyLink(); };
    addAndMakeVisible (applyLinkButton);

    connStatusLabel.setJustificationType (juce::Justification::centredLeft);
    connStatusLabel.setFont (juce::Font (juce::FontOptions (Sp3ctraTheme::kFontTiny)).italicised());
    addAndMakeVisible (connStatusLabel);

    retryButton.setButtonText ("Retry");
    retryButton.onClick = [this] { reload(); };
    addAndMakeVisible (retryButton);

    // ── CIS ───────────────────────────────────────────────────────────────────
    initSection (cisHeader, "CIS PARAMETERS");
    initLabel (dpiLabel, "DPI:");
    initCombo (dpiCombo, { "200 DPI (1728 px)", "400 DPI (3456 px)" });
    dpiCombo.onChange = [this] { if (! applyingRemote) postDpi(); };

    initLabel (ovspLabel, "Oversampling:");
    initCombo (ovspCombo, { "1", "2", "4", "8", "16", "32" });
    ovspCombo.onChange = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setOversampling",
                                   "oversampling=" + juce::String (idToOvsp (ovspCombo.getSelectedId())), {});
    };

    initLabel (lpsLabel, "Lines/sec:");
    lpsValue.setJustificationType (juce::Justification::centredLeft);
    lpsValue.setFont (juce::FontOptions (Sp3ctraTheme::kFontSettings));
    lpsValue.setColour (juce::Label::textColourId, juce::Colour (Sp3ctraTheme::kColTextMuted));
    addAndMakeVisible (lpsValue);

    initLabel (handLabel, "Handedness:");
    initCombo (handCombo, { "Left", "Right" });
    handCombo.onChange = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setHand", "hand=" + juce::String (zeroBasedVal (handCombo)), {});
    };

    calibrateCisButton.setButtonText ("Start CIS Calibration");
    calibrateCisButton.onClick = [this] {
        deviceClient.postForm ("startCalibration", "CIS_CAL_START", {});
        setConnState (connState, juce::String::fromUTF8("CIS calibration started — move over white reference"));
    };
    addAndMakeVisible (calibrateCisButton);

    // ── IMU ───────────────────────────────────────────────────────────────────
    initSection (imuHeader, "IMU PARAMETERS");
    initLabel (gyroLabel, "Gyro:");
    initCombo (gyroCombo, { "+/-2000 dps", "+/-1000 dps", "+/-500 dps", "+/-250 dps",
                            "+/-125 dps", "+/-62.5 dps", "+/-31.25 dps", "+/-15.625 dps" });
    gyroCombo.onChange = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setGyroSensitivity",
                                   "gyro_sensitivity=" + juce::String (zeroBasedVal (gyroCombo)), {});
    };

    initLabel (accelLabel, "Accel:");
    initCombo (accelCombo, { "+/-16 g", "+/-8 g", "+/-4 g", "+/-2 g" });
    accelCombo.onChange = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setAccelSensitivity",
                                   "accel_sensitivity=" + juce::String (zeroBasedVal (accelCombo)), {});
    };

    calibrateImuButton.setButtonText ("Start IMU Calibration");
    calibrateImuButton.onClick = [this] {
        deviceClient.postForm ("startIMUCalibration", "IMU_CAL_START", {});
        setConnState (connState, juce::String::fromUTF8("IMU calibration started — keep device still (~1.5s)"));
    };
    addAndMakeVisible (calibrateImuButton);

    // ── GUI & screensaver ───────────────────────────────────────────────────────
    initSection (guiHeader, "GUI & SCREENSAVER");
    initLabel (showImuLabel, "Show IMU:");
    initCombo (showImuCombo, { "Off", "On" });
    showImuCombo.onChange = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setGuiShowImu", "gui_show_imu=" + juce::String (binVal (showImuCombo)), {});
    };

    initLabel (invertLabel, "Invert CIS:");
    initCombo (invertCombo, { "Off", "On" });
    invertCombo.onChange = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setGuiInvertCisImage",
                                   "gui_invert_cis_image=" + juce::String (binVal (invertCombo)), {});
    };

    initLabel (screensaverLabel, "Timeout (s):");
    initEditor (screensaverEditor, 4, "0123456789");
    screensaverEditor.onReturnKey = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setScreensaverTimeout",
                                   "screensaver_timeout=" + screensaverEditor.getText().trim(), {});
    };
    screensaverEditor.onFocusLost = screensaverEditor.onReturnKey;

    initLabel (motionAccLabel, "Motion Acc (g):");
    initEditor (motionAccEditor, 5, "0123456789.");
    motionAccEditor.onReturnKey = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setMotionThresholdAcc",
                                   "motion_threshold_acc=" + motionAccEditor.getText().trim(), {});
    };
    motionAccEditor.onFocusLost = motionAccEditor.onReturnKey;

    initLabel (motionGyroLabel, "Motion Gyro (dps):");
    initEditor (motionGyroEditor, 5, "0123456789.");
    motionGyroEditor.onReturnKey = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setMotionThresholdGyro",
                                   "motion_threshold_gyro=" + motionGyroEditor.getText().trim(), {});
    };
    motionGyroEditor.onFocusLost = motionGyroEditor.onReturnKey;

    // ── Device network configuration ─────────────────────────────────────────────
    initSection (netHeader, "NETWORK (DEVICE)");
    initLabel (ipLabel, "IP Addr:");        netIp.init (*this);
    initLabel (maskLabel, "Subnet Mask:");  netMask.init (*this);
    initLabel (gatewayLabel, "Gateway:");   netGateway.init (*this);
    initLabel (destIpLabel, "Dest IP:");    netDestIp.init (*this);
    initLabel (cisUdpPortLabel, "Stream Port:");
    initEditor (cisUdpPortEditor, 5, "0123456789");
    initLabel (linkPortLabel, "Link Port:");
    initEditor (linkPortEditor, 5, "0123456789");
    initLabel (streamUnboundLabel, "Stream w/o host:");
    initCombo (streamUnboundCombo, { "Off", "On" });

    applyNetworkButton.setButtonText ("Apply Network");
    applyNetworkButton.onClick = [this] { postNetwork(); };
    addAndMakeVisible (applyNetworkButton);

    // ── Firmware ─────────────────────────────────────────────────────────────────
    initSection (fwHeader, "FIRMWARE");
    initLabel (fwVersionLabel, "Version: --", juce::Justification::centredLeft);
    chooseFwButton.setButtonText ("Choose .bin");
    chooseFwButton.onClick = [this] { chooseFirmware(); };
    addAndMakeVisible (chooseFwButton);
    fwFileLabel.setText ("(no file)", juce::dontSendNotification);
    fwFileLabel.setJustificationType (juce::Justification::centredLeft);
    fwFileLabel.setFont (juce::Font (juce::FontOptions (Sp3ctraTheme::kFontTiny)).italicised());
    addAndMakeVisible (fwFileLabel);
    uploadFwButton.setButtonText ("Upload Firmware");
    uploadFwButton.onClick = [this] { uploadFirmware(); };
    addAndMakeVisible (uploadFwButton);
    uploadProgressBar.setPercentageDisplay (true);
    addAndMakeVisible (uploadProgressBar);
    factoryResetButton.setButtonText ("Factory Reset");
    factoryResetButton.onClick = [this] { confirmFactoryReset(); };
    addAndMakeVisible (factoryResetButton);

    setConnState (DC::State::Idle);
    setDeviceControlsEnabled (false);
}

SourceSetupPanel::~SourceSetupPanel()
{
    stopTimer();
    if (auto* l = link()) l->removeChangeListener (this);
    deviceClient.cancel();
}

//==============================================================================
void SourceSetupPanel::visibilityChanged()
{
    if (isShowing())
    {
        if (auto* l = link()) { l->removeChangeListener (this); l->addChangeListener (this); }
        refreshLink();
        startTimer (500);
        reload();
    }
    else
    {
        stopTimer();
        if (auto* l = link()) l->removeChangeListener (this);
        deviceClient.cancel();   // HTTP is connectionless — just stop issuing
        loading = false;         // allow the next show to reload
    }
}

//==============================================================================
Sp3ctraLink* SourceSetupPanel::link() const
{
    return audioProcessor.getLink();
}

void SourceSetupPanel::timerCallback()                       { refreshLink(); }
void SourceSetupPanel::changeListenerCallback (juce::ChangeBroadcaster*) { refreshLink(); }

void SourceSetupPanel::refreshLink()
{
    auto* l = link();
    if (l == nullptr)
    {
        linkStatusLabel.setText ("Sp3ctra Link: not running (pipeline not started)", juce::dontSendNotification);
        for (auto& r : deviceRows) { r.text.setVisible (false); r.button.setVisible (false); }
        return;
    }

    const auto st  = l->status();
    const auto dev = l->devices();
    const juce::String preferred = MachinePrefs::linkPreferredUid();

    // ── device rows ──────────────────────────────────────────────────────────
    for (int i = 0; i < kMaxDeviceRows; ++i)
    {
        auto& r = deviceRows[i];
        if (i >= (int) dev.size()) { r.text.setVisible (false); r.button.setVisible (false); r.uid.clear(); continue; }
        const auto& d = dev[(size_t) i];
        const bool ours = (st.state == Sp3ctraLink::State::Bound || st.state == Sp3ctraLink::State::Binding)
                          && st.deviceUid == d.uid;
        juce::String state;
        juce::Colour col = juce::Colour (Sp3ctraTheme::kColTextMuted);
        if (! d.supported())            { state = "unsupported fw " + d.fwString() + " (need 4.0+)"; col = juce::Colours::red; }
        else if (ours)                  { state = st.state == Sp3ctraLink::State::Bound ? "bound" : "binding ..."; col = juce::Colours::green; }
        else if (d.bound)               { state = "used by " + d.boundPeerIp; col = juce::Colours::orange; }
        else                            { state = "free"; }
        if (d.uid == preferred)         state << "  (preferred)";

        r.uid = d.uid;
        r.text.setText ((ours ? juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f ")) : juce::String ("   "))
                        + d.name + "   " + d.ip + "   fw " + d.fwString() + "   " + state,
                        juce::dontSendNotification);
        r.text.setColour (juce::Label::textColourId, col);
        r.text.setVisible (true);
        r.button.setButtonText (ours ? "RELEASE" : "USE");
        r.button.setEnabled (ours || (d.supported() && ! d.bound));
        r.button.onClick = [this, uid = d.uid, ours] { if (ours) releaseDevice(); else useDevice (uid); };
        r.button.setVisible (true);
    }

    // ── status line ──────────────────────────────────────────────────────────
    juce::String text;
    juce::Colour col = juce::Colour (Sp3ctraTheme::kColTextMuted);
    switch (st.state)
    {
        case Sp3ctraLink::State::Off:       text = "Sp3ctra Link: off"; break;
        case Sp3ctraLink::State::Searching: text = "Searching ... " + juce::String ((int) dev.size()) + " device(s) seen"; col = juce::Colours::orange; break;
        case Sp3ctraLink::State::Binding:   text = "Binding " + st.deviceName + " ..."; col = juce::Colours::orange; break;
        case Sp3ctraLink::State::Bound:
        {
            slp_rx_stats rx {};
            slp_rx_stats_snapshot (&rx);
            text << "Bound to " << st.deviceName << " (" << st.deviceIp << ")  " << st.layout.dpi << " DPI  "
                 << st.lineRateLps << " lps  HID " << st.layout.hidRateHz << " Hz  rtt " << st.rttMs << " ms  "
                 << juce::String (st.tempC, 1) << " C  lost " << (int) (rx.line_lost + rx.hid_lost);
            if (rx.legacy_datagrams > 0) text << "  [legacy stream!]";
            col = juce::Colours::green;
            break;
        }
    }
    if (st.lastError.isNotEmpty() && st.state != Sp3ctraLink::State::Bound)
        text << "  -- " << st.lastError;
    linkStatusLabel.setColour (juce::Label::textColourId, col);
    linkStatusLabel.setText (text, juce::dontSendNotification);

    // A fresh session: the processor already re-pointed the device IP; refresh
    // the HTTP page so the device settings follow the bound device.
    const bool bound = st.state == Sp3ctraLink::State::Bound;
    if (bound && ! linkWasBound && st.generation != lastLinkGen)
    {
        deviceIp.set (st.deviceIp);
        juce::Component::SafePointer<SourceSetupPanel> safe (this);
        juce::Timer::callAfterDelay (200, [safe] { if (auto* s = safe.getComponent()) s->reload(); });
    }
    linkWasBound = bound;
    lastLinkGen  = st.generation;
}

void SourceSetupPanel::useDevice (const juce::String& uid)
{
    MachinePrefs::setLinkPreferredUid (uid);
    if (auto* l = link())
    {
        l->setPreferredUid (uid);
        l->bindTo (uid);
    }
    refreshLink();
}

void SourceSetupPanel::releaseDevice()
{
    MachinePrefs::setLinkPreferredUid ({});
    if (auto* l = link())
    {
        l->setPreferredUid ({});
        l->unbind();
    }
    refreshLink();
}

juce::String SourceSetupPanel::deviceHostFromApvts() const
{
    auto b = [this] (const char* id) {
        return juce::String ((int) apvts.getRawParameterValue (id)->load());
    };
    return b ("deviceIpByte1") + "."
         + b ("deviceIpByte2") + "."
         + b ("deviceIpByte3") + "."
         + b ("deviceIpByte4");
}

//==============================================================================
void SourceSetupPanel::reload()
{
    if (loading)
        return;                  // a load burst is already in flight — don't pile another
    loading = true;

    deviceClient.setHost (deviceHostFromApvts());
    setConnState (DC::State::Connecting, "Connecting to " + deviceClient.getHost() + " ...");
    setDeviceControlsEnabled (false);

    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    deviceClient.loadAll ([safe] (DC::State st, DC::DeviceConfig cfg)
    {
        if (auto* self = safe.getComponent())
            self->onLoaded (st, cfg);
    });
}

void SourceSetupPanel::onLoaded (DC::State st, DC::DeviceConfig cfg)
{
    loading = false;

    if (st == DC::State::Connected && cfg.valid)
    {
        populate (cfg);
        setConnState (DC::State::Connected,
                      "Connected -- " + deviceClient.getHost()
                          + (cfg.firmwareVersion.isNotEmpty() ? "  (fw " + cfg.firmwareVersion + ")" : ""));
        setDeviceControlsEnabled (true);
    }
    else
    {
        setConnState (DC::State::Failed, "No response from " + deviceClient.getHost() + " -- Retry");
        setDeviceControlsEnabled (false);
    }
}

void SourceSetupPanel::populate (const DC::DeviceConfig& cfg)
{
    const juce::ScopedValueSetter<bool> guard (applyingRemote, true);

    // CIS — DPI is reconciled into APVTS so UDP parsing matches the live stream.
    dpiCombo.setSelectedId (cfg.dpi == 200 ? 1 : 2, juce::dontSendNotification);
    reconcileDpiToApvts (cfg.dpi);
    ovspCombo.setSelectedId (ovspToId (cfg.oversampling), juce::dontSendNotification);
    lpsValue.setText (juce::String (cfg.freqLps) + " lps", juce::dontSendNotification);
    handCombo.setSelectedId (cfg.handedness == 0 ? 1 : 2, juce::dontSendNotification);

    // IMU
    gyroCombo.setSelectedId (juce::jlimit (1, 8, cfg.gyroSensitivity + 1), juce::dontSendNotification);
    accelCombo.setSelectedId (juce::jlimit (1, 4, cfg.accelSensitivity + 1), juce::dontSendNotification);

    // GUI
    showImuCombo.setSelectedId (binId (cfg.guiShowImu ? 1 : 0), juce::dontSendNotification);
    invertCombo.setSelectedId (binId (cfg.guiInvertCis ? 1 : 0), juce::dontSendNotification);
    screensaverEditor.setText (juce::String (cfg.screensaverTimeout), false);
    motionAccEditor.setText (juce::String (cfg.motionThresholdAcc, 2), false);
    motionGyroEditor.setText (juce::String (cfg.motionThresholdGyro, 1), false);

    // Network
    netIp.set (cfg.network.ip);
    netMask.set (cfg.network.mask);
    netGateway.set (cfg.network.gateway);
    netDestIp.set (cfg.network.destIp);
    cisUdpPortEditor.setText (juce::String (cfg.network.udpPort), false);
    linkPortEditor.setText (juce::String (cfg.network.linkPort), false);
    streamUnboundCombo.setSelectedId (binId (cfg.network.streamWhenUnbound ? 1 : 0), juce::dontSendNotification);

    fwVersionLabel.setText ("Version: " + (cfg.firmwareVersion.isNotEmpty() ? cfg.firmwareVersion
                                                                            : juce::String ("--")),
                            juce::dontSendNotification);
}

void SourceSetupPanel::setConnState (DC::State st, const juce::String& detail)
{
    connState = st;
    juce::String text = detail;
    juce::Colour col = juce::Colour (Sp3ctraTheme::kColTextMuted);
    switch (st)
    {
        case DC::State::Connecting: col = juce::Colours::orange; if (text.isEmpty()) text = "Connecting ..."; break;
        case DC::State::Connected:  col = juce::Colours::green;  if (text.isEmpty()) text = "Connected"; break;
        case DC::State::Failed:     col = juce::Colours::red;    if (text.isEmpty()) text = "Connection failed"; break;
        case DC::State::Idle:       if (text.isEmpty()) text = "Open to load device settings"; break;
    }
    connStatusLabel.setColour (juce::Label::textColourId, col);
    connStatusLabel.setText (text, juce::dontSendNotification);
}

void SourceSetupPanel::setDeviceControlsEnabled (bool on)
{
    juce::Component* ctrls[] = {
        &dpiCombo, &ovspCombo, &handCombo, &calibrateCisButton,
        &gyroCombo, &accelCombo, &calibrateImuButton,
        &showImuCombo, &invertCombo, &screensaverEditor, &motionAccEditor, &motionGyroEditor,
        &cisUdpPortEditor, &linkPortEditor, &streamUnboundCombo, &applyNetworkButton,
        &uploadFwButton, &factoryResetButton
    };
    for (auto* c : ctrls) c->setEnabled (on);
    for (auto* g : { &netIp, &netMask, &netGateway, &netDestIp })
        for (auto& b : g->box) b.setEnabled (on);
}

//==============================================================================
void SourceSetupPanel::reconcileDpiToApvts (int dpi)
{
    // APVTS sensorDpi choice: 0 = 200, 1 = 400. Writing it propagates to
    // g_sp3ctra_config.sensor_dpi (PluginProcessor parameter listener) → UDP parser.
    if (auto* p = apvts.getParameter ("sensorDpi"))
    {
        const float norm = p->convertTo0to1 (dpi == 200 ? 0.0f : 1.0f);
        if (std::abs (p->getValue() - norm) > 1.0e-4f)
        {
            p->setValueNotifyingHost (norm);
            // The device is the source of truth — mirror it machine-side too.
            MachinePrefs::saveParam (apvts, "sensorDpi");
        }
    }
}

void SourceSetupPanel::postDpi()
{
    const int dpi = (dpiCombo.getSelectedId() == 1) ? 200 : 400;
    reconcileDpiToApvts (dpi);   // keep the VST's UDP parser in sync immediately

    deviceClient.postForm ("setDPI", "dpi=" + juce::String (dpi), {});
    setConnState (DC::State::Connecting, "DPI set to " + juce::String (dpi)
                                             + " -- device rebooting, reconnecting ...");
    setDeviceControlsEnabled (false);

    // The device reboots on a DPI change; reload once it is back (mirrors the
    // web page's waitForDeviceAndReload).
    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    juce::Timer::callAfterDelay (9000, [safe] { if (auto* s = safe.getComponent()) s->reload(); });
}

void SourceSetupPanel::postNetwork()
{
    juce::String body;
    body << "ip="       << netIp.get()
         << "&mask="    << netMask.get()
         << "&gateway=" << netGateway.get()
         << "&dest_ip=" << netDestIp.get()
         << "&udp_port=" << cisUdpPortEditor.getText().trim()
         << "&link_port=" << linkPortEditor.getText().trim()
         << "&stream_when_unbound=" << binVal (streamUnboundCombo);

    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    deviceClient.postForm ("updateNetworkConfig", body, [safe] (bool ok) {
        if (auto* s = safe.getComponent())
            s->setConnState (s->connState, ok ? "Network settings applied (device IP may change)"
                                              : "Network apply failed");
    });
}

//==============================================================================
void SourceSetupPanel::chooseFirmware()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Select firmware (.bin)",
        audioProcessor.sessions()->startDirFor (PathKeys::firmware, juce::File{}),
        "*.bin");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f.existsAsFile())
        {
            audioProcessor.sessions()->rememberDirFor (PathKeys::firmware, f);
            firmwareFile = f;
            fwFileLabel.setText (f.getFileName(), juce::dontSendNotification);
        }
    });
}

void SourceSetupPanel::uploadFirmware()
{
    if (! firmwareFile.existsAsFile())
    {
        setConnState (connState, "Choose a .bin file first");
        return;
    }

    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    Sp3ctraDialog::showConfirm (
        this,
        "Upload firmware?",
        "This will flash \"" + firmwareFile.getFileName() + "\" to the device and reboot it.\n"
        "Do not disconnect during the transfer.",
        "Upload", "Cancel",
        [safe] (bool confirmed)
    {
        auto* s = safe.getComponent();
        if (s == nullptr || ! confirmed)
            return;

        s->uploadProgress = 0.0;
        s->uploadProgressBar.repaint();
        s->setConnState (DC::State::Connecting, "Uploading firmware ...");

        juce::Component::SafePointer<SourceSetupPanel> safe2 (s);
        s->deviceClient.uploadFirmware (s->firmwareFile,
            [safe2] (double frac) { if (auto* p = safe2.getComponent()) p->uploadProgress = frac; },
            [safe2] (bool ok)
            {
                if (auto* p = safe2.getComponent())
                {
                    p->uploadProgress = ok ? 1.0 : 0.0;
                    p->setConnState (ok ? DC::State::Connecting : DC::State::Failed,
                                     ok ? "Firmware uploaded -- device rebooting" : "Firmware upload failed");
                }
            });
    });
}

void SourceSetupPanel::confirmFactoryReset()
{
    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    Sp3ctraDialog::showConfirm (
        this,
        "Factory reset?",
        "This resets ALL device settings (including network) to defaults and reboots it.",
        "Reset", "Cancel",
        [safe] (bool confirmed)
    {
        if (auto* s = safe.getComponent(); s != nullptr && confirmed)
        {
            s->deviceClient.postForm ("factoryReset", "START_FACTORY_RESET", {});
            s->setConnState (DC::State::Connecting, "Factory reset -- device rebooting");
            s->setDeviceControlsEnabled (false);
        }
    });
}

//==============================================================================
void SourceSetupPanel::applyLink()
{
    // Device IP (HTTP host) — no UDP restart needed.
    auto ipB = [this] (const char* id, juce::TextEditor& e) {
        const int v = juce::jlimit (0, 255, e.getText().getIntValue());
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) v));
    };
    ipB ("deviceIpByte1", deviceIp.box[0]);
    ipB ("deviceIpByte2", deviceIp.box[1]);
    ipB ("deviceIpByte3", deviceIp.box[2]);
    ipB ("deviceIpByte4", deviceIp.box[3]);

    // UDP transport — batch so the receiver restarts only once.
    audioProcessor.beginUdpBatchUpdate();

    const int port = udpPortEditor.getText().getIntValue();
    if (port >= 1024 && port <= 65535)
        if (auto* p = apvts.getParameter ("udpPort"))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) port));

    auto applyByte = [this] (juce::TextEditor& e, const char* id) {
        const int v = e.getText().getIntValue();
        if (v >= 0 && v <= 255)
            if (auto* p = apvts.getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 ((float) v));
    };
    applyByte (udpAddr.box[0], "udpByte1");
    applyByte (udpAddr.box[1], "udpByte2");
    applyByte (udpAddr.box[2], "udpByte3");
    applyByte (udpAddr.box[3], "udpByte4");

    audioProcessor.endUdpBatchUpdate();

    // LINK config is machine-scoped: persist it in the machine file so a
    // restored session/DAW blob can never repoint this computer's link.
    for (auto* id : { "udpPort", "udpByte1", "udpByte2", "udpByte3", "udpByte4",
                      "deviceIpByte1", "deviceIpByte2",
                      "deviceIpByte3", "deviceIpByte4" })
        MachinePrefs::saveParam (apvts, id);

    applyLinkButton.setButtonText ("Applied!");
    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    juce::Timer::callAfterDelay (1500, [safe] {
        if (auto* s = safe.getComponent()) s->applyLinkButton.setButtonText ("Apply Link");
    });

    reload();   // re-point the HTTP client at the (possibly new) host and refresh
}

//==============================================================================
void SourceSetupPanel::paint (juce::Graphics& g)
{
    SetupUI::paintHeader (g, *this, "SP3CTRA -- SETUP", accent);
}

//==============================================================================
void SourceSetupPanel::resized()
{
    const int w = getWidth();
    constexpr int rowH   = Sp3ctraTheme::kRowStep;
    constexpr int labelW = Sp3ctraTheme::kLabelW;
    constexpr int ctrlH  = Sp3ctraTheme::kControlH;
    const int ctrlX = Sp3ctraTheme::kHPad + labelW + Sp3ctraTheme::kGap;
    const int ctrlW = juce::jmin (300, w - ctrlX - Sp3ctraTheme::kHPad);
    const int vc    = (rowH - ctrlH) / 2;

    int y = SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap;

    auto row = [&] (juce::Component& label, juce::Component& ctrl)
    {
        label.setBounds (Sp3ctraTheme::kHPad, y + vc, labelW, ctrlH);
        ctrl .setBounds (ctrlX,               y + vc, ctrlW,  ctrlH);
        y += rowH;
    };
    auto rowIp = [&] (juce::Component& label, IpBytes& ip)
    {
        label.setBounds (Sp3ctraTheme::kHPad, y + vc, labelW, ctrlH);
        ip.layout (ctrlX, y + vc, ctrlW, ctrlH);
        y += rowH;
    };
    auto rowButton = [&] (juce::Component& b, int bw)
    {
        b.setBounds (ctrlX, y + vc, juce::jmin (bw, ctrlW), ctrlH);
        y += rowH;
    };
    auto section = [&] (juce::Component& header)
    {
        y += Sp3ctraTheme::kSectionGap;
        header.setBounds (Sp3ctraTheme::kHPad, y, w - 2 * Sp3ctraTheme::kHPad, Sp3ctraTheme::kSectionH);
        y += Sp3ctraTheme::kSectionH;
    };

    // LINK — discovered devices (one row each), status, policy, transport
    devicesLabel.setBounds (Sp3ctraTheme::kHPad, y + vc, labelW, ctrlH);
    {
        const int useW = 80;
        const int textW = w - ctrlX - Sp3ctraTheme::kHPad - useW - Sp3ctraTheme::kGap;
        for (auto& r : deviceRows)
        {
            r.text.setBounds (ctrlX, y + vc, juce::jmax (100, textW), ctrlH);
            r.button.setBounds (ctrlX + juce::jmax (100, textW) + Sp3ctraTheme::kGap, y + vc, useW, ctrlH);
            y += rowH;
        }
    }
    linkStatusLabel.setBounds (Sp3ctraTheme::kHPad, y + vc, w - 2 * Sp3ctraTheme::kHPad, ctrlH);
    y += rowH;
    autoBindToggle.setBounds (Sp3ctraTheme::kHPad, y + vc, w - 2 * Sp3ctraTheme::kHPad, ctrlH);
    y += rowH;
    rowIp (deviceIpLabel, deviceIp);
    row   (udpPortLabel, udpPortEditor);
    rowIp (udpAddressLabel, udpAddr);
    {
        applyLinkButton.setBounds (ctrlX, y + vc, juce::jmin (120, ctrlW), ctrlH);
        retryButton.setBounds (ctrlX + juce::jmin (120, ctrlW) + Sp3ctraTheme::kGap, y + vc, 70, ctrlH);
        y += rowH;
    }
    connStatusLabel.setBounds (Sp3ctraTheme::kHPad, y + vc, w - 2 * Sp3ctraTheme::kHPad, ctrlH);
    y += rowH;

    // CIS
    section (cisHeader);
    row (dpiLabel, dpiCombo);
    row (ovspLabel, ovspCombo);
    row (lpsLabel, lpsValue);
    row (handLabel, handCombo);
    rowButton (calibrateCisButton, 180);

    // IMU
    section (imuHeader);
    row (gyroLabel, gyroCombo);
    row (accelLabel, accelCombo);
    rowButton (calibrateImuButton, 180);

    // GUI
    section (guiHeader);
    row (showImuLabel, showImuCombo);
    row (invertLabel, invertCombo);
    row (screensaverLabel, screensaverEditor);
    row (motionAccLabel, motionAccEditor);
    row (motionGyroLabel, motionGyroEditor);

    // NETWORK (device)
    section (netHeader);
    rowIp (ipLabel, netIp);
    rowIp (maskLabel, netMask);
    rowIp (gatewayLabel, netGateway);
    rowIp (destIpLabel, netDestIp);
    row (cisUdpPortLabel, cisUdpPortEditor);
    row (linkPortLabel, linkPortEditor);
    row (streamUnboundLabel, streamUnboundCombo);
    rowButton (applyNetworkButton, 140);

    // FIRMWARE
    section (fwHeader);
    fwVersionLabel.setBounds (Sp3ctraTheme::kHPad, y + vc, w - 2 * Sp3ctraTheme::kHPad, ctrlH);
    y += rowH;
    {
        // choose button + file name on one row
        chooseFwButton.setBounds (ctrlX, y + vc, juce::jmin (110, ctrlW), ctrlH);
        fwFileLabel.setBounds (ctrlX + juce::jmin (110, ctrlW) + Sp3ctraTheme::kGap, y + vc,
                               ctrlW - juce::jmin (110, ctrlW) - Sp3ctraTheme::kGap, ctrlH);
        y += rowH;
    }
    rowButton (uploadFwButton, 160);
    uploadProgressBar.setBounds (ctrlX, y + vc, ctrlW, ctrlH);
    y += rowH;
    rowButton (factoryResetButton, 140);
}
