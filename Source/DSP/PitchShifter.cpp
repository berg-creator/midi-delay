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

//==============================================================================
// SignalsmithShifter (#38). Вся библиотека видна только отсюда.

#include "signalsmith-stretch.h"

struct SignalsmithShifter::Impl
{
    /** Фиксированное зерно, а не std::random_device: движок подмешивает случайную фазу
        на транзиентах и в тишине, и на разных зёрнах офлайн-рендер выходил бы каждый раз
        другим. Критерий приёмки #19 требует воспроизводимого рендера, значит зерно
        обязано быть константой. Голоса при этом не коррелируют — у них разный вход. */
    signalsmith::stretch::SignalsmithStretch<float> stretch { 0x5713C4 };
};

SignalsmithShifter::SignalsmithShifter (float windowSeconds)
    : impl (std::make_unique<Impl>()), window (std::max (0.01f, windowSeconds)) {}
SignalsmithShifter::~SignalsmithShifter() = default;

void SignalsmithShifter::prepare (double sampleRate, int)
{
    const auto sr = static_cast<float> (sampleRate > 0.0 ? sampleRate : 44100.0);

    // Окно 0,18 с при четырёхкратном перекрытии, а не presetDefault (0,12 / 0,03).
    // Это измеренная величина, а не вкус. Точность высоты у этого движка упирается
    // в энергетический центроид полосы анализа: ошибка центроида — доли бина, то есть
    // почти постоянная величина в герцах, и в центах она бьёт тем сильнее, чем ниже
    // нота. Замер на 110-440 Гц и ±12 полутонах (test_pitch_shifter, проверка 7):
    //   окно 0,12 с — латентность 120 мс, средняя расстройка 7,0 цента, худшая 28,3
    //   окно 0,18 с — латентность 180 мс, средняя 3,7, худшая 11,1
    //   окно 0,24 с — латентность 240 мс, средняя 3,8, худшая 16,2
    //   окно 0,36 с — латентность 360 мс, средняя 1,9, худшая  5,2
    // 0,18 — колено: вдвое точнее presetDefault за 60 мс, дальше та же цена покупает
    // вдвое меньше. Латентность здесь платится минимальным delay time, а не задержкой
    // хоста (ANALYSIS §5), поэтому такое окно вообще можно себе позволить. См. ADR 0005.
    // Окно и шаг задаются полем, а не литералом: тот же класс обслуживает и дилей
    // с окном 0,18 с, и режим Follow с коротким окном. Перекрытие всегда четырёхкратное.
    impl->stretch.configure (1, static_cast<int> (sr * window), static_cast<int> (sr * window * 0.25f));
    impl->stretch.setTransposeFactor (1.0f);
    ratio = 1.0f;

    // Обе половины: анализ смотрит назад на полокна, синтез копит выход ещё на полокна.
    // Сумма — та самая константа, которую прячет в delay time голос (ADR 0002).
    latency = impl->stretch.inputLatency() + impl->stretch.outputLatency();

    reset();
}

void SignalsmithShifter::reset()
{
    impl->stretch.reset();
}

void SignalsmithShifter::setRatio (float newRatio)
{
    // Тот же кламп, что у varispeed: 24 полутона в обе стороны. Ноль и минус движок
    // отобразил бы в отрицательные частоты, а это NaN в спектре, а не низкий звук.
    const auto clamped = static_cast<float> (std::clamp (static_cast<double> (newRatio), minRatio, maxRatio));

    // Сравнение с прошлым значением — не микрооптимизация: setTransposeFactor трогает
    // std::function частотной карты, и звать его на каждый сегмент незачем.
    if (clamped == ratio)
        return;

    ratio = clamped;
    impl->stretch.setTransposeFactor (clamped);
}

void SignalsmithShifter::process (const float* in, float* out, int numSamples)
{
    if (numSamples <= 0)
        return;

    // Библиотека индексирует как buffer[channel][sample]; канал у нас один.
    // Аллокаций здесь нет: внутренний временный буфер выделен в configure под
    // блок + шаг, а process только урезает его размер, не увеличивая ёмкость.
    const float* input = in;
    float* output = out;
    impl->stretch.process (&input, numSamples, &output, numSamples);
}

int SignalsmithShifter::getLatencySamples() const
{
    return latency;
}
