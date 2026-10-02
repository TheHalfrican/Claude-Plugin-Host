#pragma once

#include <juce_core/juce_core.h>
#include <functional>

// Line-delimited JSON over TCP, bound to 127.0.0.1 only.
//
// Each request is one JSON object on one line; each reply is one JSON object
// on one line. The OS picks the port (we bind to 0) so several instances can
// run at once; InstanceRegistry publishes which port belongs to which instance.
class ControlServer : private juce::Thread
{
public:
    using Handler = std::function<juce::var (const juce::var& request)>;

    explicit ControlServer (Handler handlerToUse);
    ~ControlServer() override;

    // Returns the bound port, or 0 if the socket couldn't be opened.
    int start();
    void stop();

    int getPort() const noexcept { return port; }

private:
    void run() override;
    void serveClient (juce::StreamingSocket& client);

    Handler handler;
    juce::StreamingSocket listener;
    int port = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlServer)
};
