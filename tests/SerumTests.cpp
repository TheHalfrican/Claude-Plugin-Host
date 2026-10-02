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

TEST_CASE ("Serum's switches and menus are set by their names", "[.][serum]")
{
    HostProcessor host;
    host.prepareToPlay (sampleRate, blockSize);
    REQUIRE (isOk (run (host, R"({"cmd":"load","name":"Serum"})")));

    // Serum doesn't mark these as discrete, which is what first broke
    // setting them by name in Live.
    struct Case { const char* param; const char* text; };
    const Case cases[] = {
        { "Filter On",  "on"        },
        { "Osc S On",   "on"        },
        { "Fil Type",   "MG Low 24" },
        { "Fil Type",   "MG Low 12" },
        { "LFO1Rate",   "1/8"       },
        { "SubOscShape","Sine"      },
        { "Env1 Sus",   "-9.0"      },   // display starts at "-∞": once refused
        { "Fil Cutoff", "600"       },   // rounded display: once came out "599"
        { "Filter On",  "off"       },
    };

    for (const auto& c : cases)
    {
        const auto reply = run (host, R"({"cmd":"set","param":")" + juce::String (c.param) + R"(","text":")" + c.text + R"("})");
        INFO (c.param << " = " << c.text << ": " << juce::JSON::toString (reply, true));
        REQUIRE (isOk (reply));
        CHECK (reply.getProperty ("param", {}).getProperty ("text", {}).toString().equalsIgnoreCase (c.text));
    }
}

TEST_CASE ("VST3 plugins are listed by name - not by file path", "[.][serum]")
{
    HostProcessor host;
    const auto reply = run (host, R"({"cmd":"list_plugins","query":"serum"})");
    REQUIRE (isOk (reply));

    bool sawVst3 = false;

    for (const auto& p : *reply.getProperty ("plugins", {}).getArray())
    {
        const auto name = p.getProperty ("name", {}).toString();
        INFO (name << " (" << p.getProperty ("format", {}).toString() << ")");
        CHECK_FALSE (name.startsWithChar ('/'));
        CHECK_FALSE (name.endsWith (".vst3"));
        sawVst3 = sawVst3 || p.getProperty ("format", {}).toString() == "VST3";
    }

    CHECK (sawVst3);

    // With an AU and a VST3 of the same name, loading still prefers the AU.
    const auto loaded = run (host, R"({"cmd":"load","name":"Serum"})");
    REQUIRE (isOk (loaded));
    CHECK (loaded.getProperty ("plugin", {}).getProperty ("format", {}).toString() == "AudioUnit");
}
