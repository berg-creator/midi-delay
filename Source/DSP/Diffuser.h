#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

/** Цепочка из четырёх алл-пассов Шрёдера — диффузия хвоста (#45).

    Стоит на возврате обратной связи, до записи в кольцо: первый хвост остаётся
    чистым, а каждый следующий круг размывается сильнее. Это и есть та «глубина»,
    которой дорогой дилей отличается от простого.

    Одно звено: w[n] = x[n] + g*w[n-M], y[n] = w[n-M] - g*w[n]. Усиление ровно
    единичное на всех частотах, поэтому при feedback 95 % петля не разгоняется —
    этим диффузия выгодно отличается от резонансных фильтров.

    **Ловушка, ради которой заведён getDelaySamples.** При g = 0 звено не исчезает,
    а вырождается в чистую задержку на M сэмплов: цепочка молча удлинила бы петлю
    примерно на 60 мс и увела интервал повторов. Вызывающий обязан вычесть
    getDelaySamples из времени петли — тогда при g = 0 результат совпадает
    с недиффузированным бит-в-бит, и интервал повторов от диффузии не зависит.

    Длины звеньев у левого и правого канала разные — ширина достаётся даром.
    Намеренно не зависит от JUCE, как и DelayBuffer. */
class Diffuser
{
public:
    static constexpr int numStages = 4;

    /** Единственное место, где выделяется память. */
    void prepare (double sampleRate, int channels)
    {
        // Длины взаимно простые в миллисекундах: кратные дали бы совпадающие
        // пики и металлический призвук вместо размытия.
        static constexpr double stageMs[2][numStages] { { 7.0, 11.0, 17.0, 23.0 },
                                                        { 9.0, 13.0, 19.0, 26.0 } };

        numChannels = std::max (1, channels);
        stages.assign (static_cast<size_t> (numChannels) * numStages, {});
        chainSamples.assign (static_cast<size_t> (numChannels), 0);

        int offset = 0;

        for (int ch = 0; ch < numChannels; ++ch)
            for (int s = 0; s < numStages; ++s)
            {
                const int length = std::max (1, static_cast<int> (
                    std::lround (stageMs[ch % 2][s] * 0.001 * std::max (1.0, sampleRate))));

                stages[static_cast<size_t> (ch) * numStages + s] = { length, 0, offset };
                chainSamples[static_cast<size_t> (ch)] += length;
                offset += length;
            }

        lines.assign (static_cast<size_t> (offset), 0.0f);
    }

    void clear()
    {
        std::fill (lines.begin(), lines.end(), 0.0f);

        for (auto& s : stages)
            s.pos = 0;
    }

    /** Один сэмпл через цепочку. g — коэффициент диффузии, 0..~0,7. */
    float process (int channel, float x, float g)
    {
        if (channel < 0 || channel >= numChannels)
            return x;

        for (int s = 0; s < numStages; ++s)
        {
            auto& stage = stages[static_cast<size_t> (channel) * numStages + s];
            float* const line = lines.data() + stage.offset;

            const float delayed = line[stage.pos];
            const float w = x + g * delayed;

            line[stage.pos] = w;

            if (++stage.pos >= stage.length)
                stage.pos = 0;

            x = delayed - g * w;
        }

        return x;
    }

    /** Суммарная задержка цепочки канала. Ровно на неё вызывающий обязан укоротить
        петлю, иначе интервал повторов уедет — см. комментарий к классу. */
    int getDelaySamples (int channel) const
    {
        return channel >= 0 && channel < numChannels
             ? chainSamples[static_cast<size_t> (channel)] : 0;
    }

private:
    struct Stage { int length = 1, pos = 0, offset = 0; };

    std::vector<Stage> stages;         // numChannels * numStages подряд
    std::vector<float> lines;          // все линии задержки подряд
    std::vector<int> chainSamples;     // сумма длин по каналу
    int numChannels = 0;
};
