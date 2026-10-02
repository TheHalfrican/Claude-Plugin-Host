#include <catch2/catch_test_macros.hpp>

#include "ControlServer.h"
#include "TestHelpers.h"

#include <atomic>
#include <thread>

namespace
{
    // Replies {"ok":true,"echo":<request>} and counts calls.
    struct EchoServer
    {
        std::atomic<int> calls { 0 };

        ControlServer server { [this] (const juce::var& request)
        {
            ++calls;
            auto* reply = new juce::DynamicObject();
            reply->setProperty ("ok", true);
            reply->setProperty ("echo", request);
            return juce::var (reply);
        } };

        EchoServer() { port = server.start(); }

        int port = 0;
    };

    juce::String echoedCmd (const juce::var& reply)
    {
        return reply.getProperty ("echo", {}).getProperty ("cmd", {}).toString();
    }
}

TEST_CASE ("Control server binds an OS-assigned loopback port", "[server]")
{
    EchoServer s;
    REQUIRE (s.port > 0);
    REQUIRE (s.server.getPort() == s.port);

    test::Client client (s.port);
    REQUIRE (client.isConnected());
}

TEST_CASE ("Control server answers one request per line", "[server]")
{
    EchoServer s;
    test::Client client (s.port);

    auto reply = client.request (R"({"cmd":"info"})");
    REQUIRE (reply.has_value());
    CHECK ((bool) reply->getProperty ("ok", false));
    CHECK (echoedCmd (*reply) == "info");
}

TEST_CASE ("Control server handles several requests in one write in order", "[server]")
{
    EchoServer s;
    test::Client client (s.port);

    REQUIRE (client.sendRaw (juce::String (R"({"cmd":"a"})") + "\n" + R"({"cmd":"b"})" + "\n" + R"({"cmd":"c"})" + "\n"));

    for (auto expected : { "a", "b", "c" })
    {
        auto line = client.readLine();
        REQUIRE (line.has_value());
        CHECK (echoedCmd (juce::JSON::parse (*line)) == expected);
    }
}

TEST_CASE ("Control server reassembles a request split across writes", "[server]")
{
    EchoServer s;
    test::Client client (s.port);

    REQUIRE (client.sendRaw (R"({"cmd":)"));
    juce::Thread::sleep (60);
    REQUIRE (client.sendRaw (R"("split"})"));
    juce::Thread::sleep (60);
    REQUIRE (client.sendRaw ("\n"));

    auto line = client.readLine();
    REQUIRE (line.has_value());
    CHECK (echoedCmd (juce::JSON::parse (*line)) == "split");
    CHECK (s.calls == 1);
}

TEST_CASE ("Control server keeps a UTF-8 character split across writes intact", "[server]")
{
    EchoServer s;
    test::Client client (s.port);

    // "é" is two bytes (C3 A9); send them in separate writes.
    const char part1[] = "{\"cmd\":\"caf\xC3";
    const char part2[] = "\xA9\"}\n";
    REQUIRE (client.sendRaw (part1, (int) sizeof (part1) - 1));
    juce::Thread::sleep (60);
    REQUIRE (client.sendRaw (part2, (int) sizeof (part2) - 1));

    auto line = client.readLine();
    REQUIRE (line.has_value());
    CHECK (echoedCmd (juce::JSON::parse (*line)) == juce::String (juce::CharPointer_UTF8 ("caf\xC3\xA9")));
}

TEST_CASE ("Control server rejects bad input but keeps the connection usable", "[server]")
{
    EchoServer s;
    test::Client client (s.port);

    SECTION ("invalid JSON")
    {
        auto reply = client.request ("this is not json");
        REQUIRE (reply.has_value());
        CHECK_FALSE ((bool) reply->getProperty ("ok", true));
        CHECK (reply->getProperty ("error", {}).toString().isNotEmpty());
    }

    SECTION ("JSON that isn't an object")
    {
        auto reply = client.request ("[1, 2, 3]");
        REQUIRE (reply.has_value());
        CHECK_FALSE ((bool) reply->getProperty ("ok", true));
    }

    CHECK (s.calls == 0); // the handler never saw the bad input

    auto followUp = client.request (R"({"cmd":"still-here"})");
    REQUIRE (followUp.has_value());
    CHECK (echoedCmd (*followUp) == "still-here");
}

TEST_CASE ("Control server ignores blank lines", "[server]")
{
    EchoServer s;
    test::Client client (s.port);

    REQUIRE (client.sendRaw ("\n\n   \n{\"cmd\":\"after-blanks\"}\n"));
    auto line = client.readLine();
    REQUIRE (line.has_value());
    CHECK (echoedCmd (juce::JSON::parse (*line)) == "after-blanks");
    CHECK (s.calls == 1);
}

TEST_CASE ("Control server serves clients one after another", "[server]")
{
    EchoServer s;

    for (int i = 0; i < 3; ++i)
    {
        test::Client client (s.port);
        auto reply = client.request (R"({"cmd":"n"})");
        REQUIRE (reply.has_value());
    }

    CHECK (s.calls == 3);
}

TEST_CASE ("Control server drops a client that sends an oversized line", "[server]")
{
    EchoServer s;
    test::Client client (s.port);

    // 5 MB with no newline: over the 4 MB limit.
    juce::MemoryBlock junk (5 * 1024 * 1024);
    junk.fillWith ('x');
    client.sendRaw (junk.getData(), (int) junk.getSize()); // may fail part-way once the server closes

    CHECK (client.waitForClose (5000));
    CHECK (s.calls == 0);
}

TEST_CASE ("Control server stops promptly with an idle client connected", "[server]")
{
    auto s = std::make_unique<EchoServer>();
    test::Client client (s->port);
    REQUIRE (client.isConnected());

    const auto start = juce::Time::getMillisecondCounter();
    s->server.stop();
    const auto elapsed = juce::Time::getMillisecondCounter() - start;

    CHECK (elapsed < 1000);
    CHECK (s->server.getPort() == 0);
}

TEST_CASE ("Two control servers get different ports", "[server]")
{
    EchoServer a, b;
    REQUIRE (a.port > 0);
    REQUIRE (b.port > 0);
    CHECK (a.port != b.port);
}
