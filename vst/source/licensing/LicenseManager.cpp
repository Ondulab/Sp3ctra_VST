#include "LicenseManager.h"
#include "../utils/logger.h"

JUCE_IMPLEMENT_SINGLETON (LicenseManager)

namespace
{
// Ondulab license server (Ondulab_Website/api/license/, SQLite + webhook
// Stripe) — same JSON contract as the Lemon Squeezy License API it replaces.
// Only Sp3ctra keys exist there, so no product check is needed.
constexpr const char* kActivateUrl   = "https://www.ondulab.com/api/license/activate.php";
constexpr const char* kValidateUrl   = "https://www.ondulab.com/api/license/validate.php";
constexpr const char* kDeactivateUrl = "https://www.ondulab.com/api/license/deactivate.php";
constexpr const char* kHttpHeaders   = "Accept: application/json\r\nUser-Agent: Sp3ctra";

// Silent revalidation at most once a week (see grace policy in the header).
constexpr juce::int64 kRevalidateMs = 7ll * 24 * 3600 * 1000;

juce::String errorFrom(const juce::var& r)
{
    const auto e = r.getProperty("error", {}).toString();
    return (e.isNotEmpty() && e != "null")
               ? e : juce::String("The license server refused the request.");
}
} // namespace

//== Lifetime =================================================================
LicenseManager::LicenseManager() : juce::Thread("Sp3ctra License")
{
    loadFromDisk();
    if (! licensed_.load())
        log_info("LIC", "No active license — demo mode (saving/exports disabled)");
}

LicenseManager::~LicenseManager()
{
    signalThreadShouldExit();
    notify();
    stopThread(12000);   // network streams bound by their 10 s timeout
    clearSingletonInstance();
}

bool LicenseManager::isLicensed()
{
    return getInstance()->licensed();
}

//== Disk =====================================================================
juce::File LicenseManager::licenseFile()
{
    // Same root as SessionManager::appSupportRoot() — shared by Standalone,
    // VST3 and AU so one activation unlocks all three formats.
#if JUCE_WINDOWS
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("Sp3ctra").getChildFile("license.json");
#else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("Application Support/Sp3ctra")
               .getChildFile("license.json");
#endif
}

void LicenseManager::loadFromDisk()
{
    const auto f = licenseFile();
    if (! f.existsAsFile())
        return;

    const auto v = juce::JSON::parse(f.loadFileAsString());
    if (! v.isObject())
        return;

    const juce::ScopedLock sl(lock_);
    key_             = v.getProperty("key",           {}).toString();
    instanceId_      = v.getProperty("instanceId",    {}).toString();
    status_          = v.getProperty("status",        {}).toString();
    customerName_    = v.getProperty("customerName",  {}).toString();
    lastValidatedMs_ = (juce::int64) v.getProperty("lastValidatedMs", 0);

    licensed_.store(key_.isNotEmpty() && instanceId_.isNotEmpty()
                    && status_ == "active");
    if (licensed_.load())
        log_info("LIC", "License loaded (%s)", maskedKey().toRawUTF8());
}

void LicenseManager::saveToDisk() const
{
    auto* obj = new juce::DynamicObject();
    {
        const juce::ScopedLock sl(lock_);
        obj->setProperty("key",             key_);
        obj->setProperty("instanceId",      instanceId_);
        obj->setProperty("status",          status_);
        obj->setProperty("customerName",    customerName_);
        obj->setProperty("lastValidatedMs", lastValidatedMs_);
    }
    const auto f = licenseFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText(juce::JSON::toString(juce::var(obj)));
}

//== Public ops ===============================================================
void LicenseManager::activate(const juce::String& key)
{
    {
        const juce::ScopedLock sl(lock_);
        pendingKey_ = key.trim();
    }
    startOp(Op::activate);
}

void LicenseManager::deactivate()
{
    startOp(Op::deactivate);
}

void LicenseManager::startupValidate()
{
    if (startupChecked_.exchange(true) || ! licensed_.load())
        return;
    juce::int64 last;
    {
        const juce::ScopedLock sl(lock_);
        last = lastValidatedMs_;
    }
    if (juce::Time::currentTimeMillis() - last < kRevalidateMs)
        return;
    startOp(Op::validate);
}

juce::String LicenseManager::lastError() const
{
    const juce::ScopedLock sl(lock_);
    return error_;
}

juce::String LicenseManager::maskedKey() const
{
    const juce::ScopedLock sl(lock_);
    if (key_.length() <= 8)
        return key_.isEmpty() ? juce::String() : juce::String::fromUTF8("····");
    return key_.substring(0, 4) + juce::String::fromUTF8("…")
         + key_.substring(key_.length() - 4);
}

juce::String LicenseManager::licensedTo() const
{
    const juce::ScopedLock sl(lock_);
    return customerName_;
}

//== Thread ===================================================================
void LicenseManager::startOp(Op op)
{
    if (opState_.load() == OpState::busy)
        return;   // one operation at a time — the dialog shows the busy state
    pendingOp_.store(op);
    opState_.store(OpState::busy);
    sendChangeMessage();
    if (! isThreadRunning())
        startThread();
    notify();
}

void LicenseManager::run()
{
    while (! threadShouldExit())
    {
        const Op op = pendingOp_.exchange(Op::none);
        switch (op)
        {
            case Op::none:       wait(-1); break;
            case Op::activate:   runActivate();   break;
            case Op::deactivate: runDeactivate(); break;
            case Op::validate:
                // Startup path: give audio/UI bring-up a head start.
                wait(4000);
                if (! threadShouldExit())
                    runValidate();
                break;
        }
    }
}

void LicenseManager::finishOp(OpState s, const juce::String& error)
{
    {
        const juce::ScopedLock sl(lock_);
        error_ = error;
    }
    opState_.store(s);
    sendChangeMessage();
}

juce::var LicenseManager::postForm(const char* endpoint,
                                   const juce::StringPairArray& params,
                                   bool& networkOk)
{
    juce::URL url(endpoint);
    for (const auto& k : params.getAllKeys())
        url = url.withParameter(k, params[k]);

    int status = 0;
    auto stream = url.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
            .withConnectionTimeoutMs(10000)
            .withNumRedirectsToFollow(5)
            .withExtraHeaders(kHttpHeaders)
            .withStatusCode(&status));

    networkOk = stream != nullptr;
    if (stream == nullptr)
        return {};
    // 4xx answers still carry a JSON body ("error": …) — parse regardless.
    return juce::JSON::parse(stream->readEntireStreamAsString());
}

void LicenseManager::runActivate()
{
    juce::String key;
    {
        const juce::ScopedLock sl(lock_);
        key = pendingKey_;
    }
    if (key.isEmpty())
    {
        finishOp(OpState::failed, "Enter the license key from your purchase email.");
        return;
    }

    juce::StringPairArray p;
    p.set("license_key", key);
    // Display name in the customer portal — helps the user tell seats apart.
    p.set("instance_name", juce::SystemStats::getComputerName()
                         + " (" + juce::SystemStats::getOperatingSystemName() + ")");

    bool net = false;
    const auto r = postForm(kActivateUrl, p, net);

    if (! net)
    {
        finishOp(OpState::failed, "Could not reach the license server — "
                                  "check your internet connection and retry.");
        return;
    }
    if (! r.isObject())
    {
        finishOp(OpState::failed, "Unexpected answer from the license server.");
        return;
    }
    if (! (bool) r.getProperty("activated", false))
    {
        finishOp(OpState::failed, errorFrom(r));
        return;
    }

    {
        const juce::ScopedLock sl(lock_);
        key_             = key;
        instanceId_      = r.getProperty("instance", {}).getProperty("id", {}).toString();
        status_          = "active";
        customerName_    = r.getProperty("meta", {}).getProperty("customer_name", {}).toString();
        lastValidatedMs_ = juce::Time::currentTimeMillis();
        pendingKey_.clear();
    }
    saveToDisk();
    licensed_.store(true);
    log_info("LIC", "License activated (%s)", maskedKey().toRawUTF8());
    finishOp(OpState::succeeded);
}

void LicenseManager::runValidate()
{
    juce::StringPairArray p;
    {
        const juce::ScopedLock sl(lock_);
        if (key_.isEmpty() || instanceId_.isEmpty())
            return;
        p.set("license_key", key_);
        p.set("instance_id", instanceId_);
    }

    bool net = false;
    const auto r = postForm(kValidateUrl, p, net);

    // Grace policy: silence on anything but an explicit refusal — a musician
    // on stage without internet must never lose the full version.
    if (! net || ! r.isObject() || ! r.hasProperty("valid"))
    {
        log_info("LIC", "License revalidation skipped (server unreachable)");
        opState_.store(OpState::idle);
        return;
    }

    if ((bool) r["valid"])
    {
        {
            const juce::ScopedLock sl(lock_);
            lastValidatedMs_ = juce::Time::currentTimeMillis();
        }
        saveToDisk();
        log_info("LIC", "License revalidated");
        opState_.store(OpState::idle);
        return;
    }

    // Explicit "valid": false — key refunded/disabled or seat deactivated
    // from the customer portal. Demote to demo, keep the key on disk so the
    // dialog can show what happened.
    {
        const juce::ScopedLock sl(lock_);
        status_ = r.getProperty("license_key", {})
                   .getProperty("status", {}).toString();
        if (status_ == "active" || status_.isEmpty())
            status_ = "deactivated";
    }
    saveToDisk();
    licensed_.store(false);
    log_warning("LIC", "License no longer valid (%s) — demo mode",
                status_.toRawUTF8());
    finishOp(OpState::idle);
}

void LicenseManager::runDeactivate()
{
    juce::StringPairArray p;
    {
        const juce::ScopedLock sl(lock_);
        p.set("license_key", key_);
        p.set("instance_id", instanceId_);
    }

    bool net = false;
    const auto r = postForm(kDeactivateUrl, p, net);

    if (! net)
    {
        finishOp(OpState::failed, "Could not reach the license server — "
                                  "this machine's seat was NOT freed. "
                                  "Check your connection and retry.");
        return;
    }

    // Server answered: whether it confirmed ("deactivated": true) or the seat
    // was already gone, the local license is dropped either way.
    juce::ignoreUnused(r);
    {
        const juce::ScopedLock sl(lock_);
        key_.clear();
        instanceId_.clear();
        status_.clear();
        customerName_.clear();
        lastValidatedMs_ = 0;
    }
    licenseFile().deleteFile();
    licensed_.store(false);
    log_info("LIC", "License deactivated — demo mode");
    finishOp(OpState::succeeded);
}
