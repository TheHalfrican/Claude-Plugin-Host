#include "ControlServer.h"

#include <sys/socket.h>

namespace
{
    // A request line larger than this is treated as a broken client.
    constexpr int maxLineBytes = 4 * 1024 * 1024;

    juce::var errorReply (const juce::String& message)
    {
        auto* reply = new juce::DynamicObject();
        reply->setProperty ("ok", false);
        reply->setProperty ("error", message);
        return juce::var (reply);
    }
}

ControlServer::ControlServer (Handler handlerToUse)
    : juce::Thread ("Claude Host control server"),
      handler (std::move (handlerToUse))
{
}

ControlServer::~ControlServer()
{
    stop();
}

int ControlServer::start()
{
    // Loopback only: these commands change the plugin's sound and state, so
    // nothing else on the network should be able to reach them.
    if (! listener.createListener (0, "127.0.0.1"))
        return 0;

    port = listener.getBoundPort();
    startThread();
    return port;
}

void ControlServer::stop()
{
    signalThreadShouldExit();
    listener.close(); // unblocks waitForNextConnection()
    stopThread (3000);
    port = 0;
}

void ControlServer::run()
{
    while (! threadShouldExit())
    {
        std::unique_ptr<juce::StreamingSocket> client (listener.waitForNextConnection());

        if (client == nullptr)
            continue;

        // Writing to a socket the client already closed raises SIGPIPE, and
        // its default action kills the whole process: inside Live, that's
        // Live. JUCE doesn't guard against it on macOS, so opt out per socket.
        const int noSigPipe = 1;
        ::setsockopt (client->getRawSocketHandle(), SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof (noSigPipe));

        serveClient (*client);
    }
}

void ControlServer::serveClient (juce::StreamingSocket& client)
{
    juce::MemoryBlock pending;
    char chunk[8192];

    while (! threadShouldExit() && client.isConnected())
    {
        const auto ready = client.waitUntilReady (true, 200);

        if (ready < 0)
            return;

        if (ready == 0)
            continue;

        const auto bytesRead = client.read (chunk, (int) sizeof (chunk), false);

        if (bytesRead <= 0)
            return; // client closed the connection

        pending.append (chunk, (size_t) bytesRead);

        if ((int) pending.getSize() > maxLineBytes)
            return;

        // Handle every complete line received so far.
        for (;;)
        {
            const auto* data = static_cast<const char*> (pending.getData());
            const auto size = (int) pending.getSize();
            int newline = -1;

            for (int i = 0; i < size; ++i)
                if (data[i] == '\n') { newline = i; break; }

            if (newline < 0)
                break;

            const auto line = juce::String::fromUTF8 (data, newline).trim();
            pending.removeSection (0, (size_t) newline + 1);

            if (line.isEmpty())
                continue;

            juce::var request;
            const auto parsed = juce::JSON::parse (line, request);

            // JUCE reports arrays as objects too, so check for a real object.
            const auto reply = parsed.wasOk() && request.getDynamicObject() != nullptr && ! request.isArray()
                                   ? handler (request)
                                   : errorReply ("request must be one JSON object per line");

            const auto text = juce::JSON::toString (reply, true) + "\n";
            const auto utf8 = text.toUTF8();
            const auto length = (int) utf8.sizeInBytes() - 1;

            if (client.write (utf8.getAddress(), length) != length)
                return;
        }
    }
}
