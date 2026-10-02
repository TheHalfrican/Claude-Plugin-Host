// Integration tests for the host, run without Live. They host Apple's built-in
// Audio Units (AULowpass, AUDelay), which exist on every Mac, so results are
// repeatable on any machine, CI included.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "HostProcessor.h"
#include "InstanceRegistry.h"
#include "TestHelpers.h"

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    juce::var run (HostProcessor& host, const juce::String& json)
    {
        return host.runCommand (test::parse (json));
    }

    bool isOk (const juce::var& reply)  { return (bool) reply.getProperty ("ok", false); }
    juce::String error (const juce::var& reply) { return reply.getProperty ("error", {}).toString(); }

    std::unique_ptr<HostProcessor> makeHost()
    {
        auto host = std::make_unique<HostProcessor>();
        host->prepareToPlay (sampleRate, blockSize);
        return host;
    }

    std::unique_ptr<HostProcessor> makeHostWith (const juce::String& plugin)
    {
        auto host = makeHost();
        const auto reply = run (*host, R"({"cmd":"load","name":")" + plugin + R"("})");
        INFO (juce::JSON::toString (reply));
        REQUIRE (isOk (reply));
        return host;
    }

    juce::var param (HostProcessor& host, const juce::String& name)
    {
        return run (host, R"({"cmd":"get","param":")" + name + R"("})").getProperty ("param", {});
    }

    // Renders `blocks` blocks of a sine through the host; returns the RMS of
    // the last block (after any filter has settled) and of its input.
    std::pair<float, float> renderSine (HostProcessor& host, double frequency, int blocks = 20)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        double phase = 0.0;
        float inputRms = 0.0f;

        for (int i = 0; i < blocks; ++i)
        {
            test::fillSine (buffer, frequency, sampleRate, phase);
            inputRms = test::rms (buffer);
            host.processBlock (buffer, midi);
        }

        return { test::rms (buffer), inputRms };
    }

    juce::MemoryBlock saveState (HostProcessor& host)
    {
        juce::MemoryBlock state;
        host.getStateInformation (state);
        return state;
    }
}

//==============================================================================
TEST_CASE ("An empty host reports no plugin and refuses plugin commands", "[host]")
{
    auto host = makeHost();

    const auto info = run (*host, R"({"cmd":"info"})");
    REQUIRE (isOk (info));
    CHECK (info.getProperty ("product", {}).toString() == "Claude Host FX");
    CHECK_FALSE (info.hasProperty ("plugin"));

    for (auto cmd : { "params", "get", "set", "programs", "set_program" })
    {
        const auto reply = run (*host, R"({"cmd":")" + juce::String (cmd) + R"("})");
        CHECK_FALSE (isOk (reply));
        CHECK (error (reply).contains ("no plugin loaded"));
    }
}

TEST_CASE ("Unknown commands are rejected with the list of valid ones", "[host]")
{
    auto host = makeHostWith ("AULowpass");
    const auto reply = run (*host, R"({"cmd":"explode"})");
    CHECK_FALSE (isOk (reply));
    CHECK (error (reply).contains ("unknown cmd"));
    CHECK (error (reply).contains ("set_program"));
}

TEST_CASE ("list_plugins finds Apple's AUs and never offers the host itself", "[host][load]")
{
    auto host = makeHost();
    const auto reply = run (*host, R"({"cmd":"list_plugins"})");
    REQUIRE (isOk (reply));

    bool sawLowpass = false;

    for (const auto& p : *reply.getProperty ("plugins", {}).getArray())
    {
        const auto name = p.getProperty ("name", {}).toString();
        CHECK_FALSE (name.containsIgnoreCase ("Claude Host"));

        if (name == "AULowpass" && p.getProperty ("format", {}).toString() == "AudioUnit")
            sawLowpass = true;
    }

    CHECK (sawLowpass);

    const auto filtered = run (*host, R"({"cmd":"list_plugins","query":"lowpass"})");
    for (const auto& p : *filtered.getProperty ("plugins", {}).getArray())
        CHECK (p.getProperty ("name", {}).toString().containsIgnoreCase ("lowpass"));
}

TEST_CASE ("The effect host offers effects - not instruments", "[host][load]")
{
    auto host = makeHost();
    const auto reply = run (*host, R"({"cmd":"list_plugins"})");

    for (const auto& p : *reply.getProperty ("plugins", {}).getArray())
        if (p.getProperty ("format", {}).toString() == "AudioUnit")
            CHECK (p.getProperty ("name", {}).toString() != "DLSMusicDevice");

    CHECK_FALSE (isOk (run (*host, R"({"cmd":"load","name":"DLSMusicDevice"})")));
}

TEST_CASE ("load matches names exactly - then by unique substring", "[host][load]")
{
    auto host = makeHost();

    SECTION ("exact name, any case")
    {
        const auto reply = run (*host, R"({"cmd":"load","name":"aulowpass"})");
        REQUIRE (isOk (reply));
        CHECK (reply.getProperty ("plugin", {}).getProperty ("name", {}).toString() == "AULowpass");
        CHECK (reply.getProperty ("plugin", {}).getProperty ("format", {}).toString() == "AudioUnit");
    }

    SECTION ("unique substring")
    {
        const auto reply = run (*host, R"({"cmd":"load","name":"AUDela"})");
        REQUIRE (isOk (reply));
        CHECK (reply.getProperty ("plugin", {}).getProperty ("name", {}).toString() == "AUDelay");
    }

    SECTION ("ambiguous substring lists the candidates and loads nothing")
    {
        const auto reply = run (*host, R"({"cmd":"load","name":"AU"})");
        CHECK_FALSE (isOk (reply));
        CHECK (error (reply).contains ("matches several plugins"));
        CHECK (host->getInnerPlugin() == nullptr);
    }

    SECTION ("no match")
    {
        const auto reply = run (*host, R"({"cmd":"load","name":"Definitely Not A Plugin 123"})");
        CHECK_FALSE (isOk (reply));
        CHECK (error (reply).contains ("no plugin matches"));
    }

    SECTION ("empty name")
    {
        CHECK_FALSE (isOk (run (*host, R"({"cmd":"load","name":"  "})")));
    }

    SECTION ("format filter that excludes the plugin")
    {
        const auto reply = run (*host, R"({"cmd":"load","name":"AULowpass","format":"VST3"})");
        CHECK_FALSE (isOk (reply));
    }
}

TEST_CASE ("Loading a second plugin replaces the first - unload empties the host", "[host][load]")
{
    auto host = makeHostWith ("AULowpass");
    REQUIRE (run (*host, R"({"cmd":"load","name":"AUDelay"})").getProperty ("ok", false));
    CHECK (host->getInnerPlugin()->getName() == "AUDelay");

    const auto reply = run (*host, R"({"cmd":"unload"})");
    REQUIRE (isOk (reply));
    CHECK_FALSE (reply.hasProperty ("plugin"));
    CHECK (host->getInnerPlugin() == nullptr);
}

//==============================================================================
TEST_CASE ("params lists every parameter with value - text and type", "[host][params]")
{
    auto host = makeHostWith ("AULowpass");

    const auto all = run (*host, R"({"cmd":"params"})");
    REQUIRE (isOk (all));
    const auto total = (int) all.getProperty ("total", 0);
    REQUIRE (total == (int) host->getInnerPlugin()->getParameters().size());
    REQUIRE (all.getProperty ("params", {}).getArray()->size() == total);

    const auto filtered = run (*host, R"({"cmd":"params","filter":"cutoff"})");
    const auto& items = *filtered.getProperty ("params", {}).getArray();
    REQUIRE (items.size() == 1);

    const auto& cutoff = items.getReference (0);
    CHECK (cutoff.getProperty ("name", {}).toString() == "Cutoff Frequency");
    CHECK (cutoff.getProperty ("type", {}).toString() == "continuous");
    CHECK ((double) cutoff.getProperty ("value", -1.0) >= 0.0);
    CHECK ((double) cutoff.getProperty ("value", 2.0) <= 1.0);
    CHECK (cutoff.getProperty ("text", {}).toString().isNotEmpty());
}

TEST_CASE ("get finds a parameter by exact name - unique substring - or index", "[host][params]")
{
    auto host = makeHostWith ("AULowpass");

    CHECK (param (*host, "Cutoff Frequency").getProperty ("name", {}).toString() == "Cutoff Frequency");
    CHECK (param (*host, "resonance").getProperty ("name", {}).toString() == "Resonance");

    const auto byIndex = run (*host, R"({"cmd":"get","param":0})");
    REQUIRE (isOk (byIndex));
    CHECK ((int) byIndex.getProperty ("param", {}).getProperty ("index", -1) == 0);

    const auto outOfRange = run (*host, R"({"cmd":"get","param":9999})");
    CHECK_FALSE (isOk (outOfRange));
    CHECK (error (outOfRange).contains ("out of range"));

    const auto missing = run (*host, R"({"cmd":"get","param":"wobble"})");
    CHECK_FALSE (isOk (missing));
    CHECK (error (missing).contains ("no parameter named"));
}

TEST_CASE ("An ambiguous parameter name lists the matches", "[host][params]")
{
    auto host = makeHostWith ("AUDelay"); // several params contain "e"
    const auto reply = run (*host, R"({"cmd":"get","param":"e"})");
    CHECK_FALSE (isOk (reply));
    CHECK (error (reply).contains ("matches"));
}

TEST_CASE ("set takes a normalized value and reports what the plugin kept", "[host][params]")
{
    auto host = makeHostWith ("AULowpass");

    const auto reply = run (*host, R"({"cmd":"set","param":"Resonance","value":0.25})");
    REQUIRE (isOk (reply));
    CHECK ((double) reply.getProperty ("param", {}).getProperty ("value", -1.0) == Catch::Approx (0.25).margin (0.01));
    CHECK ((double) param (*host, "Resonance").getProperty ("value", -1.0) == Catch::Approx (0.25).margin (0.01));
}

TEST_CASE ("set takes real units as text", "[host][params]")
{
    auto host = makeHostWith ("AULowpass");

    const auto reply = run (*host, R"({"cmd":"set","param":"Cutoff Frequency","text":"500"})");
    REQUIRE (isOk (reply));

    const auto text = reply.getProperty ("param", {}).getProperty ("text", {}).toString();
    INFO ("plugin shows: " << text);
    CHECK (text.getFloatValue() == Catch::Approx (500.0f).margin (1.0f));
}

TEST_CASE ("Text values understand unit prefixes", "[host][params]")
{
    auto host = makeHostWith ("AULowpass");

    SECTION ("kHz on a parameter that displays Hz")
    {
        const auto reply = run (*host, R"({"cmd":"set","param":"Cutoff Frequency","text":"2.5 kHz"})");
        REQUIRE (isOk (reply));
        CHECK (reply.getProperty ("param", {}).getProperty ("text", {}).toString().getFloatValue() == Catch::Approx (2500.0f).margin (25.0f));
    }

    SECTION ("Hz written out")
    {
        const auto reply = run (*host, R"({"cmd":"set","param":"Cutoff Frequency","text":"1200 Hz"})");
        REQUIRE (isOk (reply));
        CHECK (reply.getProperty ("param", {}).getProperty ("text", {}).toString().getFloatValue() == Catch::Approx (1200.0f).margin (12.0f));
    }
}

TEST_CASE ("Text values convert milliseconds for a parameter shown in seconds", "[host][params]")
{
    auto host = makeHostWith ("AUDelay");
    const auto label = param (*host, "Delay Time").getProperty ("label", {}).toString();
    INFO ("Delay Time label: " << label);

    const auto reply = run (*host, R"({"cmd":"set","param":"Delay Time","text":"250 ms"})");
    REQUIRE (isOk (reply));
    const auto shown = reply.getProperty ("param", {}).getProperty ("text", {}).toString().getDoubleValue();
    CHECK (shown == Catch::Approx (label.startsWithIgnoreCase ("s") ? 0.25 : 250.0).epsilon (0.02));
}

TEST_CASE ("Text values outside the range or not understood are refused", "[host][params]")
{
    auto host = makeHostWith ("AULowpass");
    const auto before = param (*host, "Cutoff Frequency").getProperty ("value", -1.0);

    const auto tooHigh = run (*host, R"({"cmd":"set","param":"Cutoff Frequency","text":"90 kHz"})");
    CHECK_FALSE (isOk (tooHigh));
    CHECK (error (tooHigh).contains ("outside"));

    const auto nonsense = run (*host, R"({"cmd":"set","param":"Cutoff Frequency","text":"banana"})");
    CHECK_FALSE (isOk (nonsense));

    // A refused request leaves the parameter alone.
    CHECK ((double) param (*host, "Cutoff Frequency").getProperty ("value", -2.0) == Catch::Approx ((double) before));
}

TEST_CASE ("set_many applies changes in order and reports each one", "[host][params]")
{
    auto host = makeHostWith ("AULowpass");

    const auto reply = run (*host, R"({"cmd":"set_many","changes":[
        {"param":"Cutoff Frequency","text":"800"},
        {"param":"Resonance","value":0.4},
        {"param":"No Such Param","value":0.5},
        {"param":"Cutoff Frequency","text":"1200"}]})");
    REQUIRE (isOk (reply));
    CHECK_FALSE ((bool) reply.getProperty ("allOk", true));

    const auto& results = *reply.getProperty ("results", {}).getArray();
    REQUIRE (results.size() == 4);
    CHECK (isOk (results[0]));
    CHECK (isOk (results[1]));
    CHECK_FALSE (isOk (results[2]));                 // reported, not fatal
    CHECK (isOk (results[3]));

    // Later changes win: applied in order.
    CHECK (param (*host, "Cutoff Frequency").getProperty ("text", {}).toString().getFloatValue() == Catch::Approx (1200.0f).margin (12.0f));
    CHECK ((double) param (*host, "Resonance").getProperty ("value", -1.0) == Catch::Approx (0.4).margin (0.01));
}

TEST_CASE ("set_many needs a list of changes", "[host][params]")
{
    auto host = makeHostWith ("AULowpass");
    CHECK_FALSE (isOk (run (*host, R"({"cmd":"set_many"})")));
    CHECK_FALSE (isOk (run (*host, R"({"cmd":"set_many","changes":[]})")));

    const auto notObject = run (*host, R"({"cmd":"set_many","changes":[42]})");
    REQUIRE (isOk (notObject));
    CHECK_FALSE ((bool) notObject.getProperty ("allOk", true));
}

TEST_CASE ("set rejects out-of-range or missing values", "[host][params]")
{
    auto host = makeHostWith ("AULowpass");

    const auto tooBig = run (*host, R"({"cmd":"set","param":"Resonance","value":1.5})");
    CHECK_FALSE (isOk (tooBig));
    CHECK (error (tooBig).contains ("normalized"));

    CHECK_FALSE (isOk (run (*host, R"({"cmd":"set","param":"Resonance","value":-0.1})")));

    const auto nothing = run (*host, R"({"cmd":"set","param":"Resonance"})");
    CHECK_FALSE (isOk (nothing));
    CHECK (error (nothing).contains ("value"));
}

TEST_CASE ("programs and set_program report and validate factory presets", "[host][params]")
{
    auto host = makeHostWith ("AUDelay");

    const auto programs = run (*host, R"({"cmd":"programs"})");
    REQUIRE (isOk (programs));
    const auto count = programs.getProperty ("programs", {}).getArray()->size();
    CHECK (count == host->getInnerPlugin()->getNumPrograms());

    CHECK_FALSE (isOk (run (*host, R"({"cmd":"set_program","index":-1})")));
    CHECK_FALSE (isOk (run (*host, R"({"cmd":"set_program","index":)" + juce::String (count) + "}")));

    if (count > 1)
    {
        const auto reply = run (*host, R"({"cmd":"set_program","index":1})");
        REQUIRE (isOk (reply));
        CHECK ((int) reply.getProperty ("current", -1) == 1);
    }
}

//==============================================================================
TEST_CASE ("An empty host passes audio through untouched", "[host][audio]")
{
    auto host = makeHost();

    juce::AudioBuffer<float> buffer (2, blockSize), original (2, blockSize);
    double phase = 0.0;
    test::fillSine (buffer, 440.0, sampleRate, phase);
    original.makeCopyOf (buffer);

    juce::MidiBuffer midi;
    host->processBlock (buffer, midi);

    // Bit-for-bit identical, not just close.
    for (int ch = 0; ch < 2; ++ch)
        REQUIRE (std::memcmp (buffer.getReadPointer (ch), original.getReadPointer (ch),
                              sizeof (float) * (size_t) blockSize) == 0);
}

TEST_CASE ("Audio really goes through the hosted plugin", "[host][audio]")
{
    auto host = makeHostWith ("AULowpass");

    SECTION ("a 200 Hz low-pass removes a 10 kHz tone")
    {
        REQUIRE (isOk (run (*host, R"({"cmd":"set","param":"Cutoff Frequency","text":"200"})")));
        const auto [out, in] = renderSine (*host, 10000.0);
        INFO ("in " << in << " out " << out);
        CHECK (out < in * 0.05f);
    }

    SECTION ("a fully open low-pass keeps a 1 kHz tone")
    {
        REQUIRE (isOk (run (*host, R"({"cmd":"set","param":"Cutoff Frequency","value":1.0})")));
        const auto [out, in] = renderSine (*host, 1000.0);
        INFO ("in " << in << " out " << out);
        CHECK (out > in * 0.7f);
    }
}

TEST_CASE ("Processing a block larger than prepared doesn't crash", "[host][audio]")
{
    auto host = makeHostWith ("AULowpass");
    juce::AudioBuffer<float> big (2, blockSize * 4);
    big.clear();
    juce::MidiBuffer midi;
    host->processBlock (big, midi);
    SUCCEED();
}

//==============================================================================
TEST_CASE ("State round-trips the plugin and its settings", "[host][state]")
{
    auto original = makeHostWith ("AULowpass");
    REQUIRE (isOk (run (*original, R"({"cmd":"set","param":"Cutoff Frequency","text":"750"})")));
    REQUIRE (isOk (run (*original, R"({"cmd":"set","param":"Resonance","value":0.6})")));

    const auto savedCutoff = param (*original, "Cutoff Frequency").getProperty ("text", {}).toString();
    const auto savedResonance = (double) param (*original, "Resonance").getProperty ("value", -1.0);
    const auto state = saveState (*original);

    auto restored = makeHost();
    restored->setStateInformation (state.getData(), (int) state.getSize());

    REQUIRE (restored->getInnerPlugin() != nullptr);
    CHECK (restored->getInnerPlugin()->getName() == "AULowpass");
    CHECK (param (*restored, "Cutoff Frequency").getProperty ("text", {}).toString() == savedCutoff);
    CHECK ((double) param (*restored, "Resonance").getProperty ("value", -1.0) == Catch::Approx (savedResonance).margin (0.001));
}

TEST_CASE ("State of an empty host restores to an empty host", "[host][state]")
{
    auto empty = makeHost();
    const auto state = saveState (*empty);

    auto restored = makeHostWith ("AULowpass");
    restored->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (restored->getInnerPlugin() == nullptr);
}

TEST_CASE ("Garbage state is ignored", "[host][state]")
{
    auto host = makeHostWith ("AULowpass");
    const char junk[] = "definitely not plugin state";
    host->setStateInformation (junk, (int) sizeof (junk));
    CHECK (host->getInnerPlugin() != nullptr); // unchanged
}

//==============================================================================
TEST_CASE ("Every instance gets a tag in range - unique among running instances", "[host][tag]")
{
    auto a = makeHost();
    auto b = makeHost();

    CHECK (a->getTag() >= 1);
    CHECK (a->getTag() <= 9999);
    CHECK (a->getTag() != b->getTag());
}

TEST_CASE ("A duplicated instance re-rolls its tag - a reopened one keeps it", "[host][tag]")
{
    auto original = makeHostWith ("AULowpass");
    const auto tag = original->getTag();
    const auto state = saveState (*original);

    // Like duplicating a track in Live: the original is still running.
    auto duplicate = makeHost();
    duplicate->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (duplicate->getTag() != tag);

    // Like reopening a saved set: the original is gone.
    original.reset();
    auto reopened = makeHost();
    reopened->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (reopened->getTag() == tag);
}

TEST_CASE ("The registry entry tracks port - tag and loaded plugin", "[host][tag]")
{
    auto host = makeHost();

    auto findEntry = [&]() -> juce::var
    {
        for (const auto& f : InstanceRegistry::getDirectory().findChildFiles (juce::File::findFiles, false, "*.json"))
        {
            const auto entry = juce::JSON::parse (f);
            if ((int) entry.getProperty ("tag", -1) == host->getTag())
                return entry;
        }
        return {};
    };

    auto entry = findEntry();
    REQUIRE (entry.isObject());
    CHECK ((int) entry.getProperty ("port", 0) == host->getPort());
    CHECK (entry.getProperty ("plugin", "x").toString().isEmpty());

    REQUIRE (isOk (run (*host, R"({"cmd":"load","name":"AULowpass"})")));
    entry = findEntry();
    CHECK (entry.getProperty ("plugin", {}).toString() == "AULowpass");
}

//==============================================================================
TEST_CASE ("Commands work end to end over the socket", "[host][socket]")
{
    auto host = makeHost();
    const auto port = host->getPort();
    REQUIRE (port > 0);

    // The client runs on another thread; the main (message) thread keeps
    // pumping so the host can run each command there, as it would in Live.
    auto future = std::async (std::launch::async, [port]
    {
        test::Client client (port);
        auto loaded = client.request (R"({"cmd":"load","name":"AULowpass"})", 15000);
        auto set = client.request (R"({"cmd":"set","param":"Cutoff Frequency","text":"300"})", 15000);
        return std::make_pair (loaded, set);
    });

    auto result = test::pumpForFuture (future, 20000);
    REQUIRE (result.has_value());

    auto [loaded, set] = *result;
    REQUIRE (loaded.has_value());
    CHECK (isOk (*loaded));
    REQUIRE (set.has_value());
    CHECK (isOk (*set));
    CHECK (set->getProperty ("param", {}).getProperty ("text", {}).toString().getFloatValue() == Catch::Approx (300.0f).margin (1.0f));
}

TEST_CASE ("Closing the host answers an in-flight request instead of hanging", "[host][socket]")
{
    auto host = makeHost();
    const auto port = host->getPort();

    // Nobody pumps the message thread, so this request can't be served.
    auto future = std::async (std::launch::async, [port]
    {
        test::Client client (port);
        return client.request (R"({"cmd":"info"})", 10000);
    });

    juce::Thread::sleep (200); // let the request arrive
    const auto start = juce::Time::getMillisecondCounter();
    host.reset();
    CHECK (juce::Time::getMillisecondCounter() - start < 2000);

    const auto reply = future.get();
    REQUIRE (reply.has_value());
    CHECK_FALSE (isOk (*reply));
    CHECK (error (*reply).contains ("closing"));

    // Drain the queued callback: it must find the host gone and do nothing.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
}

//==============================================================================
TEST_CASE ("The editor embeds the plugin's editor and survives plugin swaps", "[host][editor]")
{
    auto host = makeHostWith ("AULowpass");

    std::unique_ptr<juce::AudioProcessorEditor> editor (host->createEditor());
    REQUIRE (editor != nullptr);
    CHECK (editor->getWidth() > 0);
    CHECK (editor->getHeight() > 40); // header plus something

    REQUIRE (isOk (run (*host, R"({"cmd":"load","name":"AUDelay"})")));
    REQUIRE (isOk (run (*host, R"({"cmd":"unload"})")));
    CHECK (editor->getHeight() > 0);

    editor.reset(); // must not touch the (now gone) inner plugin
    SUCCEED();
}
