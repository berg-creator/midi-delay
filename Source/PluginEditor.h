#pragma once
#include "PluginProcessor.h"

/** Рабочее окно: сверху диагностика (доходит ли MIDI, какой режим, что просится
    у хоста), снизу сетка органов управления.

    Это ещё не интерфейс — интерфейс это #26 и #27, веха M4: свой LookAndFeel,
    группировка по смыслу, показ активных нот и хвостов. Здесь стоит ровно то,
    без чего плагин нельзя покрутить, не разыскивая параметры в списке хоста.

    Органы строятся по типу параметра, а не расписаны по одному: пятнадцать почти
    одинаковых блоков по шесть строк каждый — это сто строк, которые нечего читать. */
class MidiDelayEditor final : public juce::AudioProcessorEditor,
                              private juce::Timer
{
public:
    explicit MidiDelayEditor (MidiDelayProcessor&);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    /** Заводит орган под параметр: список для выбора, галку для флага, ручку
        для всего остального. Привязка к APVTS живёт в соответствующем массиве. */
    void addControl (const juce::String& parameterId);

    MidiDelayProcessor& proc;

    juce::OwnedArray<juce::Component> controls;
    juce::OwnedArray<juce::Label> captions;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::SliderAttachment> sliderLinks;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboLinks;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonLinks;

    int lastCount = -1;
    int shownNote = -2;
    int shownQuality = -1;
    bool shownClamped = false;   // delay time сейчас подтянут до предела движка (#17)
    bool shownFollow = false;    // режим Follow: хвост стоит на ноте (ADR 0006)
    int shownAlignment = -1;     // сколько мс плагин просит скомпенсировать у хоста
    bool shownSync = false;      // время задано нотной длительностью, а не ручкой (#20)
    int shownDivision = -1;      // индекс делителя
    int shownBpm = -1;           // темп хоста, округлённый: перерисовывать окно
                                 // от дрожания в сотых долях BPM незачем

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiDelayEditor)
};
