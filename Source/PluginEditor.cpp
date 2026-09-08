#include "PluginEditor.h"

MidiDelayEditor::MidiDelayEditor (MidiDelayProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (420, 180);
    startTimerHz (15);
}

void MidiDelayEditor::timerCallback()
{
    const auto count = proc.midiNoteCount.load (std::memory_order_relaxed);

    if (count != lastCount)
    {
        lastCount = count;
        repaint();
    }
}

void MidiDelayEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1a1e));

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (22.0f));
    g.drawText ("MIDI Delay", getLocalBounds().removeFromTop (90),
                juce::Justification::centredBottom, false);

    const auto count = juce::jmax (0, lastCount);

    g.setColour (count > 0 ? juce::Colours::limegreen : juce::Colours::grey);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (count > 0 ? "MIDI доходит — нот принято: " + juce::String (count)
                          : "MIDI не приходит",
                getLocalBounds().withTrimmedTop (95),
                juce::Justification::centredTop, false);
}
