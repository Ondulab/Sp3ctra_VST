cmake_minimum_required(VERSION 3.15)

# ---------------------------------------------------------------------------
# Windows CRT = STATIC (/MT). CMP0091 lets CMAKE_MSVC_RUNTIME_LIBRARY drive the
# runtime choice instead of hard-coded /MD flags. Static linking bakes the
# Visual C++ runtime INTO Sp3ctra.exe and Sp3ctra.vst3, so a clean Windows has
# no VCRUNTIME140.dll / VCRUNTIME140_1.dll / MSVCP140.dll dependency to satisfy.
# Must be set before project() (policy) and before any target — including the
# FetchContent JUCE targets below — so JUCE builds /MT too. Requires the
# sherpa-onnx "MT" archive variant (selected in the TTS block).
# ---------------------------------------------------------------------------
cmake_policy(SET CMP0091 NEW)
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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

if(SP3CTRA_ENABLE_TTS AND (APPLE OR WIN32))
    set(SP3CTRA_SHERPA_VER 1.13.4)
    if(APPLE)
        set(_sherpa_archive sherpa-onnx-v${SP3CTRA_SHERPA_VER}-osx-arm64-static-lib)
        set(_sherpa_hash 57801db2bbb786a5d343f515a38ff210b401842338bdc804fa075312d1cd2404)
    else() # WIN32 — MT (STATIC CRT, matches CMAKE_MSVC_RUNTIME_LIBRARY above so
           # the app is self-contained: no VC++ redistributable needed), Release
        set(_sherpa_archive sherpa-onnx-v${SP3CTRA_SHERPA_VER}-win-x64-static-MT-Release-lib)
        set(_sherpa_hash d81bd1d25112540862d2387072e76b2b6843ef962918d6b5c7db5a19c6276b4c)
    endif()
    FetchContent_Declare(sherpa_onnx_libs
        URL https://github.com/k2-fsa/sherpa-onnx/releases/download/v${SP3CTRA_SHERPA_VER}/${_sherpa_archive}.tar.bz2
        URL_HASH SHA256=${_sherpa_hash}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(sherpa_onnx_libs)

    # The -lib archives ship no headers; fetch the single self-contained C
    # header at the same pinned tag.
    set(SP3CTRA_SHERPA_INCLUDE ${CMAKE_BINARY_DIR}/sherpa-onnx-include)
    file(DOWNLOAD
        https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/v${SP3CTRA_SHERPA_VER}/sherpa-onnx/c-api/c-api.h
        ${SP3CTRA_SHERPA_INCLUDE}/sherpa-onnx/c-api/c-api.h
        EXPECTED_HASH SHA256=587e1039cc4ee242169494f3c0ba5baecc22482341168d88d57db965e1e77fa9
    )
endif()

# ---------------------------------------------------------------------------
# Platform format set: AU is macOS-only. COPY_PLUGIN_AFTER_BUILD would try to
# write into "C:\Program Files\Common Files\VST3" on Windows (admin-only), so
# it stays a macOS convenience.
# ---------------------------------------------------------------------------
if(APPLE)
    set(SP3CTRA_PLUGIN_FORMATS VST3 AU Standalone)
    set(SP3CTRA_COPY_PLUGIN TRUE)
else()
    set(SP3CTRA_PLUGIN_FORMATS VST3 Standalone)
    set(SP3CTRA_COPY_PLUGIN FALSE)
endif()

# Define the plugin with sources directly
juce_add_plugin(Sp3ctraVST
    COMPANY_NAME "Ondulab"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    # TRUE since the MIDI TAP module: the plugin can emit extracted notes on a
    # MIDI output bus (producesMidi() follows automatically via
    # JucePlugin_ProducesMidiOutput). This changes the DECLARED BUS LAYOUT, so
    # hosts must re-scan/re-validate the plugin after this build.
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD ${SP3CTRA_COPY_PLUGIN}
    PLUGIN_MANUFACTURER_CODE "Ondu"
    PLUGIN_CODE "Sp3c"
    FORMATS ${SP3CTRA_PLUGIN_FORMATS}
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
        source/session/SessionManager.cpp
        source/session/SessionManager.h
        source/session/MachinePrefs.h
        source/session/Sp3sImporter.cpp
        source/session/Sp3sImporter.h
        source/PluginEditor.cpp
        source/PluginEditor.h
        source/AppUpdater.cpp
        source/AppUpdater.h
        source/UpdateDialog.h
        source/licensing/LicenseManager.cpp
        source/licensing/LicenseManager.h
        source/licensing/ActivationDialog.h
        source/OndulabLinks.h
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

        # Image pipeline UI
        source/image/BlobVisualizerComponent.cpp
        source/image/BlobVisualizerComponent.h

        # M4 — four-zone shell UI (chain rack / engine panels / waterfall column)
        source/ui/ChainRackComponent.cpp
        source/ui/ChainRackComponent.h
        source/ui/EngineAudioPanels.cpp
        source/ui/EngineAudioPanels.h
        source/ui/LuxGrainPanel.h
        source/ui/AudioPanelWidgets.h
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
        source/ui/setup/LuxGrainSetupPanel.h
        source/ui/setup/SamplerSetupPanel.cpp
        source/ui/setup/SamplerSetupPanel.h
        source/ui/setup/ScoreSetupPanel.cpp
        source/ui/setup/ScoreSetupPanel.h
        source/ui/setup/MidiScoreSetupPanel.h
        source/ui/setup/TimbreSetupPanel.h
        source/ui/setup/VoiceSetupPanel.h
        source/ui/setup/VideoScrollSetupPanel.cpp
        source/ui/setup/VideoScrollSetupPanel.h

        # M9 — IMAGE / VIDEO / CAMERA source modules (engines + service + UI)
        # (VideoFileReader impl is platform-specific — see block below)
        source/sources/MediaSourceEngines.cpp
        source/sources/MediaSourceEngines.h
        source/sources/MediaSourceService.h
        source/sources/VideoFileReader.h
        source/sources/ui/MediaSourcePage.cpp
        source/sources/ui/MediaSourcePage.h

        # VOICE module — offline TTS (Piper voices via sherpa-onnx)
        source/tts/PiperTts.cpp
        source/tts/PiperTts.h
        source/tts/VoiceGenJob.h
        source/image/VoiceGenTabComponent.h

        # MIDI mapping (CC/Note → any play parameter, MIDI learn)
        source/midi/MidiMappingEngine.cpp
        source/midi/MidiMappingEngine.h
        source/midi/MidiLearnAttachment.h
        source/midi/MidiTapPage.h
        source/midi/MidiTapSink.cpp
        source/midi/MidiTapSink.h

        # LuxSampler subsystem
        source/luxsampler/LuxSampler.cpp
        source/framesequencer/FrameSequencer.cpp
        source/luxsampler/LuxSampler.h
        source/framesequencer/FrameSequencer.h
        # P5-M4 -- per-instance score players
        source/luxsampler/ScorePlayerService.cpp
        source/luxsampler/ScorePlayerService.h
        source/luxsampler/score_player_hooks.h

        # Sampler UI components
        source/sampler/SlotGridComponent.cpp
        source/sampler/SlotGridComponent.h
        source/sampler/SlotEditorComponent.cpp
        source/sampler/SlotEditorComponent.h
        source/sampler/SlotSpectralEditorComponent.cpp
        source/sampler/SlotSpectralEditorComponent.h
        source/sampler/SequencerComponent.cpp
        source/sampler/SequencerComponent.h
        source/sampler/TransportBarComponent.cpp
        source/sampler/TransportBarComponent.h
        source/sampler/SamplerPageComponent.cpp
        source/sampler/SamplerPageComponent.h

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
        source/synthesis/luxstral/luxstral_wavetable.c
        source/synthesis/luxstral/luxstral_wavetable.h
        source/ui/TimbreWaveformStrip.h
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

        # LuxGrain Granular Synthesis Engine
        source/synthesis/luxgrain/synth_luxgrain_engine.c
        source/synthesis/luxgrain/synth_luxgrain_engine.h
        source/synthesis/luxgrain/luxgrain_vst_adapter.c
        source/synthesis/luxgrain/luxgrain_vst_adapter.h

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
        source/video/VideoScrollPage.h
        source/video/VideoScrollRenderCore.cpp
        source/video/VideoScrollRenderCore.h
        source/ui/VideoMixerComponent.cpp
        source/ui/VideoMixerComponent.h
        source/ui/VideoMixerColumn.cpp
        source/ui/VideoMixerColumn.h
)

# ---------------------------------------------------------------------------
# Platform-specific implementations: AVFoundation VIDEO reader on macOS,
# graceful stub elsewhere (VIDEO file source disabled). AUTO language
# detection was removed entirely (used to be macOS NaturalLanguage-only).
# ---------------------------------------------------------------------------
if(APPLE)
    target_sources(Sp3ctraVST PRIVATE
        source/sources/VideoFileReader.mm
        # VIDEO MIX + master-audio recorder (AVAssetWriter → HEVC .mov).
        source/recorder/VideoRecorder.mm
    )
elseif(WIN32)
    # VIDEO file source via Media Foundation (mirrors the AVFoundation reader).
    target_sources(Sp3ctraVST PRIVATE
        source/sources/VideoFileReaderMF.cpp
        # Recorder: macOS-only for now → graceful stub (Media Foundation TODO).
        source/recorder/VideoRecorderStub.cpp
    )
else()
    target_sources(Sp3ctraVST PRIVATE
        source/sources/VideoFileReaderStub.cpp
        source/recorder/VideoRecorderStub.cpp
    )
endif()

# ---------------------------------------------------------------------------
# Windows port: the C core is written against POSIX (pthread, unistd,
# sys/time, BSD sockets). source/compat/win32 mirrors those headers onto
# Win32 primitives (SRWLOCK/CONDITION_VARIABLE/_beginthreadex/Winsock2) so
# the C sources compile unchanged. Requires clang-cl (VS toolset -T ClangCL):
# the C sources use C11 <stdatomic.h> and __builtin_prefetch, which plain
# MSVC cl does not support.
# ---------------------------------------------------------------------------
if(WIN32)
    target_include_directories(Sp3ctraVST PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/source/compat/win32)
    target_compile_definitions(Sp3ctraVST PRIVATE
        _CRT_SECURE_NO_WARNINGS      # snprintf/getenv/strerror & co
        _CRT_NONSTDC_NO_DEPRECATE    # strdup/_mkdir POSIX-name warnings
        _USE_MATH_DEFINES            # M_PI in the DSP code
        NOMINMAX                     # keep std::min/max usable
    )
    target_link_libraries(Sp3ctraVST PRIVATE
        ws2_32     # Winsock2 (UDP device stream)
        avrt       # MMCSS "Pro Audio" scheduling for RT threads
        winmm      # timeBeginPeriod(1) — 1 ms Sleep granularity (Sp3ctraSharedCore)
        mfplat      # Media Foundation — VIDEO file source (VideoFileReaderMF)
        mfreadwrite
        mfuuid
    )
    if(MSVC)
        # /utf-8: sources contain UTF-8 literals & comments; /bigobj: large
        # JUCE-heavy translation units (PluginProcessor.cpp).
        target_compile_options(Sp3ctraVST PRIVATE /utf-8 /bigobj)
        # /Z7 + /DEBUG: emit PDB symbols in Release so field crash dumps can be
        # symbolicated (CI archives them). Debug info lives in the separate
        # .pdb — the shipped binaries are unchanged. /OPT:REF,ICF keeps the
        # usual Release linker dead-stripping that /DEBUG would otherwise
        # disable.
        foreach(_t Sp3ctraVST Sp3ctraVST_Standalone Sp3ctraVST_VST3)
            if(TARGET ${_t})
                target_compile_options(${_t} PRIVATE /Z7)
                target_link_options(${_t} PRIVATE /DEBUG /OPT:REF /OPT:ICF)
            endif()
        endforeach()
    endif()
endif()

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
        source/processing/luxgrain_feed.c
        source/processing/luxgrain_feed.h
        source/processing/lux_pitch.c
        source/processing/lux_mask.c
        source/processing/lux_reverb.c
        source/processing/lux_reverb.h
        source/processing/lux_echo.c
        source/processing/lux_echo.h
        source/processing/lux_eq.c
        source/processing/lux_eq.h
        source/processing/lux_harmo.c
        source/processing/lux_harmo.h
        source/processing/lux_centro.c
        source/processing/lux_centro.h
        source/processing/lux_drive.c
        source/processing/lux_drive.h
        source/processing/lux_dcblock.c
        source/processing/lux_dcblock.h
        source/processing/video_scroll.c
        source/processing/video_scroll.h
        source/processing/midi_tap.c
        source/processing/midi_tap.h
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
        ${CMAKE_CURRENT_SOURCE_DIR}/source/synthesis/luxgrain
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
    # (see LICENSE + THIRD-PARTY-NOTICES.md). This JUCE version does not ship a
    # splash screen at all, so JUCE_DISPLAY_SPLASH_SCREEN is intentionally not
    # defined — defining it (even to 0) only triggers a "flag is ignored"
    # compiler warning.
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
    )
endif()

# VOICE module — embedded voice bundles. Each listed Piper voice is fetched at
# configure time and baked into Contents/Resources/piper_voices of EVERY plugin
# format (~79 MB per voice per format!), so the VOICE module works with no
# external folder. Extra voices can still be added at runtime via the page's
# user-selectable external folder.
# ⚠ Embedded voices are REDISTRIBUTED with the binary: check each bundle's
#   MODEL_CARD license first (defaults: siwis = CC-BY 4.0, ljspeech = public
#   domain — see THIRD-PARTY-NOTICES.md). e.g. lessac (Blizzard licence) must
#   NOT be embedded.
set(SP3CTRA_EMBED_VOICES
    "vits-piper-fr_FR-siwis-medium;vits-piper-en_US-ljspeech-medium"
    CACHE STRING
    "Piper voices baked into the plugin bundles (semicolon list; empty = none)")

# Pinned hashes for the default bundles (others fetch unpinned).
set(SP3CTRA_VOICE_SHA_vits-piper-fr_FR-siwis-medium
    375909aa30842b3a4efa10b1beb1d761af792960ae6873b4d53889f96c66195b)
set(SP3CTRA_VOICE_SHA_vits-piper-en_US-ljspeech-medium
    3dfb4b759d8be032a4903a9538d128b0fda2a06ab1de6cbc2d93a97e2dd83dba)

if((APPLE OR WIN32) AND SP3CTRA_ENABLE_TTS AND SP3CTRA_EMBED_VOICES)
    set(SP3CTRA_EMBEDDED_VOICE_FILES "")
    set(SP3CTRA_VOICE_NAMES "")
    set(SP3CTRA_VOICE_DIRS "")
    foreach(_voice IN LISTS SP3CTRA_EMBED_VOICES)
        string(TOLOWER "${_voice}" _vname)
        string(MAKE_C_IDENTIFIER "${_vname}" _vname)
        set(_vargs URL "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/${_voice}.tar.bz2"
                   DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        if(DEFINED SP3CTRA_VOICE_SHA_${_voice})
            list(APPEND _vargs URL_HASH SHA256=${SP3CTRA_VOICE_SHA_${_voice}})
        endif()
        FetchContent_Declare(${_vname} ${_vargs})
        FetchContent_MakeAvailable(${_vname})
        list(APPEND SP3CTRA_VOICE_NAMES "${_voice}")
        list(APPEND SP3CTRA_VOICE_DIRS "${${_vname}_SOURCE_DIR}")

        if(APPLE)
            # Preserve the bundle's tree under Resources/piper_voices/<bundle>/.
            file(GLOB_RECURSE _vfiles "${${_vname}_SOURCE_DIR}/*")
            foreach(_f IN LISTS _vfiles)
                file(RELATIVE_PATH _rel "${${_vname}_SOURCE_DIR}" "${_f}")
                get_filename_component(_reldir "${_rel}" DIRECTORY)
                set_source_files_properties(${_f} PROPERTIES
                    MACOSX_PACKAGE_LOCATION "Resources/piper_voices/${_voice}/${_reldir}")
                list(APPEND SP3CTRA_EMBEDDED_VOICE_FILES ${_f})
            endforeach()
        endif()
    endforeach()

    if(APPLE)
        # Resources belong to the per-format BUNDLE targets, not the shared code lib.
        foreach(_fmt Standalone VST3 AU)
            if(TARGET Sp3ctraVST_${_fmt})
                target_sources(Sp3ctraVST_${_fmt} PRIVATE ${SP3CTRA_EMBEDDED_VOICE_FILES})
            endif()
        endforeach()
    else()
        # Windows has no bundle machinery: copy each voice next to the binary
        # (Resources/piper_voices/<bundle>), where PiperTts looks on Windows.
        # For VST3 this lands inside the .vst3 folder → self-contained plugin.
        foreach(_fmt Standalone VST3)
            if(TARGET Sp3ctraVST_${_fmt})
                list(LENGTH SP3CTRA_VOICE_NAMES _nvoices)
                math(EXPR _last "${_nvoices} - 1")
                foreach(_i RANGE ${_last})
                    list(GET SP3CTRA_VOICE_NAMES ${_i} _voice)
                    list(GET SP3CTRA_VOICE_DIRS ${_i} _vdir)
                    add_custom_command(TARGET Sp3ctraVST_${_fmt} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_directory
                            "${_vdir}"
                            "$<TARGET_FILE_DIR:Sp3ctraVST_${_fmt}>/Resources/piper_voices/${_voice}"
                        COMMENT "Embedding Piper voice ${_voice} (${_fmt})")
                endforeach()
            endif()
        endforeach()
    endif()
endif()

# VOICE module — sherpa-onnx static libs (link order from the official .pc;
# libkissfft-float members only load for symbols the in-tree KissFFT doesn't
# define — same float ABI, so either resolution is correct).
if((APPLE OR WIN32) AND SP3CTRA_ENABLE_TTS)
    target_include_directories(Sp3ctraVST PRIVATE ${SP3CTRA_SHERPA_INCLUDE})
    # Absolute paths: plain -l names don't propagate their -L dir from the
    # shared-code static lib to the Standalone/VST3/AU link steps.
    set(SP3CTRA_SHERPA_LIBDIR ${sherpa_onnx_libs_SOURCE_DIR}/lib)
    if(APPLE)
        set(_sl_prefix ${SP3CTRA_SHERPA_LIBDIR}/lib)
        set(_sl_suffix .a)
    else()
        set(_sl_prefix ${SP3CTRA_SHERPA_LIBDIR}/)
        set(_sl_suffix .lib)
    endif()
    target_link_libraries(Sp3ctraVST PRIVATE
        ${_sl_prefix}sherpa-onnx-c-api${_sl_suffix}
        ${_sl_prefix}sherpa-onnx-core${_sl_suffix}
        ${_sl_prefix}kaldi-decoder-core${_sl_suffix}
        ${_sl_prefix}sherpa-onnx-kaldifst-core${_sl_suffix}
        ${_sl_prefix}sherpa-onnx-fstfar${_sl_suffix}
        ${_sl_prefix}sherpa-onnx-fst${_sl_suffix}
        ${_sl_prefix}kaldi-native-fbank-core${_sl_suffix}
        ${_sl_prefix}kissfft-float${_sl_suffix}
        ${_sl_prefix}piper_phonemize${_sl_suffix}
        ${_sl_prefix}espeak-ng${_sl_suffix}
        ${_sl_prefix}ucd${_sl_suffix}
        ${_sl_prefix}onnxruntime${_sl_suffix}
        ${_sl_prefix}ssentencepiece_core${_sl_suffix}
    )
    target_compile_definitions(Sp3ctraVST PRIVATE SP3CTRA_HAS_TTS=1)
endif()

# Standard settings
target_compile_features(Sp3ctraVST PRIVATE cxx_std_17 c_std_11)

# ===== PROFIL RELEASE (OPTIMIZED FOR PRODUCTION) =====
# RT-SAFE OPTIMIZATION: Preserve memory ordering for atomics and barriers
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        # /fp:precise (IEEE754) — NOT /fp:fast: same RT-SAFE policy as the
        # non-MSVC branch below (“NO -ffast-math”). fast-math assumes NaN/Inf
        # don't exist (UB in JUCE's number parsing — the -Wnan-infinity-disabled
        # flood) and can change DSP results vs macOS.
        target_compile_options(Sp3ctraVST PRIVATE /O2 /fp:precise /W4)
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
