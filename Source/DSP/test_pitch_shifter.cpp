// Офлайн-тест VarispeedShifter (#14): высота сдвига, equal-power кроссфейд,
// постоянство латентности, ноль аллокаций. Без фреймворков и без JUCE.
// Собирается и запускается одной командой:
//   c++ -std=c++20 -O2 Source/DSP/DelayBuffer.cpp Source/DSP/PitchShifter.cpp \
//       Source/DSP/test_pitch_shifter.cpp -o /tmp/tps && /tmp/tps

#include "PitchShifter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

// Не assert: он исчезает под NDEBUG, и тест «проходил» бы, ничего не проверяя.
#define CHECK(cond) \
    do { if (! (cond)) { std::printf ("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
                         std::abort(); } } while (false)

// Счётчик аллокаций: подменяем глобальный operator new, чтобы «ноль аллокаций
// вне prepare» был измерением, а не обещанием в комментарии.
static long allocations = 0;

void* operator new (std::size_t n) { ++allocations; return std::malloc (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
    constexpr double pi = 3.14159265358979323846;
    constexpr double sr = 48000.0;
    constexpr double windowMs = 60.0;

    /** Прогоняет весь вход через питчер порциями заданного размера. */
    std::vector<float> run (PitchShifter& shifter, const std::vector<float>& in, int block)
    {
        std::vector<float> out (in.size(), 0.0f);

        for (size_t i = 0; i < in.size(); i += static_cast<size_t> (block))
        {
            const int n = static_cast<int> (std::min (static_cast<size_t> (block), in.size() - i));
            shifter.process (in.data() + i, out.data() + i, n);
        }

        return out;
    }

    /** Амплитуда синусоиды частоты f: обычный ДПФ в одной точке, без БПФ. */
    double amplitudeAt (const std::vector<float>& x, size_t from, size_t count, double f)
    {
        double re = 0.0, im = 0.0;

        for (size_t i = 0; i < count; ++i)
        {
            const double a = -2.0 * pi * f * static_cast<double> (i) / sr;
            re += x[from + i] * std::cos (a);
            im += x[from + i] * std::sin (a);
        }

        return 2.0 * std::sqrt (re * re + im * im) / static_cast<double> (count);
    }

    /** Частота самой сильной составляющей: перебор сетки в 1 Гц. Тупо, но честно
        и без зависимостей — искать пик БПФ-ом здесь было бы больше кода. */
    double dominantFrequency (const std::vector<float>& x, size_t from, size_t count,
                              double lo, double hi)
    {
        double best = lo, bestAmp = -1.0;

        for (double f = lo; f <= hi; f += 1.0)
        {
            const double a = amplitudeAt (x, from, count, f);

            if (a > bestAmp) { bestAmp = a; best = f; }
        }

        return best;
    }

    std::vector<float> sine (int n, double freq, double amp = 0.5)
    {
        std::vector<float> x (static_cast<size_t> (n));

        for (int i = 0; i < n; ++i)
            x[static_cast<size_t> (i)] = static_cast<float> (amp * std::sin (2.0 * pi * freq * i / sr));

        return x;
    }

    /** Шум, ограниченный сверху однополюсным фильтром. Именно ограниченный: широкий
        белый шум Catmull-Rom подрезал бы сам, и потеря RMS шла бы от интерполяции,
        а не от кроссфейда — тест мерил бы не то, что заявляет. */
    std::vector<float> bandLimitedNoise (int n)
    {
        std::vector<float> x (static_cast<size_t> (n));
        unsigned state = 12345u;
        float y = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            state = state * 1664525u + 1013904223u;
            const float white = static_cast<float> (state >> 8) / 8388608.0f - 1.0f;   // [-1, 1)
            y += 0.23f * (white - y);                                                   // ~2 кГц
            x[static_cast<size_t> (i)] = y;
        }

        return x;
    }

    double rms (const std::vector<float>& x, size_t from, size_t count)
    {
        double sum = 0.0;

        for (size_t i = 0; i < count; ++i)
            sum += static_cast<double> (x[from + i]) * x[from + i];

        return std::sqrt (sum / static_cast<double> (count));
    }

    double toDecibels (double ratio) { return 20.0 * std::log10 (ratio); }
}

int main()
{
    // --- 1. Латентность: константа, не зависит от ratio -------------------------
    {
        VarispeedShifter shifter (windowMs);
        shifter.prepare (sr, 512);

        const int latency = shifter.getLatencySamples();

        // 60 мс на 48 кГц — окно 2880 сэмплов, половина 1440, плюс минимум DelayBuffer.
        CHECK (latency == 1442);

        for (const float ratio : { 0.25f, 0.5f, 0.99f, 1.0f, 1.5f, 2.0f, 4.0f, 100.0f, -3.0f })
        {
            shifter.setRatio (ratio);

            std::vector<float> out (256, 0.0f);
            const auto in = sine (256, 440.0);
            shifter.process (in.data(), out.data(), 256);

            CHECK (shifter.getLatencySamples() == latency);
        }
    }

    // --- 2. Ratio = 1 — ровно целочисленная задержка, бит-в-бит -----------------
    // Это же и эталон для #19: на unity голос обязан звучать как обычный дилей.
    {
        VarispeedShifter shifter (windowMs);
        shifter.prepare (sr, 512);
        shifter.setRatio (1.0f);

        const auto in = sine (8192, 440.0);
        const auto out = run (shifter, in, 512);
        const int latency = shifter.getLatencySamples();

        for (size_t i = static_cast<size_t> (latency); i < in.size(); ++i)
            CHECK (out[i] == in[i - static_cast<size_t> (latency)]);

        // До латентности — ровно тишина, а не мусор из непрогретого кольца.
        for (size_t i = 0; i < static_cast<size_t> (latency); ++i)
            CHECK (out[i] == 0.0f);
    }

    // --- 3. Потоковость: размер блока на выход не влияет ------------------------
    // Питчер обязан быть бит-в-бит одинаков при любой нарезке, иначе покраснеет
    // тест размеров блока в test_processor.cpp — и это будет баг здесь, а не там.
    {
        const auto in = sine (12288, 330.0);
        std::vector<float> reference;

        for (const int block : { 1, 7, 64, 512, 4096 })
        {
            VarispeedShifter shifter (windowMs);
            shifter.prepare (sr, 4096);
            shifter.setRatio (1.25f);

            const auto out = run (shifter, in, block);

            if (reference.empty()) reference = out;
            else                   for (size_t i = 0; i < out.size(); ++i) CHECK (out[i] == reference[i]);
        }
    }

    // --- 4. Высота: ±12 полутонов в обе стороны ---------------------------------
    // Пробный тон — 500 Гц, и это не произвол. Выход varispeed принудительно лежит
    // на сетке f0 + k/T, где T — период проезда окна: состояние движка периодично с T,
    // значит и выход обязан быть периодичен по фазе с тем же T. Точное f0 * ratio
    // попадает на эту сетку только когда в полуокне укладывается целое число периодов
    // входа. При 48 кГц и окне 60 мс полуокно — 1440 сэмплов, то есть кратные 33,33 Гц;
    // 500 Гц это ровно 15 периодов. Тогда оба ридера синфазны, кроссфейд не крутит фазу,
    // и сдвиг получается точным для любого ratio. Проверка ниже документирует, что
    // бывает с частотой не с сетки — это потолок движка, а не порог теста.
    {
        constexpr double f0 = 500.0;
        constexpr int n = 65536;
        const auto in = sine (n, f0);

        for (const int semitones : { -12, -7, -5, 0, 5, 7, 12 })
        {
            const double ratio = std::pow (2.0, semitones / 12.0);
            const double expected = f0 * ratio;

            VarispeedShifter shifter (windowMs);
            shifter.prepare (sr, 2048);
            shifter.setRatio (static_cast<float> (ratio));

            const auto out = run (shifter, in, 2048);

            // Полсекунды на прогрев кольца: там ещё тишина и первый проезд окна.
            const size_t from = 24000;
            const size_t count = 32768;
            const double measured = dominantFrequency (out, from, count,
                                                       expected - 40.0, expected + 40.0);

            CHECK (std::abs (measured - expected) < 2.0);

            // Исходный тон обязан уйти: если бы движок молча пропускал вход мимо,
            // проверка выше прошла бы, а эта — нет.
            if (semitones != 0)
            {
                const double leak = amplitudeAt (out, from, count, f0);
                const double wanted = amplitudeAt (out, from, count, expected);
                CHECK (leak < 0.1 * wanted);
            }

            // Разрывов нет: шаг синуса 1 кГц амплитуды 0,5 — это 0,065 за сэмпл,
            // с учётом гребёнки двух ридеров вдвое больше. Заворот без кроссфейда
            // дал бы скачок порядка амплитуды сигнала.
            float maxStep = 0.0f;

            for (size_t i = from + 1; i < from + count; ++i)
                maxStep = std::max (maxStep, std::abs (out[i] - out[i - 1]));

            CHECK (maxStep < 0.3f);
        }
    }

    // --- 4b. Потолок движка: расстройка на частоте не с сетки --------------------
    // 440 Гц — это 13,2 периода в полуокне, то есть мимо сетки. Сдвиг квантуется
    // шагом 1/T = |1 - ratio| / окно; на октаву вверх это 16,7 Гц, и промах доходит
    // до целого шага. Проверка держит именно эту границу: она пройдёт и на движке,
    // который расстройку уберёт совсем. Убрать её — задача HQ-режима (#19, ADR 0002),
    // внутри varispeed с периодическим проездом окна она неустранима.
    {
        constexpr double f0 = 440.0;
        const auto in = sine (65536, f0);

        for (const int semitones : { -12, 12 })
        {
            const double ratio = std::pow (2.0, semitones / 12.0);
            const double expected = f0 * ratio;
            const double gridStep = std::abs (1.0 - ratio) / (windowMs * 0.001);

            VarispeedShifter shifter (windowMs);
            shifter.prepare (sr, 2048);
            shifter.setRatio (static_cast<float> (ratio));

            const auto out = run (shifter, in, 2048);
            const double measured = dominantFrequency (out, 24000, 32768,
                                                       expected - 2.0 * gridStep,
                                                       expected + 2.0 * gridStep);

            CHECK (std::abs (measured - expected) <= gridStep);
        }
    }

    // --- 5. Кроссфейд equal-power: провала RMS в центре перехода нет -------------
    // Два ридера разнесены на пол-окна, то есть на 30 мс: полосный шум за это время
    // раскоррелирован полностью, и складываются они по мощности. Equal-power держит
    // gA^2 + gB^2 = 1, значит RMS постоянен. Линейный кроссфейд дал бы в центре
    // gA = gB = 0,5, то есть 0,5 по мощности — ровно -3 dB провала.
    {
        constexpr int n = 300000;
        const auto in = bandLimitedNoise (n);

        VarispeedShifter shifter (windowMs);
        shifter.prepare (sr, 1024);
        shifter.setRatio (1.0594631f);   // +1 полутон: окно проезжается за ~48 400 сэмплов

        const auto out = run (shifter, in, 1024);

        // Пропускаем прогрев кольца и первый неполный проезд окна.
        constexpr size_t from = 10000;
        constexpr size_t win = 2048;

        double lo = 1.0e9, hi = 0.0;

        for (size_t i = from; i + win <= static_cast<size_t> (n); i += win)
        {
            const double r = rms (out, i, win);
            lo = std::min (lo, r);
            hi = std::max (hi, r);
        }

        // Разброс окон: статистика полосного шума на окне 2048 даёт около 0,3 dB,
        // порог 1,5 dB оставляет запас и при этом ловит любой провал в -3 dB.
        CHECK (toDecibels (hi / lo) < 1.5);

        // И полная мощность на месте: equal-power не только не проваливается,
        // но и не задирает — сравниваем с тем же шумом на входе.
        const double inRms  = rms (in,  from, static_cast<size_t> (n) - from);
        const double outRms = rms (out, from, static_cast<size_t> (n) - from);
        CHECK (std::abs (toDecibels (outRms / inRms)) < 0.7);
    }

    // --- 6. Ноль аллокаций вне prepare -----------------------------------------
    {
        VarispeedShifter shifter (windowMs);
        shifter.prepare (sr, 512);

        const auto in = sine (4096, 440.0);
        std::vector<float> out (in.size(), 0.0f);

        const long before = allocations;

        for (size_t i = 0; i < in.size(); i += 512)
        {
            shifter.setRatio (1.5f);
            shifter.process (in.data() + i, out.data() + i, 512);
        }

        shifter.reset();
        shifter.setRatio (0.5f);

        CHECK (allocations == before);
    }

    std::printf ("test_pitch_shifter: OK\n");
    return 0;
}
