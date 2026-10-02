// Text-to-value conversion, against fake parameters that behave like real
// plugins' parameters do.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ParameterText.h"

#include <functional>

namespace
{
    // A parameter defined by its display function, like a plugin's.
    struct FakeParameter final : juce::AudioProcessorParameter
    {
        std::function<juce::String (float)> display;
        juce::String label;
        bool discrete = false;
        int steps = 0x7fffffff;

        // What the plugin's own text conversion returns. Like Apple's AUs and
        // Serum, it gets it wrong unless told otherwise.
        std::function<float (const juce::String&)> pluginConversion = [] (const juce::String&) { return 0.0f; };

        float value = 0.0f;

        float getValue() const override                         { return value; }
        void setValue (float v) override                        { value = v; }
        float getDefaultValue() const override                  { return 0.0f; }
        juce::String getName (int) const override               { return "Fake"; }
        juce::String getLabel() const override                  { return label; }
        juce::String getText (float v, int) const override      { return display (v); }
        float getValueForText (const juce::String& t) const override { return pluginConversion (t); }
        bool isDiscrete() const override                        { return discrete; }
        int getNumSteps() const override                        { return steps; }
    };

    std::optional<float> convert (const FakeParameter& p, const juce::String& text, juce::String* errorOut = nullptr)
    {
        juce::String error;
        auto v = ParameterText::valueForText (p, text, error);
        if (errorOut != nullptr)
            *errorOut = error;
        return v;
    }

    // Turns p into a menu of entries spread evenly over 0..1.
    void makeMenu (FakeParameter& p, juce::StringArray entries)
    {
        p.display = [entries] (float v)
        {
            const auto i = juce::jlimit (0, entries.size() - 1, (int) (v * (float) entries.size()));
            return entries[i];
        };
    }
}

TEST_CASE ("parseQuantity reads numbers with units into base units", "[text]")
{
    using ParameterText::parseQuantity;

    CHECK (parseQuantity ("2.5 kHz")->value == Catch::Approx (2500.0));
    CHECK (parseQuantity ("2.5k")->value == Catch::Approx (2500.0));
    CHECK (parseQuantity ("1200 Hz")->value == Catch::Approx (1200.0));
    CHECK (parseQuantity ("-4 dB")->value == Catch::Approx (-4.0));
    CHECK (parseQuantity ("-4 dB")->hasUnit);
    CHECK (parseQuantity ("1.2 s")->value == Catch::Approx (1200.0));
    CHECK (parseQuantity ("250 ms")->value == Catch::Approx (250.0));
    CHECK (parseQuantity ("35 %")->value == Catch::Approx (35.0));
    CHECK (parseQuantity ("1e3 Hz")->value == Catch::Approx (1000.0));
    CHECK_FALSE (parseQuantity ("500")->hasUnit);

    CHECK_FALSE (parseQuantity ("Bell").has_value());
    CHECK_FALSE (parseQuantity ("MG Low 12").has_value());
    CHECK_FALSE (parseQuantity ("").has_value());
    CHECK_FALSE (parseQuantity ("1/4").has_value());   // a menu entry, not 1
    CHECK_FALSE (parseQuantity ("3:2").has_value());
    CHECK (parseQuantity ("4:1")->value == Catch::Approx (4.0));      // compressor ratio
    CHECK (parseQuantity ("4.00:1")->value == Catch::Approx (4.0));
}

TEST_CASE ("An on/off switch not marked discrete is set by word (Serum style)", "[text]")
{
    FakeParameter p;
    p.display = [] (float v) { return v < 0.5f ? "off" : "on"; };

    auto on = convert (p, "on");
    REQUIRE (on.has_value());
    CHECK (p.getText (*on, 64) == "on");

    auto off = convert (p, "OFF");
    REQUIRE (off.has_value());
    CHECK (p.getText (*off, 64) == "off");
}

TEST_CASE ("A menu not marked discrete is set by entry name - from the middle of its range", "[text]")
{
    FakeParameter p;
    makeMenu (p, { "MG Low 6", "MG Low 12", "MG Low 18", "MG Low 24" });

    auto v = convert (p, "MG Low 12");
    REQUIRE (v.has_value());
    CHECK (p.getText (*v, 64) == "MG Low 12");
    CHECK (*v == Catch::Approx (0.375).margin (0.01)); // middle of 0.25..0.5, safe from rounding
}

TEST_CASE ("A menu of number-like entries is matched exactly (LFO rates)", "[text]")
{
    FakeParameter p;
    makeMenu (p, { "1/1", "1/2", "1/4", "1/8", "1/16" });

    for (auto entry : { "1/4", "1/16", "1/1" })
    {
        auto v = convert (p, entry);
        INFO (entry);
        REQUIRE (v.has_value());
        CHECK (p.getText (*v, 64) == entry);
    }
}

TEST_CASE ("A discrete parameter is matched step by step", "[text]")
{
    FakeParameter p;
    p.discrete = true;
    p.steps = 3;
    p.display = [] (float v) { return juce::StringArray { "Left", "Right", "Stereo" }[juce::roundToInt (v * 2.0f)]; };

    auto v = convert (p, "right");
    REQUIRE (v.has_value());
    CHECK (*v == Catch::Approx (0.5f));
}

TEST_CASE ("Numbers with units in the display text (FabFilter style)", "[text]")
{
    FakeParameter p;   // 20 Hz .. 20 kHz, linear, unit in the text
    p.display = [] (float v) { return juce::String (20.0f + v * 19980.0f, 1) + " Hz"; };

    auto v = convert (p, "2.5 kHz");
    REQUIRE (v.has_value());
    CHECK (p.getText (*v, 64).getFloatValue() == Catch::Approx (2500.0f).margin (25.0f));
}

TEST_CASE ("Bare numbers with the unit in the label (Apple style)", "[text]")
{
    FakeParameter p;   // 0 .. 2 seconds, unit only in the label
    p.label = "Secs";
    p.display = [] (float v) { return juce::String (v * 2.0f, 3); };

    auto v = convert (p, "250 ms");
    REQUIRE (v.has_value());
    CHECK (p.getText (*v, 64).getFloatValue() == Catch::Approx (0.25f).margin (0.005f));

    auto bare = convert (p, "1.5");   // no unit: compared as shown
    REQUIRE (bare.has_value());
    CHECK (p.getText (*bare, 64).getFloatValue() == Catch::Approx (1.5f).margin (0.005f));
}

TEST_CASE ("The plugin's own conversion is used when its display agrees", "[text]")
{
    FakeParameter p;
    makeMenu (p, { "Bell", "Low Shelf", "Low Cut" });
    p.pluginConversion = [] (const juce::String& t) { return t == "Low Cut" ? 0.9f : 0.0f; };

    auto v = convert (p, "Low Cut");
    REQUIRE (v.has_value());
    CHECK (*v == Catch::Approx (0.9f));
}

TEST_CASE ("Out-of-range numbers and unknown words are refused", "[text]")
{
    FakeParameter freq;
    freq.display = [] (float v) { return juce::String (20.0f + v * 19980.0f, 1) + " Hz"; };

    juce::String error;
    CHECK_FALSE (convert (freq, "90 kHz", &error).has_value());
    CHECK (error.contains ("outside"));

    FakeParameter sw;
    sw.display = [] (float v) { return v < 0.5f ? "off" : "on"; };
    CHECK_FALSE (convert (sw, "banana", &error).has_value());
    CHECK (error.contains ("doesn't show"));
}
