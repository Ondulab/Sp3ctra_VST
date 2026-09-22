#pragma once
#include <cstdint>
// UI-side cumulative-counter reader. Idle yields zero; a restarted producer
// establishes a new baseline. The elapsed time is measured, never assumed.
struct CounterRate
{
    uint64_t previous = 0;
    bool primed = false;
    double sample(uint64_t count, double seconds) noexcept
    {
        const double rate = primed && count >= previous && seconds > 0
            ? double(count - previous) / seconds : 0.0;
        previous = count; primed = true;
        return rate;
    }
};
