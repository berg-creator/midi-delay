#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "DSP/DelayBuffer.h"
#include "DSP/VoiceManager.h"

/** MIDI-Driven Pitch Delay. Хвост существует только пока звучит MIDI-нота: wet
    собирается из голосов, а не из постоянного отвода. Каждый голос транспонирует
    свой хвост под нажатую ноту (#14, #15): ratio считается от Root Key в момент
    note on и живёт в голосе до конца ноты. */
class MidiDelayProcessor final : public juce::AudioProcessor,
                                 private juce::AsyncUpdater
{
public:
    MidiDelayProcessor();

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "MIDI Delay"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    /** Честная оценка хвоста: по этому числу хост решает, сколько досчитывать
        после конца дорожки при офлайн-рендере. Ноль обрезал бы хвост ровно
        на последнем сэмпле. */
    double getTailLengthSeconds() const override;

    /** Как только здесь не nullptr, обёртка перестаёт звать processBlockBypassed
        и просто выставляет параметр — значит вся логика обхода живёт
        внутри processBlock. */
    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParam; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    /** Все параметры MVP заведены сразу списком, хотя используются пока четыре.
        Заводить их по одному в каждой сессии дороже, чем один раз списком. */
    juce::AudioProcessorValueTreeState apvts;

    /** Сколько MIDI-нот пришло с прошлого опроса. Читает редактор, пишет аудиопоток —
        отсюда atomic. Нужно, чтобы сразу видеть, доехал ли MIDI из хоста. */
    std::atomic<int> midiNoteCount { 0 };

    /** Последняя принятая нота и посчитанный для неё ratio. Только для окна плагина:
        по ним видно, что маппинг #15 сработал, не подключая отладчик к хосту. */
    std::atomic<int> lastNote { -1 };
    std::atomic<float> lastRatio { 1.0f };

    /** Нижний предел delay time для движка, выбранного параметром Quality (#17):
        ниже него хвост физически не может прийти, и время подтягивается вверх.
        Число берётся у движка в prepareToPlay, а не зашито здесь — у Fast и HQ
        оно разное и меняется вместе с окном. Ноль до первого prepareToPlay. */
    double getMinDelayMs() const;

    /** MIDI-нота, на которой хвост звучит в унисон с сухим, то есть ratio ровно 1.
        Нужна окну: Root Key задаёт только класс высоты, октава зашита константой,
        и без подсказки пользователь рисует мелодию не в той октаве — проверено
        на живом прогоне в FL Studio. */
    int getUnisonNote() const;

    /** Режим Time Mode. Free — дилей: хвост стартует на ноте, а поёт то, что было
        delay time назад. Follow — хвост поёт то, что звучит прямо сейчас, то есть
        мелодия повторяется нота в ноту (ADR 0006). */
    bool isFollowMode() const;

    /** Сколько плагин просит у хоста скомпенсировать, в миллисекундах. В Free
        с неотрицательным офсетом это ноль — инвариант ANALYSIS §5 цел. Отлично
        от нуля только в Follow и при отрицательном MIDI Offset. */
    double getAlignmentMs() const;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** Кусок блока между двумя MIDI-событиями: dry в кольцо, feedback, потом голоса.
        Голоса рендерятся после записи кольца — иначе им нечего было бы читать
        на коротких delay time. */
    void renderSegment (const juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                        int numChannels, float feedback);

    /** Ноты, педаль и all-notes-off. Всё незнакомое молча мимо. */
    void handleMidiMessage (const juce::MidiMessage& message);

    /** Пересчитать выравнивание и сказать его хосту. Только из потока сообщений:
        setLatencySamples дёргает хост, и звать его из processBlock нельзя. */
    void updateLatency();
    void handleAsyncUpdate() override;

    /** Насколько весь плагин отстаёт от своего входа, в сэмплах. Одно число на оба
        механизма: латентность питчера в Follow и отрицательный MIDI Offset (ADR 0006).
        Целое, потому что уходит наружу репортом и внутрь сдвигом MIDI-событий. */
    int alignmentSamples() const;

    /** Транспонирование хвоста для ноты (#15): 2^((note - root) / 12) с клампом
        по Pitch Range. Считается один раз в момент note on и живёт в голосе до
        конца ноты — смена Root Key на лету не трогает уже звучащие голоса. */
    float ratioForNote (int midiNote) const;

    /** Октава опорной ноты. Параметр Root Key задаёт только класс высоты (C..B),
        и без фиксированной октавы вычитать было бы не из чего. C3 = MIDI 60. */
    static constexpr int rootOctaveBase = 60;

    /** Запас кольца: максимальный delay time MVP (2 с) плюс место под латентность
        питчера и под длину хвоста. 4 с при 96 кГц — ~4 МБ на два канала. */
    static constexpr double maxDelaySeconds = 4.0;

    /** Запас линии сухого сигнала: максимум выравнивания это окно Follow плюс
        максимальный отрицательный MIDI Offset, то есть заведомо меньше 0,5 с. */
    static constexpr double maxAlignSeconds = 0.5;

    /** Версия формата состояния. Меняется, когда старый проект надо мигрировать,
        а не когда просто добавился параметр: незнакомые поля APVTS игнорирует сам. */
    static constexpr int stateVersion = 1;

    static constexpr double smoothingSeconds = 0.05;

    /** Обход отдельно и быстрее: 50 мс на кнопке «мимо» ощущаются вязкими,
        а мгновенный переход — это щелчок. */
    static constexpr double bypassSeconds = 0.02;

    /** Потолок оценки хвоста. При feedback 95 % и delay 2 с честные 135 кругов
        дали бы четыре с половиной минуты досчёта после каждой дорожки. */
    static constexpr double maxTailSeconds = 20.0;

    DelayBuffer delayBuffer;
    VoiceManager voiceManager;

    /** Линия сухого сигнала: держит его ровно столько же, сколько опаздывает
        обработанный. Без неё в Follow сухой шёл бы впереди хвоста на всю латентность
        питчера. Отдельное кольцо, потому что основное несёт dry + feedback. */
    DelayBuffer dryDelay;

    /** Очередь MIDI-событий, сдвинутых в будущее: положительный MIDI Offset и вся
        компенсация Follow работают именно сдвигом события, а не позиции чтения —
        слышно то, когда хвост начался, а не то, какой кусок кольца он поёт.
        Обе ёмкости резервируются в prepare, в processBlock аллокаций нет. */
    juce::MidiBuffer midiQueue, midiCarry;

    /** То, что уходит в кольцо: dry + feedback. Отдельный буфер нужен потому, что
        DelayBuffer::write принимает планарные указатели, а не отдельный сэмпл. */
    juce::AudioBuffer<float> lineInput;

    /** Сумма голосов за блок. Отдельный буфер нужен потому, что микс, гейн и обход
        считаются по сэмплу и по всему блоку сразу, а голоса приходят сегментами. */
    juce::AudioBuffer<float> wetBuffer;

    /** bypassSmoothed — доля обработанного сигнала: 1 — плагин работает, 0 — обход. */
    juce::SmoothedValue<float> delaySamplesSmoothed, mixSmoothed, gainSmoothed, bypassSmoothed;
    double currentSampleRate = 44100.0;

    // Кэш указателей на то, что реально читает processBlock. Остальные параметры
    // пока просто висят в APVTS и доступны хосту для автоматизации.
    std::atomic<float>* pDelayTime  = nullptr;
    std::atomic<float>* pFeedback   = nullptr;
    std::atomic<float>* pMix        = nullptr;
    std::atomic<float>* pOutputGain = nullptr;
    std::atomic<float>* pBypass     = nullptr;
    std::atomic<float>* pAttack     = nullptr;
    std::atomic<float>* pRelease    = nullptr;
    std::atomic<float>* pVoices     = nullptr;
    std::atomic<float>* pRootKey    = nullptr;
    std::atomic<float>* pPitchRange = nullptr;
    std::atomic<float>* pQuality    = nullptr;
    std::atomic<float>* pTimeMode   = nullptr;
    std::atomic<float>* pMidiOffset = nullptr;
    std::atomic<float>* pWidth      = nullptr;

    juce::AudioProcessorParameter* bypassParam = nullptr;

    // Предел из getMinDelayMs, посчитанный в prepareToPlay. Пишется в подготовке,
    // читается редактором — отсюда atomic. Два числа, потому что переключение
    // Quality обязано менять показ мгновенно, а не ждать следующего prepare.
    std::atomic<double> minDelayFastMs { 0.0 }, minDelayHqMs { 0.0 };

    /** Латентность движка, выбранного параметром Quality, в миллисекундах.
        Одно число для трёх вещей: нижний предел delay time, выравнивание Follow
        и сдвиг MIDI-очереди. */
    double engineLatencyMs() const;

    /** Выравнивание, о котором хост уже знает. Пишется и читается аудиопотоком;
        расхождение с посчитанным — единственный повод разбудить поток сообщений. */
    int requestedAlignment = 0;

    // Снимаются в начале блока и читаются renderSegment: аудиопоток один,
    // атомарность тут не нужна, а таскать их пятью аргументами — шум.
    int blockAlignment = 0;
    bool blockFollow = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDelayProcessor)
};
