#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include "DSP/DelayBuffer.h"

/** MIDI-Driven Pitch Delay. На вехе M1 это ещё обычный однотапный дилей:
    хвост повторяет вход без транспонирования. Питч и голоса — M2. */
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

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

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

    /** То, что уходит в кольцо: dry + feedback. Отдельный буфер нужен потому, что
        DelayBuffer::write принимает планарные указатели, а не отдельный сэмпл. */
    juce::AudioBuffer<float> lineInput;

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

    juce::AudioProcessorParameter* bypassParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDelayProcessor)
};
