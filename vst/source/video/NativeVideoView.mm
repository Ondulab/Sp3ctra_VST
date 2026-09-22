#include "NativeVideoView.h"
#include <cstdlib>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

// A layer-hosting NSView, not an AppKit-managed backing layer. AppKit owns
// only the view's geometry. The render thread changes our layer's contents
// in explicit transactions, without a message-loop callback or paint().
@interface Sp3ctraVideoSurfaceView : NSView
@end
@implementation Sp3ctraVideoSurfaceView
- (NSView*)hitTest:(NSPoint)point { return nil; } // keep JUCE mouse gestures
@end

namespace
{
    struct Pixels
    {
        juce::Image image;
        juce::Image::BitmapData bitmap;
        explicit Pixels(const juce::Image& im)
            : image(im), bitmap(image, juce::Image::BitmapData::readOnly) {}
    };

    class LayerTarget final : public VideoPresentationTarget
    {
    public:
        explicit LayerTarget(CALayer* layer) : layer_([layer retain]),
            colourSpace_(CGColorSpaceCreateDeviceRGB()) {}
        ~LayerTarget() override
        {
            [layer_ release];
            CGColorSpaceRelease(colourSpace_);
        }

        void present(const juce::Image& image) override
        {
            @autoreleasepool
            {
                CGImageRef cg = nullptr;
                if (image.isValid())
                {
                    if (image.getFormat() != juce::Image::ARGB) return;
                    auto pixels = std::make_unique<Pixels>(image);
                    if (pixels->bitmap.pixelStride != 4) return;
                    auto* owner = pixels.get();
                    auto provider = CGDataProviderCreateWithData(owner,
                        owner->bitmap.data,
                        (size_t) owner->bitmap.lineStride * image.getHeight(),
                        [](void* p, const void*, size_t) { delete static_cast<Pixels*>(p); });
                    if (provider == nullptr) return;
                    pixels.release(); // provider retains the JUCE buffer until CA is done
                    cg = CGImageCreate(image.getWidth(), image.getHeight(), 8, 32,
                        owner->bitmap.lineStride, colourSpace_,
                        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst,
                        provider, nullptr, true, kCGRenderingIntentDefault);
                    CGDataProviderRelease(provider);
                    if (cg == nullptr) return;
                }
                // No implicit fade between square-LFO states. Explicit commit
                // is required: the render thread has no Cocoa run loop.
                [CATransaction begin];
                [CATransaction setDisableActions:YES];
                layer_.contents = (id) cg;
                [CATransaction commit];
                if (cg != nullptr) CGImageRelease(cg);
            }
        }
    private:
        CALayer* layer_;
        CGColorSpaceRef colourSpace_;
    };
}

bool NativeVideoView::enabled()
{
    const char* value = std::getenv("SP3CTRA_VIDEO_NATIVE");
    return value == nullptr || value[0] != '0';
}

NativeVideoView::NativeVideoView()
{
    auto* view = [[Sp3ctraVideoSurfaceView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    auto* layer = [CALayer layer];
    layer.backgroundColor = CGColorGetConstantColor(kCGColorWhite);
    layer.contentsGravity = kCAGravityResizeAspect;
    layer.minificationFilter = kCAFilterLinear;
    layer.magnificationFilter = kCAFilterLinear;
    layer.opaque = YES;
    // Set the supplied layer BEFORE wantsLayer: this is layer hosting.
    view.layer = layer;
    view.wantsLayer = YES;
    target_ = std::make_shared<LayerTarget>(layer);
    setView(view);
    [view release];
}

NativeVideoView::~NativeVideoView() { setView(nullptr); }
