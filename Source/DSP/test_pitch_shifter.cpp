// Офлайн-тест обоих движков питчинга (#14, #38): высота сдвига, потоковость,
// постоянство латентности, ноль аллокаций, equal-power кроссфейд у varispeed.
// Заодно меряет расстройку хвоста в центах и печатает её таблицей — это тот замер,
// ради которого HQ-движок и появился (ADR 0004, ADR 0005).
// Без фреймворков и без JUCE. Собирается и запускается одной командой:
//   c++ -std=c++20 -O2 -Ilibs/signalsmith-stretch Source/DSP/DelayBuffer.cpp \
//       Source/DSP/PitchShifter.cpp Source/DSP/test_pitch_shifter.cpp -o /tmp/tps && /tmp/tps

#include "PitchShifter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
    constexpr double windowMs = 240.0;

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

    /** Частота пика с точностью много лучше герца: грубый проход сеткой в 1 Гц,
        потом уточнение шагом 0,005 Гц. Тупой перебор, зато без окон и без БПФ —
        а на 110 Гц один герц это 157 центов, то есть грубой сетки тут не хватает. */
    double refinedFrequency (const std::vector<float>& x, size_t from, size_t count,
                             double centre, double span)
    {
        const double coarse = dominantFrequency (x, from, count, centre - span, centre + span);

        double best = coarse, bestAmp = -1.0;

        for (double f = coarse - 1.0; f <= coarse + 1.0; f += 0.005)
        {
            const double a = amplitudeAt (x, from, count, f);

            if (a > bestAmp) { bestAmp = a; best = f; }
        }

        return best;
    }

    double centsBetween (double measured, double expected)
    {
        return 1200.0 * std::log2 (measured / expected);
    }

    /** Окно движка режима Follow. Дублирует константу из Voice.cpp намеренно:
        офлайн-тест не тянет ни JUCE, ни голос, а разъехаться этим двум числам
        не даёт проверка латентности ниже. */
    constexpr float followWindowSeconds = 0.09f;

    enum class Engine { varispeed, signalsmith, follow };

    const char* engineName (Engine e)
    {
        switch (e)
        {
            case Engine::signalsmith: return "signalsmith";
            case Engine::follow:      return "follow     ";
            default:                  return "varispeed  ";
        }
    }

    std::unique_ptr<PitchShifter> makeShifter (Engine e)
    {
        switch (e)
        {
            case Engine::signalsmith: return std::make_unique<SignalsmithShifter>();
            case Engine::follow:      return std::make_unique<SignalsmithShifter> (followWindowSeconds);
            default:                  return std::make_unique<VarispeedShifter> (windowMs);
        }
    }

    std::vector<float> sine (int n, double freq, double amp = 0.5)
    {
        std::vector<float> x (static_cast<size_t> (n));

        for (int i = 0; i < n; ++i)
            x[static_cast<size_t> (i)] = static_cast<float> (amp * std::sin (2.0 * pi * freq * i / sr));

        return x;
    }

    /** Расстройка хвоста в центах: подаём чистый тон f0, просим сдвиг на semitones,
        меряем, куда движок его на самом деле поставил. Ровно та величина, из-за которой
        varispeed уступил место Signalsmith — см. ADR 0004 и ADR 0005. */
    double measureDetune (Engine engine, double f0, int semitones)
    {
        constexpr int n = 65536;
        const double ratio = std::pow (2.0, semitones / 12.0);
        const double expected = f0 * ratio;

        auto shifter = makeShifter (engine);
        shifter->prepare (sr, 2048);
        shifter->setRatio (static_cast<float> (ratio));

        const auto out = run (*shifter, sine (n, f0), 2048);

        // Полсекунды на прогрев: там ещё тишина и первый проезд окна у varispeed.
        // Окно замера 32768 сэмплов — разрешение ДПФ 1,5 Гц, уточнение доводит до сотых.
        const double measured = refinedFrequency (out, 24000, 32768, expected,
                                                  std::max (20.0, 0.05 * expected));

        return centsBetween (measured, expected);
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
    // Инвариант ADR 0002 и для обоих движков одинаково обязателен: латентность
    // вычитается из позиции чтения один раз, вне аудиопотока.
    for (const auto engine : { Engine::varispeed, Engine::signalsmith })
    {
        auto shifter = makeShifter (engine);
        shifter->prepare (sr, 512);

        const int latency = shifter->getLatencySamples();

        // Varispeed: окно 240 мс — 11520 сэмплов, половина 5760, плюс минимум DelayBuffer.
        // Signalsmith: окно 0,18 с, анализ и синтез забирают по половине — ровно 8640,
        // то есть 180 мс. Почему именно 0,18 — таблица замеров в PitchShifter.cpp.
        CHECK (latency == (engine == Engine::signalsmith ? 8640 : 5762));

        for (const float ratio : { 0.25f, 0.5f, 0.99f, 1.0f, 1.5f, 2.0f, 4.0f, 100.0f, -3.0f })
        {
            shifter->setRatio (ratio);

            std::vector<float> out (256, 0.0f);
            const auto in = sine (256, 440.0);
            shifter->process (in.data(), out.data(), 256);

            CHECK (shifter->getLatencySamples() == latency);
        }
    }

    // --- 2. Varispeed, ratio = 1 — ровно целочисленная задержка, бит-в-бит ------
    // Это же и эталон для #19: на unity голос обязан звучать как обычный дилей.
    // Только varispeed: у Signalsmith при ratio = 1 сигнал всё равно проходит через
    // анализ и синтез STFT, и бит-в-бит равенство входу от него требовать нечестно.
    {
        VarispeedShifter shifter (windowMs);
        shifter.prepare (sr, 512);
        shifter.setRatio (1.0f);

        const auto in = sine (32768, 440.0);
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
    for (const auto engine : { Engine::varispeed, Engine::signalsmith })
    {
        const auto in = sine (32768, 330.0);
        std::vector<float> reference;

        for (const int block : { 1, 7, 64, 512, 4096 })
        {
            auto shifter = makeShifter (engine);
            shifter->prepare (sr, 4096);
            shifter->setRatio (1.25f);

            const auto out = run (*shifter, in, block);

            if (reference.empty()) reference = out;
            else                   for (size_t i = 0; i < out.size(); ++i) CHECK (out[i] == reference[i]);
        }
    }

    // --- 4. Высота: ±12 полутонов в обе стороны ---------------------------------
    // Пробный тон — 500 Гц, и это не произвол. Выход varispeed принудительно лежит
    // на сетке f0 + k/T, где T — период проезда окна: состояние движка периодично с T,
    // значит и выход обязан быть периодичен по фазе с тем же T. Точное f0 * ratio
    // попадает на эту сетку только когда в полуокне укладывается целое число периодов
    // входа. При 48 кГц и окне 240 мс полуокно — 5760 сэмплов, то есть кратные 8,33 Гц;
    // 500 Гц это ровно 60 периодов. Тогда оба ридера синфазны, кроссфейд не крутит фазу,
    // и сдвиг получается точным для любого ratio. Проверка ниже документирует, что
    // бывает с частотой не с сетки — это потолок движка, а не порог теста.
    for (const auto engine : { Engine::varispeed, Engine::signalsmith })
    {
        constexpr double f0 = 500.0;
        constexpr int n = 65536;
        const auto in = sine (n, f0);

        for (const int semitones : { -12, -7, -5, 0, 5, 7, 12 })
        {
            const double ratio = std::pow (2.0, semitones / 12.0);
            const double expected = f0 * ratio;

            auto shifterOwner = makeShifter (engine);
            auto& shifter = *shifterOwner;
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

    // --- 4b. Потолок varispeed: расстройка на частоте не с сетки -----------------
    // 440 Гц — это 52,8 периода в полуокне, то есть мимо сетки. Сдвиг квантуется
    // шагом 1/T = |1 - ratio| / окно; на октаву вверх это 4,2 Гц, и промах доходит
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

    // --- 5. Varispeed, кроссфейд equal-power: провала RMS в центре перехода нет --
    // Два ридера разнесены на пол-окна, то есть на 30 мс: полосный шум за это время
    // раскоррелирован полностью, и складываются они по мощности. Equal-power держит
    // gA^2 + gB^2 = 1, значит RMS постоянен. Линейный кроссфейд дал бы в центре
    // gA = gB = 0,5, то есть 0,5 по мощности — ровно -3 dB провала.
    {
        constexpr int n = 300000;
        const auto in = bandLimitedNoise (n);

        VarispeedShifter shifter (windowMs);
        shifter.prepare (sr, 1024);
        shifter.setRatio (1.5f);   // окно проезжается за 23 040 сэмплов, то есть 13 раз

        const auto out = run (shifter, in, 1024);

        // Пропускаем прогрев кольца и первый неполный проезд окна.
        constexpr size_t from = 30000;
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
    for (const auto engine : { Engine::varispeed, Engine::signalsmith })
    {
        auto shifter = makeShifter (engine);
        shifter->prepare (sr, 512);

        const auto in = sine (4096, 440.0);
        std::vector<float> out (in.size(), 0.0f);

        // Первый блок вне счётчика: у Signalsmith внутренний временный буфер дорастает
        // до своей ёмкости на первом же вызове, дальше resize её не трогает.
        shifter->setRatio (1.5f);
        shifter->process (in.data(), out.data(), 512);

        const long before = allocations;

        for (size_t i = 0; i < in.size(); i += 512)
        {
            shifter->setRatio (1.5f);
            shifter->process (in.data() + i, out.data() + i, 512);
        }

        shifter->reset();
        shifter->setRatio (0.5f);

        CHECK (allocations == before);
    }

    // --- 7. Расстройка хвоста: замер обоих движков одним и тем же ---------------
    // Тот самый замер, ради которого поднята #38. Частоты выбраны не по вкусу:
    // 110-140 Гц — область мужского вокала, и именно там varispeed промахивался хуже
    // всего (ADR 0004). 500 Гц оставлено как контроль: оно лежит ровно на сетке
    // varispeed (60 периодов в полуокне), и там он обязан быть точен.
    {
        constexpr double freqs[] { 110.0, 130.0, 220.0, 440.0, 500.0 };
        constexpr int shifts[] { -12, -7, -3, 3, 7, 12 };

        std::printf ("\nРасстройка хвоста, центы (полутонов: ");

        for (const int st : shifts) std::printf ("%+4d", st);
        std::printf (")\n");

        double worst[3] { 0.0, 0.0, 0.0 };
        double sum[3] { 0.0, 0.0, 0.0 };
        int count = 0;

        for (const double f0 : freqs)
        {
            for (const auto engine : { Engine::varispeed, Engine::signalsmith, Engine::follow })
            {
                const int e = static_cast<int> (engine);
                std::printf ("  %6.0f Гц  %s ", f0, engineName (engine));

                for (const int st : shifts)
                {
                    const double cents = measureDetune (engine, f0, st);

                    std::printf ("%+7.1f", cents);

                    worst[e] = std::max (worst[e], std::abs (cents));
                    sum[e] += std::abs (cents);

                    if (e == 1) ++count;
                }

                std::printf ("\n");
            }
        }

        std::printf ("  varispeed:   среднее %.1f, худшее %.1f центов\n", sum[0] / count, worst[0]);
        std::printf ("  signalsmith: среднее %.1f, худшее %.1f центов\n", sum[1] / count, worst[1]);
        std::printf ("  follow:      среднее %.1f, худшее %.1f центов\n\n", sum[2] / count, worst[2]);

        // Пороги поставлены по замеру с запасом примерно вдвое. Они ловят не «стало
        // чуть хуже», а «движок сломался» или «кто-то уменьшил окно STFT».
        CHECK (worst[1] < 25.0);
        CHECK (sum[1] / count < 8.0);

        // И главное — ради чего менялся движок: HQ обязан быть точнее Fast.
        CHECK (worst[1] < worst[0]);
        CHECK (sum[1] < sum[0]);

        // Движок Follow: окно втрое короче, и точность обязана быть хуже HQ — иначе
        // длинное окно в дилее не окупается и его надо укорачивать. Но остаться он
        // обязан в пределах, где хвост ещё поёт в унисон с сухим, а не бьётся с ним.
        // Замер на этом окне: среднее 5,2, худшее 16,2 цента. Порог с запасом в полтора
        // раза — ловит поломку движка и укорачивание окна, а не дрожание в последней
        // цифре. Окно 0,06 с давало худшие 58 центов, то есть четверть тона: для
        // унисона с сухим сигналом это брак, и потому в Follow стоит 0,09 (ADR 0006).
        CHECK (sum[2] > sum[1]);
        CHECK (worst[2] < 25.0);

        // Латентность движка Follow — ровно окно: это то число, которое плагин
        // просит скомпенсировать у хоста (ADR 0006), и разъехаться ему нельзя.
        {
            auto shifter = makeShifter (Engine::follow);
            shifter->prepare (sr, 2048);

            CHECK (shifter->getLatencySamples()
                   == static_cast<int> (sr * followWindowSeconds));
        }
    }

    std::printf ("test_pitch_shifter: OK\n");
    return 0;
}
