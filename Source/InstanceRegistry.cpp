#include "InstanceRegistry.h"

#include <signal.h>
#include <unistd.h>

namespace
{
    bool processIsRunning (int pid)
    {
        return pid > 0 && (::kill (pid, 0) == 0 || errno == EPERM);
    }
}

juce::File InstanceRegistry::getDirectory()
{
    // Tests point this elsewhere so they never mix with instances inside Live.
    const auto overridden = juce::SystemStats::getEnvironmentVariable ("CLAUDE_HOST_REGISTRY_DIR", {});

    if (overridden.isNotEmpty())
        return juce::File (overridden);

    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support/ClaudePluginHost/instances");
}

InstanceRegistry::InstanceRegistry()
{
    const auto dir = getDirectory();
    dir.createDirectory();
    file = dir.getChildFile (juce::String ((int) ::getpid()) + "-" + juce::Uuid().toString() + ".json");
}

InstanceRegistry::~InstanceRegistry()
{
    remove();
}

void InstanceRegistry::publish (const juce::var& info)
{
    // Write then rename, so a reader never sees a half-written file.
    const auto temp = file.getSiblingFile (file.getFileName() + ".tmp");

    if (temp.replaceWithText (juce::JSON::toString (info)))
        temp.moveFileTo (file);
}

void InstanceRegistry::remove()
{
    file.deleteFile();
}

bool InstanceRegistry::isTagUsedByAnotherInstance (int tag) const
{
    for (const auto& other : getDirectory().findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        if (other == file)
            continue;

        const auto info = juce::JSON::parse (other);

        if (! processIsRunning ((int) info.getProperty ("pid", 0)))
            continue;

        if ((int) info.getProperty ("tag", -1) == tag)
            return true;
    }

    return false;
}
