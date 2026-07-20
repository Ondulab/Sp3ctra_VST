#include "SourceSetupPanel.h"
#include "SetupHeader.h"
#include "../../Sp3ctraConstants.h"
#include "../../UITheme.h"

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
    // ── LINK block (APVTS-persisted transport params) ────────────────────────
    initLabel (deviceIpLabel, "Device IP:");
    deviceIp.init (*this);
    deviceIp.set ((int) apvts.getRawParameterValue ("deviceIpByte1")->load(),
                  (int) apvts.getRawParameterValue ("deviceIpByte2")->load(),
                  (int) apvts.getRawParameterValue ("deviceIpByte3")->load(),
                  (int) apvts.getRawParameterValue ("deviceIpByte4")->load());

    initLabel (udpPortLabel, "UDP Port:");
    initEditor (udpPortEditor, 5, "0123456789");
    udpPortEditor.setText (juce::String ((int) apvts.getRawParameterValue ("udpPort")->load()), false);

    initLabel (udpAddressLabel, "UDP Address:");
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

    // ── MIDI button mapping ──────────────────────────────────────────────────────
    initSection (midiHeader, "MIDI CHANNELS (SW1-SW3)");
    juce::StringArray channels;
    for (int ch = 1; ch <= 16; ++ch) channels.add (juce::String (ch));
    for (int i = 0; i < 3; ++i)
    {
        initLabel (swLabel[i], "SW" + juce::String (i + 1) + ":");
        initCombo (chCombo[i], channels);
        initCombo (cmdCombo[i], { "CC", "NOTE" });
        initEditor (paramEditor[i], 3, "0123456789");
    }
    applyMidiButton.setButtonText ("Apply MIDI");
    applyMidiButton.onClick = [this] { postMidiButtons(); };
    addAndMakeVisible (applyMidiButton);

    // ── Device network configuration ─────────────────────────────────────────────
    initSection (netHeader, "NETWORK (DEVICE)");
    initLabel (ipLabel, "IP Addr:");        netIp.init (*this);
    initLabel (maskLabel, "Subnet Mask:");  netMask.init (*this);
    initLabel (gatewayLabel, "Gateway:");   netGateway.init (*this);
    initLabel (destIpLabel, "Dest IP:");    netDestIp.init (*this);
    initLabel (cisUdpPortLabel, "CIS UDP Port:");
    initEditor (cisUdpPortEditor, 5, "0123456789");

    initLabel (mdnsLabel, "mDNS:");
    initCombo (mdnsCombo, { "Off", "On" });
    mdnsCombo.onChange = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setMdnsEnabled", "mdns_enabled=" + juce::String (binVal (mdnsCombo)), {});
    };

    initLabel (rtpModeLabel, "RTP-MIDI:");
    initCombo (rtpModeCombo, { "Server", "Client" });
    rtpModeCombo.onChange = [this] {
        if (! applyingRemote)
            deviceClient.postForm ("setRtpMidiMode", "rtpmidi_mode=" + juce::String (binVal (rtpModeCombo)), {});
    };

    initLabel (midiCtrlPortLabel, "MIDI Ctrl Port:");
    initEditor (midiCtrlPortEditor, 5, "0123456789");
    midiCtrlPortEditor.onTextChange = [this] { updateMidiDataPortDisplay(); };

    initLabel (midiDataPortLabel, "MIDI Data Port:");
    midiDataPortValue.setJustificationType (juce::Justification::centredLeft);
    midiDataPortValue.setFont (juce::FontOptions (Sp3ctraTheme::kFontSettings));
    midiDataPortValue.setColour (juce::Label::textColourId, juce::Colour (Sp3ctraTheme::kColTextMuted));
    addAndMakeVisible (midiDataPortValue);

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
    deviceClient.cancel();
}

//==============================================================================
void SourceSetupPanel::visibilityChanged()
{
    if (isShowing())
        reload();
    else
    {
        deviceClient.cancel();   // HTTP is connectionless — just stop issuing
        loading = false;         // allow the next show to reload
    }
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

    // MIDI mapping
    for (int i = 0; i < 3; ++i)
    {
        chCombo[i].setSelectedId (juce::jlimit (1, 16, cfg.midi[i].channel + 1), juce::dontSendNotification);
        cmdCombo[i].setSelectedId (binId (cfg.midi[i].command), juce::dontSendNotification);
        paramEditor[i].setText (juce::String (cfg.midi[i].param), false);
    }

    // Network
    netIp.set (cfg.network.ip);
    netMask.set (cfg.network.mask);
    netGateway.set (cfg.network.gateway);
    netDestIp.set (cfg.network.destIp);
    cisUdpPortEditor.setText (juce::String (cfg.network.udpPort), false);
    mdnsCombo.setSelectedId (binId (cfg.mdnsEnabled ? 1 : 0), juce::dontSendNotification);
    rtpModeCombo.setSelectedId (binId (cfg.rtpMidiMode), juce::dontSendNotification);
    midiCtrlPortEditor.setText (juce::String (cfg.network.rtpMidiControlPort), false);
    updateMidiDataPortDisplay();

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
        &applyMidiButton,
        &cisUdpPortEditor, &mdnsCombo, &rtpModeCombo, &midiCtrlPortEditor, &applyNetworkButton,
        &uploadFwButton, &factoryResetButton
    };
    for (auto* c : ctrls) c->setEnabled (on);
    for (int i = 0; i < 3; ++i) { chCombo[i].setEnabled (on); cmdCombo[i].setEnabled (on); paramEditor[i].setEnabled (on); }
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
            p->setValueNotifyingHost (norm);
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

void SourceSetupPanel::postMidiButtons()
{
    juce::String body;
    for (int i = 0; i < 3; ++i)
    {
        const int ch  = juce::jmax (0, chCombo[i].getSelectedId() - 1);
        const int cmd = binVal (cmdCombo[i]);
        const int prm = paramEditor[i].getText().getIntValue();
        body << "b" << i << "_ch=" << ch << "&b" << i << "_cmd=" << cmd
             << "&b" << i << "_param=" << prm;
        if (i < 2) body << "&";
    }
    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    deviceClient.postForm ("setMidiButtonConfig", body, [safe] (bool ok) {
        if (auto* s = safe.getComponent())
            s->setConnState (s->connState, ok ? "MIDI mapping saved" : "MIDI save failed");
    });
}

void SourceSetupPanel::postNetwork()
{
    juce::String body;
    body << "ip="       << netIp.get()
         << "&mask="    << netMask.get()
         << "&gateway=" << netGateway.get()
         << "&dest_ip=" << netDestIp.get()
         << "&udp_port=" << cisUdpPortEditor.getText().trim()
         << "&rtpmidi_control_port=" << midiCtrlPortEditor.getText().trim();

    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    deviceClient.postForm ("updateNetworkConfig", body, [safe] (bool ok) {
        if (auto* s = safe.getComponent())
            s->setConnState (s->connState, ok ? "Network settings applied (device IP may change)"
                                              : "Network apply failed");
    });
}

void SourceSetupPanel::updateMidiDataPortDisplay()
{
    const int ctrl = midiCtrlPortEditor.getText().getIntValue();
    midiDataPortValue.setText (ctrl > 0 ? juce::String (juce::jmin (65535, ctrl + 1))
                                        : juce::String ("--"),
                               juce::dontSendNotification);
}

//==============================================================================
void SourceSetupPanel::chooseFirmware()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Select firmware (.bin)",
                                                       juce::File{}, "*.bin");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f.existsAsFile())
        {
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
    juce::AlertWindow::showOkCancelBox (
        juce::AlertWindow::WarningIcon,
        "Upload firmware?",
        "This will flash \"" + firmwareFile.getFileName() + "\" to the device and reboot it.\n"
        "Do not disconnect during the transfer.",
        "Upload", "Cancel", this,
        juce::ModalCallbackFunction::create ([safe] (int result)
    {
        auto* s = safe.getComponent();
        if (s == nullptr || result == 0)
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
    }));
}

void SourceSetupPanel::confirmFactoryReset()
{
    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    juce::AlertWindow::showOkCancelBox (
        juce::AlertWindow::WarningIcon,
        "Factory reset?",
        "This resets ALL device settings (including network) to defaults and reboots it.",
        "Reset", "Cancel", this,
        juce::ModalCallbackFunction::create ([safe] (int result)
    {
        if (auto* s = safe.getComponent(); s != nullptr && result != 0)
        {
            s->deviceClient.postForm ("factoryReset", "START_FACTORY_RESET", {});
            s->setConnState (DC::State::Connecting, "Factory reset -- device rebooting");
            s->setDeviceControlsEnabled (false);
        }
    }));
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

    // LINK
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

    // MIDI
    section (midiHeader);
    for (int i = 0; i < 3; ++i)
    {
        swLabel[i].setBounds (Sp3ctraTheme::kHPad, y + vc, labelW, ctrlH);
        const int chW  = juce::jmin (70, ctrlW / 3);
        const int cmdW = juce::jmin (80, ctrlW / 3);
        const int prmW = juce::jmax (44, ctrlW - chW - cmdW - 2 * Sp3ctraTheme::kGap);
        int x = ctrlX;
        chCombo[i].setBounds (x, y + vc, chW, ctrlH);   x += chW + Sp3ctraTheme::kGap;
        cmdCombo[i].setBounds (x, y + vc, cmdW, ctrlH);  x += cmdW + Sp3ctraTheme::kGap;
        paramEditor[i].setBounds (x, y + vc, prmW, ctrlH);
        y += rowH;
    }
    rowButton (applyMidiButton, 140);

    // NETWORK (device)
    section (netHeader);
    rowIp (ipLabel, netIp);
    rowIp (maskLabel, netMask);
    rowIp (gatewayLabel, netGateway);
    rowIp (destIpLabel, netDestIp);
    row (cisUdpPortLabel, cisUdpPortEditor);
    row (mdnsLabel, mdnsCombo);
    row (rtpModeLabel, rtpModeCombo);
    row (midiCtrlPortLabel, midiCtrlPortEditor);
    row (midiDataPortLabel, midiDataPortValue);
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
