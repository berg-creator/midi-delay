#include "PluginProcessor.h"
#include "PluginEditor.h"

MidiDelayProcessor::MidiDelayProcessor()
    : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void MidiDelayProcessor::prepareToPlay (double, int)
{
    midiNoteCount = 0;
}

bool MidiDelayProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (in.isDisabled() || out.isDisabled())
        return false;

    // Моно→стерео разрешено намеренно: источник обычно моно-вокал, а подклад широкий.
    return (in == juce::AudioChannelSet::mono()   || in  == juce::AudioChannelSet::stereo())
        && (out == juce::AudioChannelSet::mono()  || out == juce::AudioChannelSet::stereo())
        && in.size() <= out.size();
}

void MidiDelayProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    for (const auto meta : midi)
        if (meta.getMessage().isNoteOn())
            midiNoteCount.fetch_add (1, std::memory_order_relaxed);

    // DSP появится в M1. Пока аудио проходит насквозь.
}

juce::AudioProcessorEditor* MidiDelayProcessor::createEditor()
{
    return new MidiDelayEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MidiDelayProcessor();
}
