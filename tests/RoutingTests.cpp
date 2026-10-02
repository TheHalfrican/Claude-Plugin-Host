// Bus routing, sidechain and latency, checked with fake plugins whose
// behaviour is known exactly. Effect host build.

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
    const auto mono = juce::AudioChannelSet::mono();

    // Ducks its main input to a quarter when its sidechain carries signal,
    // like a compressor keyed by a kick.
    std::unique_ptr<test::FakePlugin> makeDucker()
    {
        return std::make_unique<test::FakePlugin> (
            Buses().withInput ("Input", stereo, true).withOutput ("Output", stereo, true).withInput ("Sidechain", stereo, false),
            [] (test::FakePlugin& self, juce::AudioBuffer<float>& b, juce::MidiBuffer&)
            {
                auto main = self.getBusBuffer (b, true, 0);
                auto side = self.getBusBuffer (b, true, 1);
                const auto keyed = side.getNumChannels() > 0 && side.getMagnitude (0, b.getNumSamples()) > 0.01f;
                main.applyGain (keyed ? 0.25f : 1.0f);
            });
    }

    // Sets up the host's own buses, as Live does when the user enables the
    // sidechain, then prepares it.
    void prepareHost (HostProcessor& host, bool sidechainOn)
    {
        auto layout = host.getBusesLayout();
        layout.inputBuses.getReference (1) = sidechainOn ? stereo : juce::AudioChannelSet::disabled();
        REQUIRE (host.setBusesLayout (layout));
        host.prepareToPlay (sampleRate, blockSize);
    }

    // Host buffer: main input in channels 0-1, sidechain (if enabled) in 2-3.
    juce::AudioBuffer<float> hostBuffer (HostProcessor& host, float mainLevel, float sideLevel)
    {
        juce::AudioBuffer<float> b (host.getTotalNumInputChannels(), blockSize);
        b.clear();

        auto main = host.getBusBuffer (b, true, 0);
        for (int ch = 0; ch < main.getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill (main.getWritePointer (ch), mainLevel, blockSize);

        auto side = host.getBusBuffer (b, true, 1);
        for (int ch = 0; ch < side.getNumChannels(); ++ch)
            juce::FloatVectorOperations::fill (side.getWritePointer (ch), sideLevel, blockSize);

        return b;
    }

    float mainOut (HostProcessor& host, juce::AudioBuffer<float>& b, int ch = 0)
    {
        return host.getBusBuffer (b, false, 0).getSample (ch, blockSize / 2);
    }
}

TEST_CASE ("The effect host has a sidechain input that starts off", "[routing][sidechain]")
{
    HostProcessor host;
    REQUIRE (host.getBusCount (true) == 2);
    CHECK (host.getBus (true, 1)->getName() == "Sidechain");
    CHECK_FALSE (host.getBus (true, 1)->isEnabled());

    auto layout = host.getBusesLayout();
    layout.inputBuses.getReference (1) = stereo;
    CHECK (host.checkBusesLayoutSupported (layout));
    layout.inputBuses.getReference (1) = mono;
    CHECK (host.checkBusesLayoutSupported (layout));
    layout.inputBuses.getReference (1) = juce::AudioChannelSet::create5point1();
    CHECK_FALSE (host.checkBusesLayoutSupported (layout));
}

TEST_CASE ("The sidechain reaches the hosted plugin's sidechain", "[routing][sidechain]")
{
    HostProcessor host;
    prepareHost (host, true);
    REQUIRE (host.loadPluginInstance (makeDucker()).isEmpty());

    juce::MidiBuffer midi;

    SECTION ("a loud key ducks the main signal")
    {
        auto b = hostBuffer (host, 0.8f, 0.5f);
        host.processBlock (b, midi);
        CHECK (mainOut (host, b, 0) == Catch::Approx (0.2f));
        CHECK (mainOut (host, b, 1) == Catch::Approx (0.2f));
    }

    SECTION ("a silent key leaves it alone")
    {
        auto b = hostBuffer (host, 0.8f, 0.0f);
        host.processBlock (b, midi);
        CHECK (mainOut (host, b) == Catch::Approx (0.8f));
    }
}

TEST_CASE ("With the host's sidechain off - the plugin's sidechain gets silence", "[routing][sidechain]")
{
    HostProcessor host;
    prepareHost (host, false);
    REQUIRE (host.loadPluginInstance (makeDucker()).isEmpty());

    juce::MidiBuffer midi;
    auto b = hostBuffer (host, 0.8f, 0.0f);
    host.processBlock (b, midi);
    CHECK (mainOut (host, b) == Catch::Approx (0.8f));
}

TEST_CASE ("A mono sidechain feeds both sides of a stereo plugin sidechain", "[routing][sidechain]")
{
    HostProcessor host;
    auto layout = host.getBusesLayout();
    layout.inputBuses.getReference (1) = mono;
    REQUIRE (host.setBusesLayout (layout));
    host.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> seenSide;
    auto spy = std::make_unique<test::FakePlugin> (
        Buses().withInput ("Input", stereo, true).withOutput ("Output", stereo, true).withInput ("Sidechain", stereo, false),
        [&seenSide] (test::FakePlugin& self, juce::AudioBuffer<float>& b, juce::MidiBuffer&)
        {
            auto side = self.getBusBuffer (b, true, 1);
            seenSide.makeCopyOf (side);
        });
    // The plugin only does a stereo sidechain.
    spy->acceptLayout = [] (const juce::AudioProcessor::BusesLayout& l)
    {
        return l.getChannelSet (true, 1).isDisabled() || l.getChannelSet (true, 1) == juce::AudioChannelSet::stereo();
    };
    REQUIRE (host.loadPluginInstance (std::move (spy)).isEmpty());

    juce::MidiBuffer midi;
    auto b = hostBuffer (host, 0.0f, 0.3f);
    host.processBlock (b, midi);

    // Either the plugin took a mono sidechain, or we duplicated ours into
    // its stereo one; both sides must carry the key.
    REQUIRE (seenSide.getNumChannels() >= 1);
    for (int ch = 0; ch < seenSide.getNumChannels(); ++ch)
        CHECK (seenSide.getSample (ch, 10) == Catch::Approx (0.3f));
}

TEST_CASE ("A mono-only plugin in a stereo host processes both channels", "[routing]")
{
    HostProcessor host;
    prepareHost (host, false);

    auto doubler = std::make_unique<test::FakePlugin> (
        Buses().withInput ("Input", mono, true).withOutput ("Output", mono, true),
        [] (test::FakePlugin&, juce::AudioBuffer<float>& b, juce::MidiBuffer&) { b.applyGain (2.0f); });
    doubler->acceptLayout = [] (const juce::AudioProcessor::BusesLayout& l)
    {
        return l.getMainInputChannelSet() == juce::AudioChannelSet::mono()
            && l.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
    };
    REQUIRE (host.loadPluginInstance (std::move (doubler)).isEmpty());

    juce::MidiBuffer midi;
    auto b = hostBuffer (host, 0.25f, 0.0f);
    host.processBlock (b, midi);
    CHECK (mainOut (host, b, 0) == Catch::Approx (0.5f));
    CHECK (mainOut (host, b, 1) == Catch::Approx (0.5f)); // mono output fills both sides
}

TEST_CASE ("A plugin that refuses our layout still runs with its own", "[routing]")
{
    HostProcessor host;
    prepareHost (host, true);

    // Insists on its sidechain always being on.
    auto stubborn = std::make_unique<test::FakePlugin> (
        Buses().withInput ("Input", stereo, true).withOutput ("Output", stereo, true).withInput ("Sidechain", stereo, true),
        [] (test::FakePlugin&, juce::AudioBuffer<float>& b, juce::MidiBuffer&) { b.applyGain (0.5f); });
    stubborn->acceptLayout = [] (const juce::AudioProcessor::BusesLayout& l)
    {
        return l.getChannelSet (true, 1) == juce::AudioChannelSet::stereo()
            && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
    };
    REQUIRE (host.loadPluginInstance (std::move (stubborn)).isEmpty());

    juce::MidiBuffer midi;
    auto b = hostBuffer (host, 0.8f, 0.0f);
    host.processBlock (b, midi);
    CHECK (mainOut (host, b) == Catch::Approx (0.4f));
}

TEST_CASE ("Latency follows the hosted plugin and goes back to zero on unload", "[routing][latency]")
{
    HostProcessor host;
    prepareHost (host, false);

    auto plugin = std::make_unique<test::FakePlugin> (
        Buses().withInput ("Input", stereo, true).withOutput ("Output", stereo, true),
        [] (test::FakePlugin&, juce::AudioBuffer<float>&, juce::MidiBuffer&) {});
    auto* raw = plugin.get();
    raw->reportLatency (128);
    raw->tailSeconds = 2.5;

    REQUIRE (host.loadPluginInstance (std::move (plugin)).isEmpty());
    CHECK (host.getLatencySamples() == 128);
    CHECK (host.getTailLengthSeconds() == Catch::Approx (2.5));

    raw->reportLatency (512); // e.g. a linear-phase mode switched on
    CHECK (host.getLatencySamples() == 512);

    host.unloadPlugin();
    CHECK (host.getLatencySamples() == 0);
    CHECK (host.getTailLengthSeconds() == 0.0);
}

TEST_CASE ("Re-preparing the host re-prepares the hosted plugin", "[routing]")
{
    HostProcessor host;
    prepareHost (host, false);

    auto plugin = std::make_unique<test::FakePlugin> (
        Buses().withInput ("Input", stereo, true).withOutput ("Output", stereo, true).withInput ("Sidechain", stereo, false),
        [] (test::FakePlugin&, juce::AudioBuffer<float>&, juce::MidiBuffer&) {});
    auto* raw = plugin.get();
    REQUIRE (host.loadPluginInstance (std::move (plugin)).isEmpty());
    const auto before = raw->prepareCount;

    prepareHost (host, true); // the user turns the sidechain on
    INFO ("host sidechain: " << host.getChannelLayoutOfBus (true, 1).getDescription()
          << ", plugin sidechain: " << raw->getChannelLayoutOfBus (true, 1).getDescription()
          << ", plugin buses: " << raw->getBusCount (true));
    CHECK (raw->prepareCount > before);
    CHECK (raw->getChannelLayoutOfBus (true, 1) == stereo);
}
