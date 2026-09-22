#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#if JUCE_MAC
#include <juce_gui_extra/juce_gui_extra.h>
#endif
#include <memory>

// Non-RT presentation sink. It owns its native layer independently of the
// NSView, so an in-flight frame may safely finish after a window closes.
struct VideoPresentationTarget
{
    virtual ~VideoPresentationTarget() = default;
    virtual void present(const juce::Image&) = 0;
};

// Render-thread-only routing. Retiring a surface must release its last image:
// a hidden inline layer otherwise pins a slot of the shared frame pool forever.
class VideoPresentationRoute
{
public:
    bool select(std::shared_ptr<VideoPresentationTarget> next)
    {
        if (next == active_) return false;
        if (active_) active_->present({});
        active_ = std::move(next);
        return true;
    }
    explicit operator bool() const noexcept { return active_ != nullptr; }
    void present(const juce::Image& image) { if (active_) active_->present(image); }
private:
    std::shared_ptr<VideoPresentationTarget> active_;
};

#if JUCE_MAC
class NativeVideoView : public juce::NSViewComponent
{
public:
    NativeVideoView();
    ~NativeVideoView() override;
    static bool enabled();
    std::shared_ptr<VideoPresentationTarget> target() const { return target_; }
private:
    std::shared_ptr<VideoPresentationTarget> target_;
};
#endif
