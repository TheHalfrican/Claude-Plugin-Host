#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

namespace test
{
    // A stand-in for a third-party plugin with whatever buses, latency and
    // behaviour a test needs, so routing can be checked exactly without
    // depending on which real plugins are installed.
    class FakePlugin final : public juce::AudioPluginInstance
    {
    public:
        // JUCE keeps this protected; tests need it to describe fake buses.
        using AudioPluginInstance::BusesProperties;

        using Process = std::function<void (FakePlugin&, juce::AudioBuffer<float>&, juce::MidiBuffer&)>;

        FakePlugin (const BusesProperties& buses, Process processToUse, bool isInstrument = false)
            : AudioPluginInstance (buses), process (std::move (processToUse)), instrument (isInstrument) {}

        // Accept any layout whose buses are off, mono or stereo, unless a test
        // says otherwise.
        std::function<bool (const BusesLayout&)> acceptLayout = [] (const BusesLayout& l)
        {
            auto ok = [] (const juce::AudioChannelSet& s)
            {
                return s.isDisabled() || s == juce::AudioChannelSet::mono() || s == juce::AudioChannelSet::stereo();
            };

            for (auto& b : l.inputBuses)  if (! ok (b)) return false;
            for (auto& b : l.outputBuses) if (! ok (b)) return false;
            return true;
        };

        void reportLatency (int samples)
        {
            setLatencySamples (samples);
            updateHostDisplay (ChangeDetails().withLatencyChanged (true));
        }

        double tailSeconds = 0.0;
        int prepareCount = 0;

        //==============================================================================
        void fillInPluginDescription (juce::PluginDescription& d) const override
        {
            d.name = "Fake";
            d.descriptiveName = "Fake plugin for tests";
            d.pluginFormatName = "Fake";
            d.manufacturerName = "Tests";
            d.fileOrIdentifier = "fake";
            d.isInstrument = instrument;
            d.numInputChannels = getTotalNumInputChannels();
            d.numOutputChannels = getTotalNumOutputChannels();
        }

        const juce::String getName() const override              { return "Fake"; }
        void prepareToPlay (double, int) override                { ++prepareCount; }
        void releaseResources() override                         {}
        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m) override { process (*this, b, m); }
        using AudioPluginInstance::processBlock;

        bool isBusesLayoutSupported (const BusesLayout& l) const override { return acceptLayout (l); }

        double getTailLengthSeconds() const override             { return tailSeconds; }
        bool acceptsMidi() const override                        { return instrument; }
        bool producesMidi() const override                       { return false; }
        juce::AudioProcessorEditor* createEditor() override      { return nullptr; }
        bool hasEditor() const override                          { return false; }
        int getNumPrograms() override                            { return 1; }
        int getCurrentProgram() override                         { return 0; }
        void setCurrentProgram (int) override                    {}
        const juce::String getProgramName (int) override         { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override   {}
        void setStateInformation (const void*, int) override     {}

    private:
        Process process;
        bool instrument;
    };
}
