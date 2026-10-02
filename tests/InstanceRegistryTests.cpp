#include <catch2/catch_test_macros.hpp>

#include "InstanceRegistry.h"

#include <unistd.h>

namespace
{
    juce::Array<juce::File> registryFiles()
    {
        return InstanceRegistry::getDirectory().findChildFiles (juce::File::findFiles, false, "*");
    }

    void clearRegistry()
    {
        for (const auto& f : registryFiles())
            f.deleteFile();
    }

    juce::var info (int pid, int tag)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("pid", pid);
        o->setProperty ("tag", tag);
        o->setProperty ("port", 1234);
        return juce::var (o);
    }

    // A pid that can't belong to a running process.
    constexpr int deadPid = 99999999;
}

TEST_CASE ("Registry directory honours the test override", "[registry]")
{
    const auto dir = InstanceRegistry::getDirectory();
    CHECK (dir.getFullPathName().contains ("ClaudeHostTests-registry"));
}

TEST_CASE ("Registry publishes one readable JSON file and removes it", "[registry]")
{
    clearRegistry();

    {
        InstanceRegistry registry;
        registry.publish (info ((int) ::getpid(), 42));

        const auto files = registryFiles();
        REQUIRE (files.size() == 1);
        CHECK (files[0].getFileExtension() == ".json"); // no .tmp left behind

        const auto parsed = juce::JSON::parse (files[0]);
        CHECK ((int) parsed.getProperty ("tag", 0) == 42);
        CHECK ((int) parsed.getProperty ("pid", 0) == (int) ::getpid());

        registry.publish (info ((int) ::getpid(), 43)); // republish overwrites
        REQUIRE (registryFiles().size() == 1);
        CHECK ((int) juce::JSON::parse (registryFiles()[0]).getProperty ("tag", 0) == 43);
    }

    CHECK (registryFiles().isEmpty()); // destructor removed it
}

TEST_CASE ("Registry detects a tag used by another live instance", "[registry]")
{
    clearRegistry();

    InstanceRegistry mine, other;
    mine.publish (info ((int) ::getpid(), 100));
    other.publish (info ((int) ::getpid(), 200));

    CHECK (mine.isTagUsedByAnotherInstance (200));
    CHECK_FALSE (mine.isTagUsedByAnotherInstance (100)); // its own tag doesn't count
    CHECK_FALSE (mine.isTagUsedByAnotherInstance (300));
}

TEST_CASE ("Registry ignores entries left by dead processes", "[registry]")
{
    clearRegistry();

    InstanceRegistry::getDirectory().getChildFile ("stale.json")
        .replaceWithText (juce::JSON::toString (info (deadPid, 555)));

    InstanceRegistry mine;
    CHECK_FALSE (mine.isTagUsedByAnotherInstance (555));

    clearRegistry();
}

TEST_CASE ("Registry tolerates junk files", "[registry]")
{
    clearRegistry();

    InstanceRegistry::getDirectory().getChildFile ("broken.json").replaceWithText ("{ not json");

    InstanceRegistry mine;
    CHECK_FALSE (mine.isTagUsedByAnotherInstance (1));

    clearRegistry();
}
