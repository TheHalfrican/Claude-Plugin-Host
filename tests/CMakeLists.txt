FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# The host's sources, compiled into a plain console program so they can be
# exercised without Live: the effect build, hosting Apple's built-in AUs.
juce_add_console_app(ClaudeHostTests PRODUCT_NAME "ClaudeHostTests")

target_sources(ClaudeHostTests PRIVATE
    TestMain.cpp
    ControlServerTests.cpp
    InstanceRegistryTests.cpp
    HostProcessorTests.cpp
    FabFilterTests.cpp
    ${CLAUDE_HOST_SOURCES})

target_include_directories(ClaudeHostTests PRIVATE ${CMAKE_SOURCE_DIR}/Source)

target_compile_definitions(ClaudeHostTests PRIVATE
    ${CLAUDE_HOST_DEFINITIONS}
    CLAUDE_HOST_IS_SYNTH=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JucePlugin_Name="Claude Host FX"
    JucePlugin_VersionString="0.0.0-test")

target_link_libraries(ClaudeHostTests
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        Catch2::Catch2
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

catch_discover_tests(ClaudeHostTests DISCOVERY_MODE PRE_TEST)

# Any Python with pytest. CI points this at a venv, because Homebrew's
# Python refuses pip installs (PEP 668).
set(CLAUDE_HOST_PYTHON "python3" CACHE STRING "Python interpreter with pytest, for the client tests")

add_test(NAME python_client
         COMMAND ${CLAUDE_HOST_PYTHON} -m pytest -q -p no:cacheprovider ${CMAKE_CURRENT_SOURCE_DIR}/test_host_ctl.py)

# Needs the plugin built and installed (cmake --build build --target ClaudeHostFX_All).
# Run with: ctest --test-dir build -L validation
add_test(NAME plugin_validation
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/validate_plugin.sh ${CMAKE_BINARY_DIR})
set_tests_properties(plugin_validation PROPERTIES LABELS validation TIMEOUT 900)
