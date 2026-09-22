#include "video/VideoFramePacer.h"
#include "video/VideoBilinear.h"
#include <cassert>
#include <cstring>
#include <cstdio>
static uint32_t seed=123;
static uint32_t randomWord(){seed=1664525u*seed+1013904223u;return seed;}
int main()
{
    for(int fps:{10,15,20,24,30,45,60})
    {
        VideoFramePacer screen,record;int images=0,history=0,recorded=0;
        for(int tick=0;tick<600;++tick) {
            ++history;
            const double now=tick*1000.0/60.0;
            if(screen.due(now,fps))++images;
            if(record.due(now,60))++recorded;
        }
        assert(history==600);assert(recorded==600);assert(images==fps*10);
    }
    VideoFramePacer p;
    assert(p.due(0,10));assert(!p.due(10,10));
    assert(p.due(20,60)); // a user change takes effect immediately
    assert(p.due(5000,60));assert(!p.due(5000,60)); // no catch-up burst
    assert(p.due(0,30)); // clock reset
    for(int n=0;n<1000000;++n) {
        uint8_t taps[16],expected[4],actual[4];
        for(auto& b:taps)b=uint8_t(randomWord()>>24);
        const int wx=n%8==0?0:n%8==1?65536:randomWord()%65537;
        const int wy=n%8==2?0:n%8==3?65536:randomWord()%65537;
        videoblit::bilinearScalar(expected,taps,taps+4,taps+8,taps+12,wx,wy);
        videoblit::bilinearPixel(actual,taps,taps+4,taps+8,taps+12,wx,wy);
        assert(std::memcmp(expected,actual,4)==0);
    }
    puts("PASS: 10..60 FPS independent of 60 Hz history/recording, cap changes, stalls; 1M scalar/SIMD pixels identical");
}
