#pragma once
#include "../session/MachinePrefs.h"
#include <atomic>
namespace VideoDisplaySettings
{
    inline constexpr const char* kFpsKey="video.displayFps";
    inline constexpr int kFpsChoices[]={10,15,20,24,30,45,60};
    inline std::atomic<int> fpsValue{60};
    inline int sanitise(int value)
    {
        for(int fps:kFpsChoices)if(fps==value)return fps;
        return 60;
    }
    // Message thread, before starting a renderer. Never access preferences
    // from the audio callback or render loop. Shared across open editors.
    inline void restore()
    {fpsValue.store(sanitise(MachinePrefs::file().getIntValue(kFpsKey,60)),std::memory_order_relaxed);}
    inline int fps() noexcept {return fpsValue.load(std::memory_order_relaxed);}
    inline void setFps(int value)
    {
        value=sanitise(value);
        fpsValue.store(value,std::memory_order_relaxed);
        MachinePrefs::file().setValue(kFpsKey,value);
    }
}
