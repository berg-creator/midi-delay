#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "DSP/DelayBuffer.h"
#include "DSP/VoiceManager.h"

/** MIDI-Driven Pitch Delay. Хвост существует только пока звучит MIDI-нота: wet
    собирается из голосов, а не из постоянного отвода. Каждый голос транспонирует
    свой хвост под нажатую ноту (#14, #15): ratio считается от Root Key в момент
    note on и живёт в голосе до конца ноты. */
class MidiDelayProcessor final : public juce::AudioProcessor
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

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** Кусок блока между двумя MIDI-событиями: dry в кольцо, feedback, потом голоса.
        Голоса рендерятся после записи кольца — иначе им нечего было бы читать
        на коротких delay time. */
    void renderSegment (const juce::AudioBuffer<float>& buffer, int startSample, int numSamples,
                        int numChannels, float feedback);

    /** Ноты, педаль и all-notes-off. Всё незнакомое молча мимо. */
    void handleMidiMessage (const juce::MidiMessage& message);

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

    juce::AudioProcessorParameter* bypassParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDelayProcessor)
};
