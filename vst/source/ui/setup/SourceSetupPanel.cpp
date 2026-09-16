#include "SourceSetupPanel.h"
#include "SetupHeader.h"
#include <cstring>   // f32 <-> bit-pattern packing for SLP CFG
#include "../../Sp3ctraConstants.h"
#include "../../UITheme.h"
#include "../../session/MachinePrefs.h"
#include "../../communication/link/slp_rx_state.h"
#include "../../Sp3ctraDialog.h"

using DC = Sp3ctraDeviceClient;

namespace
{
    constexpr int kOvspVals[] = { 1, 2, 4, 8, 16, 32 };

    // Status palette — juce::Colours::green/red are unreadable on the dark
    // panel (0xff1e1e1e). These are the same semantics, lifted to a legible
    // luminance so a device row / status line can actually be scanned.
    constexpr uint32_t kColOk   = 0xff7bd88f;   // bound / connected
    constexpr uint32_t kColWarn = 0xffe8b04b;   // searching / binding / in use
    constexpr uint32_t kColErr  = 0xffe86a6a;   // unsupported / failed

    /// Inline separator between the fields of a status line ("  ·  ").
    inline juce::String sep()  { return juce::String::fromUTF8 ("  \xc2\xb7  "); }
    /// "You are here" marker in front of the bound device row.
    inline juce::String dot()  { return juce::String::fromUTF8 ("\xe2\x97\x8f  "); }

    int  ovspToId   (int v)  { for (int i = 0; i < 6; ++i) if (kOvspVals[i] == v) return i + 1; return 1; }
    int  idToOvsp   (int id) { return kOvspVals[juce::jlimit (1, 6, id) - 1]; }
    int  binId      (int v)  { return v != 0 ? 2 : 1; }            // 0/1 → combo id 1/2
    int  binVal     (const juce::ComboBox& c) { return c.getSelectedId() == 2 ? 1 : 0; }
    int  zeroBasedVal (const juce::ComboBox& c) { return juce::jmax (0, c.getSelectedId() - 1); }

    // ── SLP CFG value packing (sp3ctra_link.h wire conventions) ──────────────
    slp_cfg_item cfgItem (uint16_t id, uint8_t type, uint32_t value)
    {
        slp_cfg_item it {};
        it.id = id; it.type = type; it.value = value;
        return it;
    }
    uint32_t packIp (const juce::String& dotted)
    {
        juce::StringArray p;
        p.addTokens (dotted, ".", "");
        uint32_t v = 0;
        for (int i = 0; i < 4 && i < p.size(); ++i)
            v |= (uint32_t) juce::jlimit (0, 255, p[i].trim().getIntValue()) << (8 * i);
        return v;
    }
    juce::String unpackIp (uint32_t v)
    {
        return juce::String (v & 255) + "." + juce::String ((v >> 8) & 255) + "."
             + juce::String ((v >> 16) & 255) + "." + juce::String ((v >> 24) & 255);
    }
    uint32_t f32Bits (float f)  { uint32_t u; std::memcpy (&u, &f, 4); return u; }
    float    bitsF32 (uint32_t u) { float f; std::memcpy (&f, &u, 4); return f; }
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
        // Label's default 5 px side border leaves no room in this narrow gap —
        // addFittedText then gives up and paints an ellipsis instead of the dot.
        d.setBorderSize (juce::BorderSize<int> (0));
        parent.addAndMakeVisible (d);
    }
}

void SourceSetupPanel::IpBytes::layout (int x, int y, int w, int h)
{
    constexpr int dotW = 2 * Sp3ctraTheme::kGap;
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
    // ── DEVICES: the units Sp3ctra Link sees on the network ──────────────────
    initSection (devicesHeader, "DEVICES");
    for (auto& r : deviceRows)
    {
        r.text.setFont (juce::FontOptions (Sp3ctraTheme::kFontSettings));
        r.text.setJustificationType (juce::Justification::centredLeft);
        addChildComponent (r.text);
        r.button.setButtonText ("USE");
        addChildComponent (r.button);
    }
    noDeviceLabel.setJustificationType (juce::Justification::centredLeft);
    noDeviceLabel.setFont (juce::Font (juce::FontOptions (Sp3ctraTheme::kFontTiny)).italicised());
    noDeviceLabel.setColour (juce::Label::textColourId, juce::Colour (Sp3ctraTheme::kColTextMuted));
    noDeviceLabel.setText ("no unit on the network yet ...", juce::dontSendNotification);
    addChildComponent (noDeviceLabel);

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

    initSection (linkHeader, "LINK (VST SIDE)");
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
            cfgSet (SLP_CFG_OVERSAMPLING, SLP_CFG_U8, (uint32_t) idToOvsp (ovspCombo.getSelectedId()));
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
            cfgSet (SLP_CFG_HANDEDNESS, SLP_CFG_U8, (uint32_t) zeroBasedVal (handCombo));
    };

    calibrateCisButton.setButtonText ("Start CIS Calibration");
    calibrateCisButton.onClick = [this] {
        if (auto* l = link()) l->requestCalibration (SLP_CAL_CIS);
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
            cfgSet (SLP_CFG_GYRO_FS, SLP_CFG_U8, (uint32_t) zeroBasedVal (gyroCombo));
    };

    initLabel (accelLabel, "Accel:");
    initCombo (accelCombo, { "+/-16 g", "+/-8 g", "+/-4 g", "+/-2 g" });
    accelCombo.onChange = [this] {
        if (! applyingRemote)
            cfgSet (SLP_CFG_ACCEL_FS, SLP_CFG_U8, (uint32_t) zeroBasedVal (accelCombo));
    };

    calibrateImuButton.setButtonText ("Start IMU Calibration");
    calibrateImuButton.onClick = [this] {
        if (auto* l = link()) l->requestCalibration (SLP_CAL_IMU);
        setConnState (connState, juce::String::fromUTF8("IMU calibration started — keep device still (~1.5s)"));
    };
    addAndMakeVisible (calibrateImuButton);

    // ── GUI & screensaver ───────────────────────────────────────────────────────
    initSection (guiHeader, "GUI & SCREENSAVER");
    initLabel (showImuLabel, "Show IMU:");
    initCombo (showImuCombo, { "Off", "On" });
    showImuCombo.onChange = [this] {
        if (! applyingRemote)
            cfgSet (SLP_CFG_GUI_SHOW_IMU, SLP_CFG_U8, (uint32_t) binVal (showImuCombo));
    };

    initLabel (invertLabel, "Invert CIS:");
    initCombo (invertCombo, { "Off", "On" });
    invertCombo.onChange = [this] {
        if (! applyingRemote)
            cfgSet (SLP_CFG_GUI_INVERT, SLP_CFG_U8, (uint32_t) binVal (invertCombo));
    };

    initLabel (screensaverLabel, "Timeout (s):");
    initEditor (screensaverEditor, 4, "0123456789");
    screensaverEditor.onReturnKey = [this] {
        if (! applyingRemote)
            cfgSet (SLP_CFG_SCREENSAVER_S, SLP_CFG_U16,
                    (uint32_t) screensaverEditor.getText().trim().getIntValue());
    };
    screensaverEditor.onFocusLost = screensaverEditor.onReturnKey;

    initLabel (motionAccLabel, "Motion Acc (g):");
    initEditor (motionAccEditor, 5, "0123456789.");
    motionAccEditor.onReturnKey = [this] {
        if (! applyingRemote)
            cfgSet (SLP_CFG_MOTION_THR_ACC, SLP_CFG_F32,
                    f32Bits (motionAccEditor.getText().trim().getFloatValue()));
    };
    motionAccEditor.onFocusLost = motionAccEditor.onReturnKey;

    initLabel (motionGyroLabel, "Motion Gyro (dps):");
    initEditor (motionGyroEditor, 5, "0123456789.");
    motionGyroEditor.onReturnKey = [this] {
        if (! applyingRemote)
            cfgSet (SLP_CFG_MOTION_THR_GYRO, SLP_CFG_F32,
                    f32Bits (motionGyroEditor.getText().trim().getFloatValue()));
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
    initLabel (fwVersionCaption, "Version:");
    initLabel (fwVersionLabel, "--", juce::Justification::centredLeft);
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
    netFlashButton.setButtonText ("Flash via Network (bootloader)");
    netFlashButton.onClick = [this] { netFlashFirmware(); };
    addAndMakeVisible (netFlashButton);
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
    netFlash.cancel();
}

//==============================================================================
// A page restored with the session is made visible while the window itself is
// still off screen: visibilityChanged() then sees isShowing() == false and the
// link poll never starts (status stuck on its ctor text). The hierarchy change
// that puts the window on screen is the second chance to notice.
void SourceSetupPanel::parentHierarchyChanged()
{
    if (isShowing() != isTimerRunning())
        visibilityChanged();
}

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
        deviceClient.cancel();   // firmware upload aside, HTTP is no longer used
        cfgAskedMs = 0;          // a hidden page never times out
    }
}

//==============================================================================
Sp3ctraLink* SourceSetupPanel::link() const
{
    return audioProcessor.getLink();
}

void SourceSetupPanel::timerCallback()
{
    refreshLink();
    // CFG_GET burst answered nothing for 3 s: surface it (Retry re-asks).
    if (cfgAskedMs > 0 && connState == DC::State::Connecting
        && juce::Time::getMillisecondCounterHiRes() - cfgAskedMs > 3000.0)
    {
        cfgAskedMs = 0;
        setConnState (DC::State::Failed, "No CFG reply from the device -- Retry");
        setDeviceControlsEnabled (false);
    }
}

void SourceSetupPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshLink();
    populateFromCfg();   // CFG_REPLY values land in the link's cache
}

void SourceSetupPanel::refreshLink()
{
    auto* l = link();
    if (l == nullptr)
    {
        linkStatusLabel.setText ("Sp3ctra Link: not running (pipeline not started)", juce::dontSendNotification);
        for (auto& r : deviceRows) { r.text.setVisible (false); r.button.setVisible (false); }
        if (shownDevices != 0) { shownDevices = 0; resized(); }   // collapse the list
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
        if (! d.supported())            { state = "needs fw 4.0+"; col = juce::Colour (kColErr); }
        else if (ours)                  { state = st.state == Sp3ctraLink::State::Bound ? "bound" : "binding ..."; col = juce::Colour (kColOk); }
        else if (d.bound)               { state = "used by " + d.boundPeerIp; col = juce::Colour (kColWarn); }
        else                            { state = "free"; }
        if (d.uid == preferred)         state << "  (preferred)";

        r.uid = d.uid;
        r.text.setText ((ours ? dot() : juce::String ("     "))
                        + d.name + sep() + d.ip + sep() + "fw " + d.fwString() + sep() + state,
                        juce::dontSendNotification);
        r.text.setColour (juce::Label::textColourId, col);
        r.text.setVisible (true);
        r.button.setButtonText (ours ? "RELEASE" : "USE");
        r.button.setEnabled (ours || (d.supported() && ! d.bound));
        r.button.onClick = [this, uid = d.uid, ours] { if (ours) releaseDevice(); else useDevice (uid); };
        r.button.setVisible (true);
    }

    // The list collapses to the units actually seen — no reserved blank rows.
    const int shown = juce::jmin (kMaxDeviceRows, (int) dev.size());
    if (shown != shownDevices) { shownDevices = shown; resized(); }

    // ── status line ──────────────────────────────────────────────────────────
    juce::String text;
    juce::Colour col = juce::Colour (Sp3ctraTheme::kColTextMuted);
    switch (st.state)
    {
        case Sp3ctraLink::State::Off:       text = "Sp3ctra Link: off"; break;
        case Sp3ctraLink::State::Searching: text = "Searching ... " + juce::String ((int) dev.size()) + " device(s) seen"; col = juce::Colour (kColWarn); break;
        case Sp3ctraLink::State::Binding:   text = "Binding " + st.deviceName + " ..."; col = juce::Colour (kColWarn); break;
        case Sp3ctraLink::State::Bound:
        {
            slp_rx_stats rx {};
            slp_rx_stats_snapshot (&rx);
            // The bound unit is already named in its (dotted) row above — this
            // line carries the live metrics only, so it fits the content column.
            text << "Streaming" << sep() << st.layout.dpi << " DPI" << sep()
                 << st.lineRateLps << " lps" << sep() << "HID " << st.layout.hidRateHz << " Hz" << sep()
                 << "rtt " << st.rttMs << " ms" << sep() << juce::String (st.tempC, 1) << " C" << sep()
                 << "lost " << (int) (rx.line_lost + rx.hid_lost);
            if (rx.legacy_datagrams > 0) text << sep() << "[legacy stream!]";
            col = juce::Colour (kColOk);
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
// Device settings travel over the bound SLP session (CFG_GET / CFG_SET): the
// session is the proof of ownership, so no HTTP admin password is involved.
// Only the firmware upload and the factory reset still use the web server.
//==============================================================================
void SourceSetupPanel::reload()
{
    auto* l = link();
    if (l == nullptr || ! l->isBound())
    {
        setConnState (DC::State::Idle, "No session -- bind a device above first");
        setDeviceControlsEnabled (false);
        return;
    }

    cfgAskedMs = juce::Time::getMillisecondCounterHiRes();
    setConnState (DC::State::Connecting, "Reading device settings over the link ...");
    l->requestConfig ({ SLP_CFG_DPI, SLP_CFG_OVERSAMPLING, SLP_CFG_HANDEDNESS,
                        SLP_CFG_GYRO_FS, SLP_CFG_ACCEL_FS,
                        SLP_CFG_GUI_SHOW_IMU, SLP_CFG_GUI_INVERT, SLP_CFG_SCREENSAVER_S,
                        SLP_CFG_MOTION_THR_ACC, SLP_CFG_MOTION_THR_GYRO,
                        SLP_CFG_NET_IP, SLP_CFG_NET_MASK, SLP_CFG_NET_GW,
                        SLP_CFG_NET_DEST_IP, SLP_CFG_STREAM_PORT,
                        SLP_CFG_STREAM_WHEN_UNBOUND, SLP_CFG_LINK_PORT,
                        SLP_CFG_LINE_RATE });
}

void SourceSetupPanel::cfgSet (uint16_t id, uint8_t type, uint32_t value)
{
    if (auto* l = link(); l != nullptr && l->isBound())
        l->writeConfig ({ cfgItem (id, type, value) });
}

void SourceSetupPanel::populateFromCfg()
{
    auto* l = link();
    if (l == nullptr)
        return;

    const juce::ScopedValueSetter<bool> guard (applyingRemote, true);
    Sp3ctraLink::CfgValue v;
    bool complete = true;
    auto have = [&] (uint16_t id) { const bool ok = l->configValue (id, v); complete = complete && ok; return ok; };
    // This runs on every link event: never clobber a field being edited.
    auto setIdle = [] (juce::TextEditor& e, const juce::String& text)
    { if (! e.hasKeyboardFocus (true)) e.setText (text, false); };
    auto ipIdle = [] (IpBytes& ip)
    { for (auto& b : ip.box) if (b.hasKeyboardFocus (true)) return false; return true; };

    // CIS — DPI is reconciled into APVTS so UDP parsing matches the live stream.
    if (have (SLP_CFG_DPI))
    {
        dpiCombo.setSelectedId (v.value == 200 ? 1 : 2, juce::dontSendNotification);
        reconcileDpiToApvts ((int) v.value);
    }
    if (have (SLP_CFG_OVERSAMPLING)) ovspCombo.setSelectedId (ovspToId ((int) v.value), juce::dontSendNotification);
    if (have (SLP_CFG_LINE_RATE))    lpsValue.setText (juce::String ((int) v.value) + " lps", juce::dontSendNotification);
    if (have (SLP_CFG_HANDEDNESS))   handCombo.setSelectedId (v.value == 0 ? 1 : 2, juce::dontSendNotification);

    // IMU
    if (have (SLP_CFG_GYRO_FS))  gyroCombo.setSelectedId (juce::jlimit (1, 8, (int) v.value + 1), juce::dontSendNotification);
    if (have (SLP_CFG_ACCEL_FS)) accelCombo.setSelectedId (juce::jlimit (1, 4, (int) v.value + 1), juce::dontSendNotification);

    // GUI
    if (have (SLP_CFG_GUI_SHOW_IMU))  showImuCombo.setSelectedId (binId ((int) v.value), juce::dontSendNotification);
    if (have (SLP_CFG_GUI_INVERT))    invertCombo.setSelectedId (binId ((int) v.value), juce::dontSendNotification);
    if (have (SLP_CFG_SCREENSAVER_S)) setIdle (screensaverEditor, juce::String ((int) v.value));
    if (have (SLP_CFG_MOTION_THR_ACC))  setIdle (motionAccEditor, juce::String (bitsF32 (v.value), 2));
    if (have (SLP_CFG_MOTION_THR_GYRO)) setIdle (motionGyroEditor, juce::String (bitsF32 (v.value), 1));

    // Network
    if (have (SLP_CFG_NET_IP)      && ipIdle (netIp))      netIp.set (unpackIp (v.value), juce::dontSendNotification);
    if (have (SLP_CFG_NET_MASK)    && ipIdle (netMask))    netMask.set (unpackIp (v.value), juce::dontSendNotification);
    if (have (SLP_CFG_NET_GW)      && ipIdle (netGateway)) netGateway.set (unpackIp (v.value), juce::dontSendNotification);
    if (have (SLP_CFG_NET_DEST_IP) && ipIdle (netDestIp))  netDestIp.set (unpackIp (v.value), juce::dontSendNotification);
    if (have (SLP_CFG_STREAM_PORT)) setIdle (cisUdpPortEditor, juce::String ((int) v.value));
    if (have (SLP_CFG_LINK_PORT))   setIdle (linkPortEditor, juce::String ((int) v.value));
    if (have (SLP_CFG_STREAM_WHEN_UNBOUND))
        streamUnboundCombo.setSelectedId (binId ((int) v.value), juce::dontSendNotification);

    // Firmware version comes from the bound device's ANNOUNCE, not from CFG.
    {
        const auto st = l->status();
        for (const auto& d : l->devices())
            if (d.uid == st.deviceUid)
                fwVersionLabel.setText (d.fwString(), juce::dontSendNotification);
    }

    if (complete && connState != DC::State::Connected)
    {
        cfgAskedMs = 0;
        setConnState (DC::State::Connected, "Device settings loaded over the link");
        setDeviceControlsEnabled (true);
    }
}

void SourceSetupPanel::setConnState (DC::State st, const juce::String& detail)
{
    connState = st;
    juce::String text = detail;
    juce::Colour col = juce::Colour (Sp3ctraTheme::kColTextMuted);
    switch (st)
    {
        case DC::State::Connecting: col = juce::Colour (kColWarn); if (text.isEmpty()) text = "Connecting ..."; break;
        case DC::State::Connected:  col = juce::Colour (kColOk);   if (text.isEmpty()) text = "Connected"; break;
        case DC::State::Failed:     col = juce::Colour (kColErr);  if (text.isEmpty()) text = "Connection failed"; break;
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

    // The groups below LINK only exist while the device answers: dim their
    // titles so an offline page reads as "not loaded", not as "empty".
    for (auto* h : { &cisHeader, &imuHeader, &guiHeader, &netHeader, &fwHeader })
        h->setColour (juce::Label::textColourId, on ? accent : accent.withAlpha (0.45f));
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

    cfgSet (SLP_CFG_DPI, SLP_CFG_U16, (uint32_t) dpi);
    setConnState (DC::State::Connecting, "DPI set to " + juce::String (dpi)
                                             + " -- device rebooting, reconnecting ...");
    setDeviceControlsEnabled (false);

    // The device reboots on a DPI change; the link re-binds by itself and
    // refreshLink()'s bound transition reloads the settings. The delayed
    // reload is only a safety net for a reboot the transition missed.
    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    juce::Timer::callAfterDelay (9000, [safe] { if (auto* s = safe.getComponent()) s->reload(); });
}

void SourceSetupPanel::postNetwork()
{
    auto* l = link();
    if (l == nullptr || ! l->isBound())
    {
        setConnState (connState, "No session -- bind a device first");
        return;
    }
    l->writeConfig ({
        cfgItem (SLP_CFG_NET_IP,      SLP_CFG_IP4, packIp (netIp.get())),
        cfgItem (SLP_CFG_NET_MASK,    SLP_CFG_IP4, packIp (netMask.get())),
        cfgItem (SLP_CFG_NET_GW,      SLP_CFG_IP4, packIp (netGateway.get())),
        cfgItem (SLP_CFG_NET_DEST_IP, SLP_CFG_IP4, packIp (netDestIp.get())),
        cfgItem (SLP_CFG_STREAM_PORT, SLP_CFG_U16, (uint32_t) cisUdpPortEditor.getText().trim().getIntValue()),
        cfgItem (SLP_CFG_LINK_PORT,   SLP_CFG_U16, (uint32_t) linkPortEditor.getText().trim().getIntValue()),
        cfgItem (SLP_CFG_STREAM_WHEN_UNBOUND, SLP_CFG_U8, (uint32_t) binVal (streamUnboundCombo)),
    });
    setConnState (connState, "Network settings sent -- the device reboots if they changed");
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
        s->deviceClient.setHost (s->deviceHostFromApvts());   // upload stays on HTTP
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

//==============================================================================
// Same file, other path: the bootloader's network flasher (docs/NETBOOT.md).
// The package or raw image is written over UDP after a reboot into the
// bootloader; no ST-Link, no NOR staging, and it works with a dead application.
void SourceSetupPanel::netFlashFirmware()
{
    if (! firmwareFile.existsAsFile())
    {
        setConnState (connState, "Choose a .bin file first");
        return;
    }
    if (netFlash.isRunning())
    {
        setConnState (connState, "Network flash already running");
        return;
    }

    juce::Component::SafePointer<SourceSetupPanel> safe (this);
    Sp3ctraDialog::showConfirm (
        this,
        "Flash via network?",
        "This reboots the device into its bootloader and writes \"" + firmwareFile.getFileName()
            + "\" over Ethernet (no ST-Link).\nKeep it powered until it comes back.",
        "Flash", "Cancel",
        [safe] (bool confirmed)
    {
        auto* s = safe.getComponent();
        if (s == nullptr || ! confirmed)
            return;

        s->uploadProgress = 0.0;
        s->uploadProgressBar.repaint();
        s->setConnState (DC::State::Connecting, "Network flash: entering flash mode ...");

        juce::Component::SafePointer<SourceSetupPanel> safe2 (s);
        s->netFlash.start (s->deviceHostFromApvts(), s->firmwareFile,
            [safe2] (const juce::String& phase, double frac)
            {
                if (auto* p = safe2.getComponent())
                {
                    p->uploadProgress = frac;
                    p->setConnState (DC::State::Connecting, "Network flash: " + phase);
                }
            },
            [safe2] (bool ok, const juce::String& msg)
            {
                if (auto* p = safe2.getComponent())
                {
                    p->uploadProgress = ok ? 1.0 : 0.0;
                    p->setConnState (ok ? DC::State::Connecting : DC::State::Failed, msg);
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
            s->deviceClient.setHost (s->deviceHostFromApvts());   // reset stays on HTTP
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
    // The page rule stops with the content column, like every section rule.
    SetupUI::paintHeader (g, *this, "SP3CTRA -- SETUP", accent,
                          contentRight - Sp3ctraTheme::kHPad);

    // Every section title carries the same hairline rule under it, spanning the
    // content column: the page reads as a stack of aligned groups instead of a
    // continuous run of rows.
    g.setColour (accent.withAlpha (0.22f));
    for (auto* h : { &devicesHeader, &linkHeader, &cisHeader, &imuHeader,
                     &guiHeader, &netHeader, &fwHeader })
    {
        const auto b = h->getBounds();
        g.fillRect (b.getX(), b.getBottom() - 2, contentRight - b.getX(), 1);
    }
}

//==============================================================================
void SourceSetupPanel::resized()
{
    constexpr int rowH   = Sp3ctraTheme::kRowStep;
    constexpr int labelW = Sp3ctraTheme::kLabelW;
    constexpr int ctrlH  = Sp3ctraTheme::kControlH;
    constexpr int gap    = Sp3ctraTheme::kGap;
    constexpr int hpad   = Sp3ctraTheme::kHPad;
    constexpr int vc     = (rowH - ctrlH) / 2;

    // ONE content column for the whole page. Everything — rows, list, status
    // lines, buttons, progress bar — lives between hpad and contentRight, so
    // the page has a single left edge and a single right edge. On a wide zone
    // the column stops at kMaxContentW instead of stretching to the window.
    const int w        = getWidth();
    const int contentW = juce::jmax (240, juce::jmin (Sp3ctraTheme::kMaxContentW, w - 2 * hpad));
    const int ctrlX    = hpad + labelW + gap;      // left edge of the field column
    const int fieldW   = contentW - labelW - gap;  // every control is exactly this wide
    contentRight       = hpad + contentW;

    int y = SetupUI::kHeaderH + Sp3ctraTheme::kSectionGap;

    // label (right-aligned column) + control (field column)
    auto row = [&] (juce::Component& label, juce::Component& ctrl)
    {
        label.setBounds (hpad,  y + vc, labelW, ctrlH);
        ctrl .setBounds (ctrlX, y + vc, fieldW, ctrlH);
        y += rowH;
    };
    auto rowIp = [&] (juce::Component& label, IpBytes& ip)
    {
        label.setBounds (hpad, y + vc, labelW, ctrlH);
        ip.layout (ctrlX, y + vc, fieldW, ctrlH);
        y += rowH;
    };
    // a control with no label of its own — buttons, status lines, progress
    auto rowField = [&] (juce::Component& c)
    {
        c.setBounds (ctrlX, y + vc, fieldW, ctrlH);
        y += rowH;
    };
    // full-width block row (list rows, list status, policy toggle)
    auto rowWide = [&] (juce::Component& c)
    {
        c.setBounds (hpad, y + vc, contentW, ctrlH);
        y += rowH;
    };
    auto section = [&] (juce::Component& header)
    {
        y += 2 * Sp3ctraTheme::kSectionGap;
        header.setBounds (hpad, y, contentW, Sp3ctraTheme::kSectionH);
        y += Sp3ctraTheme::kSectionH + Sp3ctraTheme::kSectionGap;
    };

    // ── DEVICES — the discovered units, one row each ─────────────────────────
    section (devicesHeader);
    {
        constexpr int useW = 84;                       // USE / RELEASE column
        const int textW = contentW - useW - gap;
        int shown = 0;
        for (auto& r : deviceRows)
        {
            if (! r.text.isVisible())                  // unseen unit: no blank row
                continue;
            r.text  .setBounds (hpad, y + vc, textW, ctrlH);
            r.button.setBounds (contentRight - useW, y + vc, useW, ctrlH);
            y += rowH;
            ++shown;
        }
        noDeviceLabel.setVisible (shown == 0);
        if (shown == 0)
            rowWide (noDeviceLabel);
    }
    rowWide (linkStatusLabel);
    rowWide (autoBindToggle);

    // ── LINK (VST side) — how this plug-in reaches the unit ──────────────────
    section (linkHeader);
    rowIp (deviceIpLabel, deviceIp);
    row   (udpPortLabel, udpPortEditor);
    rowIp (udpAddressLabel, udpAddr);
    {
        const int applyW = (fieldW - gap) * 2 / 3;     // pair splits the field
        applyLinkButton.setBounds (ctrlX, y + vc, applyW, ctrlH);
        retryButton    .setBounds (ctrlX + applyW + gap, y + vc, fieldW - applyW - gap, ctrlH);
        y += rowH;
    }
    rowField (connStatusLabel);

    // ── CIS ──────────────────────────────────────────────────────────────────
    section (cisHeader);
    row (dpiLabel, dpiCombo);
    row (ovspLabel, ovspCombo);
    row (lpsLabel, lpsValue);
    row (handLabel, handCombo);
    rowField (calibrateCisButton);

    // ── IMU ──────────────────────────────────────────────────────────────────
    section (imuHeader);
    row (gyroLabel, gyroCombo);
    row (accelLabel, accelCombo);
    rowField (calibrateImuButton);

    // ── GUI ──────────────────────────────────────────────────────────────────
    section (guiHeader);
    row (showImuLabel, showImuCombo);
    row (invertLabel, invertCombo);
    row (screensaverLabel, screensaverEditor);
    row (motionAccLabel, motionAccEditor);
    row (motionGyroLabel, motionGyroEditor);

    // ── NETWORK (device) ─────────────────────────────────────────────────────
    section (netHeader);
    rowIp (ipLabel, netIp);
    rowIp (maskLabel, netMask);
    rowIp (gatewayLabel, netGateway);
    rowIp (destIpLabel, netDestIp);
    row (cisUdpPortLabel, cisUdpPortEditor);
    row (linkPortLabel, linkPortEditor);
    row (streamUnboundLabel, streamUnboundCombo);
    rowField (applyNetworkButton);

    // ── FIRMWARE ─────────────────────────────────────────────────────────────
    section (fwHeader);
    row (fwVersionCaption, fwVersionLabel);
    {
        const int chooseW = (fieldW - gap) / 3;        // button + chosen file name
        chooseFwButton.setBounds (ctrlX, y + vc, chooseW, ctrlH);
        fwFileLabel   .setBounds (ctrlX + chooseW + gap, y + vc, fieldW - chooseW - gap, ctrlH);
        y += rowH;
    }
    rowField (uploadFwButton);
    rowField (netFlashButton);
    rowField (uploadProgressBar);
    rowField (factoryResetButton);
}
