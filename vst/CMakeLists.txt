cmake_minimum_required(VERSION 3.15)

project(Sp3ctraVST VERSION 0.0.1)

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
        
        # Settings tabs
        source/settings/GeneralSettingsTab.cpp
        source/settings/GeneralSettingsTab.h
        source/settings/NetworkSettingsTab.cpp
        source/settings/NetworkSettingsTab.h
        source/settings/ImageSettingsTab.cpp
        source/settings/ImageSettingsTab.h
        source/settings/LuxStralSettingsTab.cpp
        source/settings/LuxStralSettingsTab.h
        source/settings/LuxSamplerSettingsTab.cpp
        source/settings/LuxSamplerSettingsTab.h

        # Image pipeline UI
        source/image/ImagePageComponent.cpp
        source/image/ImagePageComponent.h
        source/image/BlobVisualizerComponent.cpp
        source/image/BlobVisualizerComponent.h

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
        source/sampler/SlotTimelineComponent.cpp
        source/sampler/SlotTimelineComponent.h
        source/sampler/SequencerComponent.cpp
        source/sampler/SequencerComponent.h
        source/sampler/TransportBarComponent.cpp
        source/sampler/TransportBarComponent.h
        source/sampler/SamplerPageComponent.cpp
        source/sampler/SamplerPageComponent.h
        
        # 🎵 PHASE 3: LuxStral Synthesis Engine (VST-adapted)
        source/synthesis/luxstral/vst_adapters.cpp
        source/synthesis/luxstral/vst_adapters.h
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

        # LuxSynth Settings Tab
        source/settings/LuxSynthSettingsTab.cpp
        source/settings/LuxSynthSettingsTab.h
)

# ✨ BRICK 1: UDP Reception - Add C sources
# Only include what's needed for UDP + buffers (no synthesis yet)
target_sources(Sp3ctraVST
    PRIVATE
        # UDP communication
        source/communication/network/udp.c
        
        # Configuration system
        source/config/config_loader.c
        
        # Logging system
        source/utils/logger.c
        source/utils/error.c
        source/utils/rt_profiler.c
        
        # Threading (for udpThread and buffer initialization)
        source/threading/multithreading.c

        # Image preprocessing (required for VST path)
        source/processing/image_preprocessor.c

        # Image pipeline refactoring (Phase 1+2)
        source/processing/image_source_router.c
        source/processing/image_blend.c
        source/processing/image_pipeline_stages.c
        source/processing/image_pipeline.c

        # Pan system (required for stereo mode)
        source/audio/pan/lock_free_pan.c

        # KissFFT (required by image_preprocessor luxsynth path)
        source/synthesis/luxsynth/kissfft/kiss_fft.c
        source/synthesis/luxsynth/kissfft/kiss_fftr.c
        
        # Audio buffers (DoubleBuffer, AudioImageBuffers)
        source/audio/buffers/audio_image_buffers.c
        
        # Stubs for missing functions
        source/utils/stubs.c
)

# Add include directories for C headers (all migrated into vst/source/)
target_include_directories(Sp3ctraVST
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/source
        ${CMAKE_CURRENT_SOURCE_DIR}/source/core
        ${CMAKE_CURRENT_SOURCE_DIR}/source/config
        ${CMAKE_CURRENT_SOURCE_DIR}/source/utils
        ${CMAKE_CURRENT_SOURCE_DIR}/source/communication/network
        ${CMAKE_CURRENT_SOURCE_DIR}/source/audio/buffers
        ${CMAKE_CURRENT_SOURCE_DIR}/source/audio/pan
        ${CMAKE_CURRENT_SOURCE_DIR}/source/audio/rtaudio
        ${CMAKE_CURRENT_SOURCE_DIR}/source/threading
        ${CMAKE_CURRENT_SOURCE_DIR}/source/processing
        ${CMAKE_CURRENT_SOURCE_DIR}/source/synthesis/common
        ${CMAKE_CURRENT_SOURCE_DIR}/source/synthesis/luxwave
        ${CMAKE_CURRENT_SOURCE_DIR}/source/synthesis/luxsynth
        ${CMAKE_CURRENT_SOURCE_DIR}/source/synthesis/luxstral
        ${CMAKE_CURRENT_SOURCE_DIR}/source/luxsampler
        ${CMAKE_CURRENT_SOURCE_DIR}/source/sampler
)

# Compile definitions
target_compile_definitions(Sp3ctraVST PRIVATE
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    NO_SFML=1              # VST does not use SFML (C code uses this for conditional compilation)
    VST_MODE=1
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
)

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
