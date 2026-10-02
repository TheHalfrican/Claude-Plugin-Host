#include "HostProcessor.h"
#include "HostEditor.h"

#include <optional>
#include <unistd.h>

namespace
{
    constexpr int maxTag = 9999;
    constexpr int messageThreadTimeoutMs = 20000;

    juce::var ok (juce::DynamicObject* payload = nullptr)
    {
        auto* reply = payload != nullptr ? payload : new juce::DynamicObject();
        reply->setProperty ("ok", true);
        return juce::var (reply);
    }

    juce::var fail (const juce::String& message)
    {
        auto* reply = new juce::DynamicObject();
        reply->setProperty ("ok", false);
        reply->setProperty ("error", message);
        return juce::var (reply);
    }

    juce::DynamicObject* object() { return new juce::DynamicObject(); }

    struct Quantity
    {
        double value;
        bool hasUnit;
    };

    // Reads "2.5 kHz", "-4 dB", "120 ms", "1.2 s", "35 %" into a number in a
    // base unit (Hz, ms, dB, %), so a request and a plugin's display text can
    // be compared even when they use different prefixes.
    std::optional<Quantity> parseQuantity (const juce::String& text)
    {
        const auto t = text.trim();
        // The unit starts at the first letter or %; 'e' is left out so
        // exponents like "1e3" stay part of the number.
        const auto numberEnd = t.indexOfAnyOf ("abcdfghijklmnopqrstuvwxyzABCDFGHIJKLMNOPQRSTUVWXYZ%");
        const auto numberPart = (numberEnd < 0 ? t : t.substring (0, numberEnd)).trim();

        if (! numberPart.containsAnyOf ("0123456789"))
            return std::nullopt;

        auto value = numberPart.getDoubleValue();
        const auto unit = (numberEnd < 0 ? juce::String() : t.substring (numberEnd)).trim().toLowerCase();

        if (unit.startsWith ("k"))
            value *= 1000.0;                       // kHz -> Hz
        else if (juce::StringArray { "s", "sec", "secs", "second", "seconds" }.contains (unit))
            value *= 1000.0;                       // s -> ms

        return Quantity { value, unit.isNotEmpty() };
    }

    bool quantitiesMatch (double a, double b)
    {
        return std::abs (a - b) <= juce::jmax (0.01, std::abs (b) * 0.01);
    }

    // Finds the normalized value whose display text reads `text`.
    //
    // JUCE's getValueForText() only works when the plugin itself converts
    // text to values (FabFilter does; Apple's AUs don't, and JUCE then
    // returns the raw number, which clamps to the top of the range). So the
    // result is checked against what the plugin displays, and if it doesn't
    // match, the range is searched using the plugin's own display text.
    std::optional<float> valueForText (const juce::AudioProcessorParameter& p, const juce::String& text, juce::String& error)
    {
        const auto direct = p.getValueForText (text);
        const auto wanted = parseQuantity (text);

        if (! wanted.has_value())
        {
            // A word ("On", "Bell"). Only accept it if the plugin really
            // displays that word; otherwise many plugins quietly turn unknown
            // text into 0.
            if (direct >= 0.0f && direct <= 1.0f && p.getText (direct, 64).trim().equalsIgnoreCase (text.trim()))
                return direct;

            const auto steps = p.getNumSteps();

            if (p.isDiscrete() && steps > 1 && steps <= 4096)
                for (int i = 0; i < steps; ++i)
                {
                    const auto v = (float) i / (float) (steps - 1);

                    if (p.getText (v, 64).trim().equalsIgnoreCase (text.trim()))
                        return v;
                }

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
            const auto shown = parseQuantity (wanted->hasUnit ? shownText + " " + label : shownText);
            return shown ? std::optional<double> (shown->value) : std::nullopt;
        };

        if (direct >= 0.0f && direct <= 1.0f)
            if (auto shown = displayed (direct); shown && quantitiesMatch (*shown, wanted->value))
                return direct;

        const auto low = displayed (0.0f), high = displayed (1.0f);

        if (! low || ! high)
        {
            error = "can't read this parameter's display as a number; use a 0..1 \"value\"";
            return std::nullopt;
        }

        const auto rising = *high >= *low;
        const auto lo = juce::jmin (*low, *high), hi = juce::jmax (*low, *high);

        const auto target = wanted->value;

        if (target < lo - std::abs (lo) * 0.001 || target > hi + std::abs (hi) * 0.001)
        {
            error = "\"" + text + "\" is outside this parameter's range ("
                  + p.getText (0.0f, 64) + " to " + p.getText (1.0f, 64) + ")";
            return std::nullopt;
        }

        // Bisection, assuming the display is monotonic in the value.
        float a = 0.0f, b = 1.0f;

        for (int i = 0; i < 40; ++i)
        {
            const auto mid = 0.5f * (a + b);
            const auto shown = displayed (mid);

            if (! shown)
                break;

            if ((*shown < target) == rising)
                a = mid;
            else
                b = mid;
        }

        return 0.5f * (a + b);
    }

    // Whether this host should offer a plugin. AU identifiers carry the
    // component type ("AudioUnit:Synths/...", "AudioUnit:Effects/..."), so
    // AUs can be sorted without loading them. VST3 paths don't say, so
    // they're always offered (AU is preferred anyway).
    bool suitsThisHost (const juce::String& format, const juce::String& identifier)
    {
        if (format != "AudioUnit")
            return true;

        const auto isInstrument = identifier.contains (":Synths/");

       #if CLAUDE_HOST_IS_SYNTH
        return isInstrument;
       #else
        return ! isInstrument && ! identifier.contains (":MidiEffects/");
       #endif
    }

    // AU first: most of the user's plugins are AUv2 and their sets use AU.
    int formatRank (const juce::String& format)
    {
        return format == "AudioUnit" ? 0 : format == "VST3" ? 1 : 2;
    }
}

//==============================================================================
juce::AudioProcessor::BusesProperties HostProcessor::defaultBuses()
{
   #if CLAUDE_HOST_IS_SYNTH
    return BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true);
   #else
    return BusesProperties().withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                            .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
   #endif
}

HostProcessor::HostProcessor()
    : AudioProcessor (defaultBuses()),
      server ([this] (const juce::var& request) { return handleRequest (request); })
{
    juce::addDefaultFormatsToManager (formatManager);

    // The tag lets a client match this instance to a device in Live: Live's
    // API can read this (tiny) wrapper's own parameters, even though it can't
    // see the inner plugin's.
    addParameter (tagParameter = new juce::AudioParameterInt (juce::ParameterID { "instanceTag", 1 },
                                                              "Instance Tag", 0, maxTag, 0));
    ensureUniqueTag();

    server.start();
    publishRegistry();
}

HostProcessor::~HostProcessor()
{
    shuttingDown = true; // lets an in-flight request stop waiting
    server.stop();
    registry.remove();

    if (inner != nullptr)
        inner->removeListener (this);

    const juce::ScopedLock sl (innerLock);
    inner.reset();
}

int HostProcessor::getTag() const noexcept
{
    return tagParameter->get();
}

void HostProcessor::ensureUniqueTag()
{
    auto& random = juce::Random::getSystemRandom();
    auto tag = tagParameter->get();

    for (int attempt = 0; (tag == 0 || registry.isTagUsedByAnotherInstance (tag)) && attempt < 50; ++attempt)
        tag = 1 + random.nextInt (maxTag);

    *tagParameter = tag;
}

//==============================================================================
void HostProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;
    isPrepared = true;

    scratch.setSize (32, samplesPerBlock, false, true, true);

    const juce::ScopedLock sl (innerLock);

    if (inner != nullptr)
        prepareInner (*inner);
}

void HostProcessor::releaseResources()
{
    isPrepared = false;

    const juce::ScopedLock sl (innerLock);

    if (inner != nullptr)
        inner->releaseResources();
}

bool HostProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();

    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;

   #if CLAUDE_HOST_IS_SYNTH
    return true;
   #else
    return layouts.getMainInputChannelSet() == out;
   #endif
}

void HostProcessor::prepareInner (juce::AudioPluginInstance& plugin)
{
    // Ask for the same main layout as ours; plugins that refuse keep their own.
    auto layout = plugin.getBusesLayout();

    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference (0) = getChannelLayoutOfBus (false, 0);

   #if CLAUDE_HOST_IS_SYNTH
    // An instrument host has no audio input, so switch off the synth's
    // inputs (sidechain etc.) where it allows that.
    auto noInputs = layout;

    for (auto& bus : noInputs.inputBuses)
        bus = juce::AudioChannelSet::disabled();

    if (! plugin.setBusesLayout (noInputs))
        plugin.setBusesLayout (layout);
   #else
    if (! layout.inputBuses.isEmpty())
        layout.inputBuses.getReference (0) = getChannelLayoutOfBus (true, 0);

    plugin.setBusesLayout (layout);
   #endif
    plugin.setRateAndBufferSizeDetails (currentSampleRate, currentBlockSize);
    plugin.setNonRealtime (isNonRealtime());
    plugin.prepareToPlay (currentSampleRate, currentBlockSize);

    setLatencySamples (plugin.getLatencySamples());
}

void HostProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Never block the audio thread: if a load/unload holds the lock, skip
    // processing for this block (pass audio through, or silence for synths).
    const juce::ScopedTryLock sl (innerLock);

    if (! sl.isLocked() || inner == nullptr)
    {
       #if CLAUDE_HOST_IS_SYNTH
        buffer.clear();
       #endif
        return;
    }

    inner->setPlayHead (getPlayHead());

   #if CLAUDE_HOST_IS_SYNTH
    // Live's buffer for an instrument isn't guaranteed to be silent; the
    // synth renders into it, so anything left over must not reach the
    // synth's inputs or get mixed into its output.
    buffer.clear();
   #endif

    const auto numSamples = buffer.getNumSamples();
    const auto innerChannels = juce::jmax (inner->getTotalNumInputChannels(), inner->getTotalNumOutputChannels());

    if (innerChannels <= buffer.getNumChannels())
    {
        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), innerChannels, numSamples);
        inner->processBlock (view, midi);
        return;
    }

    // The inner plugin wants more channels than Live gave us (e.g. a disabled
    // sidechain counted in its layout): run it on scratch space, copy back ours.
    if (innerChannels > scratch.getNumChannels() || numSamples > scratch.getNumSamples())
        return;

    juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), innerChannels, numSamples);
    view.clear();

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        view.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    inner->processBlock (view, midi);

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom (ch, 0, view, ch, 0, numSamples);
}

double HostProcessor::getTailLengthSeconds() const
{
    return inner != nullptr ? inner->getTailLengthSeconds() : 0.0;
}

juce::AudioProcessorEditor* HostProcessor::createEditor()
{
    return new HostEditor (*this);
}

void HostProcessor::audioProcessorChanged (AudioProcessor* changed, const ChangeDetails& details)
{
    if (changed == inner.get() && details.latencyChanged)
        setLatencySamples (inner->getLatencySamples());
}

//==============================================================================
// Loading

const juce::Array<HostProcessor::AvailablePlugin>& HostProcessor::getAvailablePlugins()
{
    if (availableScanned)
        return availablePlugins;

    availableScanned = true;

    for (auto* format : formatManager.getFormats())
    {
        const auto ids = format->searchPathsForPlugins (format->getDefaultLocationsToSearch(), true, false);

        for (const auto& id : ids)
        {
            // Don't offer ourselves: a host inside a host inside a host...
            if (id.contains ("Clde") || id.containsIgnoreCase ("Claude Host"))
                continue;

            if (! suitsThisHost (format->getName(), id))
                continue;

            availablePlugins.add ({ format->getNameOfPluginFromIdentifier (id), format->getName(), id });
        }
    }

    std::sort (availablePlugins.begin(), availablePlugins.end(), [] (const auto& a, const auto& b)
    {
        const auto byName = a.name.compareIgnoreCase (b.name);
        return byName != 0 ? byName < 0 : formatRank (a.format) < formatRank (b.format);
    });

    return availablePlugins;
}

juce::String HostProcessor::loadPlugin (const juce::String& nameQuery, const juce::String& formatQuery)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    const auto query = nameQuery.trim();

    if (query.isEmpty())
        return "no plugin name given";

    // Exact name first, then a unique substring match; AU preferred either way.
    const AvailablePlugin* exact = nullptr;
    juce::Array<const AvailablePlugin*> partial;

    for (const auto& p : getAvailablePlugins())
    {
        if (formatQuery.isNotEmpty() && ! p.format.equalsIgnoreCase (formatQuery))
            continue;

        if (p.name.equalsIgnoreCase (query))
        {
            if (exact == nullptr || formatRank (p.format) < formatRank (exact->format))
                exact = &p;
        }
        else if (p.name.containsIgnoreCase (query))
        {
            partial.add (&p);
        }
    }

    const AvailablePlugin* chosen = exact;

    if (chosen == nullptr)
    {
        juce::StringArray distinctNames;

        for (auto* p : partial)
            distinctNames.addIfNotAlreadyThere (p->name);

        if (distinctNames.isEmpty())
            return "no plugin matches \"" + query + "\"";

        if (distinctNames.size() > 1)
            return "\"" + query + "\" matches several plugins: " + distinctNames.joinIntoString (", ");

        for (auto* p : partial)
            if (chosen == nullptr || formatRank (p->format) < formatRank (chosen->format))
                chosen = p;
    }

    juce::OwnedArray<juce::PluginDescription> types;

    for (auto* format : formatManager.getFormats())
        if (format->getName() == chosen->format)
            format->findAllTypesForFile (types, chosen->identifier);

    if (types.isEmpty())
        return "couldn't read plugin info for " + chosen->name;

    return loadPlugin (*types.getFirst());
}

juce::String HostProcessor::loadPlugin (const juce::PluginDescription& description)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    juce::String error;
    auto created = formatManager.createPluginInstance (description, currentSampleRate, currentBlockSize, error);

    if (created == nullptr)
        return error.isNotEmpty() ? error : "couldn't create " + description.name;

    installInner (std::move (created));
    return {};
}

void HostProcessor::unloadPlugin()
{
    installInner (nullptr);
}

void HostProcessor::installInner (std::unique_ptr<juce::AudioPluginInstance> newInner)
{
    // Prepare before taking the lock, so the audio thread is only ever
    // skipped for the instant of the pointer swap.
    if (newInner != nullptr && isPrepared)
        prepareInner (*newInner);

    listeners.call ([] (Listener& l) { l.innerPluginWillChange(); });

    std::unique_ptr<juce::AudioPluginInstance> old;

    {
        const juce::ScopedLock sl (innerLock);
        old = std::move (inner);
        inner = std::move (newInner);
    }

    if (old != nullptr)
        old->removeListener (this);

    if (inner != nullptr)
        inner->addListener (this);
    else
        setLatencySamples (0);

    old.reset(); // destroyed here, on the message thread, outside the lock

    publishRegistry();
    updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    listeners.call ([] (Listener& l) { l.innerPluginChanged(); });
}

//==============================================================================
// State

void HostProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::XmlElement root ("ClaudeHost");
    root.setAttribute ("version", 1);
    root.setAttribute ("tag", getTag());

    // The inner plugin is only ever swapped on the message thread, so reading
    // it there needs no lock. Taking the lock would make the audio thread skip
    // a block (an audible gap) every time Live saves or autosaves.
    const auto onMessageThread = juce::MessageManager::getInstance()->isThisTheMessageThread();
    const juce::ScopedLock sl (onMessageThread ? noLock : innerLock);

    if (inner != nullptr)
    {
        if (auto description = inner->getPluginDescription().createXml())
            root.addChildElement (description.release());

        juce::MemoryBlock innerState;
        inner->getStateInformation (innerState);
        root.createNewChildElement ("InnerState")->addTextElement (innerState.toBase64Encoding());
    }

    copyXmlToBinary (root, destData);
}

void HostProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto root = getXmlFromBinary (data, sizeInBytes);

    if (root == nullptr || ! root->hasTagName ("ClaudeHost"))
        return;

    *tagParameter = root->getIntAttribute ("tag", getTag());
    ensureUniqueTag(); // a duplicated track restores the same tag

    juce::PluginDescription description;
    const auto* descriptionXml = root->getChildByName ("PLUGIN");
    const auto hasPlugin = descriptionXml != nullptr && description.loadFromXml (*descriptionXml);

    juce::MemoryBlock innerState;

    if (auto* stateXml = root->getChildByName ("InnerState"))
        innerState.fromBase64Encoding (stateXml->getAllSubText());

    auto restore = [weakThis = juce::WeakReference<HostProcessor> (this), hasPlugin, description, innerState]
    {
        auto* self = weakThis.get();

        if (self == nullptr)
            return;

        if (! hasPlugin)
        {
            self->unloadPlugin();
            return;
        }

        if (self->loadPlugin (description).isEmpty() && innerState.getSize() > 0)
            if (auto* plugin = self->getInnerPlugin())
                plugin->setStateInformation (innerState.getData(), (int) innerState.getSize());

        self->publishRegistry();
    };

    // Plugins must be created on the message thread.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        restore();
    else
        juce::MessageManager::callAsync (restore);
}

void HostProcessor::updateTrackProperties (const TrackProperties& properties)
{
    trackName = properties.name.value_or (juce::String());

    juce::MessageManager::callAsync ([weakThis = juce::WeakReference<HostProcessor> (this)]
    {
        if (auto* self = weakThis.get())
            self->publishRegistry();
    });
}

void HostProcessor::publishRegistry()
{
    auto* info = object();
    info->setProperty ("pid", (int) ::getpid());
    info->setProperty ("port", getPort());
    info->setProperty ("tag", getTag());
    info->setProperty ("product", juce::String (JucePlugin_Name));
    info->setProperty ("version", juce::String (JucePlugin_VersionString));
    info->setProperty ("track", trackName);
    info->setProperty ("plugin", inner != nullptr ? inner->getName() : juce::String());
    info->setProperty ("updated", juce::Time::getCurrentTime().toISO8601 (true));
    registry.publish (juce::var (info));
}

//==============================================================================
// Control requests

juce::var HostProcessor::handleRequest (const juce::var& request)
{
    // Called on the server thread. Everything that touches the inner plugin
    // runs on the message thread, where plugins expect to be called.
    auto result = std::make_shared<juce::var>();
    auto done = std::make_shared<juce::WaitableEvent>();

    juce::MessageManager::callAsync ([weakThis = juce::WeakReference<HostProcessor> (this), request, result, done]
    {
        if (auto* self = weakThis.get())
            *result = self->runCommand (request);
        else
            *result = fail ("plugin instance is closing");

        done->signal();
    });

    for (int waited = 0; ! done->wait (50); waited += 50)
    {
        if (shuttingDown)
            return fail ("plugin instance is closing");

        if (waited >= messageThreadTimeoutMs)
            return fail ("timed out waiting for Live's message thread");
    }

    return *result;
}

juce::var HostProcessor::runCommand (const juce::var& request)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

    const auto command = request.getProperty ("cmd", {}).toString();

    if (command == "info")
        return describe();

    if (command == "list_plugins")
        return listPlugins (request.getProperty ("query", {}).toString());

    if (command == "load")
    {
        const auto error = loadPlugin (request.getProperty ("name", {}).toString(),
                                       request.getProperty ("format", {}).toString());
        return error.isEmpty() ? describe() : fail (error);
    }

    if (command == "unload")
    {
        unloadPlugin();
        return describe();
    }

    if (inner == nullptr)
        return fail ("no plugin loaded; send {\"cmd\":\"load\",\"name\":\"...\"} first");

    if (command == "params")
        return listParameters (request.getProperty ("filter", {}).toString());

    if (command == "get")
    {
        juce::String error;
        const auto key = request.getProperty ("param", {});

        if (auto* p = findParameter (key, error))
        {
            auto* reply = object();
            reply->setProperty ("param", describeParameter (p->getParameterIndex(), *p));
            return ok (reply);
        }

        return fail (error);
    }

    if (command == "set")
        return applySetRequest (request);

    if (command == "set_many")
    {
        // Several changes in one round trip, applied in order (order matters:
        // Pro-Q 2 ignores a band's settings until its State is on). Each
        // change reports its own result; one failure doesn't stop the rest.
        const auto* changes = request.getProperty ("changes", {}).getArray();

        if (changes == nullptr || changes->isEmpty())
            return fail ("give \"changes\": a list of {\"param\": ..., \"value\" or \"text\": ...}");

        juce::Array<juce::var> results;
        bool allOk = true;

        for (const auto& change : *changes)
        {
            const auto result = change.isObject() ? applySetRequest (change) : fail ("each change must be an object");
            allOk = allOk && (bool) result.getProperty ("ok", false);
            results.add (result);
        }

        auto* reply = object();
        reply->setProperty ("results", results);
        reply->setProperty ("allOk", allOk);
        return ok (reply);
    }

    if (command == "programs")
        return listPrograms();

    if (command == "set_program")
    {
        const int index = request.getProperty ("index", -1);

        if (! juce::isPositiveAndBelow (index, inner->getNumPrograms()))
            return fail ("program index out of range (0.." + juce::String (inner->getNumPrograms() - 1) + ")");

        inner->setCurrentProgram (index);
        return listPrograms();
    }

    return fail ("unknown cmd \"" + command + "\" (try info, list_plugins, load, unload, params, get, set, set_many, programs, set_program)");
}

juce::var HostProcessor::describe() const
{
    auto* reply = object();
    reply->setProperty ("product", juce::String (JucePlugin_Name));
    reply->setProperty ("version", juce::String (JucePlugin_VersionString));
    reply->setProperty ("tag", getTag());
    reply->setProperty ("port", getPort());
    reply->setProperty ("track", trackName);

    if (inner != nullptr)
    {
        const auto description = inner->getPluginDescription();
        auto* plugin = object();
        plugin->setProperty ("name", description.name);
        plugin->setProperty ("manufacturer", description.manufacturerName);
        plugin->setProperty ("format", description.pluginFormatName);
        plugin->setProperty ("version", description.version);
        plugin->setProperty ("numParameters", inner->getParameters().size());
        plugin->setProperty ("numPrograms", inner->getNumPrograms());
        plugin->setProperty ("latencySamples", inner->getLatencySamples());
        reply->setProperty ("plugin", juce::var (plugin));
    }

    return ok (reply);
}

juce::var HostProcessor::listPlugins (const juce::String& query)
{
    juce::Array<juce::var> items;

    for (const auto& p : getAvailablePlugins())
    {
        if (query.isNotEmpty() && ! p.name.containsIgnoreCase (query))
            continue;

        auto* item = object();
        item->setProperty ("name", p.name);
        item->setProperty ("format", p.format);
        items.add (juce::var (item));
    }

    auto* reply = object();
    reply->setProperty ("plugins", items);
    return ok (reply);
}

juce::var HostProcessor::describeParameter (int index, juce::AudioProcessorParameter& p)
{
    auto* item = object();
    item->setProperty ("index", index);
    item->setProperty ("name", p.getName (128));
    item->setProperty ("value", p.getValue());
    item->setProperty ("text", p.getCurrentValueAsText());
    item->setProperty ("label", p.getLabel());
    item->setProperty ("default", p.getDefaultValue());

    if (p.isBoolean())
        item->setProperty ("type", "boolean");
    else if (p.isDiscrete())
        item->setProperty ("type", "discrete");
    else
        item->setProperty ("type", "continuous");

    const auto steps = p.getNumSteps();

    if (p.isDiscrete() && steps > 1 && steps <= 64)
    {
        // Spell out every choice so a client can pick by name.
        juce::StringArray choices;

        for (int i = 0; i < steps; ++i)
            choices.add (p.getText ((float) i / (float) (steps - 1), 64));

        item->setProperty ("choices", choices);
    }

    return juce::var (item);
}

juce::var HostProcessor::listParameters (const juce::String& filter) const
{
    juce::Array<juce::var> items;
    const auto& params = inner->getParameters();

    for (int i = 0; i < params.size(); ++i)
    {
        auto* p = params.getUnchecked (i);

        if (filter.isNotEmpty() && ! p->getName (128).containsIgnoreCase (filter))
            continue;

        items.add (describeParameter (i, *p));
    }

    auto* reply = object();
    reply->setProperty ("plugin", inner->getName());
    reply->setProperty ("total", params.size());
    reply->setProperty ("params", items);
    return ok (reply);
}

juce::AudioProcessorParameter* HostProcessor::findParameter (const juce::var& key, juce::String& error) const
{
    const auto& params = inner->getParameters();

    if (key.isInt() || key.isInt64() || key.isDouble())
    {
        const int index = key;

        if (juce::isPositiveAndBelow (index, params.size()))
            return params.getUnchecked (index);

        error = "parameter index out of range (0.." + juce::String (params.size() - 1) + ")";
        return nullptr;
    }

    const auto name = key.toString().trim();

    if (name.isEmpty())
    {
        error = "give \"param\" as a name or index";
        return nullptr;
    }

    juce::Array<juce::AudioProcessorParameter*> partial;

    for (auto* p : params)
    {
        const auto pName = p->getName (128);

        if (pName.equalsIgnoreCase (name))
            return p;

        if (pName.containsIgnoreCase (name))
            partial.add (p);
    }

    if (partial.size() == 1)
        return partial.getFirst();

    if (partial.isEmpty())
    {
        error = "no parameter named \"" + name + "\"";
        return nullptr;
    }

    juce::StringArray names;

    for (int i = 0; i < juce::jmin (partial.size(), 12); ++i)
        names.add (partial[i]->getName (128));

    error = "\"" + name + "\" matches " + juce::String (partial.size()) + " parameters: "
          + names.joinIntoString (", ") + (partial.size() > 12 ? ", ..." : "");
    return nullptr;
}

juce::var HostProcessor::applySetRequest (const juce::var& request)
{
    juce::String error;
    auto* p = findParameter (request.getProperty ("param", {}), error);

    if (p == nullptr)
        return fail (error);

    float target = 0.0f;
    const auto value = request.getProperty ("value", {});
    const auto text = request.getProperty ("text", {});

    if (! text.isVoid())
    {
        // Real units, as the plugin displays them ("2.5 kHz", "-3 dB", "On").
        juce::String textError;
        const auto converted = valueForText (*p, text.toString(), textError);

        if (! converted.has_value())
            return fail (textError);

        target = *converted;
    }
    else if (! value.isVoid())
    {
        target = (float) (double) value;

        if (target < 0.0f || target > 1.0f)
            return fail ("\"value\" is normalized 0..1; use \"text\" for real units");
    }
    else
    {
        return fail ("give \"value\" (0..1) or \"text\" (e.g. \"2.5 kHz\")");
    }

    p->beginChangeGesture();
    p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, target));
    p->endChangeGesture();

    // Read back what the plugin actually kept: switches and stepped
    // parameters snap, and the client should see the real result.
    auto* reply = object();
    reply->setProperty ("requested", target);
    reply->setProperty ("param", describeParameter (p->getParameterIndex(), *p));
    return ok (reply);
}

juce::var HostProcessor::listPrograms() const
{
    juce::StringArray names;

    for (int i = 0; i < inner->getNumPrograms(); ++i)
        names.add (inner->getProgramName (i));

    auto* reply = object();
    reply->setProperty ("current", inner->getCurrentProgram());
    reply->setProperty ("programs", names);
    return ok (reply);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HostProcessor();
}
