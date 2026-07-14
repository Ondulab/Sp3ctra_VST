cmake_minimum_required(VERSION 3.15)

# The full version (x.y.z) lives in vst/VERSION. The patch digit z is
# auto-incremented on every build (bump x/y manually for releases) and
# injected through the generated header Sp3ctraVersion.h — see the
# "Auto-incremented build version" section below.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SP3CTRA_BASE_VERSION)
string(STRIP "${SP3CTRA_BASE_VERSION}" SP3CTRA_BASE_VERSION)

project(Sp3ctraVST VERSION ${SP3CTRA_BASE_VERSION})

# ---------------------------------------------------------------------------
# Suppress macOS ranlib "no symbols" warnings produced by conditionally
# compiled JUCE module stubs (e.g. LV2 and ARA object files that are built
# but contain no exported symbols when those features are disabled).
# Must be set before any target is created so it propagates to FetchContent.
# ---------------------------------------------------------------------------
if(APPLE)
    # Force ar+ranlib (bypass libtool) so -no_warning_for_no_symbols flag is respected.
    # macOS libtool ignores CMAKE_CXX_ARCHIVE_FINISH, so we must override CREATE too.
    set(CMAKE_C_ARCHIVE_CREATE   "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_C_ARCHIVE_FINISH   "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
    set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>")
    set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
endif()

# Add JUCE using FetchContent
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# VOICE module — offline TTS engine (sherpa-onnx running Piper VITS voices).
# Statically linked from the prebuilt arm64 archive: no dylibs, no rpath, no
# codesign changes. OFF compiles PiperTts.cpp as a stub (module shows a
# "TTS engine not available" banner). espeak-ng inside the archive is GPLv3 —
# a distributed binary with TTS enabled is a GPL-encumbered combined work.
# ---------------------------------------------------------------------------
option(SP3CTRA_ENABLE_TTS "Enable the VOICE module TTS engine (sherpa-onnx, static)" ON)

if(APPLE AND SP3CTRA_ENABLE_TTS)
    set(SP3CTRA_SHERPA_VER 1.13.4)
    FetchContent_Declare(sherpa_onnx_libs
        URL https://github.com/k2-fsa/sherpa-onnx/releases/download/v${SP3CTRA_SHERPA_VER}/sherpa-onnx-v${SP3CTRA_SHERPA_VER}-osx-arm64-static-lib.tar.bz2
        URL_HASH SHA256=57801db2bbb786a5d343f515a38ff210b401842338bdc804fa075312d1cd2404
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sherpa_onnx_libs)

    # The -static-lib archive ships no headers; fetch the single self-contained
    # C header at the same pinned tag.
    set(SP3CTRA_SHERPA_INCLUDE ${CMAKE_BINARY_DIR}/sherpa-onnx-include)
    file(DOWNLOAD
        https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/v${SP3CTRA_SHERPA_VER}/sherpa-onnx/c-api/c-api.h
        ${SP3CTRA_SHERPA_INCLUDE}/sherpa-onnx/c-api/c-api.h
        EXPECTED_HASH SHA256=587e1039cc4ee242169494f3c0ba5baecc22482341168d88d57db965e1e77fa9
    )
endif()

# Define the plugin with sources directly
juce_add_plugin(Sp3ctraVST
    COMPANY_NAME "Ondulab"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD TRUE
    PLUGIN_MANUFACTURER_CODE "Ondu"
    PLUGIN_CODE "Sp3c"
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "Sp3ctra"
    # App/bundle icon (dock + Finder). JUCE's juceaide builds the .icns from
    # this PNG and sets CFBundleIconFile on the Standalone .app. Source: the
    # in-app 5-bar logo picto rendered on a dark rounded square (see
    # resources/Sp3ctra_icon.png / Icons::drawSp3ctraLogoPicto in IconPaths.h).
    ICON_BIG   "${CMAKE_CURRENT_SOURCE_DIR}/resources/Sp3ctra_icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/resources/Sp3ctra_icon.png"
    # M9 — CAMERA source module (juce::CameraDevice / AVFoundation capture)
    CAMERA_PERMISSION_ENABLED TRUE
    CAMERA_PERMISSION_TEXT "Sp3ctra uses the camera as a scan-line source (CAMERA module)."
)

# Source files for the plugin wrapper (C++)
target_sources(Sp3ctraVST
    PRIVATE
        source/PluginProcessor.cpp
        source/PluginProcessor.h
        source/PluginEditor.cpp
        source/PluginEditor.h
        source/SettingsWindow.cpp
        source/SettingsWindow.h
        source/CisVisualizerComponent.cpp
        source/CisVisualizerComponent.h
        source/Sp3ctraCore.cpp
        source/Sp3ctraCore.h
        source/Sp3ctraSharedCore.cpp
        source/Sp3ctraSharedCore.h
        source/Sp3ctraConstants.h
        source/UdpReceiverThread.h
        source/global_stubs.c
        
        # Settings tabs (gear wheel — machine-level only since M5)
        source/settings/SystemSettingsTab.cpp
        source/settings/SystemSettingsTab.h

        # Image pipeline UI
        source/image/BlobVisualizerComponent.cpp
        source/image/BlobVisualizerComponent.h

        # M4 — four-zone shell UI (chain rack / engine panels / waterfall column)
        source/ui/ChainRackComponent.cpp
        source/ui/ChainRackComponent.h
        source/ui/EngineAudioPanels.cpp
        source/ui/EngineAudioPanels.h
        source/ui/AudioPanelWidgets.h
        source/ui/WaterfallColumnComponent.cpp
        source/ui/WaterfallColumnComponent.h
        source/ui/SplitterBar.h

        # M6 — editable chain model + drag-and-drop module catalogue
        source/ui/ModuleCatalog.h
        source/ui/ChainModel.cpp
        source/ui/ChainModel.h
        source/ui/ModuleCatalogComponent.h

        # M5 — musical graphical editors (keyboard ruler + draggable ADSR)
        source/ui/KeyboardRulerComponent.cpp
        source/ui/KeyboardRulerComponent.h
        source/ui/EnvelopeEditorComponent.cpp
        source/ui/EnvelopeEditorComponent.h

        # M5 — zone-3 SETUP faces (per-block settings, ex gear-wheel tabs)
        source/ui/setup/SetupHeader.h
        source/ui/setup/SourceSetupPanel.cpp
        source/ui/setup/SourceSetupPanel.h
        source/ui/setup/PitchSetupPanel.cpp
        source/ui/setup/PitchSetupPanel.h
        source/ui/setup/MaskSetupPanel.cpp
        source/ui/setup/MaskSetupPanel.h
        source/ui/setup/LuxStralSetupPanel.cpp
        source/ui/setup/LuxStralSetupPanel.h
        source/ui/setup/LuxSynthSetupPanel.cpp
        source/ui/setup/LuxSynthSetupPanel.h
        source/ui/setup/LuxWaveSetupPanel.cpp
        source/ui/setup/LuxWaveSetupPanel.h
        source/ui/setup/SamplerSetupPanel.cpp
        source/ui/setup/SamplerSetupPanel.h
        source/ui/setup/ScoreSetupPanel.cpp
        source/ui/setup/ScoreSetupPanel.h

        # M9 — IMAGE / VIDEO / CAMERA source modules (engines + service + UI)
        source/sources/MediaSourceEngines.cpp
        source/sources/MediaSourceEngines.h
        source/sources/MediaSourceService.h
        source/sources/VideoFileReader.h
        source/sources/VideoFileReader.mm
        source/sources/ui/MediaSourcePage.cpp
        source/sources/ui/MediaSourcePage.h
        source/sources/ui/MediaSourceSetupPanel.cpp
        source/sources/ui/MediaSourceSetupPanel.h

        # VOICE module — offline TTS (Piper voices via sherpa-onnx) + AUTO
        # language detection (NLLanguageRecognizer) + generation job + page
        source/tts/PiperTts.cpp
        source/tts/PiperTts.h
        source/tts/LanguageDetector.h
        source/tts/LanguageDetector.mm
        source/tts/VoiceGenJob.h
        source/image/VoiceGenTabComponent.h

        # MIDI mapping (CC/Note → any play parameter, MIDI learn)
        source/midi/MidiMappingEngine.cpp
        source/midi/MidiMappingEngine.h
        source/midi/MidiLearnAttachment.h

        # LuxSampler subsystem
        source/luxsampler/LuxSampler.cpp
        source/framesequencer/FrameSequencer.cpp
        source/luxsampler/LuxSampler.h
        source/framesequencer/FrameSequencer.h

        # Sampler UI components
        source/sampler/SlotGridComponent.cpp
        source/sampler/SlotGridComponent.h
        source/sampler/SlotEditorComponent.cpp
        source/sampler/SlotEditorComponent.h
        source/sampler/SlotSpectralEditorComponent.cpp
        source/sampler/SlotSpectralEditorComponent.h
        source/sampler/SlotTimelineComponent.cpp
        source/sampler/SlotTimelineComponent.h
        source/sampler/SequencerComponent.cpp
        source/sampler/SequencerComponent.h
        source/sampler/TransportBarComponent.cpp
        source/sampler/TransportBarComponent.h
        source/sampler/SamplerPageComponent.cpp
        source/sampler/SamplerPageComponent.h
        source/sampler/SequencerPageComponent.cpp
        source/sampler/SequencerPageComponent.h

        # 🎵 PHASE 3: LuxStral Synthesis Engine (VST-adapted)
        source/synthesis/luxstral/vst_adapters.cpp
        source/synthesis/luxstral/vst_adapters.h
        source/synthesis/luxstral/luxstral_engine.h
        source/synthesis/luxstral/synth_luxstral.c
        source/synthesis/luxstral/synth_luxstral.h
        source/synthesis/luxstral/synth_luxstral_algorithms.c
        source/synthesis/luxstral/synth_luxstral_algorithms.h
        source/synthesis/luxstral/synth_luxstral_math.c
        source/synthesis/luxstral/synth_luxstral_math.h
        source/synthesis/luxstral/synth_luxstral_math_neon.c
        source/synthesis/luxstral/synth_luxstral_runtime.c
        source/synthesis/luxstral/synth_luxstral_runtime.h
        source/synthesis/luxstral/synth_luxstral_state.c
        source/synthesis/luxstral/synth_luxstral_state.h
        source/synthesis/luxstral/synth_luxstral_stereo.c
        source/synthesis/luxstral/synth_luxstral_stereo.h
        source/synthesis/luxstral/synth_luxstral_threading.c
        source/synthesis/luxstral/synth_luxstral_threading.h
        source/synthesis/luxstral/synth_luxstral_threading_rt.c
        source/synthesis/luxstral/wave_generation.c
        source/synthesis/luxstral/wave_generation.h
        source/synthesis/luxstral/pow_approx.c
        source/synthesis/luxstral/pow_approx.h
        source/synthesis/luxstral/strokeforge.c
        source/synthesis/luxstral/strokeforge.h

        # 🎵 LuxSynth Additive Synthesis Engine
        source/synthesis/luxsynth/synth_luxsynth_engine.c
        source/synthesis/luxsynth/synth_luxsynth_engine.h
        source/synthesis/luxsynth/luxsynth_vst_adapter.c
        source/synthesis/luxsynth/luxsynth_vst_adapter.h
        source/synthesis/common/voice_manager.c
        source/synthesis/common/voice_manager.h
        source/LuxSynthProcessingThread.h

        # LuxWave Wavetable Synthesis Engine
        source/synthesis/luxwave/synth_luxwave_engine.c
        source/synthesis/luxwave/synth_luxwave_engine.h
        source/synthesis/luxwave/luxwave_vst_adapter.c
        source/synthesis/luxwave/luxwave_vst_adapter.h

        # LuxPitch UI Tab
        source/image/LuxPitchTabComponent.h

        # LuxMask UI Tab
        source/image/LuxMaskTabComponent.h

        # Reverb / Echo FX UI Tabs + interactive editors
        source/image/LuxReverbTabComponent.h
        source/image/LuxEchoTabComponent.h
        source/ui/ReverbEditorComponent.h
        source/ui/EchoEditorComponent.h

        # SCORE — offline printable spectrogram generator (ex Sp3ctraGen)
        source/image/ScoreGenTabComponent.h
        source/image/ScoreGenThread.h
        source/image/ScoreGenRenderer.h
        source/image/ScoreGenRenderer.cpp

        # TIMBRE — parametric instrument-spectrum generator (6 sounds / A4)
        source/image/TimbreGenTabComponent.h
        source/image/TimbreGenRenderer.h
        source/image/TimbreGenRenderer.cpp

        # MIDI SCORE — MIDI-file → printable score generator (per-voice timbres)
        source/image/MidiScoreGenTabComponent.h
        source/image/MidiScoreGenRenderer.h
        source/image/MidiScoreGenRenderer.cpp

        # Video scroll system
        source/video/VideoScrollMode.h
        source/video/VideoDisplayComponent.cpp
        source/video/VideoDisplayComponent.h
        source/video/VideoWindow.cpp
        source/video/VideoWindow.h
        source/video/VideoScrollTab.cpp
        source/video/VideoScrollTab.h
        source/video/VideoScrollPage.h
        source/video/VideoScrollRenderCore.cpp
        source/video/VideoScrollRenderCore.h
        source/ui/VideoMixerComponent.cpp
        source/ui/VideoMixerComponent.h
        source/ui/VideoMixerColumn.cpp
        source/ui/VideoMixerColumn.h
)

# ✨ BRICK 1: UDP Reception - Add C sources
# Only include what's needed for UDP + buffers (no synthesis yet)
target_sources(Sp3ctraVST
    PRIVATE
        # UDP communication
        source/communication/network/udp.c

        # Device HTTP control-plane client (config.html API)
        source/communication/device/Sp3ctraDeviceClient.cpp

        # Logging system
        source/utils/logger.c
        source/utils/error.c
        source/utils/rt_profiler.c
        
        # Threading (for udpThread and buffer initialization)
        source/threading/multithreading.c

        # Image preprocessing (required for VST path)
        source/processing/image_preprocessor.c

        # Image pipeline refactoring (Phase 1+2)
        source/processing/image_pipeline_stages.c
        source/processing/image_pipeline.c
        source/processing/image_chain.c
        source/processing/chain_plan.c
        source/processing/chain_plan.h
        source/processing/synth_staging.c
        source/processing/synth_staging.h
        source/processing/luxsynth_feed.c
        source/processing/luxsynth_feed.h
        source/processing/lux_pitch.c
        source/processing/lux_mask.c
        source/processing/lux_reverb.c
        source/processing/lux_reverb.h
        source/processing/lux_echo.c
        source/processing/lux_echo.h
        source/processing/lux_eq.c
        source/processing/lux_eq.h
        source/processing/video_scroll.c
        source/processing/video_scroll.h
        source/processing/internal_source.c
        source/processing/internal_source.h

        # SCORE — offline spectrogram engine (KissFFT port of Sp3ctraGen)
        source/processing/score_engine.c

        # KissFFT (required by image_preprocessor luxsynth path)
        source/synthesis/luxsynth/kissfft/kiss_fft.c
        source/synthesis/luxsynth/kissfft/kiss_fftr.c
        
        # Audio buffers (DoubleBuffer, AudioImageBuffers)
        source/audio/buffers/audio_image_buffers.c
)

# ---------------------------------------------------------------------------
# Auto-incremented build version
# The patch digit of vst/VERSION is bumped on every `cmake --build` and baked
# into the generated header Sp3ctraVersion.h (used by PluginEditor's header
# bar).
# ---------------------------------------------------------------------------
set(SP3CTRA_VERSION_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SP3CTRA_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_version_header.cmake")

# Configure-time generation (no increment) so the header always exists.
execute_process(COMMAND ${CMAKE_COMMAND}
    -DVERSION_FILE=${CMAKE_CURRENT_SOURCE_DIR}/VERSION
    -DOUTPUT_HEADER=${SP3CTRA_VERSION_HEADER_DIR}/Sp3ctraVersion.h
    -DINCREMENT=OFF
    -P ${SP3CTRA_VERSION_SCRIPT})

add_custom_target(Sp3ctraVersionHeader
    COMMAND ${CMAKE_COMMAND}
        -DVERSION_FILE=${CMAKE_CURRENT_SOURCE_DIR}/VERSION
        -DOUTPUT_HEADER=${SP3CTRA_VERSION_HEADER_DIR}/Sp3ctraVersion.h
        -DINCREMENT=ON
        -P ${SP3CTRA_VERSION_SCRIPT}
    COMMENT "Bumping Sp3ctra patch version"
    VERBATIM)
add_dependencies(Sp3ctraVST Sp3ctraVersionHeader)

# Add include directories for C headers (all migrated into vst/source/)
target_include_directories(Sp3ctraVST
    PRIVATE
        ${SP3CTRA_VERSION_HEADER_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/source
        ${CMAKE_CURRENT_SOURCE_DIR}/source/core
        ${CMAKE_CURRENT_SOURCE_DIR}/source/config
        ${CMAKE_CURRENT_SOURCE_DIR}/source/utils
        ${CMAKE_CURRENT_SOURCE_DIR}/source/communication/network
        ${CMAKE_CURRENT_SOURCE_DIR}/source/audio/buffers
        ${CMAKE_CURRENT_SOURCE_DIR}/source/audio/rtaudio
        ${CMAKE_CURRENT_SOURCE_DIR}/source/threading
        ${CMAKE_CURRENT_SOURCE_DIR}/source/processing
        ${CMAKE_CURRENT_SOURCE_DIR}/source/synthesis/common
        ${CMAKE_CURRENT_SOURCE_DIR}/source/synthesis/luxwave
        ${CMAKE_CURRENT_SOURCE_DIR}/source/synthesis/luxsynth
        ${CMAKE_CURRENT_SOURCE_DIR}/source/synthesis/luxstral
        ${CMAKE_CURRENT_SOURCE_DIR}/source/luxsampler
        ${CMAKE_CURRENT_SOURCE_DIR}/source/sampler
        ${CMAKE_CURRENT_SOURCE_DIR}/source/video
        ${CMAKE_CURRENT_SOURCE_DIR}/source/settings
)

# Compile definitions
target_compile_definitions(Sp3ctraVST PRIVATE
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_CAMERA=1      # M9 — CAMERA source module (juce::CameraDevice)
    NO_SFML=1              # VST does not use SFML (C code uses this for conditional compilation)
    VST_MODE=1
    # Licensing election: Sp3ctra is distributed under GPLv3-or-later, JUCE is
    # used under its AGPLv3 option and the VST3 SDK under its GPLv3 option
    # (see LICENSE + THIRD-PARTY-NOTICES.md). Disabling the JUCE splash screen
    # is permitted under the (A)GPL usage terms.
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

# Link libraries
target_link_libraries(Sp3ctraVST PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_graphics
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_video       # M9 — CAMERA source (juce::CameraDevice)
)

# M9 — VideoFileReader.mm (headless AVPlayer frame reader for the VIDEO source)
if(APPLE)
    target_link_libraries(Sp3ctraVST PRIVATE
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework QuartzCore"
        "-framework NaturalLanguage"   # VOICE — AUTO language detection
    )
endif()

# VOICE module — sherpa-onnx static libs (link order from the official .pc;
# libkissfft-float members only load for symbols the in-tree KissFFT doesn't
# define — same float ABI, so either resolution is correct).
if(APPLE AND SP3CTRA_ENABLE_TTS)
    target_include_directories(Sp3ctraVST PRIVATE ${SP3CTRA_SHERPA_INCLUDE})
    # Absolute paths: plain -l names don't propagate their -L dir from the
    # shared-code static lib to the Standalone/VST3/AU link steps.
    set(SP3CTRA_SHERPA_LIBDIR ${sherpa_onnx_libs_SOURCE_DIR}/lib)
    target_link_libraries(Sp3ctraVST PRIVATE
        ${SP3CTRA_SHERPA_LIBDIR}/libsherpa-onnx-c-api.a
        ${SP3CTRA_SHERPA_LIBDIR}/libsherpa-onnx-core.a
        ${SP3CTRA_SHERPA_LIBDIR}/libkaldi-decoder-core.a
        ${SP3CTRA_SHERPA_LIBDIR}/libsherpa-onnx-kaldifst-core.a
        ${SP3CTRA_SHERPA_LIBDIR}/libsherpa-onnx-fstfar.a
        ${SP3CTRA_SHERPA_LIBDIR}/libsherpa-onnx-fst.a
        ${SP3CTRA_SHERPA_LIBDIR}/libkaldi-native-fbank-core.a
        ${SP3CTRA_SHERPA_LIBDIR}/libkissfft-float.a
        ${SP3CTRA_SHERPA_LIBDIR}/libpiper_phonemize.a
        ${SP3CTRA_SHERPA_LIBDIR}/libespeak-ng.a
        ${SP3CTRA_SHERPA_LIBDIR}/libucd.a
        ${SP3CTRA_SHERPA_LIBDIR}/libonnxruntime.a
        ${SP3CTRA_SHERPA_LIBDIR}/libssentencepiece_core.a
    )
    target_compile_definitions(Sp3ctraVST PRIVATE SP3CTRA_HAS_TTS=1)
endif()

# Standard settings
target_compile_features(Sp3ctraVST PRIVATE cxx_std_17 c_std_11)

# ===== PROFIL RELEASE (OPTIMIZED FOR PRODUCTION) =====
# RT-SAFE OPTIMIZATION: Preserve memory ordering for atomics and barriers
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        target_compile_options(Sp3ctraVST PRIVATE /O2 /fp:fast /W4)
    else()
        target_compile_options(Sp3ctraVST PRIVATE
            -O2                      # Moderate optimization (safer than -O3 for RT audio)
            -fno-strict-aliasing     # Prevent aggressive pointer aliasing
            -Wall                    # All warnings
            -Wextra                  # Extra warnings
            -fPIC                    # Position Independent Code
            -Wno-unused-parameter
            # Suppress warnings from JUCE's bundled third-party code (HarfBuzz, VST3 SDK)
            # These are in code we do not own and cannot modify.
            -Wno-nontrivial-memcall
            -Wno-unused-but-set-variable
            # RT-SAFE: NO -ffast-math (breaks IEEE754 and can affect atomics)
            # RT-SAFE: NO -Ofast (too aggressive, breaks memory ordering)
        )
    endif()
endif()

# ===== PROFIL DEBUG =====
# ASan/UBSan are opt-in via -DUSE_ASAN=ON because VST3 CFBundle link does not
# automatically pull in the sanitizer runtime (JUCE multi-target CMake layout).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(MSVC)
        target_compile_options(Sp3ctraVST PRIVATE /W4)
    else()
        target_compile_options(Sp3ctraVST PRIVATE
            -O0                      # No optimization
            -g                       # Full debug symbols
            -Wall
            -Wextra
            -Wno-unused-parameter
            -fno-omit-frame-pointer  # Better stack traces
            # Suppress warnings from JUCE's bundled third-party code (HarfBuzz, VST3 SDK)
            # These are in code we do not own and cannot modify.
            -Wno-nontrivial-memcall
            -Wno-unused-but-set-variable
        )
        if(USE_ASAN)
            target_compile_options(Sp3ctraVST PRIVATE
                -fsanitize=address
                -fsanitize=undefined
            )
            target_link_options(Sp3ctraVST PRIVATE
                -fsanitize=address
                -fsanitize=undefined
            )
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Suppress warnings from JUCE's bundled third-party libraries.
# In JUCE 8, format-specific code (VST3 SDK, AU) is compiled in each format
# target rather than a shared SharedCode target.
# Apply the suppression flags to all plugin format targets.
# ---------------------------------------------------------------------------
if(NOT MSVC)
    foreach(_fmt_target
            Sp3ctraVST_VST3
            Sp3ctraVST_AU
            Sp3ctraVST_Standalone)
        if(TARGET ${_fmt_target})
            target_compile_options(${_fmt_target} PRIVATE
                -Wno-nontrivial-memcall       # VST3 SDK fvariant.h memset on FVariant
                -Wno-unused-but-set-variable  # JUCE AU / macOS stubs
            )
        endif()
    endforeach()
endif()
