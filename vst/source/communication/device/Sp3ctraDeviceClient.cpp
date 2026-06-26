#include "Sp3ctraDeviceClient.h"
#include <juce_events/juce_events.h>   // MessageManager::callAsync

//==============================================================================
namespace
{
    // Endpoint timeouts (ms). The device is on a local link so replies are fast;
    // a short timeout keeps the UI snappy when it is unreachable.
    constexpr int kGetTimeoutMs  = 1500;
    constexpr int kPostTimeoutMs = 3000;

    // The device's embedded TCP stack (lwIP) has only a handful of PCBs AND must
    // keep streaming CIS over UDP at the same time. Each createInputStream opens
    // a fresh TCP connection (JUCE does not keep-alive), so firing the ~16 load
    // GETs back-to-back exhausts its sockets and starves the UDP task — the image
    // stutters and the device stops responding. Space the requests out so it can
    // recycle each connection and service UDP between calls.
    constexpr int kRequestSpacingMs = 150;

    bool httpStatusOk (int status) { return status == 0 || (status >= 200 && status < 300); }
}

//==============================================================================
Sp3ctraDeviceClient::Sp3ctraDeviceClient() = default;

Sp3ctraDeviceClient::~Sp3ctraDeviceClient()
{
    // Supersede any queued callback, then let `pool` (destroyed first) join.
    bumpGeneration();
}

//==============================================================================
void Sp3ctraDeviceClient::setHost (juce::String ipAddress, int httpPort)
{
    {
        const juce::ScopedLock sl (hostLock);
        host = ipAddress.trim();
        port = httpPort;
    }
    bumpGeneration();   // any in-flight reply now targets a stale host
}

juce::String Sp3ctraDeviceClient::getHost() const
{
    const juce::ScopedLock sl (hostLock);
    return host;
}

juce::String Sp3ctraDeviceClient::hostUrl() const
{
    const juce::ScopedLock sl (hostLock);
    juce::String h = host.isNotEmpty() ? host : juce::String ("192.168.100.1");
    return "http://" + h + ":" + juce::String (port);
}

int Sp3ctraDeviceClient::bumpGeneration()
{
    return generation->fetch_add (1) + 1;
}

void Sp3ctraDeviceClient::cancel()
{
    bumpGeneration();
}

//==============================================================================
bool Sp3ctraDeviceClient::httpGet (const juce::String& url, juce::String& bodyOut)
{
    int status = 0;
    juce::URL u (url);
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs (kGetTimeoutMs)
                    .withNumRedirectsToFollow (0)
                    // Force the device to free the TCP PCB right after the reply
                    // instead of holding a keep-alive connection we never reuse.
                    .withExtraHeaders ("Connection: close")
                    .withStatusCode (&status);

    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr)
        return false;

    bodyOut = in->readEntireStreamAsString();
    return httpStatusOk (status);
}

bool Sp3ctraDeviceClient::httpPost (const juce::String& url, const juce::String& body)
{
    int status = 0;
    juce::URL u = juce::URL (url).withPOSTData (body);
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                    .withConnectionTimeoutMs (kPostTimeoutMs)
                    .withNumRedirectsToFollow (0)
                    .withExtraHeaders ("Content-Type: application/x-www-form-urlencoded\r\nConnection: close")
                    .withStatusCode (&status);

    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr)
        return false;

    in->readEntireStreamAsString();   // drain the response
    return httpStatusOk (status);
}

//==============================================================================
void Sp3ctraDeviceClient::loadAll (std::function<void (State, DeviceConfig)> onComplete)
{
    const int myGen        = bumpGeneration();
    auto      gen          = generation;          // shared — outlives `this`
    const juce::String base = hostUrl();

    pool.addJob ([base, myGen, gen, onComplete]()
    {
        // Gentle, abortable GET: bail the instant this load is superseded
        // (panel closed / host changed / Retry) so we stop touching the device,
        // and pace requests so the embedded stack keeps up with UDP streaming.
        auto get = [&base, &gen, myGen] (const char* ep, juce::String& out) -> bool
        {
            if (gen->load() != myGen)
                return false;
            juce::Thread::sleep (kRequestSpacingMs);
            return httpGet (base + "/" + ep, out);
        };

        DeviceConfig cfg;
        juce::String t;

        // Connectivity probe — if the device does not answer DPI, treat the
        // whole load as failed (no point hammering the remaining endpoints).
        if (! get ("getDPI", t))
        {
            juce::MessageManager::callAsync ([onComplete, myGen, gen]()
            {
                if (gen->load() == myGen)
                    onComplete (State::Failed, DeviceConfig{});
            });
            return;
        }
        cfg.dpi = t.getIntValue();

        // Remaining endpoints — individual failures keep the field default.
        if (get ("getOversampling",      t)) cfg.oversampling      = t.getIntValue();
        if (get ("getFreq",              t)) cfg.freqLps           = t.getIntValue();
        if (get ("getHand",              t)) cfg.handedness        = t.getIntValue();
        if (get ("getGyroSensitivity",   t)) cfg.gyroSensitivity   = t.getIntValue();
        if (get ("getAccelSensitivity",  t)) cfg.accelSensitivity  = t.getIntValue();
        if (get ("getGuiShowImu",        t)) cfg.guiShowImu        = (t.getIntValue() != 0);
        if (get ("getGuiInvertCisImage", t)) cfg.guiInvertCis      = (t.getIntValue() != 0);
        if (get ("getScreensaverTimeout",t)) cfg.screensaverTimeout = t.getIntValue();
        if (get ("getMotionThresholdAcc",t)) cfg.motionThresholdAcc = t.getFloatValue();
        if (get ("getMotionThresholdGyro",t)) cfg.motionThresholdGyro = t.getFloatValue();
        if (get ("getMdnsEnabled",       t)) cfg.mdnsEnabled       = (t.getIntValue() != 0);
        if (get ("getRtpMidiMode",       t)) cfg.rtpMidiMode       = t.getIntValue();
        if (get ("getFirmwareVersion",   t)) cfg.firmwareVersion   = t.trim();

        // MIDI button mapping — JSON {"buttons":[{ch,cmd,param},...]}
        if (get ("getMidiButtonConfig", t))
        {
            auto json = juce::JSON::parse (t);
            if (auto* buttons = json.getProperty ("buttons", juce::var()).getArray())
            {
                for (int i = 0; i < juce::jmin (3, buttons->size()); ++i)
                {
                    const auto& b = buttons->getReference (i);
                    cfg.midi[i].channel = (int) b.getProperty ("ch",    0);
                    cfg.midi[i].command = (int) b.getProperty ("cmd",   0);
                    cfg.midi[i].param   = (int) b.getProperty ("param", 0);
                }
            }
        }

        // Network configuration — JSON {ip,mask,gw,dest_ip,udp_port,rtpmidi_control_port}
        if (get ("getNetworkConfig", t))
        {
            auto json = juce::JSON::parse (t);
            if (json.isObject())
            {
                cfg.network.ip       = json.getProperty ("ip",      cfg.network.ip).toString();
                cfg.network.mask     = json.getProperty ("mask",    cfg.network.mask).toString();
                cfg.network.gateway  = json.getProperty ("gw",      cfg.network.gateway).toString();
                cfg.network.destIp   = json.getProperty ("dest_ip", cfg.network.destIp).toString();
                cfg.network.udpPort  = (int) json.getProperty ("udp_port", 0);
                cfg.network.rtpMidiControlPort =
                    (int) json.getProperty ("rtpmidi_control_port", 0);
            }
        }

        cfg.valid = true;
        juce::MessageManager::callAsync ([onComplete, myGen, gen, cfg]()
        {
            if (gen->load() == myGen)
                onComplete (State::Connected, cfg);
        });
    });
}

//==============================================================================
void Sp3ctraDeviceClient::postForm (const juce::String& endpoint,
                                    const juce::String& body,
                                    std::function<void (bool)> onComplete)
{
    auto      gen     = generation;
    const int myGen   = gen->load();
    juce::String url  = hostUrl() + "/" + endpoint.trimCharactersAtStart ("/");
    juce::String data = body;

    pool.addJob ([url, data, myGen, gen, onComplete]()
    {
        const bool ok = httpPost (url, data);
        if (onComplete)
            juce::MessageManager::callAsync ([onComplete, ok, myGen, gen]()
            {
                if (gen->load() == myGen)
                    onComplete (ok);
            });
    });
}

//==============================================================================
void Sp3ctraDeviceClient::uploadFirmware (const juce::File& binFile,
                                          std::function<void (double)> onProgress,
                                          std::function<void (bool)> onComplete)
{
    auto      gen   = generation;
    const int myGen = gen->load();
    juce::String url = hostUrl() + "/upload";

    pool.addJob ([url, binFile, onProgress, onComplete, myGen, gen]()
    {
        int status = 0;
        juce::URL u = juce::URL (url).withFileToUpload ("firmware", binFile,
                                                        "application/octet-stream");

        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                        .withNumRedirectsToFollow (0)
                        .withStatusCode (&status)
                        .withProgressCallback ([onProgress, myGen, gen] (int sent, int total) -> bool
                        {
                            if (gen->load() != myGen)
                                return false;   // superseded → abort upload
                            const double frac = total > 0 ? (double) sent / (double) total : 0.0;
                            if (onProgress)
                                juce::MessageManager::callAsync ([onProgress, frac]() { onProgress (frac); });
                            return true;
                        });

        std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
        const bool ok = (in != nullptr) && httpStatusOk (status);
        if (in != nullptr)
            in->readEntireStreamAsString();

        if (onComplete)
            juce::MessageManager::callAsync ([onComplete, ok, myGen, gen]()
            {
                if (gen->load() == myGen)
                    onComplete (ok);
            });
    });
}
