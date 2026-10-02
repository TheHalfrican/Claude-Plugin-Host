#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "HostProcessor.h"

// A slim header (instance tag, port, a box to load a plugin by name) with the
// inner plugin's own editor embedded underneath. The window resizes to fit it.
class HostEditor final : public juce::AudioProcessorEditor,
                         private HostProcessor::Listener,
                         private juce::ComponentListener
{
public:
    explicit HostEditor (HostProcessor&);
    ~HostEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void innerPluginWillChange() override;
    void innerPluginChanged() override;
    void componentMovedOrResized (juce::Component&, bool wasMoved, bool wasResized) override;

    void rebuildInnerEditor();
    void updateHeader();
    void loadFromSearchBox();

    static constexpr int headerHeight = 40;
    static constexpr int emptyWidth = 560;
    static constexpr int emptyHeight = 160;

    HostProcessor& host;

    juce::Label title, status;
    juce::TextEditor searchBox;
    juce::TextButton loadButton { "Load" }, unloadButton { "Unload" };

    std::unique_ptr<juce::AudioProcessorEditor> innerEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostEditor)
};
