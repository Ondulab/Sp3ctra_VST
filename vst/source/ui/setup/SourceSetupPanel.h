/**
 * @file SourceSetupPanel.h
 * @brief SETUP face of the SP3CTRA (SOURCE CIS) block — zone 3, M5.
 *
 * Two layers, top to bottom:
 *
 *  1. VST ↔ device LINK (persisted in APVTS — the few transport params the VST
 *     needs to even reach the device, independent of any HTTP session):
 *        Device IP (HTTP host) · UDP listen port · UDP listen/multicast address
 *     Edited here, applied as a batch (UDP socket restarts once).
 *
 *  2. DEVICE configuration (live, loaded over HTTP from the device's embedded
 *     web server — the same API that backs config.html). The DEVICE is the
 *     source of truth; these values are NOT saved in the DAW state. They are
 *     loaded when the panel becomes visible and POSTed back on change. The HTTP
 *     "connection" is dropped when the panel hides (HTTP is connectionless —
 *     nothing to keep alive). A Retry button re-runs the load.
 *
 * The one value that lives in BOTH worlds is the CIS DPI: it is a device
 * setting AND drives the VST's UDP packet parsing (1728 vs 3456 px/line). It is
 * therefore reconciled into APVTS (PARAM_SENSOR_DPI) on every load/change so the
 * parser always matches the live stream.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../../PluginProcessor.h"
#include "../../communication/device/Sp3ctraDeviceClient.h"
#include "../../communication/link/Sp3ctraLink.h"

class SourceSetupPanel : public juce::Component,
                         private juce::Timer,
                         private juce::ChangeListener
{
public:
    SourceSetupPanel(Sp3ctraAudioProcessor& processor, juce::Colour accentColour);
    ~SourceSetupPanel() override;

    /** Natural content height — hosted in a scrolling viewport (zone-3). */
    static constexpr int kPreferredH = 1300;

    void paint(juce::Graphics&) override;
    void resized() override;
    void visibilityChanged() override;   // load on show / cancel on hide

private:
    //==========================================================================
    /** A dotted "a.b.c.d" group of 4 byte editors. */
    struct IpBytes
    {
        juce::TextEditor box[4];
        juce::Label      dot[3];
        void init (juce::Component& parent);
        void layout (int x, int y, int w, int h);
        void set (const juce::String& dotted, juce::NotificationType n = juce::dontSendNotification);
        void set (int b0, int b1, int b2, int b3);
        juce::String get() const;             // "a.b.c.d"
        void onAnyChange (std::function<void()> cb);
    };

    void initLabel  (juce::Label& l, const juce::String& text,
                     juce::Justification j = juce::Justification::centredRight);
    void initSection(juce::Label& l, const juce::String& text);
    void initEditor (juce::TextEditor& e, int maxLen, const juce::String& allowed);
    void initCombo  (juce::ComboBox& c, const juce::StringArray& items);

    // ── LINK (Sp3ctra Link + APVTS transport params) ─────────────────────────
    void applyLink();                  // write APVTS, restart UDP, re-point client
    juce::String deviceHostFromApvts() const;
    Sp3ctraLink* link() const;         // process-wide control channel (may be null)
    void refreshLink();                // device rows + status line from the link
    void useDevice (const juce::String& uid);
    void releaseDevice();
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    // ── DEVICE (HTTP) ─────────────────────────────────────────────────────────
    void reload();                     // GET everything from the device
    void onLoaded (Sp3ctraDeviceClient::State, Sp3ctraDeviceClient::DeviceConfig);
    void populate (const Sp3ctraDeviceClient::DeviceConfig&);
    void setConnState (Sp3ctraDeviceClient::State, const juce::String& detail = {});
    void setDeviceControlsEnabled (bool);

    void postDpi();                    // also reconciles APVTS + handles reboot
    void postNetwork();
    void chooseFirmware();
    void uploadFirmware();
    void confirmFactoryReset();
    void reconcileDpiToApvts (int dpi);

    Sp3ctraAudioProcessor& audioProcessor;
    juce::AudioProcessorValueTreeState& apvts;
    juce::Colour accent;

    Sp3ctraDeviceClient deviceClient;
    Sp3ctraDeviceClient::State connState = Sp3ctraDeviceClient::State::Idle;
    bool applyingRemote = false;       // guard: suppress POST while populating
    bool loading = false;              // guard: dedupe overlapping load bursts

    // ── LINK controls ─────────────────────────────────────────────────────────
    static constexpr int kMaxDeviceRows = 4;
    struct DeviceRow
    {
        juce::Label      text;
        juce::TextButton button;   // USE / RELEASE
        juce::String     uid;
    };
    juce::Label    devicesLabel;
    DeviceRow      deviceRows[kMaxDeviceRows];
    juce::Label    linkStatusLabel;
    juce::ToggleButton autoBindToggle;
    juce::Label    deviceIpLabel;   IpBytes deviceIp;      // HELLO unicast target + HTTP host
    juce::Label    udpPortLabel;    juce::TextEditor udpPortEditor;
    juce::Label    udpAddressLabel; IpBytes udpAddr;       // multicast group (blank/unicast = direct)
    juce::TextButton applyLinkButton;
    juce::Label    connStatusLabel; juce::TextButton retryButton;
    uint32_t       lastLinkGen = 0;
    bool           linkWasBound = false;

    // ── CIS ────────────────────────────────────────────────────────────────────
    juce::Label cisHeader;
    juce::Label dpiLabel;     juce::ComboBox dpiCombo;
    juce::Label ovspLabel;    juce::ComboBox ovspCombo;
    juce::Label lpsLabel;     juce::Label    lpsValue;
    juce::Label handLabel;    juce::ComboBox handCombo;
    juce::TextButton calibrateCisButton;

    // ── IMU ────────────────────────────────────────────────────────────────────
    juce::Label imuHeader;
    juce::Label gyroLabel;    juce::ComboBox gyroCombo;
    juce::Label accelLabel;   juce::ComboBox accelCombo;
    juce::TextButton calibrateImuButton;

    // ── GUI & screensaver ───────────────────────────────────────────────────────
    juce::Label guiHeader;
    juce::Label showImuLabel;     juce::ComboBox showImuCombo;
    juce::Label invertLabel;      juce::ComboBox invertCombo;
    juce::Label screensaverLabel; juce::TextEditor screensaverEditor;
    juce::Label motionAccLabel;   juce::TextEditor motionAccEditor;
    juce::Label motionGyroLabel;  juce::TextEditor motionGyroEditor;


    // ── Device network configuration ─────────────────────────────────────────────
    juce::Label netHeader;
    juce::Label ipLabel;        IpBytes netIp;
    juce::Label maskLabel;      IpBytes netMask;
    juce::Label gatewayLabel;   IpBytes netGateway;
    juce::Label destIpLabel;    IpBytes netDestIp;
    juce::Label cisUdpPortLabel;  juce::TextEditor cisUdpPortEditor;   // stream port while unbound
    juce::Label linkPortLabel;    juce::TextEditor linkPortEditor;     // SLP control port
    juce::Label streamUnboundLabel; juce::ComboBox streamUnboundCombo; // stream without a host session
    juce::TextButton applyNetworkButton;

    // ── Firmware ─────────────────────────────────────────────────────────────────
    juce::Label fwHeader;
    juce::Label fwVersionLabel;
    juce::TextButton chooseFwButton;
    juce::Label      fwFileLabel;
    juce::TextButton uploadFwButton;
    double           uploadProgress = 0.0;
    juce::ProgressBar uploadProgressBar { uploadProgress };
    juce::TextButton factoryResetButton;
    juce::File       firmwareFile;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourceSetupPanel)
};
