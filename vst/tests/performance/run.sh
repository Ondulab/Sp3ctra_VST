#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/sp3ctra-rt-tests.XXXXXX")
trap 'rm -rf "$out"' EXIT
sanitizer=${SANITIZER:-address,undefined}
${CC:-clang} -std=c11 -O1 -g -fsanitize="$sanitizer" -fno-omit-frame-pointer \
    -I "$root/source" -I "$root/source/synthesis/common" "$root/tests/performance/rt_regressions.c" \
    "$root/source/processing/chain_plan.c" \
    "$root/source/synthesis/luxwave/synth_luxwave_engine.c" \
    "$root/source/synthesis/luxsynth/synth_luxsynth_engine.c" \
    "$root/source/synthesis/luxgrain/synth_luxgrain_engine.c" \
    "$root/source/synthesis/common/voice_manager.c" \
    "$root/source/utils/rt_block_metrics.c" -lm -lpthread -o "$out/rt-tests"
"$out/rt-tests"

${CC:-clang} -std=c11 -O1 -g -fsanitize="$sanitizer" -fno-omit-frame-pointer \
    "$root/tests/performance/callback_handoff.c" \
    "$root/source/synthesis/luxstral/vst_callback_sync.c" -lpthread -o "$out/handoff-tests"
"$out/handoff-tests"

${CC:-clang} -std=c11 -O1 -g -fsanitize="$sanitizer" -fno-omit-frame-pointer \
    -I "$root/source" "$root/tests/performance/work_dispatch.c" \
    "$root/source/synthesis/luxstral/synth_work_dispatch.c" -lpthread -o "$out/work-dispatch-tests"
"$out/work-dispatch-tests"

${CC:-clang} -std=c11 -O1 -g -fsanitize="$sanitizer" -fno-omit-frame-pointer \
    -I "$root/source" -I "$root/source/config" -I "$root/source/synthesis/luxstral" \
    "$root/tests/performance/video_audio_staging.c" \
    "$root/source/processing/synth_staging.c" "$root/source/utils/pipeline_metrics.c" -lm -o "$out/video-audio-tests"
"$out/video-audio-tests"

${CC:-clang} -std=c11 -O1 -g -fsanitize="$sanitizer" \
    -c "$root/source/utils/pipeline_metrics.c" -o "$out/pipeline-metrics.o"
${CXX:-clang++} -std=c++17 -O1 -g -fsanitize="$sanitizer" \
    -I "$root/source" "$root/tests/performance/pipeline_metrics.cpp" \
    "$out/pipeline-metrics.o" -lpthread -o "$out/pipeline-metrics-tests"
"$out/pipeline-metrics-tests"

${CXX:-clang++} -std=c++17 -O1 -g -fsanitize="$sanitizer" \
    -I "$root/source" "$root/tests/performance/video_display.cpp" -o "$out/video-display-tests"
"$out/video-display-tests"
