// Офлайн-тест DelayBuffer (#7) и интерполяции дробного чтения (#8).
// Без фреймворков: assert и int main. Собирается и запускается одной командой:
//   c++ -std=c++20 -O2 Source/DSP/DelayBuffer.cpp Source/DSP/test_delay_buffer.cpp -o /tmp/tdb && /tmp/tdb

#include "DelayBuffer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

// Счётчик аллокаций: подменяем глобальный operator new, чтобы проверка
// «ноль аллокаций вне prepare» была измерением, а не обещанием в комментарии.
static long allocations = 0;

void* operator new (std::size_t n) { ++allocations; return std::malloc (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
    constexpr double pi = 3.14159265358979323846;
    constexpr double sr = 48000.0;

    void writeOne (DelayBuffer& b, float sample)
    {
        const float* p = &sample;
        b.write (&p, 1, 0, 1);
    }

    void writeSilence (DelayBuffer& b, int n)
    {
        for (int i = 0; i < n; ++i)
            writeOne (b, 0.0f);
    }

    bool near (double a, double b, double eps) { return std::fabs (a - b) <= eps; }

    /** Амплитуда синусоиды в бине bin: обычный ДПФ в одной точке, 20 строк вместо БПФ. */
    double binAmplitude (const std::vector<float>& x, int bin)
    {
        const int n = static_cast<int> (x.size());
        double re = 0.0, im = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double a = -2.0 * pi * bin * i / n;
            re += x[i] * std::cos (a);
            im += x[i] * std::sin (a);
        }

        return 2.0 * std::sqrt (re * re + im * im) / n;
    }

    /** THD+N: вся энергия, кроме основного тона, к энергии основного тона.
        Работает без окна только потому, что сигналы подобраны целопериодными. */
    double thdPlusN (const std::vector<float>& x, int fundamentalBin)
    {
        double total = 0.0;
        for (float v : x)
            total += static_cast<double> (v) * v;

        const double amp = binAmplitude (x, fundamentalBin);
        const double fundamental = 0.5 * amp * amp * x.size();

        return std::sqrt (std::max (0.0, total - fundamental) / fundamental);
    }

    double toDb (double ratio) { return 20.0 * std::log10 (std::max (ratio, 1e-12)); }
}

// --- 1. Импульс выходит ровно через N сэмплов -------------------------------

void testImpulse()
{
    DelayBuffer b;
    b.prepare (sr, 1, 0.01);            // 480 сэмплов + запас -> ёмкость 512
    assert (b.getCapacitySamples() == 512);

    writeOne (b, 1.0f);
    writeSilence (b, 100);              // записано 101 сэмпл, голова на 101

    assert (near (b.read (0, 101.0), 1.0, 1e-6));   // импульс ровно на своём месте
    assert (near (b.read (0, 100.0), 0.0, 1e-6));
    assert (near (b.read (0, 102.0), 0.0, 1e-6));

    std::puts ("  ok: импульс читается ровно через N сэмплов");
}

// --- 2. То же самое на стыке кольца ------------------------------------------

void testWrapAround()
{
    DelayBuffer b;
    b.prepare (sr, 2, 0.01);
    const int cap = b.getCapacitySamples();

    // Прокручиваем кольцо больше чем на оборот, чтобы голова заехала за стык.
    writeSilence (b, cap * 2 + cap / 2 - 7);

    writeOne (b, 1.0f);
    writeSilence (b, cap - 20);         // импульс уехал почти на весь круг

    const double d = static_cast<double> (cap - 19);
    assert (near (b.read (0, d), 1.0, 1e-6));
    assert (near (b.read (0, d - 1.0), 0.0, 1e-6));
    assert (near (b.read (0, d + 1.0), 0.0, 1e-6));

    // Дробное чтение прямо на стыке: четыре точки лежат по разные стороны склейки,
    // но интерполяция обязана остаться в пределах соседей.
    const double v = b.read (0, d + 0.5);
    assert (v >= -0.6 && v <= 0.6);     // между 0 и 1 с поправкой на выброс Catmull-Rom

    std::puts ("  ok: запись и чтение корректны на границе кольца");
}

// --- 3. Дробное чтение лежит между соседними отсчётами -----------------------

void testFractionalBetweenNeighbours()
{
    DelayBuffer b;
    b.prepare (sr, 1, 0.05);

    // Плавный сигнал: период 1000 сэмплов, между соседями почти прямая.
    for (int i = 0; i < 2000; ++i)
        writeOne (b, static_cast<float> (0.9 * std::sin (2.0 * pi * i / 1000.0)));

    for (int k = 10; k < 900; k += 7)
    {
        const double y0 = b.read (0, k + 1.0);      // «левый» по времени
        const double y1 = b.read (0, static_cast<double> (k));
        const double lo = std::min (y0, y1) - 1e-4;
        const double hi = std::max (y0, y1) + 1e-4;

        for (double f = 0.1; f < 0.95; f += 0.2)
        {
            const double v = b.read (0, k + 1.0 - f);
            assert (v >= lo && v <= hi);
        }
    }

    std::puts ("  ok: дробное чтение лежит между соседними отсчётами");
}

// --- 4. Мусор на входе не роняет и не читает за пределами буфера --------------

void testClamping()
{
    DelayBuffer b;
    b.prepare (sr, 1, 0.02);
    const int cap = b.getCapacitySamples();

    for (int i = 0; i < cap * 3; ++i)
        writeOne (b, static_cast<float> (std::sin (i * 0.01)));

    const double nan = std::nan ("");
    const double inf = std::numeric_limits<double>::infinity();

    for (double d : { -1e9, -1.0, 0.0, 0.5, 1.0, 1.9,
                      static_cast<double> (cap), static_cast<double> (cap) * 10.0,
                      1e300, inf, -inf, nan })
    {
        const float v = b.read (0, d);
        assert (std::isfinite (v));
        assert (std::fabs (v) <= 1.0001f);   // сигнал был в пределах +-1, мусора нет
    }

    // Канал за пределами раскладки — тишина, а не выход за вектор.
    assert (b.read (-1, 100.0) == 0.0f);
    assert (b.read (5, 100.0) == 0.0f);

    // Нижний кламп совпадает с чтением на minDelay = 2.
    assert (b.read (0, 0.0) == b.read (0, 2.0));
    assert (b.read (0, nan) == b.read (0, 2.0));

    std::puts ("  ok: значения вне диапазона клампятся, NaN не читает мусор");
}

// --- 5. Свип: чтение с плывущей задержкой не даёт разрывов --------------------

void testSweepContinuity()
{
    DelayBuffer b;
    b.prepare (sr, 1, 0.2);

    const int n = 20000;
    double maxJump = 0.0, prev = 0.0;

    for (int i = 0; i < n; ++i)
    {
        writeOne (b, static_cast<float> (0.8 * std::sin (2.0 * pi * 220.0 * i / sr)));

        if (i < 4000)
            continue;

        // Задержка гуляет на +-500 сэмплов с частотой 0,5 Гц — классический флэнжер-свип.
        const double d = 2000.0 + 500.0 * std::sin (2.0 * pi * 0.5 * i / sr);
        const double v = b.read (0, d);

        assert (std::isfinite (v) && std::fabs (v) <= 0.9);

        if (i > 4001)
            maxJump = std::max (maxJump, std::fabs (v - prev));

        prev = v;
    }

    // Соседние отсчёты 220 Гц отличаются максимум на ~0,03; порог 0,05 ловит щелчок,
    // но не срабатывает на самом сигнале.
    assert (maxJump < 0.05);
    std::printf ("  ok: свип без разрывов, максимальный скачок %.5f\n", maxJump);
}

// --- 6. Ноль аллокаций вне prepare -------------------------------------------

void testNoAllocations()
{
    DelayBuffer b;
    b.prepare (sr, 2, 0.5);

    std::vector<float> l (512, 0.25f), r (512, -0.25f);
    const float* block[2] = { l.data(), r.data() };

    const long before = allocations;

    for (int i = 0; i < 200; ++i)
    {
        b.write (block, 2, 0, 512);          // целый блок
        b.write (block, 2, 137, 64);         // сегмент между MIDI-событиями

        for (int ch = 0; ch < 2; ++ch)
            for (double d = 100.0; d < 5000.0; d += 333.7)
                (void) b.read (ch, d);
    }

    b.clear();

    assert (allocations == before);
    std::puts ("  ok: ноль аллокаций в write, read и clear");
}

// --- 7. Catmull-Rom против линейной: цифры --------------------------------

/** Гоняет синус через буфер с задержкой d(n) и возвращает N отсчётов
    двумя интерполяциями сразу: Catmull-Rom (штатный read) и линейной,
    собранной из двух целочисленных чтений того же буфера. */
void runInterpolators (double freqBins, int n, double d0, double slope,
                       std::vector<float>& catmull, std::vector<float>& linear)
{
    DelayBuffer b;
    b.prepare (sr, 1, 0.17);            // 8160 + запас -> 8192
    assert (b.getCapacitySamples() == 8192);

    catmull.assign (n, 0.0f);
    linear.assign (n, 0.0f);

    const double w = 2.0 * pi * freqBins / n;
    int phase = 0;

    for (int i = 0; i < 4000; ++i, ++phase)     // прогрев: наполнить историю
        writeOne (b, static_cast<float> (0.5 * std::sin (w * phase)));

    for (int i = 0; i < n; ++i, ++phase)
    {
        writeOne (b, static_cast<float> (0.5 * std::sin (w * phase)));

        const double d = d0 + slope * i;
        catmull[i] = b.read (0, d);

        const double di = std::floor (d);
        const double df = d - di;
        linear[i] = static_cast<float> ((1.0 - df) * b.read (0, di) + df * b.read (0, di + 1.0));
    }
}

void testInterpolationQuality()
{
    constexpr int n = 4096;
    std::vector<float> cr, lin;

    // (а) Статичный дробный сдвиг. Тут обе интерполяции — обычные LTI-фильтры,
    //     поэтому гармоник не порождают вовсе: THD упирается в шум float.
    //     Реальная разница видна в АЧХ, её и меряем.
    std::printf ("\n  Статичный сдвиг d = 2000.5, THD+N (обе интерполяции LTI):\n");
    runInterpolators (85.0, n, 2000.5, 0.0, cr, lin);
    std::printf ("    Catmull-Rom %7.1f dB | линейная %7.1f dB\n",
                 toDb (thdPlusN (cr, 85)), toDb (thdPlusN (lin, 85)));

    std::printf ("  Ошибка усиления на статичном сдвиге (0 dB — идеал):\n");

    for (int bin : { 85, 426, 853, 1280 })      // ~1, 5, 10 и 15 кГц
    {
        runInterpolators (bin, n, 2000.5, 0.0, cr, lin);
        const double gCr  = toDb (binAmplitude (cr,  bin) / 0.5);
        const double gLin = toDb (binAmplitude (lin, bin) / 0.5);

        std::printf ("    %5.0f Hz: Catmull-Rom %+6.2f dB | линейная %+6.2f dB\n",
                     bin * sr / n, gCr, gLin);

        // Запас 0,6 намеренно скромный: у 15 кГц четырёх точек уже не хватает
        // обеим интерполяциям, и выигрыш там всего вдвое, а не на порядок.
        assert (std::fabs (gCr) < std::fabs (gLin) * 0.6);
    }

    // (б) Плывущий сдвиг — то, как буфер реально читается питчером. Здесь система
    //     уже не LTI, и ошибка интерполяции превращается в слышимые гармоники.
    //     slope 0.2 -> скорость чтения 0.8 -> сдвиг вниз на ~3,9 полутона,
    //     основной тон переезжает с бина 85 на 68 ровно, без растекания.
    runInterpolators (85.0, n, 2000.0, 0.2, cr, lin);
    const double thdCr  = thdPlusN (cr,  68);
    const double thdLin = thdPlusN (lin, 68);

    std::printf ("  Плывущий сдвиг (varispeed 0.8x), THD+N:\n");
    std::printf ("    Catmull-Rom %7.1f dB | линейная %7.1f dB | выигрыш %.1f dB\n",
                 toDb (thdCr), toDb (thdLin), toDb (thdLin) - toDb (thdCr));

    assert (thdCr < thdLin / 3.0);       // искажений на varispeed заметно меньше

    std::puts ("  ok: Catmull-Rom лучше линейной и по АЧХ, и по THD");
}

int main()
{
    std::puts ("DelayBuffer:");
    testImpulse();
    testWrapAround();
    testFractionalBetweenNeighbours();
    testClamping();
    testSweepContinuity();
    testNoAllocations();
    testInterpolationQuality();
    std::puts ("\nвсе тесты пройдены");
    return 0;
}
