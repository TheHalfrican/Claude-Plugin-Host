#include "ParameterText.h"

namespace
{
    bool quantitiesMatch (double a, double b)
    {
        return std::abs (a - b) <= juce::jmax (0.01, std::abs (b) * 0.01);
    }

    juce::String shown (const juce::AudioProcessorParameter& p, float value)
    {
        return p.getText (value, 64).trim();
    }

    // The value whose display reads exactly `text` (ignoring case).
    //
    // Discrete parameters are checked step by step. Many plugins (Serum, for
    // one) don't mark their switches and menus as discrete, so the whole range
    // is also scanned with the plugin's own display, taking the middle of the
    // first stretch that shows the text: the middle is safest against
    // rounding at either edge.
    std::optional<float> findDisplayed (const juce::AudioProcessorParameter& p, const juce::String& text)
    {
        const auto wanted = text.trim();
        const auto steps = p.getNumSteps();

        if (p.isDiscrete() && steps > 1 && steps <= 4096)
            for (int i = 0; i < steps; ++i)
            {
                const auto v = (float) i / (float) (steps - 1);

                if (shown (p, v).equalsIgnoreCase (wanted))
                    return v;
            }

        constexpr int scanSteps = 1000;
        int runStart = -1, runEnd = -1;

        for (int i = 0; i <= scanSteps; ++i)
        {
            const auto matches = shown (p, (float) i / (float) scanSteps).equalsIgnoreCase (wanted);

            if (matches && runStart < 0)
                runStart = i;

            if (matches)
                runEnd = i;
            else if (runStart >= 0)
                break;
        }

        if (runStart < 0)
            return std::nullopt;

        return (float) (runStart + runEnd) * 0.5f / (float) scanSteps;
    }
}

namespace ParameterText
{
    std::optional<Quantity> parseQuantity (const juce::String& text)
    {
        auto t = text.trim();

        // Compressor ratios: "4:1" and "4.00:1" read as 4.
        if (t.endsWith (":1") && t.dropLastCharacters (2).trim().containsOnly ("0123456789.")
            && t.dropLastCharacters (2).trim().isNotEmpty())
            return Quantity { t.dropLastCharacters (2).trim().getDoubleValue(), false };

        // The unit starts at the first letter or %; 'e' is left out so
        // exponents like "1e3" stay part of the number.
        const auto numberEnd = t.indexOfAnyOf ("abcdfghijklmnopqrstuvwxyzABCDFGHIJKLMNOPQRSTUVWXYZ%");
        const auto numberPart = (numberEnd < 0 ? t : t.substring (0, numberEnd)).trim();

        // Only a plain number counts: "1/4" or "3:2" are menu entries that
        // merely look numeric, and must be matched as text.
        if (! numberPart.containsAnyOf ("0123456789") || ! numberPart.containsOnly ("0123456789.+-eE"))
            return std::nullopt;

        auto value = numberPart.getDoubleValue();
        const auto unit = (numberEnd < 0 ? juce::String() : t.substring (numberEnd)).trim().toLowerCase();

        if (unit.startsWith ("k"))
            value *= 1000.0;                       // kHz -> Hz
        else if (juce::StringArray { "s", "sec", "secs", "second", "seconds" }.contains (unit))
            value *= 1000.0;                       // s -> ms

        return Quantity { value, unit.isNotEmpty() };
    }

    // JUCE's getValueForText() only works when the plugin itself converts text
    // to values (FabFilter does; Apple's AUs don't, and JUCE then returns the
    // raw number, which clamps to the top of the range). So every result is
    // checked against what the plugin displays.
    std::optional<float> valueForText (const juce::AudioProcessorParameter& p, const juce::String& text, juce::String& error)
    {
        const auto direct = p.getValueForText (text);
        const auto directIsValid = direct >= 0.0f && direct <= 1.0f;

        // The plugin's own conversion, if its display then reads exactly what
        // was asked for ("On", "Bell", "1/4", "250.00 Hz").
        if (directIsValid && shown (p, direct).equalsIgnoreCase (text.trim()))
            return direct;

        const auto wanted = parseQuantity (text);

        if (! wanted.has_value())
        {
            // A word. Only accept it if the plugin really displays that word;
            // many plugins quietly turn unknown text into 0.
            if (auto v = findDisplayed (p, text))
                return v;

            error = "the plugin doesn't show \"" + text + "\" for this parameter";
            return std::nullopt;
        }

        // Some plugins put the unit in the text ("250.00 Hz"); Apple's AUs show
        // a bare number and keep the unit in the label ("Hz", "Secs"). If the
        // request has no unit, compare bare numbers.
        const auto label = p.getLabel();
        auto displayed = [&p, &label, &wanted] (float v) -> std::optional<double>
        {
            const auto shownText = p.getText (v, 64);
            const auto q = parseQuantity (wanted->hasUnit ? shownText + " " + label : shownText);
            return q ? std::optional<double> (q->value) : std::nullopt;
        };

        if (directIsValid)
            if (auto s = displayed (direct); s && quantitiesMatch (*s, wanted->value))
                return direct;

        // Where the display is numeric. Faders and Serum's sustain show
        // "-inf"/"-∞" at the bottom, so the numeric stretch may start (or end)
        // a little way in.
        float rangeStart = 0.0f, rangeEnd = 1.0f;
        constexpr int edgeScan = 1000;

        for (int i = 0; i <= edgeScan && ! displayed (rangeStart); ++i)
            rangeStart = (float) i / (float) edgeScan;

        for (int i = edgeScan; i >= 0 && ! displayed (rangeEnd); --i)
            rangeEnd = (float) i / (float) edgeScan;

        const auto low = displayed (rangeStart), high = displayed (rangeEnd);

        if (! low || ! high || rangeEnd <= rangeStart)
        {
            // Not a numeric parameter after all (e.g. a menu of "1/4", "1/8"
            // that only looks like numbers): fall back to an exact match.
            if (auto v = findDisplayed (p, text))
                return v;

            error = "can't read this parameter's display as a number; use a 0..1 \"value\"";
            return std::nullopt;
        }

        const auto rising = *high >= *low;
        const auto lo = juce::jmin (*low, *high), hi = juce::jmax (*low, *high);
        const auto target = wanted->value;

        if (target < lo - std::abs (lo) * 0.001 || target > hi + std::abs (hi) * 0.001)
        {
            if (auto v = findDisplayed (p, text))
                return v;

            error = "\"" + text + "\" is outside this parameter's range ("
                  + shown (p, 0.0f) + " to " + shown (p, 1.0f) + ")";
            return std::nullopt;
        }

        // Some positions show a word instead of a number (pan's "C"); look
        // just either side of them.
        auto displayedNear = [&displayed] (float v) -> std::optional<double>
        {
            for (auto nudge : { 0.0f, 1.0e-3f, -1.0e-3f, 1.0e-2f, -1.0e-2f })
                if (auto s = displayed (juce::jlimit (0.0f, 1.0f, v + nudge)))
                    return s;

            return std::nullopt;
        };

        // First value (going up the range) whose display satisfies pred.
        auto firstWhere = [&displayedNear, rangeStart, rangeEnd] (auto pred)
        {
            float a = rangeStart, b = rangeEnd;

            for (int i = 0; i < 40; ++i)
            {
                const auto mid = 0.5f * (a + b);

                if (auto s = displayedNear (mid); s && pred (*s))
                    b = mid;
                else
                    a = mid;
            }

            return b;
        };

        // Displays are rounded ("-6.0 dB" covers a stretch of values; Serum's
        // "600" covers a stretch of cutoffs). Find the stretch that shows the
        // target and take its middle: its edges round to the neighbouring
        // reading, which is how "600" once came out as "599".
        const auto eps = juce::jmax (std::abs (target) * 1.0e-9, 1.0e-12);
        float runStart, runEnd;

        if (rising)
        {
            runStart = firstWhere ([&] (double n) { return n >= target - eps; });
            runEnd   = firstWhere ([&] (double n) { return n >  target + eps; });
        }
        else
        {
            runStart = firstWhere ([&] (double n) { return n <= target + eps; });
            runEnd   = firstWhere ([&] (double n) { return n <  target - eps; });
        }

        const auto middle = 0.5f * (runStart + runEnd);

        if (auto s = displayedNear (middle); s && quantitiesMatch (*s, target))
            return middle;

        // A display that isn't monotonic (a menu of numbers) can send the
        // search astray; fall back to an exact match.
        if (auto v = findDisplayed (p, text))
            return v;

        return runStart; // the target falls between two readings: the nearest one up
    }
}
