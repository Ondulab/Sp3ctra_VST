/**
 * @file DeviceFeedback.h
 * @brief What the VST sends BACK to the CIS over Sp3ctra Link: the button
 *        backlights and the OLED overlay of the parameters being edited.
 *
 * docs/PLAN_SP3CTRA_LINK.md D9 / D10 / §12.4.
 *
 * OLED: every parameter change (mouse, learnt MIDI — the CIS itself included —
 * or host automation) stamps a per-parameter "touched" time. Each tick keeps
 * the three most recently touched DISTINCT parameters that are still inside
 * the Hold window, filters them (Chain = parameters of a chain hosting an IN
 * SP3CTRA module, the SP3CTRA module itself and its acquisition transport;
 * All = every parameter except the session / UI plumbing) and sends an
 * OLED_OVERLAY (coalesced, <= 20 Hz, only when the content changed, refreshed
 * before the device TTL expires). An empty set clears the overlay once.
 *
 * LEDs: per LED mode — Press (device-local, nothing sent), Off (dark, local
 * press feedback inhibited), Follow (mirrors the parameter the button's MIDI
 * is learnt on: 2-state → on/off, continuous → brightness) and Manual (the
 * automatable sp3ctraLedNLevel). Sent on change and every 2 s (UDP).
 *
 * Message thread only (the processor's 30 ms timer); the touch stamps are
 * written from any thread (relaxed atomics).
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <map>
#include <vector>
#include "../communication/link/Sp3ctraLink.h"   // OverlayItem in signatures

class Sp3ctraAudioProcessor;

class DeviceFeedback
{
public:
    explicit DeviceFeedback (Sp3ctraAudioProcessor& p);

    /** Size the touch table once the parameters exist (processor ctor). */
    void prepare();

    /** Any thread: a parameter changed (AudioProcessorListener hook). */
    void noteParamTouched (int paramIndex) noexcept;

    /** Any thread: a VIRTUAL (non-APVTS) target changed — the sampler play
     *  params (SamplerMidiTargets encoding). Stamped by virtualApply (MIDI)
     *  and by the sampler UI handlers, which write the engine directly. */
    void noteVirtualTouched (int targetId) noexcept;

    /** Message thread, ~30 ms. */
    void tick();

private:
    struct Recent { int index = -1; int vt = -1; uint32_t touchedMs = 0; };

    bool eligible (int paramIndex, int mode);
    bool virtualEligible (int targetId, int mode);
    bool composeVirtualItem (int targetId, Sp3ctraLink::OverlayItem& item);
    void tickOverlay (Sp3ctraLink& link, uint32_t now);
    void tickLeds (Sp3ctraLink& link, uint32_t now, bool resendAll);

    Sp3ctraAudioProcessor& proc_;
    std::unique_ptr<std::atomic<uint32_t>[]> touchMs_;
    int  numParams_ = 0;
    std::atomic<uint32_t> touchFlag_ { 0 };
    uint32_t suppressUntilMs_ = 0;
    std::vector<juce::String> ids_;

    // virtual-touch ring — (ms << 32) | uint32(targetId), 0 = empty slot
    static constexpr int kVirtualRing = 8;
    std::atomic<uint64_t> vRing_[kVirtualRing] {};
    std::atomic<uint32_t> vRingW_ { 0 };

    // overlay state
    std::vector<Recent> recent_;
    juce::String lastOverlaySig_;
    uint32_t lastOverlaySendMs_ = 0;
    bool overlayActive_ = false;
    std::map<int, std::pair<bool, uint32_t>> eligCache_;   // index → (eligible, stampMs)
    std::map<int, std::pair<bool, uint32_t>> vEligCache_;  // targetId → (eligible, stampMs)
    int lastOverlayMode_ = -1;

    // led state
    struct LedState { int mode = -1; int brightness = -1; uint8_t flags = 0xFF; uint32_t sentMs = 0; };
    LedState leds_[3];
    uint32_t lastLinkGen_ = 0;
    bool wasBound_ = false;
    uint32_t greetingUntilMs_ = 0;   // the bind greeting owns the LEDs + OLED until then
};
