#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ControlServer.h"
#include "InstanceRegistry.h"

// Hosts one third-party plugin ("the inner plugin") and passes audio/MIDI
// through it. Live sees only this wrapper, so Live's API can't reach the inner
// plugin's knobs; instead the ControlServer exposes every inner parameter by
// name to a local client.
class HostProcessor final : public juce::AudioProcessor,
                            private juce::AudioProcessorListener
{
public:
    HostProcessor();
    ~HostProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return CLAUDE_HOST_IS_SYNTH; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    // Live can't be told to rename a plugin device, but it shows the
    // plugin's current program, so ours is named after the hosted plugin.
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void updateTrackProperties (const TrackProperties& properties) override;

    //==============================================================================
    // Message thread only.
    juce::String loadPlugin (const juce::String& nameQuery, const juce::String& formatQuery = {});
    juce::String loadPlugin (const juce::PluginDescription& description);
    void unloadPlugin();

    // Hosts an already-created plugin (tests use this with fake plugins).
    juce::String loadPluginInstance (std::unique_ptr<juce::AudioPluginInstance>);

    static constexpr int numInstrumentOutputs = 8;   // stereo buses: main + 7 aux
    static constexpr int maxInnerChannels = 64;      // scratch space per block

    // Runs one control command (the same JSON the socket accepts) and returns
    // the reply. The socket path calls this; tests call it directly.
    juce::var runCommand (const juce::var& request);

    juce::AudioPluginInstance* getInnerPlugin() const noexcept { return inner.get(); }
    int getTag() const noexcept;

    // "FF Pro-C 2 - Claude Host #1005": the hosted plugin first, as the user
    // wants to see it. Used for the program name and the editor header.
    juce::String getDisplayName() const;
    int getPort() const noexcept { return server.getPort(); }
    juce::String getTrackName() const { return trackName; }

    // Editors listen so they can drop the inner plugin's editor before the
    // inner plugin is destroyed, and rebuild afterwards.
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void innerPluginWillChange() = 0;
        virtual void innerPluginChanged() = 0;
    };

    void addHostListener (Listener* l) { listeners.add (l); }
    void removeHostListener (Listener* l) { listeners.remove (l); }

private:
    //==============================================================================
    static BusesProperties defaultBuses();

    juce::var handleRequest (const juce::var& request);  // server thread

    juce::var describe() const;
    juce::var listPlugins (const juce::String& query);
    juce::var listParameters (const juce::String& filter) const;
    juce::var applySetRequest (const juce::var& request);
    juce::var listPrograms() const;

    juce::AudioProcessorParameter* findParameter (const juce::var& key, juce::String& error) const;
    static juce::var describeParameter (int index, juce::AudioProcessorParameter& p);

    void prepareInner (juce::AudioPluginInstance&);
    void installInner (std::unique_ptr<juce::AudioPluginInstance> newInner);
    void publishRegistry();
    void ensureUniqueTag();

    // AudioProcessorListener (inner plugin)
    void audioProcessorParameterChanged (AudioProcessor*, int, float) override {}
    void audioProcessorChanged (AudioProcessor*, const ChangeDetails&) override;

    //==============================================================================
    struct AvailablePlugin
    {
        juce::String name, format, identifier;
    };

    const juce::Array<AvailablePlugin>& getAvailablePlugins();

    juce::AudioPluginFormatManager formatManager;
    juce::Array<AvailablePlugin> availablePlugins;
    bool availableScanned = false;

    std::unique_ptr<juce::AudioPluginInstance> inner;
    juce::CriticalSection innerLock;      // held by audio thread while processing
    juce::CriticalSection noLock;         // stand-in when no locking is needed
    std::atomic<bool> shuttingDown { false };
    juce::AudioBuffer<float> scratch;     // used when the inner plugin wants more channels

    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    bool isPrepared = false;

    juce::AudioParameterInt* tagParameter = nullptr;
    juce::String trackName;

    InstanceRegistry registry;
    ControlServer server;
    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_WEAK_REFERENCEABLE (HostProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostProcessor)
};
