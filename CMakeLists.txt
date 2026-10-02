cmake_minimum_required(VERSION 3.22)

project(ClaudePluginHost VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "" FORCE)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# Shared by the plugins and the test program.
set(CLAUDE_HOST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/HostProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/HostEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/ControlServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/InstanceRegistry.cpp)

set(CLAUDE_HOST_DEFINITIONS
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# One codebase, two plugins: an effect host (phase 1) and, later, an
# instrument host. macOS registers effects and instruments as different
# Audio Unit types, so each needs its own bundle.
function(claude_host_plugin target product_name plugin_code is_synth)
    juce_add_plugin(${target}
        COMPANY_NAME             "Claude"
        BUNDLE_ID                "com.thehalfrican.${target}"
        PLUGIN_MANUFACTURER_CODE Clde
        PLUGIN_CODE              ${plugin_code}
        PRODUCT_NAME             "${product_name}"
        FORMATS                  AU VST3
        IS_SYNTH                 ${is_synth}
        NEEDS_MIDI_INPUT         ${is_synth}
        NEEDS_MIDI_OUTPUT        FALSE
        IS_MIDI_EFFECT           FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD  TRUE)

    target_sources(${target} PRIVATE ${CLAUDE_HOST_SOURCES})

    target_compile_definitions(${target} PUBLIC
        CLAUDE_HOST_IS_SYNTH=$<BOOL:${is_synth}>
        ${CLAUDE_HOST_DEFINITIONS})

    target_link_libraries(${target}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endfunction()

claude_host_plugin(ClaudeHostFX   "Claude Host FX"         Chfx FALSE)
claude_host_plugin(ClaudeHostInst "Claude Host Instrument" Chin TRUE)

option(CLAUDE_HOST_BUILD_TESTS "Build the test suite" ON)

if (CLAUDE_HOST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
