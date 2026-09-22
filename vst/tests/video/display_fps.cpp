#include "Stub.h"
#include "VideoScrollRenderCore.h"
#include "VideoFramePacer.h"
#include "../processing/video_scroll.h"
#include <cstdio>
#include <cassert>
#include <ctime>
#include <cstring>
int main(){
 video_scroll_init_all();
 Sp3ctraAudioProcessor p;
 p.params[vsParam(0,"speed")]=0.6f;p.params[vsParam(0,"blur")]=0.5f;p.params[vsParam(0,"fade")]=0.5f;
 juce::Image reference;
 for(int fps:{60,30,15,10}){
  VideoScrollRenderCore core(p,0);core.setDisplaySize(640,480);
  VideoFramePacer gate;juce::Image output(juce::Image::ARGB,640,480,true,juce::SoftwareImageType());
  uint8_t row[2048];int submitted=0,rendered=0;auto cpu=std::clock();
  for(int tick=0;tick<240;++tick){
   for(int n=0;n<16+(tick%3==0);++n){for(int x=0;x<2048;++x)row[x]=uint8_t((x*13+submitted*19)%256);video_scroll_capture_line(video_scroll_instance(0),row,row,row,2048);++submitted;}
   core.tick(tick*1000.0/60,1000.0/60);
   if(gate.due(tick*1000.0/60,fps)){core.buildWarp();core.drawWarp(output);++rendered;}
  }
  auto elapsed=double(std::clock()-cpu)/CLOCKS_PER_SEC;
  // Compare the final history at exactly the same time, independently of
  // which earlier ticks were presented. This final check is outside timing.
  core.buildWarp();core.drawWarp(output);
  if(reference.isValid()){
   juce::Image::BitmapData a(reference,juce::Image::BitmapData::readOnly),b(output,juce::Image::BitmapData::readOnly);
   for(int y=0;y<480;++y)assert(!std::memcmp(a.getLinePointer(y),b.getLinePointer(y),640*4));
  }else reference=output.createCopy();
  assert(rendered==fps*4);assert(submitted==3920);
  printf("%d FPS: %d/240 image passes, 3920 input lines, same final image; process CPU %.3f s\n",fps,rendered,elapsed);
 }
}
