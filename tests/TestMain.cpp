#include <catch2/catch_session.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdlib>
#include <unistd.h>

int main (int argc, char* argv[])
{
    // Each run gets its own registry folder, so tests never see (or disturb)
    // instances running inside Live. Must be set before any host is created.
    const auto registry = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("ClaudeHostTests-registry-" + juce::String ((int) ::getpid()));
    ::setenv ("CLAUDE_HOST_REGISTRY_DIR", registry.getFullPathName().toRawUTF8(), 1);

    // Makes this (main) thread JUCE's message thread, as it is inside a host.
    juce::ScopedJuceInitialiser_GUI juce;

    const auto result = Catch::Session().run (argc, argv);

    registry.deleteRecursively();
    return result;
}
