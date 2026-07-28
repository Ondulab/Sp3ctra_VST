/**
 * @file LicenseManager.h
 * @brief Machine-level license state + activation client for the Ondulab
 *        license server (ondulab.com/api/license — Stripe checkout upstream).
 *
 * Sp3ctra stays GPL: this is a commercial courtesy gate, not DRM — anyone can
 * rebuild without it. The demo build is fully playable; only the "keep what
 * you made" flows are disabled (session saves, exports, plugin state in a DAW
 * project — see LicenseGate::blockIfDemo call sites).
 *
 * The license lives per MACHINE (Application Support/Sp3ctra/license.json) so
 * one activation unlocks Standalone + VST3 + AU at once, and survives
 * reinstalls without burning an activation seat.
 *
 * Network ops (activate / validate / deactivate) run on this object's own
 * thread against the Ondulab license server — the key itself is the only
 * credential, no account involved. UI observes via ChangeBroadcaster
 * (thread-safe), same idiom as AppUpdater.
 *
 * Grace policy: a licensed install NEVER locks out on network failure (stage
 * use!). Silent revalidation runs at most once a week, and only an explicit
 * "valid: false" answer from the API demotes to demo.
 */
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class LicenseManager : public juce::Thread,
                       public juce::ChangeBroadcaster,
                       private juce::DeletedAtShutdown
{
public:
    /** The one question the rest of the app asks. Message thread only (the
     *  first call loads license.json). */
    static bool isLicensed();

    bool licensed() const noexcept { return licensed_.load(); }

    //== Async operations (call from the message thread) ======================
    enum class OpState { idle, busy, succeeded, failed };

    /** Activate @p key for this machine (activation dialog). Result arrives
     *  via ChangeBroadcaster: opState() + lastError(). */
    void activate(const juce::String& key);

    /** Free this machine's activation seat and drop the local license. */
    void deactivate();

    /** Once per process: silent weekly revalidation, delayed a few seconds so
     *  it never competes with audio/UI bring-up. No-op while unlicensed. */
    void startupValidate();

    OpState      opState() const noexcept { return opState_.load(); }
    juce::String lastError() const;

    //== Display (activation dialog) ==========================================
    juce::String maskedKey()  const;   ///< "38b1…4d51" — never the full key
    juce::String licensedTo() const;   ///< customer name (may be empty)

    /** True exactly once per process — the launch demo reminder uses it. */
    bool shouldShowStartupNag() { return ! nagShown_.exchange(true); }

    JUCE_DECLARE_SINGLETON (LicenseManager, false)

private:
    LicenseManager();
    ~LicenseManager() override;

    enum class Op { none, activate, validate, deactivate };

    static juce::File licenseFile();
    void loadFromDisk();               // ctor only
    void saveToDisk() const;           // any thread (fields under lock_)
    void startOp(Op op);
    void run() override;

    void runActivate();
    void runValidate();
    void runDeactivate();
    void finishOp(OpState s, const juce::String& error = {});

    /** POST form-encoded @p params, parse the JSON answer. @p networkOk turns
     *  false when the server was unreachable (≠ an API refusal). */
    static juce::var postForm(const char* endpoint,
                              const juce::StringPairArray& params,
                              bool& networkOk);

    std::atomic<bool>    licensed_  { false };
    std::atomic<OpState> opState_   { OpState::idle };
    std::atomic<Op>      pendingOp_ { Op::none };
    std::atomic<bool>    nagShown_        { false };
    std::atomic<bool>    startupChecked_  { false };

    mutable juce::CriticalSection lock_;   // guards the strings below
    juce::String key_, instanceId_, status_, customerName_, error_;
    juce::String pendingKey_;              // handed from activate() to the thread
    juce::int64  lastValidatedMs_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicenseManager)
};
