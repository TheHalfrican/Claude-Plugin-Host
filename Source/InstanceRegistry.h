#pragma once

#include <juce_core/juce_core.h>

// Publishes one small JSON file per running instance in
//   ~/Library/Application Support/ClaudePluginHost/instances/
// so a client can find each instance's port, and match it to a Live device by
// its tag (the "Instance Tag" parameter, which Live's API can read).
class InstanceRegistry
{
public:
    InstanceRegistry();
    ~InstanceRegistry();

    void publish (const juce::var& info);
    void remove();

    // True if another *live* instance (process still running, different file)
    // already uses this tag. Used to re-roll tags after a duplicated track.
    bool isTagUsedByAnotherInstance (int tag) const;

    static juce::File getDirectory();

private:
    juce::File file;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstanceRegistry)
};
