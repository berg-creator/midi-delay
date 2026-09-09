#include "PluginProcessor.h"
#include "PluginEditor.h"

MidiDelayProcessor::MidiDelayProcessor()
    : AudioProcessor (BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MidiDelayState", createParameterLayout())
{
    pDelayTime  = apvts.getRawParameterValue ("delayTime");
    pFeedback   = apvts.getRawParameterValue ("feedback");
    pMix        = apvts.getRawParameterValue ("mix");
    pOutputGain = apvts.getRawParameterValue ("outputGain");
}

//==============================================================================
// Параметры. Плоский список намеренно: фабрика или макрос вокруг пятнадцати
// однотипных строк читается хуже, чем сами строки, и правится с раскопками.
// Все подписи — ASCII по-английски, кириллица в UI рассыпается (см. CLAUDE.md).
juce::AudioProcessorValueTreeState::ParameterLayout MidiDelayProcessor::createParameterLayout()
{
    using namespace juce;
    using Range = NormalisableRange<float>;

    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "delayTime", 1 }, "Delay Time",
        Range { 1.0f, 2000.0f, 0.01f, 0.35f }, 400.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));

    params.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { "sync", 1 }, "Tempo Sync", false));

    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "division", 1 }, "Note Division",
        StringArray { "1/1", "1/2.", "1/2", "1/2T", "1/4.", "1/4", "1/4T",
                      "1/8.", "1/8", "1/8T", "1/16.", "1/16", "1/16T" }, 5));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "feedback", 1 }, "Feedback",
        Range { 0.0f, 95.0f, 0.1f }, 35.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "mix", 1 }, "Mix",
        Range { 0.0f, 100.0f, 0.1f }, 50.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "outputGain", 1 }, "Output Gain",
        Range { -24.0f, 12.0f, 0.1f }, 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "filterLo", 1 }, "Low Cut",
        Range { 20.0f, 2000.0f, 1.0f, 0.35f }, 100.0f,
        AudioParameterFloatAttributes().withLabel ("Hz")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "filterHi", 1 }, "High Cut",
        Range { 200.0f, 20000.0f, 1.0f, 0.35f }, 12000.0f,
        AudioParameterFloatAttributes().withLabel ("Hz")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "width", 1 }, "Width",
        Range { 0.0f, 200.0f, 1.0f }, 100.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "rootKey", 1 }, "Root Key",
        StringArray { "C", "C#", "D", "D#", "E", "F",
                      "F#", "G", "G#", "A", "A#", "B" }, 0));

    params.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { "pitchRange", 1 }, "Pitch Range", 1, 24, 12,
        AudioParameterIntAttributes().withLabel ("st")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "attack", 1 }, "Attack",
        Range { 1.0f, 500.0f, 0.1f, 0.4f }, 10.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "release", 1 }, "Release",
        Range { 5.0f, 2000.0f, 0.1f, 0.4f }, 300.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));

    params.push_back (std::make_unique<AudioParameterInt> (
        ParameterID { "voices", 1 }, "Voices", 1, 8, 8));

    // Ручной калибровочный винт под MIDI-роутинг FL Studio: см. ANALYSIS §6.2.
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "midiOffset", 1 }, "MIDI Offset",
        Range { -50.0f, 50.0f, 0.1f }, 0.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));

    return { params.begin(), params.end() };
}

//==============================================================================
void MidiDelayProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    const int numChannels = juce::jmax (1, getTotalNumOutputChannels());

    // Единственное место аллокации. После prepare кольцо нулевое — смена sample rate
    // на лету обрывает хвост тишиной, а не мусором, и потому не щёлкает.
    delayBuffer.prepare (currentSampleRate, numChannels, maxDelaySeconds);
    lineInput.setSize (numChannels, juce::jmax (1, maximumExpectedSamplesPerBlock),
                       false, true, false);

    delaySamplesSmoothed.reset (currentSampleRate, smoothingSeconds);
    mixSmoothed.reset (currentSampleRate, smoothingSeconds);
    gainSmoothed.reset (currentSampleRate, smoothingSeconds);

    // Первый блок после prepare не должен въезжать в значения рампой.
    delaySamplesSmoothed.setCurrentAndTargetValue (
        static_cast<float> (pDelayTime->load() * 0.001 * currentSampleRate));
    mixSmoothed.setCurrentAndTargetValue (pMix->load() * 0.01f);
    gainSmoothed.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (pOutputGain->load()));

    // Латентность не репортим намеренно: латентность питчера будет вычтена
    // из позиции чтения, а не выставлена хосту. ANALYSIS §5.
    setLatencySamples (0);

    midiNoteCount = 0;
}

void MidiDelayProcessor::releaseResources()
{
    delayBuffer.clear();
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

    const int numSamples = buffer.getNumSamples();
    const int numInputs  = getTotalNumInputChannels();

    // Моно→стерео: хост даёт двухканальный буфер, но заполнен в нём только левый.
    // Правый пришёл с чужим мусором, его надо не просто занулить, а продублировать —
    // иначе dry-сигнал справа пропадёт.
    if (numInputs <= 0)
        buffer.clear();
    else
        for (int ch = numInputs; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);

    for (const auto meta : midi)
        if (meta.getMessage().isNoteOn())
            midiNoteCount.fetch_add (1, std::memory_order_relaxed);

    const int numChannels = juce::jmin (buffer.getNumChannels(),
                                        delayBuffer.getNumChannels(),
                                        lineInput.getNumChannels());

    if (numChannels <= 0 || numSamples > lineInput.getNumSamples())
        return;   // Блок больше обещанного в prepare — писать некуда, лучше пропустить.

    const float feedback = pFeedback->load (std::memory_order_relaxed) * 0.01f;

    delaySamplesSmoothed.setTargetValue (static_cast<float> (
        pDelayTime->load (std::memory_order_relaxed) * 0.001 * currentSampleRate));
    mixSmoothed.setTargetValue (pMix->load (std::memory_order_relaxed) * 0.01f);
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (
        pOutputGain->load (std::memory_order_relaxed)));

    const float* const* linePointers = lineInput.getArrayOfReadPointers();

    for (int i = 0; i < numSamples; ++i)
    {
        const float delaySamples = delaySamplesSmoothed.getNextValue();
        const float mix  = mixSmoothed.getNextValue();
        const float gain = gainSmoothed.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dry = buffer.getSample (ch, i);
            // Читаем до записи текущего сэмпла, поэтому смещение точное: writePos
            // ещё указывает на слот сэмпла i, и read(d) отдаёт ровно x[i - d].
            const float delayed = delayBuffer.read (ch, delaySamples);

            // Feedback снимается ДО питч-стадии (её пока нет, но точка отбора уже здесь):
            // иначе каждый круг транспонировал бы хвост заново. Инвариант из CLAUDE.md.
            lineInput.setSample (ch, i, dry + delayed * feedback);

            buffer.setSample (ch, i, (dry * (1.0f - mix) + delayed * mix) * gain);
        }

        // По сэмплу, а не блоком: при коротком delay time голова записи обгонит
        // позицию чтения внутри одного блока, и блочная запись затрёт хвост.
        delayBuffer.write (linePointers, numChannels, i, 1);
    }
}

//==============================================================================
void MidiDelayProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("stateVersion", stateVersion, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MidiDelayProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // getXmlFromBinary сам отсеивает мусор и обрезанные данные — вернёт nullptr.
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    // Версия пока одна, и незнакомые поля APVTS игнорирует сама. Когда появится
    // вторая — развилка миграции встанет ровно сюда, до replaceState.
    apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessorEditor* MidiDelayProcessor::createEditor()
{
    return new MidiDelayEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MidiDelayProcessor();
}
