#include "PitchShifter.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double pi = 3.14159265358979323846;

    /** Минимальная задержка, которую отдаёт DelayBuffer: ниже неё четыре точки
        Catmull-Rom не помещаются в историю. Задержка ридера — это minDelay + sweep. */
    constexpr double minDelay = 2.0;

    /** Кламп ratio ровно под максимум Pitch Range: 24 полутона вниз и вверх.
        Ноль или минус развернули бы чтение назад, слишком большой ratio за один
        сэмпл перепрыгнул бы окно и кроссфейд перестал бы прятать заворот. */
    constexpr double minRatio = 0.25;
    constexpr double maxRatio = 4.0;
}

VarispeedShifter::VarispeedShifter (double newWindowMs)
    : windowMs (newWindowMs)
{
}

void VarispeedShifter::prepare (double sampleRate, int)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;

    // Окно берётся чётным нарочно. Тогда половина окна — целое число сэмплов, при
    // ratio = 1 позиция чтения оказывается целой, Catmull-Rom возвращает отсчёт как
    // есть, и движок вырождается ровно в целочисленную задержку, бит-в-бит.
    const int half = std::max (1, static_cast<int> (std::lround (sr * windowMs * 0.001)) / 2);

    window = 2.0 * half;
    latency = static_cast<int> (minDelay) + half;

    // DelayBuffer считает ёмкость как sampleRate * seconds, поэтому размер задаётся
    // прямо в сэмплах: «частота» 1 Гц, «секунды» — длина окна. +4 на верхний кламп.
    ring.prepare (1.0, 1, window + minDelay + 4.0);

    reset();
}

void VarispeedShifter::reset()
{
    ring.clear();

    // Середина окна: вес первого ридера здесь единица, второго — ноль. Стартовать
    // на стыке значило бы начать ноту ровно в точке склейки.
    sweep = 0.5 * window;
}

void VarispeedShifter::setRatio (float ratio)
{
    // Ридер обязан ехать со скоростью ratio, голова записи — со скоростью 1, значит
    // задержка между ними меняется на (1 - ratio) за сэмпл. Это и есть весь varispeed.
    step = 1.0 - std::clamp (static_cast<double> (ratio), minRatio, maxRatio);
}

void VarispeedShifter::process (const float* in, float* out, int numSamples)
{
    const double half = 0.5 * window;

    for (int i = 0; i < numSamples; ++i)
    {
        const double a = sweep;
        const double b = a < half ? a + half : a - half;

        // Вес ридера — ноль на стыке окна (sweep = 0) и единица в его середине, ровно
        // там, где второй ридер на стыке. Второй вес считается не второй синусоидой,
        // а из тождества: gA^2 + gB^2 = 1 по построению, а не по совпадению формул.
        // Это и есть equal-power; линейный кроссфейд дал бы -3 dB в центре перехода.
        const float gainA = static_cast<float> (std::sin (pi * a / window));
        const float gainB = std::sqrt (std::max (0.0f, 1.0f - gainA * gainA));

        // Читаем до записи текущего сэмпла: тогда задержка ровно d, а не d - 1.
        out[i] = gainA * ring.read (0, minDelay + a) + gainB * ring.read (0, minDelay + b);

        const float sample = in[i];
        const float* one = &sample;
        ring.write (&one, 1, 0, 1);

        sweep += step;
        if (sweep >= window)     sweep -= window;
        else if (sweep < 0.0)    sweep += window;
    }
}

int VarispeedShifter::getLatencySamples() const
{
    // Фактическая задержка гуляет в пределах полуокна — это природа алгоритма
    // (находка сессии 02). Наружу отдаётся середина окна: она не зависит от ratio,
    // а этого требует ADR 0002 — вычитается она один раз, вне аудиопотока.
    return latency;
}
