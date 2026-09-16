#include "Sp3ctraNetFlash.h"

#include <cstring>

namespace
{
// netboot_protocol.h, kept byte-identical
constexpr uint32_t kMagic        = 0x31424E53u;  // "SNB1"
constexpr uint32_t kMaxData      = 1024u;
constexpr uint32_t kSector       = 0x20000u;
constexpr uint32_t kWritableFrom = 0x08020000u;
constexpr uint32_t kFlashEnd     = 0x08200000u;
constexpr uint32_t kCm7Addr      = 0x08100000u;
constexpr uint32_t kCm4Addr      = 0x08040000u;
constexpr uint32_t kCm7MaxSize   = 0x80000u;
constexpr uint32_t kCm4MaxSize   = 0x20000u;
constexpr uint8_t  kFlagRelease  = 0x01u;

enum : uint8_t { DISCOVER = 0x01, ERASE = 0x02, WRITE = 0x03, READ = 0x04, CRC = 0x05,
                 LOG = 0x06, JOURNAL = 0x07, BOOT = 0x08, PING = 0x09,
                 INFO = 0x81, ACK = 0x82, DATA = 0x84, CRC_REPLY = 0x85 };

const char* statusText (uint8_t st)
{
    switch (st)
    {
        case 0: return "ok";
        case 1: return "bad argument";
        case 2: return "address out of range";
        case 3: return "flash error";
        case 4: return "busy: another host holds the session";
        case 5: return "verify mismatch";
        case 6: return "target not erased";
        default: return "unknown request";
    }
}

void put32 (juce::MemoryOutputStream& s, uint32_t v) { s.writeInt ((int) v); }
void put16 (juce::MemoryOutputStream& s, uint16_t v) { s.writeShort ((short) v); }
uint32_t get32 (const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
uint16_t get16 (const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }
}

//==============================================================================
Sp3ctraNetFlash::Sp3ctraNetFlash() : juce::Thread ("Sp3ctraNetFlash")
{
    socket.bindToPort (0);
}

Sp3ctraNetFlash::~Sp3ctraNetFlash()
{
    cancel();
}

bool Sp3ctraNetFlash::start (const juce::String& h, const juce::File& img, ProgressFn p, DoneFn d)
{
    if (isThreadRunning())
        return false;
    host = h;
    image = img;
    onProgress = std::move (p);
    onDone = std::move (d);
    startThread();
    return true;
}

void Sp3ctraNetFlash::cancel()
{
    signalThreadShouldExit();
    stopThread (3000);
}

//==============================================================================
void Sp3ctraNetFlash::run()
{
    std::vector<Image> images;
    juce::String err;

    if (! parseImages (image, images, err))
    {
        finish (false, err);
        return;
    }

    report ("entering flash mode", 0.0);
    if (! enterFlashMode (err))
    {
        finish (false, err);
        return;
    }

    const double perImage = 0.85 / (double) images.size();
    double base = 0.05;
    juce::StringArray done;

    for (auto& img : images)
    {
        if (threadShouldExit()) { release(); finish (false, "cancelled"); return; }

        if (! erase (img, err, base, perImage * 0.45)
            || ! write (img, err, base + perImage * 0.45, perImage * 0.5)
            || ! verify (img, err))
        {
            release();
            finish (false, img.name + ": " + err);
            return;
        }
        done.add (img.name + " (" + juce::String ((int) img.data.getSize()) + " bytes)");
        base += perImage;
    }

    report ("rebooting", 0.92);
    boot();

    const bool back = waitForApplication (40000);
    finish (true, "Flashed " + done.joinIntoString (", ")
                  + (back ? " -- application back" : " -- application not seen yet"));
}

//==============================================================================
bool Sp3ctraNetFlash::parseImages (const juce::File& file, std::vector<Image>& out, juce::String& err)
{
    juce::MemoryBlock all;
    if (! file.loadFileAsData (all) || all.getSize() == 0)
    {
        err = "cannot read " + file.getFileName();
        return false;
    }

    const auto* p = static_cast<const uint8_t*> (all.getData());
    const size_t n = all.getSize();

    if (n >= 28 && std::memcmp (p, "BOOT", 4) == 0)
    {
        // OTA package: header 24 B, CM7, CM4, (external), CRC-32 footer over the rest
        const uint32_t cm7 = get32 (p + 4), cm4 = get32 (p + 8), ext = get32 (p + 12);
        const uint64_t body = 24ull + cm7 + cm4 + ext;
        if (body + 4 != n)
        {
            err = "package size does not match its header";
            return false;
        }
        if (crc32 (0, p, (size_t) body) != get32 (p + body))
        {
            err = "package CRC-32 mismatch";
            return false;
        }
        if (cm7 == 0 || cm7 > kCm7MaxSize || cm4 > kCm4MaxSize)
        {
            err = "image size out of range";
            return false;
        }
        out.push_back ({ "CM7", kCm7Addr, juce::MemoryBlock (p + 24, cm7) });
        if (cm4 > 0)
            out.push_back ({ "CM4", kCm4Addr, juce::MemoryBlock (p + 24 + cm7, cm4) });
        return true;
    }

    // Raw image: assumed to be the CM7 application (slot A)
    if (n > kCm7MaxSize)
    {
        err = "raw image larger than the CM7 slot";
        return false;
    }
    out.push_back ({ "CM7", kCm7Addr, all });
    return true;
}

//==============================================================================
bool Sp3ctraNetFlash::enterFlashMode (juce::String& err)
{
    if (bootloaderAnswers (600))
        return true;                                    // already in flash mode

    int status = 0;
    const bool posted = httpPostEmpty ("http://" + host + "/netboot", 3000, status);
    if (posted && status != 202)
    {
        err = "the application refused /netboot (HTTP " + juce::String (status) + "): firmware too old?";
        return false;
    }

    report (posted ? "waiting for the bootloader" : "no application: looking for the bootloader", 0.02);
    if (bootloaderAnswers (posted ? 20000 : 6000))
        return true;

    err = posted ? "the bootloader did not show up within 20 s"
                 : "no application at " + host + " and no bootloader on UDP " + juce::String (kPort)
                   + ". Hold the two outer buttons while powering on.";
    return false;
}

bool Sp3ctraNetFlash::bootloaderAnswers (int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (uint32_t) timeoutMs;
    do
    {
        Reply r;
        if (request (DISCOVER, nullptr, 0, r, 500, 0) && r.type == INFO)
            return true;
        if (threadShouldExit())
            return false;
    } while (juce::Time::getMillisecondCounter() < deadline);
    return false;
}

//==============================================================================
bool Sp3ctraNetFlash::request (uint8_t type, const void* payload, size_t len, Reply& reply,
                               int timeoutMs, int retries, uint8_t flags)
{
    const uint16_t mySeq = ++seq;

    juce::MemoryOutputStream out;
    put32 (out, kMagic);
    out.writeByte ((char) type);
    out.writeByte ((char) flags);
    put16 (out, mySeq);
    if (len > 0)
        out.write (payload, len);

    for (int attempt = 0; attempt <= retries; ++attempt)
    {
        if (threadShouldExit())
            return false;
        socket.write (host, kPort, out.getData(), (int) out.getDataSize());

        const auto deadline = juce::Time::getMillisecondCounter() + (uint32_t) timeoutMs;
        for (;;)
        {
            const int left = (int) (deadline - juce::Time::getMillisecondCounter());
            if (left <= 0 || (int) (juce::Time::getMillisecondCounter() - deadline) >= 0)
                break;
            if (socket.waitUntilReady (true, left) <= 0)
                break;

            uint8_t buf[2048];
            const int n = socket.read (buf, (int) sizeof (buf), false);
            if (n < 8 || get32 (buf) != kMagic || get16 (buf + 6) != mySeq)
                continue;                                // stale or foreign datagram
            reply.type = buf[4];
            reply.body = juce::MemoryBlock (buf + 8, (size_t) (n - 8));
            return true;
        }
    }
    return false;
}

bool Sp3ctraNetFlash::ack (uint8_t type, const void* payload, size_t len, int timeoutMs, int retries, juce::String& err)
{
    Reply r;
    if (! request (type, payload, len, r, timeoutMs, retries))
    {
        err = "no answer from the bootloader";
        return false;
    }
    if (r.type != ACK || r.body.getSize() < 12)
    {
        err = "unexpected reply";
        return false;
    }
    const auto* b = static_cast<const uint8_t*> (r.body.getData());
    if (b[0] != 0)
    {
        err = juce::String (statusText (b[0])) + " (0x" + juce::String::toHexString ((int) get32 (b + 4)) + ")";
        return false;
    }
    return true;
}

bool Sp3ctraNetFlash::erase (const Image& img, juce::String& err, double fracBase, double fracSpan)
{
    const uint32_t first = img.addr & ~(kSector - 1u);
    const uint32_t last  = (img.addr + (uint32_t) img.data.getSize() - 1u) & ~(kSector - 1u);
    const int count = (int) ((last - first) / kSector) + 1;

    if (img.addr < kWritableFrom || img.addr + img.data.getSize() > kFlashEnd)
    {
        err = "image outside the writable range";
        return false;
    }

    int i = 0;
    for (uint32_t a = first; a <= last; a += kSector, ++i)
    {
        report ("erasing " + img.name + " sector " + juce::String (i + 1) + "/" + juce::String (count),
                fracBase + fracSpan * (double) i / (double) count);
        juce::MemoryOutputStream p;
        put32 (p, a);
        put32 (p, kSector);
        if (! ack (ERASE, p.getData(), p.getDataSize(), 8000, 1, err))
            return false;
    }
    return true;
}

bool Sp3ctraNetFlash::write (const Image& img, juce::String& err, double fracBase, double fracSpan)
{
    const auto* src = static_cast<const uint8_t*> (img.data.getData());
    const size_t total = img.data.getSize();
    size_t off = 0;

    while (off < total)
    {
        if (threadShouldExit()) { err = "cancelled"; return false; }

        size_t len = juce::jmin ((size_t) kMaxData, total - off);
        uint8_t chunk[kMaxData];
        std::memset (chunk, 0xFF, sizeof (chunk));
        std::memcpy (chunk, src + off, len);
        const size_t padded = (len + 31u) & ~(size_t) 31u;   // whole 32-byte flash words

        juce::MemoryOutputStream p;
        put32 (p, img.addr + (uint32_t) off);
        put16 (p, (uint16_t) padded);
        put16 (p, 0);
        p.write (chunk, padded);
        if (! ack (WRITE, p.getData(), p.getDataSize(), 1000, 5, err))
            return false;

        off += len;
        report ("writing " + img.name + " " + juce::String ((int) (off * 100 / total)) + " %",
                fracBase + fracSpan * (double) off / (double) total);
    }
    return true;
}

bool Sp3ctraNetFlash::verify (const Image& img, juce::String& err)
{
    // CRC over the padded length, as written
    juce::MemoryBlock padded (img.data);
    const size_t rem = padded.getSize() % 32;
    if (rem != 0)
    {
        const size_t old = padded.getSize();
        padded.setSize (old + (32 - rem), false);
        std::memset (static_cast<uint8_t*> (padded.getData()) + old, 0xFF, 32 - rem);
    }

    juce::MemoryOutputStream p;
    put32 (p, img.addr);
    put32 (p, (uint32_t) padded.getSize());
    Reply r;
    if (! request (CRC, p.getData(), p.getDataSize(), r, 3000, 2) || r.type != CRC_REPLY || r.body.getSize() < 12)
    {
        err = "no CRC reply";
        return false;
    }
    const uint32_t device = get32 (static_cast<const uint8_t*> (r.body.getData()) + 8);
    const uint32_t local  = crc32 (0, padded.getData(), padded.getSize());
    if (device != local)
    {
        err = "CRC mismatch after write (device 0x" + juce::String::toHexString ((int) device)
              + ", file 0x" + juce::String::toHexString ((int) local) + ")";
        return false;
    }
    return true;
}

void Sp3ctraNetFlash::boot()
{
    const uint8_t mode[4] = { 0, 0, 0, 0 };
    juce::String ignored;
    ack (BOOT, mode, sizeof (mode), 1000, 0, ignored);
}

void Sp3ctraNetFlash::release()
{
    Reply r;
    request (PING, nullptr, 0, r, 300, 0, kFlagRelease);
}

bool Sp3ctraNetFlash::waitForApplication (int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + (uint32_t) timeoutMs;
    juce::Thread::sleep (2500);
    while (juce::Time::getMillisecondCounter() < deadline && ! threadShouldExit())
    {
        if (httpGetOk ("http://" + host + "/getFirmwareVersion", 1500))
            return true;
        juce::Thread::sleep (700);
    }
    return false;
}

//==============================================================================
void Sp3ctraNetFlash::report (const juce::String& phase, double fraction)
{
    if (onProgress)
        juce::MessageManager::callAsync ([cb = onProgress, phase, fraction]() { cb (phase, fraction); });
}

void Sp3ctraNetFlash::finish (bool ok, const juce::String& message)
{
    if (onDone)
        juce::MessageManager::callAsync ([cb = onDone, ok, message]() { cb (ok, message); });
}

uint32_t Sp3ctraNetFlash::crc32 (uint32_t crc, const void* data, size_t len)
{
    // zlib polynomial, nibble table -- same values as zlib.crc32 / ota_crc32
    static const uint32_t table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu };
    const auto* p = static_cast<const uint8_t*> (data);
    crc = ~crc;
    while (len-- > 0)
    {
        crc ^= *p++;
        crc = (crc >> 4) ^ table[crc & 0x0Fu];
        crc = (crc >> 4) ^ table[crc & 0x0Fu];
    }
    return ~crc;
}

bool Sp3ctraNetFlash::httpPostEmpty (const juce::String& url, int timeoutMs, int& statusOut)
{
    juce::URL u = juce::URL (url).withPOSTData (juce::String());
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                    .withConnectionTimeoutMs (timeoutMs)
                    .withStatusCode (&statusOut)
                    .withHttpRequestCmd ("POST");
    std::unique_ptr<juce::InputStream> in (u.createInputStream (opts));
    if (in == nullptr)
        return false;
    in->readEntireStreamAsString();
    return true;
}

bool Sp3ctraNetFlash::httpGetOk (const juce::String& url, int timeoutMs)
{
    int status = 0;
    auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs (timeoutMs)
                    .withStatusCode (&status);
    std::unique_ptr<juce::InputStream> in (juce::URL (url).createInputStream (opts));
    if (in == nullptr)
        return false;
    in->readEntireStreamAsString();
    return status == 200;
}
