FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# The host's sources, compiled into plain console programs so they can be
# exercised without Live, hosting Apple's built-in AUs. One program per host
# type, because the effect/instrument choice is made at compile time.
function(claude_host_tests target is_synth product_name test_prefix)
    juce_add_console_app(${target} PRODUCT_NAME "${target}")

    target_sources(${target} PRIVATE TestMain.cpp ${ARGN} ${CLAUDE_HOST_SOURCES})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/Source)

    target_compile_definitions(${target} PRIVATE
        ${CLAUDE_HOST_DEFINITIONS}
        CLAUDE_HOST_IS_SYNTH=${is_synth}
        JUCE_MODAL_LOOPS_PERMITTED=1
        JucePlugin_Name="${product_name}"
        JucePlugin_VersionString="0.0.0-test")

    target_link_libraries(${target}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            Catch2::Catch2
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    catch_discover_tests(${target} DISCOVERY_MODE PRE_TEST TEST_PREFIX "${test_prefix}")
endfunction()

claude_host_tests(ClaudeHostTests 0 "Claude Host FX" ""
    ControlServerTests.cpp
    ParameterTextTests.cpp
    RoutingTests.cpp
    InstanceRegistryTests.cpp
    HostProcessorTests.cpp
    FabFilterTests.cpp)

claude_host_tests(ClaudeHostInstTests 1 "Claude Host Instrument" "instrument: "
    InstrumentHostTests.cpp
    InstrumentRoutingTests.cpp
    SerumTests.cpp)

# Any Python with pytest. CI points this at a venv, because Homebrew's
# Python refuses pip installs (PEP 668).
set(CLAUDE_HOST_PYTHON "python3" CACHE STRING "Python interpreter with pytest, for the client tests")

add_test(NAME python_client
         COMMAND ${CLAUDE_HOST_PYTHON} -m pytest -q -p no:cacheprovider ${CMAKE_CURRENT_SOURCE_DIR}/test_host_ctl.py)

# The Claude Bridge Remote Script: commands against fake Live objects, its
# socket server, and tools/bridge_ctl.py.
add_test(NAME python_bridge
         COMMAND ${CLAUDE_HOST_PYTHON} -m pytest -q -p no:cacheprovider ${CMAKE_CURRENT_SOURCE_DIR}/bridge)

# Needs the plugin built and installed (cmake --build build --target ClaudeHostFX_All).
# Run with: ctest --test-dir build -L validation
add_test(NAME plugin_validation
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/validate_plugin.sh ${CMAKE_BINARY_DIR})
set_tests_properties(plugin_validation PROPERTIES LABELS validation TIMEOUT 900)
