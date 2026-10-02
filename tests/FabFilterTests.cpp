// Tests against the user's own FabFilter plugins. Hidden by default ("[.]")
// because they need FabFilter installed and licensed; run with:
//   ClaudeHostTests "[fabfilter]"
//
// They also pin down Pro-Q 2's unlabelled codes (its AU shows "0".."7"
// instead of names), measured from the audio rather than guessed.

#include <catch2/catch_test_macros.hpp>

#include "HostProcessor.h"
#include "TestHelpers.h"

namespace
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    juce::var run (HostProcessor& host, const juce::String& json)
    {
        auto reply = host.runCommand (test::parse (json));

        // FabFilter applies parameter changes from the message thread, which
        // Live keeps running. Here we pump it so changes take effect before
        // audio is measured; without this, every reading lags one change.
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        return reply;
    }

    bool isOk (const juce::var& reply) { return (bool) reply.getProperty ("ok", false); }

    void set (HostProcessor& host, const juce::String& param, const juce::String& text)
    {
        const auto reply = run (host, R"({"cmd":"set","param":")" + param + R"(","text":")" + text + R"("})");
        INFO (param << " = " << text << ": " << juce::JSON::toString (reply, true));
        REQUIRE (isOk (reply));
    }

    // Gain change (dB) of a tone through the host.
    float gainAt (HostProcessor& host, double frequency)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        double phase = 0.0;
        float in = 0.0f;

        for (int i = 0; i < 40; ++i)
        {
            test::fillSine (buffer, frequency, sampleRate, phase);
            in = test::rms (buffer);
            host.processBlock (buffer, midi);
        }

        return juce::Decibels::gainToDecibels (test::rms (buffer) / in);
    }

    std::unique_ptr<HostProcessor> proQWithBand1 (const juce::String& state)
    {
        auto host = std::make_unique<HostProcessor>();
        host->prepareToPlay (sampleRate, blockSize);
        REQUIRE (isOk (run (*host, R"({"cmd":"load","name":"FF Pro-Q 2"})")));

        set (*host, "Band 1 State", state);
        set (*host, "Band 1 Shape", "0");
        set (*host, "Band 1 Frequency", "1000 Hz");
        set (*host, "Band 1 Gain", "-12 dB");
        set (*host, "Band 1 Q", "1");
        return host;
    }
}

TEST_CASE ("Pro-Q 2: Band State 1 is on; 0 and 2 leave audio alone", "[.][fabfilter]")
{
    CHECK (std::abs (gainAt (*proQWithBand1 ("1"), 1000.0) + 12.0f) < 0.5f);
    CHECK (std::abs (gainAt (*proQWithBand1 ("0"), 1000.0)) < 0.1f);
    CHECK (std::abs (gainAt (*proQWithBand1 ("2"), 1000.0)) < 0.1f); // the default: band unused
}

TEST_CASE ("Pro-Q 2: Band Shape codes", "[.][fabfilter]")
{
    auto host = proQWithBand1 ("1");

    struct Expect { const char* code; const char* name; float at100, at1k, at10k; };

    // Measured with Band 1 at 1 kHz, -12 dB, Q 1. Cuts read as -100 (silence).
    const Expect shapes[] = {
        { "0", "Bell",        0.0f,   -12.0f,   0.0f   },
        { "1", "Low Shelf",  -12.0f,   -6.0f,   0.0f   },
        { "2", "Low Cut",    -100.0f,  -3.0f,   0.0f   },
        { "3", "High Shelf",  0.0f,    -6.0f,  -12.0f  },
        { "4", "High Cut",    0.0f,    -3.0f,  -100.0f },
        { "5", "Notch",       0.0f,  -100.0f,   0.0f   },
        { "7", "Tilt Shelf",  12.0f,    0.0f,  -12.0f  },
    };

    for (const auto& e : shapes)
    {
        set (*host, "Band 1 Shape", e.code);
        INFO ("Shape " << e.code << " should be " << e.name);
        CHECK (std::abs (gainAt (*host, 100.0)   - e.at100)  < 2.0f);
        CHECK (std::abs (gainAt (*host, 1000.0)  - e.at1k)   < 2.0f);
        CHECK (std::abs (gainAt (*host, 10000.0) - e.at10k)  < 2.0f);
    }

    // 6 is Band Pass: 1 kHz untouched, both sides strongly cut.
    set (*host, "Band 1 Shape", "6");
    CHECK (std::abs (gainAt (*host, 1000.0)) < 1.0f);
    CHECK (gainAt (*host, 100.0) < -40.0f);
    CHECK (gainAt (*host, 10000.0) < -40.0f);
}
