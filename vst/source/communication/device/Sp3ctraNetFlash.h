#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <vector>

/**
 * Sp3ctra Net Boot client: flashes the CIS through the bootloader's network
 * flasher (UDP 55152), the ST-Link replacement. Wire contract:
 * Sp3ctra_CIS_Firmware/Common/Inc/netboot_protocol.h, workflow in
 * Sp3ctra_CIS_Firmware/docs/NETBOOT.md.
 *
 * Accepts an OTA package (cis_package_x.y.z.bin: "BOOT" header, CM7 then CM4
 * image, CRC-32 footer) or a raw CM7 image. Sequence: POST /netboot to the
 * application (skipped when the bootloader already answers), wait for the
 * bootloader, erase sector by sector, write 1 KB chunks (stop-and-wait with
 * retransmission), verify by a CRC read back in place, BOOT, then wait for the
 * application's HTTP server. Runs on its own thread; both callbacks are
 * delivered on the message thread.
 */
class Sp3ctraNetFlash : private juce::Thread
{
public:
    using ProgressFn = std::function<void (const juce::String& phase, double fraction)>;
    using DoneFn     = std::function<void (bool ok, const juce::String& message)>;

    Sp3ctraNetFlash();
    ~Sp3ctraNetFlash() override;

    /** host = device IP address. Returns false when a flash is already running. */
    bool start (const juce::String& host, const juce::File& image, ProgressFn onProgress, DoneFn onDone);
    void cancel();
    bool isRunning() const { return isThreadRunning(); }

    static constexpr int kPort = 55152;

private:
    struct Image
    {
        juce::String name;
        uint32_t addr = 0;
        juce::MemoryBlock data;
    };

    struct Reply
    {
        uint8_t type = 0;
        juce::MemoryBlock body;
    };

    void run() override;

    bool parseImages (const juce::File& file, std::vector<Image>& out, juce::String& err);
    bool enterFlashMode (juce::String& err);
    bool bootloaderAnswers (int timeoutMs);
    bool request (uint8_t type, const void* payload, size_t len, Reply& reply, int timeoutMs, int retries, uint8_t flags = 0);
    bool ack (uint8_t type, const void* payload, size_t len, int timeoutMs, int retries, juce::String& err);
    bool erase (const Image& img, juce::String& err, double fracBase, double fracSpan);
    bool write (const Image& img, juce::String& err, double fracBase, double fracSpan);
    bool verify (const Image& img, juce::String& err);
    void boot();
    void release();
    bool waitForApplication (int timeoutMs);

    void report (const juce::String& phase, double fraction);
    void finish (bool ok, const juce::String& message);

    static uint32_t crc32 (uint32_t crc, const void* data, size_t len);
    static bool httpPostEmpty (const juce::String& url, int timeoutMs, int& statusOut);
    static bool httpGetOk (const juce::String& url, int timeoutMs);

    juce::String host;
    juce::File image;
    ProgressFn onProgress;
    DoneFn onDone;
    juce::DatagramSocket socket { false };
    uint16_t seq = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sp3ctraNetFlash)
};
