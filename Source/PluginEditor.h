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
    int shownNote = -2;
    int shownQuality = -1;
    bool shownClamped = false;   // delay time сейчас подтянут до предела движка (#17)
    bool shownFollow = false;    // режим Follow: хвост стоит на ноте (ADR 0006)
    int shownAlignment = -1;     // сколько мс плагин просит скомпенсировать у хоста

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDelayEditor)
};
