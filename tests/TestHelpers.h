#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <sys/socket.h>

#include <chrono>
#include <functional>
#include <future>
#include <optional>

namespace test
{
    inline juce::var parse (const juce::String& json)
    {
        auto v = juce::JSON::parse (json);
        jassert (v.isObject());
        return v;
    }

    // The main thread is JUCE's message thread in the test program. Code that
    // waits for message-thread work (the socket path) needs it pumped.
    inline bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 10000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! done())
        {
            if (juce::Time::getMillisecondCounter() > deadline)
                return false;

            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        }

        return true;
    }

    template <typename T>
    std::optional<T> pumpForFuture (std::future<T>& f, int timeoutMs = 10000)
    {
        const auto ready = pumpUntil ([&] { return f.wait_for (std::chrono::milliseconds (0)) == std::future_status::ready; },
                                      timeoutMs);
        if (! ready)
            return std::nullopt;

        return f.get();
    }

    // Minimal line-oriented client for the control protocol.
    class Client
    {
    public:
        explicit Client (int port)
        {
            connected = socket.connect ("127.0.0.1", port, 2000);

            // Some tests keep writing after the server hangs up on purpose.
            const int noSigPipe = 1;
            ::setsockopt (socket.getRawSocketHandle(), SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof (noSigPipe));
        }

        bool isConnected() const { return connected; }

        bool sendRaw (const void* data, int size)
        {
            return socket.write (data, size) == size;
        }

        bool sendRaw (const juce::String& text)
        {
            const auto utf8 = text.toUTF8();
            return sendRaw (utf8.getAddress(), (int) utf8.sizeInBytes() - 1);
        }

        // Returns the next reply line, or nullopt on timeout/close. Works on
        // raw bytes and only decodes whole lines, so a UTF-8 character split
        // across reads stays intact.
        std::optional<juce::String> readLine (int timeoutMs = 5000)
        {
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

            for (;;)
            {
                const auto* data = static_cast<const char*> (bytes.getData());
                const auto size = (int) bytes.getSize();

                for (int i = 0; i < size; ++i)
                {
                    if (data[i] == '\n')
                    {
                        auto line = juce::String::fromUTF8 (data, i);
                        bytes.removeSection (0, (size_t) i + 1);
                        return line;
                    }
                }

                if (juce::Time::getMillisecondCounter() > deadline)
                    return std::nullopt;

                if (socket.waitUntilReady (true, 50) != 1)
                    continue;

                char buf[4096];
                const auto n = socket.read (buf, (int) sizeof (buf), false);

                if (n <= 0)
                    return std::nullopt;

                bytes.append (buf, (size_t) n);
            }
        }

        std::optional<juce::var> request (const juce::String& json, int timeoutMs = 5000)
        {
            if (! sendRaw (json + "\n"))
                return std::nullopt;

            if (auto line = readLine (timeoutMs))
                return juce::JSON::parse (*line);

            return std::nullopt;
        }

        // True once the server has closed the connection.
        bool waitForClose (int timeoutMs)
        {
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

            while (juce::Time::getMillisecondCounter() < deadline)
            {
                const auto ready = socket.waitUntilReady (true, 50);

                if (ready < 0)
                    return true;

                if (ready == 1)
                {
                    char buf[256];
                    if (socket.read (buf, (int) sizeof (buf), false) <= 0)
                        return true;
                }
            }

            return false;
        }

    private:
        juce::StreamingSocket socket;
        bool connected = false;
        juce::MemoryBlock bytes;
    };

    inline float rms (const juce::AudioBuffer<float>& b, int channel = 0)
    {
        return b.getRMSLevel (channel, 0, b.getNumSamples());
    }

    inline void fillSine (juce::AudioBuffer<float>& b, double frequency, double sampleRate, double& phase, float gain = 0.5f)
    {
        const auto delta = juce::MathConstants<double>::twoPi * frequency / sampleRate;

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto s = gain * (float) std::sin (phase);
            phase += delta;

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                b.setSample (ch, i, s);
        }
    }
}
