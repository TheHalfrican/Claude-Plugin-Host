// Multi-output routing and MIDI for the instrument host, with fake
// instruments whose outputs are known exactly.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakePlugin.h"
#include "HostProcessor.h"
#include "TestHelpers.h"

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;

    using Buses = test::FakePlugin::BusesProperties;
    const auto stereo = juce::AudioChannelSet::stereo();

    // An instrument with `numOutputs` stereo outputs; output N carries a
    // constant 0.1 * (N + 1), so every bus is recognisable downstream.
    std::unique_ptr<test::FakePlugin> makeMultiOut (int numOutputs)
    {
        auto buses = Buses().withOutput ("Out 1", stereo, true);

        for (int i = 1; i < numOutputs; ++i)
            buses = buses.withOutput ("Out " + juce::String (i + 1), stereo, false);

        return std::make_unique<test::FakePlugin> (buses, [] (test::FakePlugin& self, juce::AudioBuffer<float>& b, juce::MidiBuffer&)
        {
            for (int bus = 0; bus < self.getBusCount (false); ++bus)
            {
                auto out = self.getBusBuffer (b, false, bus);
                for (int ch = 0; ch < out.getNumChannels(); ++ch)
                    juce::FloatVectorOperations::fill (out.getWritePointer (ch), 0.1f * (float) (bus + 1), b.getNumSamples());
            }
        }, true);
    }

    // Enables the host's first `numEnabled` output buses, as Live does when
    // the user routes a track from one of them.
    void prepareHost (HostProcessor& host, int numEnabled)
    {
        auto layout = host.getBusesLayout();

        for (int i = 1; i < layout.outputBuses.size(); ++i)
            layout.outputBuses.getReference (i) = i < numEnabled ? stereo : juce::AudioChannelSet::disabled();

        REQUIRE (host.setBusesLayout (layout));
        host.prepareToPlay (sampleRate, blockSize);
    }

    float levelOnBus (HostProcessor& host, juce::AudioBuffer<float>& b, int bus)
    {
        auto out = host.getBusBuffer (b, false, bus);
        return out.getNumChannels() > 0 ? out.getSample (0, blockSize / 2) : -1.0f;
    }
}

TEST_CASE ("The instrument host has 8 stereo outputs - aux ones off by default", "[routing][multiout]")
{
    HostProcessor host;
    REQUIRE (host.getBusCount (false) == HostProcessor::numInstrumentOutputs);
    CHECK (host.getBus (false, 0)->isEnabled());

    for (int i = 1; i < host.getBusCount (false); ++i)
    {
        CHECK_FALSE (host.getBus (false, i)->isEnabled());
        CHECK (host.getBus (false, i)->getName() == "Aux " + juce::String (i));
    }
}

TEST_CASE ("Each output of a multi-out instrument reaches the matching host output", "[routing][multiout]")
{
    HostProcessor host;
    prepareHost (host, 8);
    REQUIRE (host.loadPluginInstance (makeMultiOut (4)).isEmpty());

    juce::AudioBuffer<float> b (host.getTotalNumOutputChannels(), blockSize);
    juce::MidiBuffer midi;
    host.processBlock (b, midi);

    CHECK (levelOnBus (host, b, 0) == Catch::Approx (0.1f));
    CHECK (levelOnBus (host, b, 1) == Catch::Approx (0.2f));
    CHECK (levelOnBus (host, b, 2) == Catch::Approx (0.3f));
    CHECK (levelOnBus (host, b, 3) == Catch::Approx (0.4f));

    for (int bus = 4; bus < 8; ++bus)   // outputs the instrument doesn't have
        CHECK (levelOnBus (host, b, bus) == 0.0f);
}

TEST_CASE ("With aux outputs off only the main output plays", "[routing][multiout]")
{
    HostProcessor host;
    prepareHost (host, 1);
    REQUIRE (host.loadPluginInstance (makeMultiOut (4)).isEmpty());

    juce::AudioBuffer<float> b (host.getTotalNumOutputChannels(), blockSize);
    REQUIRE (b.getNumChannels() == 2);
    juce::MidiBuffer midi;
    host.processBlock (b, midi);
    CHECK (levelOnBus (host, b, 0) == Catch::Approx (0.1f));
}

TEST_CASE ("An instrument with more outputs than the host maps the first 8", "[routing][multiout]")
{
    HostProcessor host;
    prepareHost (host, 8);
    REQUIRE (host.loadPluginInstance (makeMultiOut (16)).isEmpty());   // like Kontakt's 16 stereo outs

    juce::AudioBuffer<float> b (host.getTotalNumOutputChannels(), blockSize);
    juce::MidiBuffer midi;
    host.processBlock (b, midi);

    for (int bus = 0; bus < 8; ++bus)
        CHECK (levelOnBus (host, b, bus) == Catch::Approx (0.1f * (float) (bus + 1)));
}

TEST_CASE ("MIDI reaches the hosted instrument with its timing", "[routing][midi]")
{
    HostProcessor host;
    prepareHost (host, 1);

    juce::Array<int> seenNotes, seenOffsets;
    auto recorder = std::make_unique<test::FakePlugin> (
        Buses().withOutput ("Out", stereo, true),
        [&] (test::FakePlugin&, juce::AudioBuffer<float>&, juce::MidiBuffer& m)
        {
            for (const auto meta : m)
                if (meta.getMessage().isNoteOn())
                {
                    seenNotes.add (meta.getMessage().getNoteNumber());
                    seenOffsets.add (meta.samplePosition);
                }
        }, true);
    REQUIRE (host.loadPluginInstance (std::move (recorder)).isEmpty());

    juce::AudioBuffer<float> b (2, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 48, (juce::uint8) 100), 10);
    midi.addEvent (juce::MidiMessage::noteOn (1, 55, (juce::uint8) 100), 200);
    host.processBlock (b, midi);

    CHECK (seenNotes == juce::Array<int> { 48, 55 });
    CHECK (seenOffsets == juce::Array<int> { 10, 200 });
}
