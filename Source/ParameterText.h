#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>

// Turning text like "2.5 kHz", "-4 dB", "on" or "MG Low 12" into a
// parameter's 0..1 value, checked against what the plugin itself displays.
namespace ParameterText
{
    struct Quantity
    {
        double value;
        bool hasUnit;
    };

    // Reads "2.5 kHz", "-4 dB", "120 ms", "1.2 s", "35 %" into a number in a
    // base unit (Hz, ms, dB, %), so a request and a plugin's display text can
    // be compared even when they use different prefixes. nullopt if the text
    // doesn't start with a number.
    std::optional<Quantity> parseQuantity (const juce::String& text);

    // The normalized value whose display reads `text`, or nullopt with
    // `error` set (the parameter should then be left alone).
    std::optional<float> valueForText (const juce::AudioProcessorParameter& p,
                                       const juce::String& text,
                                       juce::String& error);
}
