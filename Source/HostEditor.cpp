#include "HostEditor.h"

namespace
{
    const auto background = juce::Colour (0xff1e1f22);
    const auto headerColour = juce::Colour (0xff2b2d31);
    const auto accent = juce::Colour (0xffd97757);
}

HostEditor::HostEditor (HostProcessor& p)
    : AudioProcessorEditor (p), host (p)
{
    title.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, accent);
    addAndMakeVisible (title);

    status.setFont (juce::FontOptions (13.0f));
    status.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    status.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (status);

    searchBox.setTextToShowWhenEmpty ("Plugin name, e.g. FF Pro-Q 2", juce::Colours::grey);
    searchBox.onReturnKey = [this] { loadFromSearchBox(); };
    addAndMakeVisible (searchBox);

    loadButton.onClick = [this] { loadFromSearchBox(); };
    addAndMakeVisible (loadButton);

    unloadButton.onClick = [this] { host.unloadPlugin(); };
    addAndMakeVisible (unloadButton);

    host.addHostListener (this);
    rebuildInnerEditor();
}

HostEditor::~HostEditor()
{
    host.removeHostListener (this);
    innerPluginWillChange();
}

void HostEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
    g.setColour (headerColour);
    g.fillRect (getLocalBounds().removeFromTop (headerHeight));

    if (innerEditor == nullptr)
    {
        g.setColour (juce::Colours::grey);
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText ("No plugin loaded. Type a name above, or let Claude load one.",
                          getLocalBounds().withTrimmedTop (headerHeight).reduced (16),
                          juce::Justification::centred, 2);
    }
}

void HostEditor::resized()
{
    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (8, 6);

    unloadButton.setBounds (header.removeFromRight (64));
    header.removeFromRight (6);
    loadButton.setBounds (header.removeFromRight (56));
    header.removeFromRight (6);

    const auto searchWidth = juce::jmin (220, header.getWidth() / 3);
    searchBox.setBounds (header.removeFromRight (searchWidth));
    header.removeFromRight (8);

    // The name ("FF Pro-C 2 - Claude Host #1005") takes what's left; the
    // port (or a load error) sits between it and the search box.
    status.setBounds (header.removeFromRight (juce::jmin (header.getWidth() / 3, 260)));
    title.setBounds (header);

    if (innerEditor != nullptr)
        innerEditor->setTopLeftPosition (0, headerHeight);
}

void HostEditor::innerPluginWillChange()
{
    // The inner editor must go before the inner plugin it belongs to.
    if (innerEditor != nullptr)
    {
        innerEditor->removeComponentListener (this);
        removeChildComponent (innerEditor.get());
        innerEditor.reset();
    }
}

void HostEditor::innerPluginChanged()
{
    rebuildInnerEditor();
}

void HostEditor::rebuildInnerEditor()
{
    innerPluginWillChange();

    if (auto* plugin = host.getInnerPlugin())
    {
        innerEditor.reset (plugin->createEditorAndMakeActive());

        if (innerEditor == nullptr && plugin->hasEditor() == false)
            innerEditor = std::make_unique<juce::GenericAudioProcessorEditor> (*plugin);

        if (innerEditor != nullptr)
        {
            addAndMakeVisible (*innerEditor);
            innerEditor->addComponentListener (this);
        }
    }

    updateHeader();

    if (innerEditor != nullptr)
        setSize (juce::jmax (emptyWidth, innerEditor->getWidth()), headerHeight + innerEditor->getHeight());
    else
        setSize (emptyWidth, emptyHeight);

    resized();
    repaint();
}

void HostEditor::componentMovedOrResized (juce::Component& c, bool, bool wasResized)
{
    // Plugins like Pro-Q resize their own window; follow them.
    if (wasResized && &c == innerEditor.get())
        setSize (juce::jmax (emptyWidth, c.getWidth()), headerHeight + c.getHeight());
}

void HostEditor::updateHeader()
{
    title.setText (host.getDisplayName(), juce::dontSendNotification);
    status.setText ("port " + juce::String (host.getPort()), juce::dontSendNotification);
}

void HostEditor::loadFromSearchBox()
{
    const auto name = searchBox.getText().trim();

    // Return or Load with an empty box (easy to hit while clicking around
    // the plugin window) shouldn't replace the header with an error.
    if (name.isEmpty())
        return;

    const auto error = host.loadPlugin (name);

    if (error.isNotEmpty())
    {
        status.setText (error, juce::dontSendNotification);

        // Show the error briefly, then go back to the normal header.
        juce::Timer::callAfterDelay (4000, [safeThis = juce::Component::SafePointer<HostEditor> (this)]
        {
            if (safeThis != nullptr)
                safeThis->updateHeader();
        });
    }
    else
    {
        searchBox.clear();
    }
}
