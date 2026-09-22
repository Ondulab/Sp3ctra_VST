#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace videotiming
{
    // Opt-in only, read outside the audio thread. Each owner has its own
    // accumulator: the render thread and message thread share no mutable state.
    inline bool enabled() noexcept
    {
        static const bool on = [] {
            const char* value = std::getenv("SP3CTRA_VIDEO_TIMING");
            return value != nullptr && value[0] == '1';
        }();
        return on;
    }

    struct Window
    {
        double sinceMs = -1.0, lastFrameMs = -1.0;
        double totalWorkMs = 0.0, maxWorkMs = 0.0, maxGapMs = 0.0;
        unsigned calls = 0, frames = 0;

        bool add(double nowMs, double workMs, bool newFrame) noexcept
        {
            if (sinceMs < 0.0) sinceMs = nowMs;
            ++calls;
            totalWorkMs += workMs;
            maxWorkMs = std::max(maxWorkMs, workMs);
            if (newFrame)
            {
                ++frames;
                if (lastFrameMs >= 0.0)
                    maxGapMs = std::max(maxGapMs, nowMs - lastFrameMs);
                lastFrameMs = nowMs;
            }
            // Include an ongoing stall, even if no frame has arrived yet.
            if (lastFrameMs >= 0.0)
                maxGapMs = std::max(maxGapMs, nowMs - lastFrameMs);
            return nowMs - sinceMs >= 2000.0;
        }

        void report(const char* stage, double nowMs)
        {
            const double elapsed = std::max(1.0, nowMs - sinceMs);
            std::fprintf(stderr,
                "[VIDEO TIMING] %s: %.1f fps, work avg %.2f ms / max %.2f ms, "
                "frame gap max %.1f ms (%u frames / %u calls)\n",
                stage, 1000.0 * frames / elapsed,
                calls != 0 ? totalWorkMs / calls : 0.0, maxWorkMs,
                maxGapMs, frames, calls);
            sinceMs = nowMs;
            calls = frames = 0;
            totalWorkMs = maxWorkMs = maxGapMs = 0.0;
        }
    };
}
