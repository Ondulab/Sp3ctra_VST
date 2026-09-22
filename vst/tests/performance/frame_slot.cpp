#include "luxsampler/LuxSampler.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) std::abort(); } while (0)
int main() {
    FrameSlot slot;
    slot.allocate(2);
    CHECK(slot.capacity == 2 && slot.frame_count == 0);
    slot.frame_count = 2;
    slot.has_content = true;
    slot.frames[0].line_id = 42; slot.frames[1].line_id = 99;
    slot.reserve(4);
    CHECK(slot.capacity == 4 && slot.frame_count == 2 && slot.has_content);
    CHECK(slot.frames[0].line_id == 42 && slot.frames[1].line_id == 99);
    slot.allocate(1);
    CHECK(slot.capacity == 1 && !slot.has_content && slot.frame_count == 0);
    slot.clear();
    CHECK(slot.capacity == 0 && !slot.frames);
    std::printf("PASS: exact import capacity, overdub preservation, replacement and release; frame=%zu bytes\n", sizeof(CapturedFrame));
}
