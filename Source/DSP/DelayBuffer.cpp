#include "DelayBuffer.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace
{
    /** Минимально допустимое смещение чтения.

        Снизу мешает не y[-1], как можно подумать, а y[+2]: при delaySamples = d
        база интерполяции base = floor(writePos - d), и самый правый отсчёт base + 2
        при d < 2 попадает на ячейку writePos — то есть на слот, который ещё не записан
        и хранит данные capacity сэмплов назад. Дробное чтение с d = 1.5 подмешало бы
        туда мусор с весом -0,0625. При d >= 2 все четыре точки лежат в истории.
        (y[-1] требует всего d >= 1, он тут не ограничивает.) */
    constexpr double minDelay = 2.0;

    /** Сверху: самый левый отсчёт base - 1 не должен уехать за круг. Тройка —
        запас на floor и на то, что ячейка writePos занята «завтрашним» сэмплом. */
    inline double maxDelay (int capacity) { return static_cast<double> (capacity - 3); }
}

void DelayBuffer::prepare (double sampleRate, int channels, double maxDelaySeconds)
{
    numChannels = std::max (1, channels);

    // +4 — запас под четыре точки интерполяции: без него ровно степень двойки
    // на входе (например 48000 * 1/3 c) не оставила бы места под кламп сверху.
    const auto needed = static_cast<unsigned> (std::ceil (std::max (0.0, sampleRate)
                                                          * std::max (0.0, maxDelaySeconds))) + 4u;

    // Округление вверх до степени двойки: заворачивание кольца становится
    // маской вместо ветвления и деления по модулю в горячем цикле.
    capacity = static_cast<int> (std::bit_ceil (std::max (8u, needed)));

    data.assign (static_cast<size_t> (numChannels) * static_cast<size_t> (capacity), 0.0f);
    writePos = 0;
}

void DelayBuffer::clear()
{
    std::fill (data.begin(), data.end(), 0.0f);
    writePos = 0;
}

void DelayBuffer::write (const float* const* input, int channels, int startSample, int numSamples)
{
    if (capacity == 0 || numSamples <= 0)
        return;

    // Блок длиннее кольца — вырожденный случай (кольцо меньше буфера хоста),
    // но пусть лучше он потеряет хвост, чем уедет за границу вектора.
    numSamples = std::min (numSamples, capacity);

    const int mask = capacity - 1;
    const int first = std::min (numSamples, capacity - writePos);   // до стыка кольца
    const int rest  = numSamples - first;

    for (int ch = 0; ch < std::min (channels, numChannels); ++ch)
    {
        const float* src = input[ch] + startSample;
        float* dst = data.data() + static_cast<size_t> (ch) * static_cast<size_t> (capacity);

        std::copy (src, src + first, dst + writePos);
        std::copy (src + first, src + first + rest, dst);
    }

    // Голова двигается один раз на весь сегмент, а не по сэмплу.
    writePos = (writePos + numSamples) & mask;
}

float DelayBuffer::read (int channel, double delaySamples) const
{
    if (capacity == 0 || channel < 0 || channel >= numChannels)
        return 0.0f;

    // Порядок сравнений выбран так, чтобы NaN уехал в minDelay: !(NaN >= x) истинно.
    double d = delaySamples;
    if (! (d >= minDelay))          d = minDelay;
    else if (d > maxDelay (capacity)) d = maxDelay (capacity);

    const double position = static_cast<double> (writePos) - d;
    const double base = std::floor (position);
    const float t = static_cast<float> (position - base);

    const int mask = capacity - 1;
    // + 2 * capacity: position может уйти в минус на почти целое кольцо, и ещё -1
    // за y[-1]. Двух оборотов хватает с запасом, маска потом сводит всё в диапазон.
    const int i = static_cast<int> (base) + 2 * capacity;

    const float* line = data.data() + static_cast<size_t> (channel) * static_cast<size_t> (capacity);
    // Каждый индекс заворачивается отдельно: соседние точки легко оказываются
    // по разные стороны стыка кольца.
    const float ym1 = line[(i - 1) & mask];
    const float y0  = line[ i      & mask];
    const float y1  = line[(i + 1) & mask];
    const float y2  = line[(i + 2) & mask];

    // Catmull-Rom (он же Lagrange 3-го порядка в форме Горнера).
    const float a = -0.5f * ym1 + 1.5f * y0 - 1.5f * y1 + 0.5f * y2;
    const float b =         ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c = -0.5f * ym1               + 0.5f * y1;
    return ((a * t + b) * t + c) * t + y0;
}

int DelayBuffer::getCapacitySamples() const { return capacity; }
int DelayBuffer::getNumChannels() const     { return numChannels; }
