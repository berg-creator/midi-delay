#pragma once
#include <vector>

/** Общий кольцевой буфер записи: пишут все, читают все голоса со своими смещениями.
    Планарный, без интерливинга. Аллокация только в prepare — задача #7.

    Намеренно не зависит от JUCE: так офлайн-тесты собираются одним clang.
    Указатели float* const* совместимы с AudioBuffer::getArrayOfReadPointers(). */
class DelayBuffer
{
public:
    /** Единственное место, где выделяется память. maxDelaySeconds должен включать
        запас на латентность питчера и на максимальную длину хвоста. */
    void prepare (double sampleRate, int numChannels, double maxDelaySeconds);
    void clear();

    /** Пишет numSamples из input[ch] начиная с startSample и двигает голову записи.
        Вызывается по сегментам между MIDI-событиями, поэтому startSample есть. */
    void write (const float* const* input, int numChannels, int startSample, int numSamples);

    /** Чтение назад от головы записи на delaySamples. Дробная позиция, интерполяция
        не ниже 3-го порядка (#8): линейной мало, ридер ходит с переменной скоростью.
        delaySamples клампится в допустимый диапазон, за пределы буфера не выходит. */
    float read (int channel, double delaySamples) const;

    int getCapacitySamples() const;
    int getNumChannels() const;

private:
    std::vector<float> data;   // numChannels блоков по capacity сэмплов подряд
    int capacity = 0;
    int numChannels = 0;
    int writePos = 0;
};
