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
    pFormants   = apvts.getRawParameterValue ("formants");
    pTimeMode   = apvts.getRawParameterValue ("timeMode");
    pMidiOffset = apvts.getRawParameterValue ("midiOffset");
    pWidth      = apvts.getRawParameterValue ("width");
    pDiffusion  = apvts.getRawParameterValue ("diffusion");
    pModulation = apvts.getRawParameterValue ("modulation");
    pDucking    = apvts.getRawParameterValue ("ducking");
    pFilterLo   = apvts.getRawParameterValue ("filterLo");
    pFilterHi   = apvts.getRawParameterValue ("filterHi");
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

    // Режим времени. Free — обычный дилей. Follow — хвост поёт то, что звучит прямо
    // сейчас: мелодия повторяется нота в ноту, латентность питчера уходит в репорт
    // хосту (ADR 0006). Tempo Sync встанет сюда третьим пунктом задачей #20.
    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "timeMode", 1 }, "Time Mode",
        StringArray { "Free", "Follow" }, 0));

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

    // Плотность диффузии хвоста (#45): 0 — цепочка алл-пассов обойдена целиком
    // и выход бит-в-бит совпадает с простым дилеем, дальше растёт коэффициент g.
    // Потолок 0,7 — выше алл-пасс Шрёдера начинает звенеть металлом.
    // Значение по умолчанию выбрано на слух: пользователь послушал рендеры сессии 12,
    // на 60 % звона не услышал и попросил для умолчания вариант понейтральнее.
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "diffusion", 1 }, "Diffusion",
        Range { 0.0f, 100.0f, 0.1f }, 50.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    // Модуляция времени (#46). Крутится всегда, подмешивается глубиной: на нуле
    // ручки позиция чтения ровно та же, что была, бит-в-бит. Модулируется петля,
    // а не позиция чтения голосов — почему именно так, в комментарии renderSegment.
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "modulation", 1 }, "Modulation",
        Range { 0.0f, 100.0f, 0.1f }, 20.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    // Приседание хвоста под сухим (#47). 50 % по умолчанию — тоже по слуху: рендеры
    // с приседанием и без пользователь описал как «каша в обоих, но с приседанием
    // лучше». Ноль был бы честнее как «плагин ничего не делает молча», но на голосе
    // с частыми повторами хвост без приседания дерётся с сухим за то же место.
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "ducking", 1 }, "Ducking",
        Range { 0.0f, 100.0f, 0.1f }, 50.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

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

    // Форманты (#24). Shift по умолчанию — и это решение уха против замера. Замер
    // говорит однозначно: Hold держит формантную область на месте, Shift уносит её
    // вверх вдвое, то есть делает ровно тот «бурундук», ради которого задача заведена.
    // Пользователь послушал оба рендера и выбрал Shift. Правдоподобное объяснение:
    // Hold на октаве вверх тише на 6 dB и темнее — компенсация срезает верх, который
    // при Shift оставался, — и хвост уходит в кашу вместо того, чтобы читаться слоем.
    // Для фонового слоя яркость важнее анатомической правильности голоса.
    // Hold остаётся ручкой: на длинных нотах и небольших сдвигах он ближе к голосу.
    // Оба работают только на HQ: варигонка растягивает спектр целиком, формант для неё
    // физически не существует. Разница Fast и HQ — это разница характера.
    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "formants", 1 }, "Formants",
        StringArray { "Shift", "Hold" }, 0));

    // Ручной калибровочный винт под MIDI-роутинг FL Studio: см. ANALYSIS §6.2.
    // Диапазон ±100 как в #18. Плюс — событие держится в очереди и стоит только её;
    // минус — сухой сигнал придерживается на столько же, и это уходит в репорт
    // латентности хосту. То есть отрицательная половина не бесплатна (ADR 0006).
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "midiOffset", 1 }, "MIDI Offset",
        Range { -100.0f, 100.0f, 0.1f }, 0.0f,
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

    // Линии сухого сигнала нужен ещё и запас в один блок хоста: читается она
    // от головы записи, а голова к моменту чтения уже прошла весь блок.
    dryDelay.prepare (currentSampleRate, numChannels,
                      maxAlignSeconds + juce::jmax (1, maximumExpectedSamplesPerBlock) / currentSampleRate);

    lineInput.setSize (numChannels, juce::jmax (1, maximumExpectedSamplesPerBlock),
                       false, true, false);
    wetBuffer.setSize (numChannels, juce::jmax (1, maximumExpectedSamplesPerBlock),
                       false, true, false);

    // Единственное место, где очередь MIDI выделяет память. Событие в MidiBuffer
    // это девять байт; килобайта хватает на сотню событий в блоке с запасом.
    midiQueue.clear();
    midiCarry.clear();
    midiQueue.ensureSize (1024);
    midiCarry.ensureSize (1024);

    // Диффузор и фильтры петли (#45, #22). После prepare оба нулевые: смена sample rate
    // на лету обрывает хвост тишиной, а не мусором, как и само кольцо.
    diffuser.prepare (currentSampleRate, numChannels);
    diffuser.clear();
    loState[0] = loState[1] = hiState[0] = hiState[1] = 0.0f;

    // Дрейф и детектор приседания (#46, #47). Фазы с нуля, огибающая с нуля:
    // после prepare плагин обязан быть в том же состоянии, что после конструктора.
    modPhaseA = modPhaseB = 0.0;
    duckEnv[0] = duckEnv[1] = 0.0f;
    duckAttackCoeff  = onePoleCoeff (static_cast<float> (1000.0 / (juce::MathConstants<double>::twoPi * duckAttackMs)));
    duckReleaseCoeff = onePoleCoeff (static_cast<float> (1000.0 / (juce::MathConstants<double>::twoPi * duckReleaseMs)));

    voiceManager.prepare (currentSampleRate, juce::jmax (1, maximumExpectedSamplesPerBlock));
    voiceManager.reset();
    voiceManager.setEngine (pQuality->load() > 0.5f ? PitchEngine::hq : PitchEngine::fast);

    // Нижний предел delay time — латентность движка (#17). Спрашивается у движка сразу
    // после его подготовки: зашитое число разъехалось бы с окном при первой же правке.
    const double msPerSample = 1000.0 / currentSampleRate;
    minDelayFastMs = voiceManager.getLatencySamples (PitchEngine::fast) * msPerSample;
    minDelayHqMs   = voiceManager.getLatencySamples (PitchEngine::hq)   * msPerSample;

    delaySamplesSmoothed.reset (currentSampleRate, smoothingSeconds);
    mixSmoothed.reset (currentSampleRate, smoothingSeconds);
    gainSmoothed.reset (currentSampleRate, smoothingSeconds);
    bypassSmoothed.reset (currentSampleRate, bypassSeconds);
    diffusionSmoothed.reset (currentSampleRate, smoothingSeconds);
    modDepthSmoothed.reset (currentSampleRate, smoothingSeconds);
    duckDepthSmoothed.reset (currentSampleRate, smoothingSeconds);
    loCoeffSmoothed.reset (currentSampleRate, smoothingSeconds);
    hiCoeffSmoothed.reset (currentSampleRate, smoothingSeconds);

    // Первый блок после prepare не должен въезжать в значения рампой.
    delaySamplesSmoothed.setCurrentAndTargetValue (
        static_cast<float> (juce::jmax (pDelayTime->load() * 0.001 * currentSampleRate,
                                        getMinDelayMs() * 0.001 * currentSampleRate)));
    mixSmoothed.setCurrentAndTargetValue (pMix->load() * 0.01f);
    gainSmoothed.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (pOutputGain->load()));
    bypassSmoothed.setCurrentAndTargetValue (pBypass->load() < 0.5f ? 1.0f : 0.0f);
    diffusionSmoothed.setCurrentAndTargetValue (pDiffusion->load() * 0.01f);
    modDepthSmoothed.setCurrentAndTargetValue (pModulation->load() * 0.01f);
    duckDepthSmoothed.setCurrentAndTargetValue (pDucking->load() * 0.01f);
    loCoeffSmoothed.setCurrentAndTargetValue (onePoleCoeff (pFilterLo->load()));
    hiCoeffSmoothed.setCurrentAndTargetValue (onePoleCoeff (pFilterHi->load()));

    // В Free с неотрицательным офсетом здесь ноль, и инвариант ANALYSIS §5 цел:
    // латентность питчера вычитается из позиции чтения, а не выставляется хосту.
    // Отлично от нуля это число только там, где прятать латентность физически
    // некуда, — в Follow и при отрицательном MIDI Offset (ADR 0006).
    updateLatency();

    midiNoteCount = 0;
}

void MidiDelayProcessor::releaseResources()
{
    delayBuffer.clear();
    dryDelay.clear();
    diffuser.clear();
    loState[0] = loState[1] = hiState[0] = hiState[1] = 0.0f;
    duckEnv[0] = duckEnv[1] = 0.0f;
    midiQueue.clear();
    midiCarry.clear();
    voiceManager.reset();
}

//==============================================================================
float MidiDelayProcessor::onePoleCoeff (float frequencyHz) const
{
    // a = 1 - exp(-2*pi*f/fs). Кламп сверху нужен на низких sample rate: срез,
    // заехавший за Найквиста, дал бы a > 1 и раскачку однополюсника.
    const auto a = 1.0 - std::exp (-juce::MathConstants<double>::twoPi
                                   * juce::jmax (0.0f, frequencyHz) / currentSampleRate);

    return static_cast<float> (juce::jlimit (0.0, 1.0, a));
}

double MidiDelayProcessor::engineLatencyMs() const
{
    return (pQuality->load (std::memory_order_relaxed) > 0.5f ? minDelayHqMs : minDelayFastMs)
        .load (std::memory_order_relaxed);
}

int MidiDelayProcessor::alignmentSamples() const
{
    // Follow: питчеру негде спрятать своё окно, дилея под ним нет. Отрицательный
    // офсет: событие раньше сухого сигнала сделать нельзя, можно только придержать
    // сухой. Оба слагаемых складываются, и сумма уходит хосту (ADR 0006).
    const double followLatency = isFollowMode()
        ? engineLatencyMs() * 0.001 * currentSampleRate : 0.0;

    const double offset = pMidiOffset->load (std::memory_order_relaxed) * 0.001 * currentSampleRate;

    return static_cast<int> (std::lround (followLatency + std::max (0.0, -offset)));
}

void MidiDelayProcessor::updateLatency()
{
    setLatencySamples (alignmentSamples());
}

void MidiDelayProcessor::handleAsyncUpdate()
{
    updateLatency();
}

int MidiDelayProcessor::getUnisonNote() const
{
    return rootOctaveBase + static_cast<int> (pRootKey->load (std::memory_order_relaxed));
}

bool MidiDelayProcessor::isFollowMode() const
{
    return pTimeMode->load (std::memory_order_relaxed) > 0.5f;
}

double MidiDelayProcessor::getAlignmentMs() const
{
    return alignmentSamples() * 1000.0 / currentSampleRate;
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

    // Двойка тут не косметика: состояния фильтров петли — массивы на два канала,
    // и шире стерео раскладка не бывает (isBusesLayoutSupported). Кламп стоит на
    // границе доверия к хосту, а не в горячем цикле.
    const int numChannels = juce::jmin (2, buffer.getNumChannels(),
                                        delayBuffer.getNumChannels(),
                                        juce::jmin (lineInput.getNumChannels(),
                                                    wetBuffer.getNumChannels()));

    if (numChannels <= 0 || numSamples > lineInput.getNumSamples())
        return;   // Блок больше обещанного в prepare — писать некуда, лучше пропустить.

    const float feedback = pFeedback->load (std::memory_order_relaxed) * 0.01f;

    // Режим и Quality снимаются до delay time: предел на время — функция движка (#17),
    // а движок в Follow навязан режимом, а не параметром качества.
    const bool follow = isFollowMode();
    const bool wantHq = pQuality->load (std::memory_order_relaxed) > 0.5f;

    voiceManager.setEngine (wantHq ? PitchEngine::hq : PitchEngine::fast);
    voiceManager.setWidth (pWidth->load (std::memory_order_relaxed));
    voiceManager.setFormantHold (pFormants->load (std::memory_order_relaxed) > 0.5f);

    // Выравнивание: на сколько сэмплов весь плагин отстаёт от собственного входа.
    // Сухой сигнал придерживается ровно на столько же, и это же число просится
    // у хоста. Считается по параметрам, а не накапливается, — одна формула на все
    // случаи (ADR 0006). Целое: им двигаются MIDI-события и репорт латентности.
    blockAlignment = alignmentSamples();
    blockFollow = follow;

    const double offsetSamples = pMidiOffset->load (std::memory_order_relaxed)
                               * 0.001 * currentSampleRate;

    // Сдвиг события в будущее. Компенсация Follow и положительный офсет — это
    // именно задержка события, а не смещение позиции чтения: на слух слышно то,
    // когда хвост начался, а сдвиг чтения этого как раз не трогает (ADR 0006).
    const int midiShift = static_cast<int> (std::lround (
        (follow ? engineLatencyMs() * 0.001 * currentSampleRate : 0.0)
        + std::max (0.0, offsetSamples)));

    // Хост узнаёт о смене выравнивания из потока сообщений: setLatencySamples дёргает
    // обёртку и хост, и звать его отсюда нельзя.
    if (blockAlignment != requestedAlignment)
    {
        requestedAlignment = blockAlignment;
        triggerAsyncUpdate();
    }

    // Кламп снизу на латентность движка. Без него при коротком времени смещение чтения
    // упиралось бы в кламп внутри голоса, и хвост приходил бы позже заказанного молча —
    // ровно то, что запрещает ANALYSIS §5. Подтягивается эффективное время, а не значение
    // параметра: писать в параметр из плагина значило бы драться с автоматизацией хоста
    // и терять выставленные 120 мс при возврате на Fast. Пользователю предел виден в окне.
    // В Follow предела нет: голос читает кольцо по выравниванию, а delay time там задаёт
    // только интервал повторов обратной связи.
    delaySamplesSmoothed.setTargetValue (static_cast<float> (juce::jmax (
        pDelayTime->load (std::memory_order_relaxed) * 0.001 * currentSampleRate,
        getMinDelayMs() * 0.001 * currentSampleRate)));

    mixSmoothed.setTargetValue (pMix->load (std::memory_order_relaxed) * 0.01f);
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (
        pOutputGain->load (std::memory_order_relaxed)));
    bypassSmoothed.setTargetValue (pBypass->load (std::memory_order_relaxed) < 0.5f ? 1.0f : 0.0f);

    // Окраска петли (#45, #22). Фильтры на краях диапазона обходятся целиком:
    // «выключено» обязано значить выключено, а не «однополюсник на 20 кГц».
    const float loHz = pFilterLo->load (std::memory_order_relaxed);
    const float hiHz = pFilterHi->load (std::memory_order_relaxed);

    blockUseLo = loHz > filterLoOff;
    blockUseHi = hiHz < filterHiOff;

    diffusionSmoothed.setTargetValue (pDiffusion->load (std::memory_order_relaxed) * 0.01f);
    modDepthSmoothed.setTargetValue (pModulation->load (std::memory_order_relaxed) * 0.01f);
    duckDepthSmoothed.setTargetValue (pDucking->load (std::memory_order_relaxed) * 0.01f);
    loCoeffSmoothed.setTargetValue (onePoleCoeff (loHz));
    hiCoeffSmoothed.setTargetValue (onePoleCoeff (hiHz));

    voiceManager.setEnvelope (pAttack->load (std::memory_order_relaxed),
                              pRelease->load (std::memory_order_relaxed));
    voiceManager.setVoiceLimit (static_cast<int> (pVoices->load (std::memory_order_relaxed)));

    wetBuffer.clear (0, numSamples);

    // Сухой путь в свою линию — до того, как микс перепишет buffer. Читается он
    // назад на blockAlignment, и при нуле выравнивания линия просто не опрашивается:
    // сухой остаётся бит-в-бит собой, и обход по ADR 0003 не трогается.
    dryDelay.write (buffer.getArrayOfReadPointers(), numChannels, 0, numSamples);

    // Входящие события уезжают в очередь со сдвигом; играются те, что попали в этот
    // блок, остальные ждут следующего. При нулевом сдвиге это ровно исходный буфер.
    midiQueue.addEvents (midi, 0, numSamples, midiShift);

    // Блок режется на сегменты по sample offset каждого события: иначе нота дрожала бы
    // на размер буфера (ANALYSIS §6.1). MidiBuffer отдаёт события уже по возрастанию.
    int segmentStart = 0;

    for (const auto meta : midiQueue)
    {
        if (meta.samplePosition >= numSamples)
            break;   // события будущих блоков; очередь упорядочена, дальше только они

        const int position = juce::jmax (0, meta.samplePosition);

        if (position > segmentStart)
        {
            renderSegment (buffer, segmentStart, position - segmentStart, numChannels, feedback);
            segmentStart = position;
        }

        handleMidiMessage (meta.getMessage());
    }

    if (segmentStart < numSamples)
        renderSegment (buffer, segmentStart, numSamples - segmentStart, numChannels, feedback);

    // Хвост очереди переезжает к началу следующего блока. swapWith, а не присваивание:
    // оба буфера сохраняют выделенную в prepare ёмкость, и аллокаций тут нет.
    midiCarry.clear();
    midiCarry.addEvents (midiQueue, numSamples, -1, -numSamples);
    midiQueue.swapWith (midiCarry);

    // Микс, гейн и обход — одним проходом по всему блоку. Сегментация на них не влияет:
    // сглаживание едет по сэмплам, и результат не зависит от того, где прошли границы.
    for (int i = 0; i < numSamples; ++i)
    {
        const float mix  = mixSmoothed.getNextValue();
        const float gain = gainSmoothed.getNextValue();
        const float wetPath = bypassSmoothed.getNextValue();
        const float duckDepth = duckDepthSmoothed.getNextValue();   // вне цикла каналов: рампа одна на сэмпл

        for (int ch = 0; ch < numChannels; ++ch)
        {
            // Голова записи линии стоит за концом блока, поэтому сэмплу i
            // соответствует смещение (numSamples - i) плюс само выравнивание.
            const float dry = blockAlignment > 0
                ? dryDelay.read (ch, static_cast<double> (numSamples - i + blockAlignment))
                : buffer.getSample (ch, i);

            // Ducking (#47). Детектор снимает ровно тот сухой, который уходит в микс,
            // а не сырой вход. Это расхождение с промптом сессии, и оно намеренное:
            // в Follow весь плагин опаздывает от входа на выравнивание, и хвост тоже —
            // значит детектор на сыром входе присел бы на 180 мс раньше сухого, то есть
            // до того, как слог вообще прозвучал. Взятый после линии, он совпадает
            // с сухим в обоих режимах Time Mode, а это и есть критерий задачи.
            const float rectified = std::abs (dry);
            duckEnv[ch] += (rectified > duckEnv[ch] ? duckAttackCoeff : duckReleaseCoeff)
                         * (rectified - duckEnv[ch]);

            // На нуле ручки множитель ровно единица — выход бит-в-бит прежний.
            const float duck = 1.0f - duckDepth * duckingMaxDepth
                             * juce::jmin (1.0f, duckEnv[ch] * duckingSensitivity);

            const float wet = wetBuffer.getSample (ch, i) * duck;

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
    // Позиция чтения голоса = выравнивание + время дилея; латентность питчера
    // вычтет сам голос. В Follow времени нет: голос читает ровно то, что звучит
    // сейчас, и весь его отступ — это выравнивание (ADR 0006).
    voiceManager.setDelaySamples (blockAlignment
                                  + (blockFollow ? 0.0 : delaySamplesSmoothed.getCurrentValue()));

    const float* const* linePointers = lineInput.getArrayOfReadPointers();
    const int end = startSample + numSamples;

    // Шаг фаз дрейфа за сэмпл. Считается на сегмент, а не на сэмпл: частоты — константы.
    const double modStepA = modulationRateA / currentSampleRate;
    const double modStepB = modulationRateB / currentSampleRate;
    const auto modScale = static_cast<float> (modulationMaxMs * 0.001 * currentSampleRate);

    for (int i = startSample; i < end; ++i)
    {
        const float delaySamples = delaySamplesSmoothed.getNextValue();

        // Модуляция времени (#46). Модулируется позиция чтения ПЕТЛИ, а не голосов,
        // и это решение, а не умолчание. Дрожание, поданное на вход питчера, фазовый
        // вокодер воспроизвёл бы честно — то есть оно стало бы уходом высоты, а критерий
        // задачи требует ровно обратного: «не читается как расстройка». В петле же
        // дрейф расцепляет круги между собой, накапливается от повтора к повтору
        // и высоты хвоста не трогает вовсе — тот самый ленточный характер.
        //
        // Генераторы крутятся всегда и подмешиваются глубиной, как диффузор:
        // на нуле ручки позиция чтения ровно прежняя, бит-в-бит, и включение
        // не даёт скачка (находки сессии 12).
        modPhaseA += modStepA;
        modPhaseB += modStepB;
        if (modPhaseA >= 1.0) modPhaseA -= 1.0;
        if (modPhaseB >= 1.0) modPhaseB -= 1.0;

        const auto drift = static_cast<float> (
            0.6 * std::sin (juce::MathConstants<double>::twoPi * modPhaseA)
          + 0.4 * std::sin (juce::MathConstants<double>::twoPi * modPhaseB));

        // Смещение только в плюс, то есть петля дышит в сторону удлинения. Знакопеременное
        // на минимальном delay time упиралось бы в нижний предел кольца, и дрейф стал бы
        // односторонним сам — молча и только на коротких временах. Цена честного варианта:
        // постоянная составляющая в полглубины, до 2 мс на полной ручке. Против 400 мс
        // петли это не слышно, а вот разное поведение на разных временах слышно бы было.
        const float modOffset = modDepthSmoothed.getNextValue() * modScale * (0.5f * (drift + 1.0f));

        // Диффузия (#45). Цепочка алл-пассов крутится всегда, даже на нуле ручки:
        // так она остаётся прогретой, и ввод её в петлю не даёт ни щелчка, ни всплеска
        // застоявшегося звука. Подмешивается она множителем blend, и на нуле ручки
        // в петлю уходит ровно недиффузированный сигнал — бит-в-бит.
        const float amount = diffusionSmoothed.getNextValue();
        const float diffusionGain = amount * diffusionMaxGain;
        const float blend = juce::jmin (1.0f, amount * diffusionBlendSlope);

        const float loCoeff = loCoeffSmoothed.getNextValue();
        const float hiCoeff = hiCoeffSmoothed.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float dry = buffer.getSample (ch, i);
            // Читаем до записи текущего сэмпла, поэтому смещение точное: writePos
            // ещё указывает на слот сэмпла i, и read(d) отдаёт ровно x[i - d].
            const float delayed = delayBuffer.read (ch, delaySamples + modOffset);

            // Вторая точка чтения — та же петля, укороченная на длину цепочки:
            // алл-пасс при любом g несёт свои M сэмплов задержки, и без этого вычета
            // интервал повторов уехал бы примерно на 60 мс. Кламп снизу упирается
            // в предел DelayBuffer и работает только там, где петля короче цепочки, —
            // то есть в Follow на совсем малом времени. ponytail: там интервал повторов
            // упирается в длину цепочки; отдельные длины под короткую петлю — если
            // такой режим кому-то понадобится.
            const float shifted = delayBuffer.read (ch, juce::jmax (2.0f,
                delaySamples + modOffset - static_cast<float> (diffuser.getDelaySamples (ch))));

            const float diffused = diffuser.process (ch, shifted, diffusionGain);

            // Feedback снимается ДО питч-стадии: голоса читают уже записанное кольцо,
            // и транспонирование в петлю не попадает. Инвариант из CLAUDE.md.
            float lineIn = dry + (delayed + blend * (diffused - delayed)) * feedback;

            // Фильтры стоят на входе кольца, а не на выходе wet (#22). На выходе это
            // был бы просто эквалайзер; здесь первый хвост окрашен один раз, второй
            // два, и хвост темнеет с каждым кругом — то, ради чего фильтры в дилее
            // и нужны. Однополюсные и без резонанса: резонансный биквад в петле
            // при feedback 95 % — классический способ получить свист.
            if (blockUseHi)
            {
                hiState[ch] += hiCoeff * (lineIn - hiState[ch]);
                lineIn = hiState[ch];
            }

            if (blockUseLo)
            {
                loState[ch] += loCoeff * (lineIn - loState[ch]);
                lineIn -= loState[ch];
            }

            lineInput.setSample (ch, i, lineIn);
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

        // Пан выбирает пул по номеру слота (#23): снаружи этот номер неизвестен.
        voiceManager.noteOn (note, message.getFloatVelocity(), ratio);
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
    // В Follow предела нет: голос читает по выравниванию, а delay time там задаёт
    // только интервал повторов обратной связи, и коротким ему быть можно.
    if (isFollowMode())
        return 0.0;

    const double latency = engineLatencyMs();

    // Отрицательный офсет придерживает сухой сигнал, а значит освобождает ровно
    // столько же в бюджете позиции чтения — предел едет вниз вместе с ним.
    const double offsetMs = pMidiOffset->load (std::memory_order_relaxed);

    return std::max (0.0, latency - std::max (0.0, -offsetMs));
}

double MidiDelayProcessor::getTailLengthSeconds() const
{
    const double delaySeconds = pDelayTime->load (std::memory_order_relaxed) * 0.001;
    const double feedback     = pFeedback->load (std::memory_order_relaxed) * 0.01;

    // Сколько кругов до -60 dB: feedback^n = 0.001. При нулевом feedback круг ровно один.
    const double rounds = feedback > 0.001 ? std::log (0.001) / std::log (feedback) : 1.0;

    // Круг длиннее заказанного времени на среднее смещение дрейфа — половину глубины (#46).
    const double modSeconds = pModulation->load (std::memory_order_relaxed) * 0.01
                            * modulationMaxMs * 0.0005;

    // Диффузия удлиняет затухание: алл-пасс добавляет петле фазу, то есть на части частот
    // удлиняет её круг, и при feedback 95 % замеренное затухание идёт как 0,97 за круг
    // вместо 0,95 (находки сессии 12). В кругах это log(0,95)/log(0,97) = 1,7 раза.
    // Без этой поправки хост при офлайн-сведении обрезает хвост раньше, чем тот стих.
    const double diffusion = pDiffusion->load (std::memory_order_relaxed) * 0.01;

    return juce::jmin ((delaySeconds + modSeconds) * rounds * (1.0 + 0.7 * diffusion),
                       maxTailSeconds);
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
