// Integration tests for the instrument host ("Claude Host Instrument"), run
// without Live. They host Apple's DLSMusicDevice, a General MIDI synth that
// ships with every Mac and makes sound with nothing to load.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "HostProcessor.h"
#include "TestHelpers.h"

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;
    constexpr int middleC = 60;

    juce::var run (HostProcessor& host, const juce::String& json)
    {
        return host.runCommand (test::parse (json));
    }

    bool isOk (const juce::var& reply) { return (bool) reply.getProperty ("ok", false); }

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

    // Renders one block with the given MIDI; the buffer starts full of junk,
    // as Live's instrument buffers may.
    juce::AudioBuffer<float> renderBlock (HostProcessor& host, juce::MidiBuffer midi = {})
    {
        juce::AudioBuffer<float> buffer (2, blockSize);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < blockSize; ++i)
                buffer.setSample (ch, i, 0.9f * ((i % 7) - 3) / 3.0f);

        host.processBlock (buffer, midi);
        return buffer;
    }

    float peak (const juce::AudioBuffer<float>& b, int start = 0, int num = -1)
    {
        if (num < 0)
            num = b.getNumSamples() - start;

        return juce::jmax (b.getMagnitude (0, start, num), b.getMagnitude (1, start, num));
    }

    juce::MidiBuffer noteOn (int note, int sampleOffset = 0)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), sampleOffset);
        return m;
    }

    juce::MidiBuffer noteOff (int note, int sampleOffset = 0)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::noteOff (1, note), sampleOffset);
        return m;
    }

    // Peak over the next `blocks` blocks with no new MIDI.
    float sustainPeak (HostProcessor& host, int blocks)
    {
        float p = 0.0f;

        for (int i = 0; i < blocks; ++i)
            p = juce::jmax (p, peak (renderBlock (host)));

        return p;
    }
}

//==============================================================================
TEST_CASE ("The instrument host reports itself as an instrument", "[instrument]")
{
    auto host = makeHost();
    CHECK (host->acceptsMidi());
    CHECK (host->getTotalNumInputChannels() == 0);
    CHECK (host->getTotalNumOutputChannels() == 2);
    CHECK (run (*host, R"({"cmd":"info"})").getProperty ("product", {}).toString() == "Claude Host Instrument");
}

TEST_CASE ("An empty instrument host outputs silence - whatever is in the buffer", "[instrument][audio]")
{
    auto host = makeHost();
    CHECK (peak (renderBlock (*host, noteOn (middleC))) == 0.0f);
}

TEST_CASE ("The instrument host offers instruments - not effects", "[instrument][load]")
{
    auto host = makeHost();
    const auto reply = run (*host, R"({"cmd":"list_plugins"})");
    REQUIRE (isOk (reply));

    juce::StringArray auNames;

    for (const auto& p : *reply.getProperty ("plugins", {}).getArray())
        if (p.getProperty ("format", {}).toString() == "AudioUnit")
            auNames.add (p.getProperty ("name", {}).toString());

    CHECK (auNames.contains ("DLSMusicDevice"));
    CHECK_FALSE (auNames.contains ("AULowpass"));
    CHECK_FALSE (auNames.contains ("AUDelay"));
    CHECK_FALSE (auNames.contains ("Claude Host Instrument"));

    const auto effect = run (*host, R"({"cmd":"load","name":"AULowpass"})");
    CHECK_FALSE (isOk (effect));
}

//==============================================================================
TEST_CASE ("A hosted synth is silent until it gets a note - then plays", "[instrument][audio]")
{
    auto host = makeHostWith ("DLSMusicDevice");

    CHECK (sustainPeak (*host, 4) < 1.0e-4f);           // no MIDI: silence, junk cleared

    renderBlock (*host, noteOn (middleC));
    CHECK (sustainPeak (*host, 8) > 0.01f);             // the note sounds
}

TEST_CASE ("Note timing within a block is kept", "[instrument][audio][midi]")
{
    auto host = makeHostWith ("DLSMusicDevice");
    sustainPeak (*host, 2);

    constexpr int offset = 384;
    auto block = renderBlock (*host, noteOn (middleC, offset));

    // Nothing before the note's sample offset; something after it (in this
    // block or the next, allowing for the synth's attack).
    CHECK (peak (block, 0, offset - 16) < 1.0e-4f);
    const auto after = juce::jmax (peak (block, offset), peak (renderBlock (*host)));
    CHECK (after > 0.001f);
}

TEST_CASE ("Note-off lets the sound die away", "[instrument][audio][midi]")
{
    auto host = makeHostWith ("DLSMusicDevice");

    renderBlock (*host, noteOn (middleC));
    const auto playing = sustainPeak (*host, 8);
    REQUIRE (playing > 0.01f);

    renderBlock (*host, noteOff (middleC));
    sustainPeak (*host, 200);                            // ~2 s of release
    CHECK (sustainPeak (*host, 4) < playing * 0.05f);
}

TEST_CASE ("Unloading the synth mid-note goes straight to silence", "[instrument][audio]")
{
    auto host = makeHostWith ("DLSMusicDevice");
    renderBlock (*host, noteOn (middleC));
    REQUIRE (sustainPeak (*host, 4) > 0.01f);

    REQUIRE (isOk (run (*host, R"({"cmd":"unload"})")));
    CHECK (sustainPeak (*host, 2) == 0.0f);
}

//==============================================================================
TEST_CASE ("The synth's parameters are listed and settable", "[instrument][params]")
{
    auto host = makeHostWith ("DLSMusicDevice");

    const auto params = run (*host, R"({"cmd":"params"})");
    REQUIRE (isOk (params));
    REQUIRE ((int) params.getProperty ("total", 0) > 0);

    const auto first = params.getProperty ("params", {})[0];
    const auto name = first.getProperty ("name", {}).toString();
    INFO ("first parameter: " << name);

    const auto reply = run (*host, R"({"cmd":"set","param":0,"value":0.3})");
    REQUIRE (isOk (reply));
    CHECK ((double) reply.getProperty ("param", {}).getProperty ("value", -1.0) == Catch::Approx (0.3).margin (0.02));
}

TEST_CASE ("Instrument state round-trips the synth and its settings", "[instrument][state]")
{
    auto original = makeHostWith ("DLSMusicDevice");
    REQUIRE (isOk (run (*original, R"({"cmd":"set","param":0,"value":0.3})")));
    const auto saved = (double) run (*original, R"({"cmd":"get","param":0})").getProperty ("param", {}).getProperty ("value", -1.0);

    juce::MemoryBlock state;
    original->getStateInformation (state);

    auto restored = makeHost();
    restored->setStateInformation (state.getData(), (int) state.getSize());

    REQUIRE (restored->getInnerPlugin() != nullptr);
    CHECK (restored->getInnerPlugin()->getName() == "DLSMusicDevice");
    CHECK ((double) run (*restored, R"({"cmd":"get","param":0})").getProperty ("param", {}).getProperty ("value", -1.0)
           == Catch::Approx (saved).margin (0.001));

    // And it still plays.
    renderBlock (*restored, noteOn (middleC));
    CHECK (sustainPeak (*restored, 8) > 0.01f);
}

TEST_CASE ("Instrument commands work end to end over the socket", "[instrument][socket]")
{
    auto host = makeHost();
    const auto port = host->getPort();

    auto future = std::async (std::launch::async, [port]
    {
        test::Client client (port);
        return client.request (R"({"cmd":"load","name":"DLSMusicDevice"})", 15000);
    });

    auto result = test::pumpForFuture (future, 20000);
    REQUIRE (result.has_value());
    REQUIRE (result->has_value());
    CHECK (isOk (**result));
    CHECK ((*result)->getProperty ("product", {}).toString() == "Claude Host Instrument");
}
