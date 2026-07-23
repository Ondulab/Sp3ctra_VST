/**
 * @file MediaSourceEngines.h
 * @brief M9 — Engines behind the IMAGE / VIDEO / CAMERA SRC modules.
 *
 * Each engine produces "CIS lines" (3 planar uint8 channels, chain width) and
 * publishes them into the C-side internal_source pool (processing/
 * internal_source.h). The per-synth routing then substitutes these lines for
 * the live SP3CTRA feed on chains whose SOURCE module is IMAGE/VIDEO/CAMERA —
 * with the device streaming (udpThread substitution) or without it
 * (internal_sources_process_tick driven by MediaSourceService).
 *
 *   IMAGE  — a still image resized to the chain width; ONE row is the line.
 *            The row is movable (PLAY face / host automation) and has a
 *            sampler-like transport: scan the image once / loop / reverse
 *            loop / ping-pong over a configurable traversal duration,
 *            confined to user-draggable scan bounds [start, end].
 *   VIDEO  — a video file (AVFoundation); ONE chosen row of the running video
 *            is the line. Transport: play once / loop / reverse / ping-pong
 *            at a speed factor.
 *   CAMERA — same as VIDEO but the frames come from a live capture device
 *            (juce::CameraDevice); no transport, just the chosen row.
 *
 * Threading:
 *   - load/unload/open/close and param setters: message thread.
 *   - tick(): MediaSourceService thread only.
 *   - getPreview / playhead getters: any thread (mutex/atomics).
 */
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include "../processing/internal_source.h"
}

class VideoFileReader;
namespace juce { class CameraDevice; }

namespace MediaSrc
{
    /** Loop modes — same semantics (and order) as the sampler's LoopMode. */
    enum LoopMode { Once = 0, Loop = 1, Reverse = 2, PingPong = 3 };

    /** Extract row `frac` (0 = top, 1 = bottom) of an image and resample it
     *  horizontally to nPixels planar RGB. Any-thread pure helper. */
    void extractLineFromImage(const juce::Image& img, float frac,
                              uint8_t* r, uint8_t* g, uint8_t* b, int nPixels);
}

//==============================================================================
// ImageSourceEngine — still image → movable/scannable line
//==============================================================================
class ImageSourceEngine
{
public:
    ImageSourceEngine() = default;

    // ── media (message thread) ───────────────────────────────────────────────
    bool loadFile(const juce::File& f, juce::String& error);
    void unload();
    bool isLoaded() const noexcept { return loaded_.load(std::memory_order_acquire); }
    juce::File  getFile() const;
    juce::Image getPreviewImage() const;      ///< aspect-preserving copy for the UI
    int getRowCount() const;

    // ── pool slot (P5-M3): which (IMAGE, slot) line this engine publishes ───
    void setSlot(int s) noexcept   { slot_ = s; }
    int  getSlot() const noexcept  { return slot_; }

    // ── module presence / params (message thread, atomics) ──────────────────
    // Re-activation (module re-added / source re-enabled) forces a fresh
    // publish (seekPending): deactivation ERASED the pool line.
    void setModulePresent(bool p)  { present_.store(p);  if (p) seekPending_.store(true); updateActive(); }
    bool isModulePresent() const   { return present_.load(); }
    void setEnabled(bool e)        { enabled_.store(e);  if (e) seekPending_.store(true); updateActive(); }
    bool isEnabled() const         { return enabled_.load(); }
    void setPosition(float frac);              ///< manual / automated line seek
    void setDurationS(float s)     { durS_.store(juce::jlimit(0.05f, 600.f, s)); }
    void setLoopMode(int m)        { loopMode_.store(juce::jlimit(0, 3, m)); }
    // Orientation: 0..3 quarter turns clockwise. The strip + preview are
    // rebuilt from the kept source image on the service thread (tick), so the
    // setter stays cheap/wait-free for host automation.
    void setRotation(int quarterTurns);
    int  getRotation() const       { return rot_.load(); }
    // Scan bounds: the transport reads only [start, end] of the image (manual
    // seeks stay free). Crossed markers are normalised at tick time.
    void setScanStart(float f)     { scanStart_.store(juce::jlimit(0.f, 1.f, f)); }
    void setScanEnd(float f)       { scanEnd_.store(juce::jlimit(0.f, 1.f, f)); }
    void setPlaying(bool p);
    bool isPlaying() const         { return playing_.load(std::memory_order_acquire); }
    float getPlayheadFrac() const  { return headPub_.load(std::memory_order_relaxed); }

    /** Fired (via MessageManager::callAsync) when a ONCE traversal reaches the
     *  end — lets the processor snap the play param back off. */
    std::function<void()> onPlaybackFinished;

    // ── service thread ───────────────────────────────────────────────────────
    void tick(double nowMs);

private:
    void updateActive();
    void publishRow(double frac);
    void applyImage(const juce::Image& oriented);   ///< (re)build strips+preview

    mutable std::mutex     mediaMutex_;   // strip + preview + file + source
    std::vector<uint8_t>   stripR_, stripG_, stripB_;   // rows_ × width_ planar
    int                    rows_ = 0, width_ = 0;
    juce::File             file_;
    juce::Image            preview_;
    juce::Image            source_;      // original decode — rotation rebuilds

    int                slot_ { 0 };   // P5-M3 — (IMAGE, slot) pool line
    std::atomic<bool>  present_  { false };
    std::atomic<bool>  enabled_  { true };
    std::atomic<bool>  loaded_   { false };
    std::atomic<float> targetPos_{ 0.5f };
    std::atomic<bool>  seekPending_{ true };
    std::atomic<float> durS_     { 5.0f };
    std::atomic<float> scanStart_{ 0.0f };
    std::atomic<float> scanEnd_  { 1.0f };
    std::atomic<int>   loopMode_ { MediaSrc::Loop };
    std::atomic<int>   rot_     { 0 };
    std::atomic<bool>  rotDirty_{ false };
    std::atomic<bool>  playing_  { false };
    std::atomic<bool>  justStarted_{ false };
    std::atomic<float> headPub_  { 0.5f };

    // service-thread state
    double head_   = 0.5;
    int    dir_    = +1;
    double lastMs_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImageSourceEngine)
};

//==============================================================================
// VideoSourceEngine — video file → one chosen row, transported
//==============================================================================
class VideoSourceEngine
{
public:
    VideoSourceEngine();
    ~VideoSourceEngine();

    // ── media (message thread) ───────────────────────────────────────────────
    bool loadFile(const juce::File& f, juce::String& error);
    void unload();
    bool isLoaded() const noexcept { return loaded_.load(std::memory_order_acquire); }
    juce::File  getFile() const;
    juce::Image getPreviewImage() const;
    double getDurationS() const;
    bool   canPlayReverse() const;

    // ── pool slot (P5-M3): which (VIDEO, slot) line this engine publishes ───
    void setSlot(int s) noexcept   { slot_ = s; }
    int  getSlot() const noexcept  { return slot_; }

    // ── module presence / params (message thread, atomics) ──────────────────
    // Re-activation republishes the current line (deactivation erased it).
    void setModulePresent(bool p)  { present_.store(p);  if (p) lineDirty_.store(true); updateActive(); }
    bool isModulePresent() const   { return present_.load(); }
    void setEnabled(bool e)        { enabled_.store(e);  if (e) lineDirty_.store(true); updateActive(); }
    bool isEnabled() const         { return enabled_.load(); }
    void setLineFrac(float f)      { lineFrac_.store(juce::jlimit(0.f, 1.f, f)); lineDirty_.store(true); }
    float getLineFrac() const      { return lineFrac_.load(); }
    void setSpeed(float s)         { speed_.store(juce::jlimit(0.05f, 8.f, s)); rateDirty_.store(true); }
    void setLoopMode(int m)        { loopMode_.store(juce::jlimit(0, 3, m)); rateDirty_.store(true); }
    void setPlaying(bool p);
    bool isPlaying() const         { return playing_.load(std::memory_order_acquire); }
    void   seekFrac(double f);                 ///< scrub, 0..1 of duration
    double getPositionFrac() const;            ///< playhead for the UI

    std::function<void()> onPlaybackFinished;

    // ── service thread ───────────────────────────────────────────────────────
    void tick(double nowMs);

private:
    void updateActive();
    void applyRate();
    void publishLine();

    int                slot_ { 0 };   // P5-M3 — (VIDEO, slot) pool line
    std::unique_ptr<VideoFileReader> reader_;
    mutable std::mutex  mediaMutex_;      // reader open/close + preview
    juce::File          file_;
    juce::Image         frame_;           // service-thread working frame
    juce::Image         preview_;

    std::atomic<bool>  present_  { false };
    std::atomic<bool>  enabled_  { true };
    std::atomic<bool>  loaded_   { false };
    std::atomic<float> lineFrac_ { 0.5f };
    std::atomic<float> speed_    { 1.0f };
    std::atomic<int>   loopMode_ { MediaSrc::Loop };
    std::atomic<bool>  playing_  { false };
    std::atomic<bool>  justStarted_{ false };
    std::atomic<bool>  lineDirty_{ true };
    std::atomic<bool>  rateDirty_{ true };
    std::atomic<float> posFracPub_{ 0.f };
    std::atomic<double> pendingSeek_{ -1.0 };

    // service-thread state
    int    dir_          = +1;
    double lastPreviewMs_ = 0.0;
    double lastStepMs_    = 0.0;   // reverse step-seek fallback pacing

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoSourceEngine)
};

//==============================================================================
// CameraSourceEngine — live capture device → one chosen row
//==============================================================================
class CameraSourceEngine
{
public:
    CameraSourceEngine();
    ~CameraSourceEngine();

    static juce::StringArray getDeviceNames();

    // ── device (message thread) ──────────────────────────────────────────────
    bool openDevice(int index, juce::String& error);
    void closeDevice();
    bool isOpen() const noexcept { return open_.load(std::memory_order_acquire); }
    int  getOpenDeviceIndex() const { return deviceIndex_.load(); }
    juce::String getOpenDeviceName() const;
    juce::Image  getPreviewImage() const;

    // ── pool slot (P5-M3): which (CAMERA, slot) line this engine publishes ──
    void setSlot(int s) noexcept   { slot_ = s; }
    int  getSlot() const noexcept  { return slot_; }

    // ── module presence / params ─────────────────────────────────────────────
    // Re-activation republishes the current line (deactivation erased it).
    void setModulePresent(bool p)  { present_.store(p);  if (p) lineDirty_.store(true); updateActive(); }
    bool isModulePresent() const   { return present_.load(); }
    void setEnabled(bool e)        { enabled_.store(e);  if (e) lineDirty_.store(true); updateActive(); }
    bool isEnabled() const         { return enabled_.load(); }
    void setLineFrac(float f)      { lineFrac_.store(juce::jlimit(0.f, 1.f, f)); lineDirty_.store(true); }
    float getLineFrac() const      { return lineFrac_.load(); }

    // ── service thread ───────────────────────────────────────────────────────
    void tick(double nowMs);

private:
    void updateActive();

    int                slot_ { 0 };   // P5-M3 — (CAMERA, slot) pool line
    struct FrameListener;
    std::unique_ptr<juce::CameraDevice> device_;
    std::unique_ptr<FrameListener>      listener_;
    juce::String                        deviceName_;

    mutable std::mutex frameMutex_;   // lastFrame_ + preview_ + deviceName_
    juce::Image        lastFrame_;
    juce::Image        preview_;

    std::atomic<bool>  present_    { false };
    std::atomic<bool>  enabled_    { true };
    std::atomic<bool>  open_       { false };
    std::atomic<int>   deviceIndex_{ -1 };
    std::atomic<float> lineFrac_   { 0.5f };
    std::atomic<bool>  lineDirty_  { true };
    std::atomic<bool>  newFrame_   { false };

    double lastPreviewMs_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CameraSourceEngine)
};
