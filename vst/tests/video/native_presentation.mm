#include "video/NativeVideoView.h"
#include "video/VideoAudioFollow.h"
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <thread>
#include <atomic>
#include <cassert>
#include <cstdio>
int main() {
 juce::ScopedJuceInitialiser_GUI gui;
 @autoreleasepool {
 // Reproduce a pinned three-frame pool: hidden inline contents + retained
 // solo preview + current front. The production route must retire the hidden
 // surface BEFORE the renderer tries to acquire another frame.
 {
  struct HoldingTarget : VideoPresentationTarget {
   juce::Image held;
   void present(const juce::Image& image) override {held=image;}
  };
  juce::Image buffers[3];for(auto& im:buffers)im=juce::Image(juce::Image::ARGB,32,32,true);
  auto inlineTarget=std::make_shared<HoldingTarget>();
  auto fullscreenTarget=std::make_shared<HoldingTarget>();
  VideoPresentationRoute route;route.select(inlineTarget);route.present(buffers[0]);
  juce::Image solo=buffers[1],front=buffers[2];
  for(auto& im:buffers)assert(im.getReferenceCount()>1);
  assert(route.select(fullscreenTarget));
  assert(!inlineTarget->held.isValid());assert(buffers[0].getReferenceCount()==1);
  route.present(front);route.select(inlineTarget);
  assert(!fullscreenTarget->held.isValid());route.select({});
 }
 auto view=std::make_unique<NativeVideoView>();
 auto* ns=(NSView*)view->getView();
 juce::DocumentWindow window("Sp3ctra native presentation test",juce::Colours::black,0);
 window.setUsingNativeTitleBar(true);
 juce::Component container;
 container.addAndMakeVisible(*view);
 window.setContentNonOwned(&container,false);
 window.centreWithSize(680,400);
 view->setBounds(0,0,320,240);
 window.setVisible(true);
 [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
 view->setBounds(10,10,640,360);
 [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
 assert(ns.window != nil); // JUCE attached the native view to its peer
 printf("view %.0fx%.0f layer %.0fx%.0f\n",ns.bounds.size.width,ns.bounds.size.height,ns.layer.bounds.size.width,ns.layer.bounds.size.height);
 assert(ns.layer.bounds.size.width == ns.bounds.size.width);
 assert(ns.layer.bounds.size.height == ns.bounds.size.height);
 // Exercise the native JUCE fullscreen peer, not just a resized NSView.
 window.setFullScreen(true);
 [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.3]];
 view->setBounds(container.getLocalBounds());
 assert(window.isFullScreen());assert(ns.window != nil);
 auto target=view->target();
 juce::Image pool[3]; for(auto& im:pool)im=juce::Image(juce::Image::ARGB,640,360,true,juce::SoftwareImageType());
 std::atomic<int> submitted{0},missed{0};
 std::thread worker([&]{ for(int i=0;i<120;++i) {
  juce::Image* free=nullptr; for(auto& im:pool)if(im.getReferenceCount()==1){free=&im;break;}
  if(free){free->clear(free->getBounds(), i%2?juce::Colours::red:juce::Colours::blue);target->present(*free);++submitted;}else ++missed;
  std::this_thread::sleep_for(std::chrono::milliseconds(16));
 }});
 // Deliberately no AppKit/JUCE events for the whole worker run.
 worker.join();
 printf("main thread blocked 2s: submitted=%d pool misses=%d\n",submitted.load(),missed.load());
 assert(submitted==120); assert(missed==0);
 auto cg=(CGImageRef)ns.layer.contents; assert(cg); assert(CGImageGetWidth(cg)==640);
 auto bytes=CGDataProviderCopyData(CGImageGetDataProvider(cg));auto* pixel=CFDataGetBytePtr(bytes);
 assert(pixel[0]==0 && pixel[1]==0 && pixel[2]==255 && pixel[3]==255);CFRelease(bytes);
 view.reset();
 std::thread afterClose([&]{target->present(pool[2]);target->present({});target.reset();});afterClose.join();
 window.setVisible(false);window.clearContentComponent();
 // Four spokes: right selects model chain 2, left selects model chain 4.
 for(int i=0;i<1600;++i){auto w=VideoAudioFollow::weights(0x4321, i%2?1.f:-1.f,0,true);
  assert(((w>>8)&255)==(i%2?255:0));assert(((w>>24)&255)==(i%2?0:255));}
 assert(VideoAudioFollow::weights(0x4321,1,0,false)==UINT64_MAX);
 assert(VideoAudioFollow::weights(0,1,0,true)==UINT64_MAX);
 auto duplicate=VideoAudioFollow::weights(0x2121,0,1,true);
 assert((duplicate&255)==255);assert(((duplicate>>8)&255)==0);
 puts("fullscreen native submission, retired-surface pool release, geometry, image lifetime and 1600 focus switches OK");
 }
}
