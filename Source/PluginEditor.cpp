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

    // Текст интерфейса — только ASCII: juce::String трактует обычный литерал
    // как ASCII, и любая кириллица превращается в мусор. См. CLAUDE.md.
    const auto count = juce::jmax (0, lastCount);
    auto area = getLocalBounds().withTrimmedTop (95);

    g.setColour (count > 0 ? juce::Colours::limegreen : juce::Colours::grey);
    g.setFont (juce::FontOptions (15.0f));
    g.drawText (count > 0 ? "MIDI OK - notes received: " + juce::String (count)
                          : "No MIDI input",
                area.removeFromTop (24), juce::Justification::centredTop, false);

    if (count == 0)
    {
        g.setColour (juce::Colours::grey.withAlpha (0.7f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Set the same MIDI port in MIDI Out and in the wrapper settings",
                    area.removeFromTop (20), juce::Justification::centredTop, false);
    }
}
