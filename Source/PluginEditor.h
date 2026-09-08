#pragma once
#include "PluginProcessor.h"

/** Временный редактор вехи M0: показывает, что плагин жив и что MIDI до него доходит.
    Настоящий интерфейс — веха M4. */
class MidiDelayEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit MidiDelayEditor (MidiDelayProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override {}

private:
    void timerCallback() override;

    MidiDelayProcessor& proc;
    int lastCount = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDelayEditor)
};
