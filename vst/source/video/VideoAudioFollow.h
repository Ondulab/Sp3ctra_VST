#pragma once
#include "VideoMixFocus.h"
#include <cstdint>

namespace VideoAudioFollow
{
    // Eight 4-bit chain+1 entries, in radar order; 0 terminates the list.
    // Published as one atomic word on topology edits. Never walk a UI model
    // from the audio callback.
    inline uint64_t weights(uint64_t topology, float x, float y, bool armed) noexcept
    {
        if (! armed) return UINT64_MAX;
        int count = 0;
        while (count < 8 && ((topology >> (count * 4)) & 15u) != 0) ++count;
        uint64_t packed = UINT64_MAX;
        unsigned seen = 0;
        for (int i = 0; i < count; ++i)
        {
            const int c = (int) ((topology >> (i * 4)) & 15u) - 1;
            if (c < 0 || c >= 8) continue;
            auto level = (unsigned) std::lround(255.0f * VideoMixFocus::audioWeight(i, count, x, y));
            if ((seen & (1u << c)) != 0)
                level = juce::jmax(level, (unsigned) ((packed >> (c * 8)) & 255u));
            seen |= 1u << c;
            packed = (packed & ~(UINT64_C(255) << (c * 8))) | ((uint64_t) level << (c * 8));
        }
        return packed;
    }
}
