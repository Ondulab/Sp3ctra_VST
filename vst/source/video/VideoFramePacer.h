#pragma once
#include <algorithm>
#include <cmath>
// Display-only deadline gate. Keep history/input work outside this gate.
// No catch-up bursts after a delayed frame; changing the cap applies at once.
class VideoFramePacer
{
public:
    bool due(double nowMs, int fps) noexcept
    {
        fps=std::clamp(fps,10,60);
        if (fps!=fps_ || nowMs<lastMs_) {fps_=fps;nextMs_=nowMs;}
        lastMs_=nowMs;
        if(nowMs+1e-6<nextMs_)return false;
        const double period=1000.0/fps;
        nextMs_ += (std::floor(std::max(0.0,nowMs-nextMs_)/period)+1.0)*period;
        return true;
    }
private:
    int fps_=0;
    double nextMs_=0,lastMs_=0;
};
