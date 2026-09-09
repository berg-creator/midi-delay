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
    pBypass     = apvts.getRawParameterValue ("bypass");
    pAttack     = apvts.getRawParameterValue ("attack");
    pRelease    = apvts.getRawParameterValue ("release");
    pVoices     = apvts.getRawParameterValue ("voices");
    pRootKey    = apvts.getRawParameterValue ("rootKey");
    pPitchRange = apvts.getRawParameterValue ("pitchRange");
    pQuality    = apvts.getRawParameterValue ("quality");
    bypassParam = apvts.getParameter ("bypass");
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

    // Свой параметр обхода, а не флаг хоста: FL Studio автоматизирует только то,
    // что видит в списке параметров, а VST3-обёртка JUCE помечает этот параметр
    // как bypass именно по возврату getBypassParameter().
    params.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { "bypass", 1 }, "Bypass", false));

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

    // Движок питчинга (#38). HQ по умолчанию: varispeed расстраивает хвост неустранимо
    // (ADR 0004), и держать расстроенный звук значением по умолчанию незачем. Fast
    // остаётся для слабых машин и для того, кому нужен именно ленточный характер.
    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "quality", 1 }, "Quality",
        StringArray { "Fast", "HQ" }, 1));

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
    wetBuffer.setSize (numChannels, juce::jmax (1, maximumExpectedSamplesPerBlock),
                       false, true, false);

    voiceManager.prepare (currentSampleRate, juce::jmax (1, maximumExpectedSamplesPerBlock));
    voiceManager.reset();
    voiceManager.setQuality (pQuality->load() > 0.5f);

    // Нижний предел delay time — латентность движка (#17). Спрашивается у движка сразу
    // после его подготовки: зашитое число разъехалось бы с окном при первой же правке.
    const double msPerSample = 1000.0 / currentSampleRate;
    minDelayFastMs = voiceManager.getLatencySamples (false) * msPerSample;
    minDelayHqMs   = voiceManager.getLatencySamples (true)  * msPerSample;

    delaySamplesSmoothed.reset (currentSampleRate, smoothingSeconds);
    mixSmoothed.reset (currentSampleRate, smoothingSeconds);
    gainSmoothed.reset (currentSampleRate, smoothingSeconds);
    bypassSmoothed.reset (currentSampleRate, bypassSeconds);

    // Первый блок после prepare не должен въезжать в значения рампой.
    delaySamplesSmoothed.setCurrentAndTargetValue (
        static_cast<float> (juce::jmax (pDelayTime->load() * 0.001 * currentSampleRate,
                                        (double) voiceManager.getLatencySamples (pQuality->load() > 0.5f))));
    mixSmoothed.setCurrentAndTargetValue (pMix->load() * 0.01f);
    gainSmoothed.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (pOutputGain->load()));
    bypassSmoothed.setCurrentAndTargetValue (pBypass->load() < 0.5f ? 1.0f : 0.0f);

    // Латентность не репортим намеренно: латентность питчера будет вычтена
    // из позиции чтения, а не выставлена хосту. ANALYSIS §5.
    setLatencySamples (0);

    midiNoteCount = 0;
}

void MidiDelayProcessor::releaseResources()
{
    delayBuffer.clear();
    voiceManager.reset();
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

    const int numChannels = juce::jmin (buffer.getNumChannels(),
                                        delayBuffer.getNumChannels(),
                                        lineInput.getNumChannels(),
                                        wetBuffer.getNumChannels());

    if (numChannels <= 0 || numSamples > lineInput.getNumSamples())
        return;   // Блок больше обещанного в prepare — писать некуда, лучше пропустить.

    const float feedback = pFeedback->load (std::memory_order_relaxed) * 0.01f;

    // Quality снимается до delay time: предел на время — функция активного движка (#17).
    const bool wantHq = pQuality->load (std::memory_order_relaxed) > 0.5f;
    voiceManager.setQuality (wantHq);

    // Кламп снизу на латентность движка. Без него при коротком времени смещение чтения
    // упиралось бы в кламп внутри голоса, и хвост приходил бы позже заказанного молча —
    // ровно то, что запрещает ANALYSIS §5. Подтягивается эффективное время, а не значение
    // параметра: писать в параметр из плагина значило бы драться с автоматизацией хоста
    // и терять выставленные 120 мс при возврате на Fast. Пользователю предел виден в окне.
    delaySamplesSmoothed.setTargetValue (static_cast<float> (juce::jmax (
        pDelayTime->load (std::memory_order_relaxed) * 0.001 * currentSampleRate,
        (double) voiceManager.getLatencySamples (wantHq))));

    mixSmoothed.setTargetValue (pMix->load (std::memory_order_relaxed) * 0.01f);
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (
        pOutputGain->load (std::memory_order_relaxed)));
    bypassSmoothed.setTargetValue (pBypass->load (std::memory_order_relaxed) < 0.5f ? 1.0f : 0.0f);

    voiceManager.setEnvelope (pAttack->load (std::memory_order_relaxed),
                              pRelease->load (std::memory_order_relaxed));
    voiceManager.setVoiceLimit (static_cast<int> (pVoices->load (std::memory_order_relaxed)));

    wetBuffer.clear (0, numSamples);

    // Блок режется на сегменты по sample offset каждого события: иначе нота дрожала бы
    // на размер буфера (ANALYSIS §6.1). MidiBuffer отдаёт события уже по возрастанию.
    int segmentStart = 0;

    for (const auto meta : midi)
    {
        const int position = juce::jlimit (0, numSamples, meta.samplePosition);

        if (position > segmentStart)
        {
            renderSegment (buffer, segmentStart, position - segmentStart, numChannels, feedback);
            segmentStart = position;
        }

        handleMidiMessage (meta.getMessage());
    }

    if (segmentStart < numSamples)
        renderSegment (buffer, segmentStart, numSamples - segmentStart, numChannels, feedback);

    // Микс, гейн и обход — одним проходом по всему блоку. Сегментация на них не влияет:
    // сглаживание едет по сэмплам, и результат не зависит от того, где прошли границы.
    for (int i = 0; i < numSamples; ++i)
    {
        const float mix  = mixSmoothed.getNextValue();
        const float gain = gainSmoothed.getNextValue();
        const float wetPath = bypassSmoothed.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dry = buffer.getSample (ch, i);
            const float wet = wetBuffer.getSample (ch, i);

            const float processed = (dry * (1.0f - mix) + wet * mix) * gain;

            // Кроссфейд обхода линейный, а не equal-power: dry и processed
            // коррелированы, и equal-power дал бы горб +3 dB в середине. ADR 0003.
            // Форма именно такая: при wetPath = 1 остаётся ровно processed,
            // при 0 — ровно dry, бит-в-бит.
            buffer.setSample (ch, i, processed * wetPath + dry * (1.0f - wetPath));
        }
    }
}

void MidiDelayProcessor::renderSegment (const juce::AudioBuffer<float>& buffer,
                                        int startSample, int numSamples,
                                        int numChannels, float feedback)
{
    // Значение снимается на границе сегмента, а не по сэмплу: голос читает кольцо
    // блоком, одним смещением на весь сегмент. При статичном delay time это то же
    // самое число, и результат не зависит от размера блока.
    voiceManager.setDelaySamples (delaySamplesSmoothed.getCurrentValue());

    const float* const* linePointers = lineInput.getArrayOfReadPointers();
    const int end = startSample + numSamples;

    for (int i = startSample; i < end; ++i)
    {
        const float delaySamples = delaySamplesSmoothed.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dry = buffer.getSample (ch, i);
            // Читаем до записи текущего сэмпла, поэтому смещение точное: writePos
            // ещё указывает на слот сэмпла i, и read(d) отдаёт ровно x[i - d].
            const float delayed = delayBuffer.read (ch, delaySamples);

            // Feedback снимается ДО питч-стадии: голоса читают уже записанное кольцо,
            // и транспонирование в петлю не попадает. Инвариант из CLAUDE.md.
            lineInput.setSample (ch, i, dry + delayed * feedback);
        }

        // По сэмплу, а не блоком: при коротком delay time голова записи обгонит
        // позицию чтения внутри одного блока, и блочная запись затрёт хвост.
        delayBuffer.write (linePointers, numChannels, i, 1);
    }

    // Кольцо записано на весь сегмент — голоса отсчитывают смещение от его конца.
    voiceManager.process (wetBuffer.getArrayOfWritePointers(), numChannels,
                          startSample, numSamples, delayBuffer);
}

float MidiDelayProcessor::ratioForNote (int midiNote) const
{
    // Root Key задан классом высоты без октавы, октава зафиксирована на C3 = MIDI 60.
    // Альтернатива — считать ближайшее расстояние к нажатой ноте, но тогда нота на
    // октаву выше Root дала бы ratio 1.0, то есть ровно ничего: критерий приёмки #15
    // требует обратного. Фиксированная октава — единственный вариант, где октава слышна.
    const int rootNote = rootOctaveBase + static_cast<int> (pRootKey->load (std::memory_order_relaxed));
    const int range    = static_cast<int> (pPitchRange->load (std::memory_order_relaxed));

    // За границей Pitch Range — clamp, а не перенос октавами внутрь диапазона. Перенос
    // сохранил бы интервалы, но соседние клавиши разъезжались бы на октаву, и на слух
    // это читается как сбой, а не как настройка. Clamp просто упирается в потолок.
    const int semitones = juce::jlimit (-range, range, midiNote - rootNote);

    return std::exp2 (static_cast<float> (semitones) / 12.0f);
}

void MidiDelayProcessor::handleMidiMessage (const juce::MidiMessage& message)
{
    // isNoteOn() по умолчанию не считает нотой velocity 0, а isNoteOff() — считает.
    // Отдельная ветка под этот случай не нужна: JUCE уже развела его правильно.
    if (message.isNoteOn())
    {
        midiNoteCount.fetch_add (1, std::memory_order_relaxed);

        const int note = message.getNoteNumber();
        const float ratio = ratioForNote (note);

        lastNote.store (note, std::memory_order_relaxed);
        lastRatio.store (ratio, std::memory_order_relaxed);

        // Пан по голосам — #23, поэтому все в центре.
        voiceManager.noteOn (note, message.getFloatVelocity(), ratio, 0.0f);
    }
    else if (message.isNoteOff())
    {
        voiceManager.noteOff (message.getNoteNumber());
    }
    else if (message.isAllNotesOff() || message.isAllSoundOff())
    {
        voiceManager.allNotesOff();
    }
    else if (message.isSustainPedalOn())
    {
        voiceManager.setSustain (true);
    }
    else if (message.isSustainPedalOff())
    {
        voiceManager.setSustain (false);
    }

    // Pitch bend, aftertouch, program change, sysex и всё прочее — молча мимо.
}

double MidiDelayProcessor::getMinDelayMs() const
{
    return (pQuality->load (std::memory_order_relaxed) > 0.5f ? minDelayHqMs : minDelayFastMs)
        .load (std::memory_order_relaxed);
}

double MidiDelayProcessor::getTailLengthSeconds() const
{
    const double delaySeconds = pDelayTime->load (std::memory_order_relaxed) * 0.001;
    const double feedback     = pFeedback->load (std::memory_order_relaxed) * 0.01;

    // Сколько кругов до -60 dB: feedback^n = 0.001. При нулевом feedback круг ровно один.
    const double rounds = feedback > 0.001 ? std::log (0.001) / std::log (feedback) : 1.0;

    return juce::jmin (delaySeconds * rounds, maxTailSeconds);
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
