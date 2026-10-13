# =============================================================================
#  TWISTED BREAKS — jungle / drum & bass performance sampler
#  CMake build for JUCE 8 (VST3 + AU on macOS, VST3 on Windows/Linux)
# =============================================================================
cmake_minimum_required(VERSION 3.22)

project(TwistedBreaks VERSION 0.5.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Apple Silicon first, but build universal binaries so Rosetta hosts still work.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

option(TB_BUILD_TESTS        "Build the headless test runner"                      ON)
option(TB_COPY_AFTER_BUILD   "Copy plugins into ~/Library/Audio/Plug-Ins after build" OFF)
option(TB_MINIMAL_LINUX_DEPS "Linux CI only: disable optional X11/ALSA features"    OFF)
set(TB_JUCE_PATH "" CACHE PATH "Path to a local JUCE checkout (leave empty to download JUCE 8.0.4)")

# -----------------------------------------------------------------------------
#  JUCE dependency
# -----------------------------------------------------------------------------
if(TB_JUCE_PATH)
    add_subdirectory(${TB_JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# -----------------------------------------------------------------------------
#  Plugin target
# -----------------------------------------------------------------------------
set(TB_FORMATS VST3)
if(APPLE)
    list(APPEND TB_FORMATS AU)
endif()

juce_add_plugin(TwistedBreaks
    PRODUCT_NAME                "TwistedBreaks"
    COMPANY_NAME                "Twisted Genetics"
    BUNDLE_ID                   "com.twistedgenetics.twistedbreaks"
    PLUGIN_MANUFACTURER_CODE    Twgn
    PLUGIN_CODE                 Twbk
    FORMATS                     ${TB_FORMATS}
    IS_SYNTH                    TRUE        # an instrument: Ableton shows it under Instruments
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             Instrument Sampler Drum
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD     ${TB_COPY_AFTER_BUILD})

juce_generate_juce_header(TwistedBreaks)

# Source list lives in its own file so each phase only touches one list.
include(cmake/Sources.cmake)
target_sources(TwistedBreaks PRIVATE ${TB_PLUGIN_SOURCES})
target_include_directories(TwistedBreaks PRIVATE Source)

set(TB_JUCE_DEFINES
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

if(TB_MINIMAL_LINUX_DEPS)
    list(APPEND TB_JUCE_DEFINES
        JUCE_ALSA=0 JUCE_JACK=0
        JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XSHM=0
        JUCE_USE_XRENDER=0 JUCE_USE_XCURSOR=0)
endif()

target_compile_definitions(TwistedBreaks PUBLIC ${TB_JUCE_DEFINES})

# UI strings contain Cyrillic; make MSVC read sources as UTF-8 (clang/gcc already do).
if(MSVC)
    target_compile_options(TwistedBreaks PRIVATE /utf-8)
endif()

target_link_libraries(TwistedBreaks
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# -----------------------------------------------------------------------------
#  Headless test runner (no DAW needed): exercises the processor with fake
#  host transport + MIDI, and renders the editor to a PNG for skin review.
# -----------------------------------------------------------------------------
if(TB_BUILD_TESTS)
    juce_add_console_app(TwistedBreaksTests PRODUCT_NAME "TwistedBreaksTests")
    target_sources(TwistedBreaksTests PRIVATE Tests/TestMain.cpp ${TB_PLUGIN_SOURCES})
    target_include_directories(TwistedBreaksTests PRIVATE Source)
    target_compile_definitions(TwistedBreaksTests PRIVATE
        ${TB_JUCE_DEFINES}
        JucePlugin_Name="TwistedBreaks"
        JucePlugin_IsSynth=1
        JucePlugin_WantsMidiInput=1
        JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0
        JucePlugin_VersionString="${PROJECT_VERSION}"
        JUCE_MODAL_LOOPS_PERMITTED=1
        TB_TEST_BUILD=1)
    if(MSVC)
        target_compile_options(TwistedBreaksTests PRIVATE /utf-8)
    endif()
    target_link_libraries(TwistedBreaksTests PRIVATE
        juce::juce_audio_utils juce::juce_audio_formats juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    juce_generate_juce_header(TwistedBreaksTests)
endif()

# -----------------------------------------------------------------------------
#  Host round-trip test: loads the BUILT VST3 through JUCE's VST3 hosting, the
#  same boundary Ableton uses (state save/restore, host parameter changes).
# -----------------------------------------------------------------------------
if(TB_BUILD_TESTS)
    juce_add_console_app(TwistedBreaksHostTest PRODUCT_NAME "TwistedBreaksHostTest")
    target_sources(TwistedBreaksHostTest PRIVATE Tests/HostTest.cpp)
    target_compile_definitions(TwistedBreaksHostTest PRIVATE
        ${TB_JUCE_DEFINES} JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1
        $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=1>)
    target_link_libraries(TwistedBreaksHostTest PRIVATE
        juce::juce_audio_utils juce::juce_audio_processors
        juce::juce_recommended_config_flags)
    juce_generate_juce_header(TwistedBreaksHostTest)
    add_dependencies(TwistedBreaksHostTest TwistedBreaks_VST3)
endif()
