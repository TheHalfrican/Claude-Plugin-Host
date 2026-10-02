// Tests against the user's own Serum (1). Hidden by default ("[.]") because
// they need Serum installed and authorized; run with:
//   ClaudeHostInstTests "[serum]"
// Serum 2 isn't licensed on this machine, so only Serum 1 is tested.

#include <catch2/catch_test_macros.hpp>

#include "HostProcessor.h"
#include "TestHelpers.h"

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    juce::var run (HostProcessor& host, const juce::String& json)
    {
        auto reply = host.runCommand (test::parse (json));
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30); // as Live keeps it running
        return reply;
    }

    bool isOk (const juce::var& reply) { return (bool) reply.getProperty ("ok", false); }

    float playAndMeasure (HostProcessor& host, int note, int blocks)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        float peak = 0.0f;

        for (int i = 0; i < blocks; ++i)
        {
            juce::MidiBuffer midi;

            if (i == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), 0);

            buffer.clear();
            host.processBlock (buffer, midi);
            peak = juce::jmax (peak, buffer.getMagnitude (0, 0, blockSize));
        }

        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        host.processBlock (buffer, off);
        return peak;
    }
}

TEST_CASE ("Serum loads as an AU instrument and plays", "[.][serum]")
{
    HostProcessor host;
    host.prepareToPlay (sampleRate, blockSize);

    const auto reply = run (host, R"({"cmd":"load","name":"Serum"})");
    INFO (juce::JSON::toString (reply));
    REQUIRE (isOk (reply));

    const auto plugin = reply.getProperty ("plugin", {});
    CHECK (plugin.getProperty ("name", {}).toString() == "Serum");      // not Serum 2
    CHECK (plugin.getProperty ("format", {}).toString() == "AudioUnit");
    CHECK ((int) plugin.getProperty ("numParameters", 0) > 50);

    WARN ("Serum: " << (int) plugin.getProperty ("numParameters", 0) << " parameters, "
                    << (int) plugin.getProperty ("numPrograms", 0) << " programs");

    CHECK (playAndMeasure (host, 48, 20) > 0.01f);
}

TEST_CASE ("Serum's main controls are reachable by name", "[.][serum]")
{
    HostProcessor host;
    host.prepareToPlay (sampleRate, blockSize);
    REQUIRE (isOk (run (host, R"({"cmd":"load","name":"Serum"})")));

    // Print the names that matter for sound design, to record in the skill.
    juce::StringArray found;

    for (auto filter : { "Fil", "Env", "Macro", "Osc", "A Vol", "B Vol", "Master", "LFO" })
    {
        const auto reply = run (host, R"({"cmd":"params","filter":")" + juce::String (filter) + R"("})");

        for (const auto& p : *reply.getProperty ("params", {}).getArray())
            found.addIfNotAlreadyThere (p.getProperty ("name", {}).toString() + " = " + p.getProperty ("text", {}).toString());
    }

    WARN ("Serum parameters:\n" << found.joinIntoString ("\n"));
    CHECK (found.size() > 10);
}
