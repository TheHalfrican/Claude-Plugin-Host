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

TEST_CASE ("Pro-Q 2: Band State 1 is on - 0 and 2 leave audio alone", "[.][fabfilter]")
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

namespace
{
    // Gain (dB) per channel for a 1 kHz tone fed as given (left, right gains).
    std::pair<float, float> stereoGain (HostProcessor& host, float inLeft, float inRight, double frequency = 1000.0)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        double phase = 0.0;

        for (int i = 0; i < 40; ++i)
        {
            test::fillSine (buffer, frequency, sampleRate, phase);
            buffer.applyGain (0, 0, blockSize, inLeft);
            buffer.applyGain (1, 0, blockSize, inRight);
            host.processBlock (buffer, midi);
        }

        auto db = [&] (int ch, float in) { return in == 0.0f ? juce::Decibels::gainToDecibels (buffer.getRMSLevel (ch, 0, blockSize))
                                                             : juce::Decibels::gainToDecibels (buffer.getRMSLevel (ch, 0, blockSize) / (0.5f * in / std::sqrt (2.0f))); };
        return { db (0, inLeft), db (1, inRight) };
    }
}

TEST_CASE ("Pro-Q 2: Band Slope codes (measure)", "[.][fabfilter][measure]")
{
    auto host = proQWithBand1 ("1");
    set (*host, "Band 1 Shape", "2"); // Low Cut at 1 kHz

    const auto slope = run (*host, R"({"cmd":"get","param":"Band 1 Slope"})").getProperty ("param", {});
    const auto steps = slope.getProperty ("choices", {}).size();

    for (int code = 0; code < steps; ++code)
    {
        set (*host, "Band 1 Slope", juce::String (code));
        // An octave apart, well below the cutoff: the difference is the slope.
        const auto at500 = gainAt (*host, 500.0), at250 = gainAt (*host, 250.0);
        WARN ("Slope " << code << ": 500 Hz " << at500 << " dB, 250 Hz " << at250 << " dB, per octave " << (at500 - at250));
    }
}

TEST_CASE ("Pro-Q 2: Band Stereo Placement codes (measure)", "[.][fabfilter][measure]")
{
    auto host = proQWithBand1 ("1"); // Bell at 1 kHz, -12 dB

    for (int code = 0; code < 3; ++code)
    {
        set (*host, "Band 1 Stereo Placement", juce::String (code));
        const auto mono  = stereoGain (*host, 1.0f, 1.0f);
        const auto left  = stereoGain (*host, 1.0f, 0.0f);
        const auto side  = stereoGain (*host, 1.0f, -1.0f);
        WARN ("Placement " << code << ": mono L/R " << mono.first << "/" << mono.second
              << "  left-only L " << left.first << "  side (L=-R) L/R " << side.first << "/" << side.second);
    }

    const auto channelMode = run (*host, R"({"cmd":"get","param":"Channel Mode"})").getProperty ("param", {});
    WARN ("Channel Mode now " << channelMode.getProperty ("text", {}).toString() << " of " << channelMode.getProperty ("choices", {}).size());
}

TEST_CASE ("Saturn: loads and its controls (measure)", "[.][fabfilter][measure]")
{
    HostProcessor host;
    host.prepareToPlay (sampleRate, blockSize);
    const auto reply = run (host, R"({"cmd":"load","name":"FF Saturn"})");
    INFO (juce::JSON::toString (reply));
    REQUIRE (isOk (reply));

    const auto params = run (host, R"({"cmd":"params"})");
    juce::StringArray lines;

    for (const auto& p : *params.getProperty ("params", {}).getArray())
    {
        auto line = p.getProperty ("index", 0).toString() + " " + p.getProperty ("name", {}).toString()
                  + " = " + p.getProperty ("text", {}).toString() + " " + p.getProperty ("label", {}).toString()
                  + " [" + p.getProperty ("type", {}).toString() + "]";

        if (auto* choices = p.getProperty ("choices", {}).getArray())
            line << " " << juce::StringArray::fromTokens (juce::JSON::toString (p.getProperty ("choices", {}), true), false).joinIntoString (" ").substring (0, 80);

        if (! p.getProperty ("name", {}).toString().containsIgnoreCase ("Band 2")
            && ! p.getProperty ("name", {}).toString().containsIgnoreCase ("Band 3")
            && ! p.getProperty ("name", {}).toString().containsIgnoreCase ("Band 4")
            && ! p.getProperty ("name", {}).toString().containsIgnoreCase ("Band 5")
            && ! p.getProperty ("name", {}).toString().containsIgnoreCase ("Band 6"))
            lines.add (line);
    }

    WARN ("Saturn " << (int) params.getProperty ("total", 0) << " params (bands 2-6 omitted):\n" << lines.joinIntoString ("\n"));
}

namespace
{
    // Level (dB, relative to the input tone) of one frequency in the output,
    // via the Goertzel algorithm. Used to see harmonics added by distortion.
    float levelAt (HostProcessor& host, double toneHz, double measureHz)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;
        double phase = 0.0;
        std::vector<float> out;

        for (int i = 0; i < 48; ++i)
        {
            test::fillSine (buffer, toneHz, sampleRate, phase);
            host.processBlock (buffer, midi);

            if (i >= 16) // after things settle
                out.insert (out.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        }

        const auto w = juce::MathConstants<double>::twoPi * measureHz / sampleRate;
        const auto coeff = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;

        for (auto x : out)
        {
            const auto s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        const auto power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        const auto amplitude = 2.0 * std::sqrt (juce::jmax (0.0, power)) / (double) out.size();
        return juce::Decibels::gainToDecibels ((float) (amplitude / 0.5), -200.0f);
    }
}

TEST_CASE ("Pro-Q 2: Band Slope codes are 6 12 18 24 30 36 48 72 96 dB per octave", "[.][fabfilter]")
{
    auto host = proQWithBand1 ("1");
    set (*host, "Band 1 Shape", "2"); // Low Cut at 1 kHz

    const float slopes[] = { 6, 12, 18, 24, 30, 36, 48, 72, 96 };

    for (int code = 0; code < 9; ++code)
    {
        set (*host, "Band 1 Slope", juce::String (code));
        INFO ("Slope code " << code << " should be " << slopes[code] << " dB/oct");
        // One octave below the cutoff, a cut of N dB/oct is down about N dB.
        CHECK (std::abs (gainAt (*host, 500.0) + slopes[code]) < 1.5f);
    }
}

TEST_CASE ("Pro-Q 2: Stereo Placement codes in Left/Right mode are Left Right Stereo", "[.][fabfilter]")
{
    auto host = proQWithBand1 ("1"); // Bell at 1 kHz, -12 dB

    set (*host, "Band 1 Stereo Placement", "0");
    auto g = stereoGain (*host, 1.0f, 1.0f);
    CHECK (std::abs (g.first + 12.0f) < 0.5f);
    CHECK (std::abs (g.second) < 0.5f);

    set (*host, "Band 1 Stereo Placement", "1");
    g = stereoGain (*host, 1.0f, 1.0f);
    CHECK (std::abs (g.first) < 0.5f);
    CHECK (std::abs (g.second + 12.0f) < 0.5f);

    set (*host, "Band 1 Stereo Placement", "2");
    g = stereoGain (*host, 1.0f, 1.0f);
    CHECK (std::abs (g.first + 12.0f) < 0.5f);
    CHECK (std::abs (g.second + 12.0f) < 0.5f);
}

TEST_CASE ("Pro-Q 2: Channel Mode 1 makes Placement Mid and Side (measure)", "[.][fabfilter][measure]")
{
    auto host = proQWithBand1 ("1");
    set (*host, "Channel Mode", "1");

    for (int code = 0; code < 3; ++code)
    {
        set (*host, "Band 1 Stereo Placement", juce::String (code));
        const auto mid  = stereoGain (*host, 1.0f, 1.0f);  // mono: all mid
        const auto side = stereoGain (*host, 1.0f, -1.0f); // L = -R: all side
        WARN ("Channel Mode 1, Placement " << code << ": mid content " << mid.first << " dB, side content " << side.first << " dB");
    }
}

TEST_CASE ("Saturn: Drive adds harmonics and Mix 0% stays dry", "[.][fabfilter]")
{
    HostProcessor host;
    host.prepareToPlay (sampleRate, blockSize);
    REQUIRE (isOk (run (host, R"({"cmd":"load","name":"FF Saturn"})")));

    set (host, "Band 1 Drive", "0%");
    const auto cleanH3 = levelAt (host, 500.0, 1500.0);

    set (host, "Band 1 Drive", "60%");
    const auto drivenH3 = levelAt (host, 500.0, 1500.0);
    INFO ("3rd harmonic: clean " << cleanH3 << " dB, driven " << drivenH3 << " dB");
    // Saturn's default style colours the sound even at 0% drive (3rd harmonic
    // around -36 dB); 60% drive raised it by about 18 dB when measured.
    CHECK (drivenH3 > cleanH3 + 10.0f);

    set (host, "Mix", "0%");
    const auto dryH3 = levelAt (host, 500.0, 1500.0);
    INFO ("3rd harmonic with Mix 0%: " << dryH3 << " dB");
    CHECK (dryH3 < drivenH3 - 20.0f);
}
