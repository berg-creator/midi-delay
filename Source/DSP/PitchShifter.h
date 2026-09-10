#pragma once
#include <memory>

#include "DelayBuffer.h"

/** Какой движок питчинга звучит. Выбирается параметром Quality и только им:
    режим Time Mode на выбор движка не влияет. Короткий движок специально под Follow
    был и оказался браком — замер на живом вокале в ADR 0006. */
enum class PitchEngine { fast, hq };

/** Транспонирование одного моно-голоса. Потоковый: сколько сэмплов подали, столько
    и получили. Абстракция существует ради одной конкретной цели — подменить движок
    на HQ (Signalsmith Stretch) без правок голоса и процессора. См. ADR 0002. */
class PitchShifter
{
public:
    virtual ~PitchShifter() = default;

    /** Вся память выделяется здесь. В аудиопотоке — ни одной аллокации. */
    virtual void prepare (double sampleRate, int maxBlockSamples) = 0;
    virtual void reset() = 0;

    /** 1.0 — без сдвига, 2.0 — октава вверх. Дёргается из аудиопотока, должно быть дёшево. */
    virtual void setRatio (float ratio) = 0;

    /** Держать форманты на месте, пока едет высота (#24). Пустой по умолчанию, и это
        не заглушка «на потом»: varispeed растягивает спектр целиком, формант для него
        физически не существует — держать там нечего. Разница Fast и HQ — это разница
        характера, а не «быстрый и медленный». */
    virtual void setFormantHold (bool) {}

    virtual void process (const float* in, float* out, int numSamples) = 0;

    /** Внутренняя латентность движка в сэмплах. Прячется в delay time (#17), поэтому
        обязана быть постоянной между вызовами prepare() и не зависеть от ratio. */
    virtual int getLatencySamples() const = 0;
};

/** Движок MVP (#14): два ридера в пол-окна, чтение со скоростью ratio, equal-power
    кроссфейд. Окно 50–80 мс, его половина и есть латентность.

    Своё кольцо внутри питчера — цена потокового интерфейса из ADR 0002: снаружи
    такое же кольцо уже есть, но Signalsmith в «читай сам из чужого буфера» не влезает. */
class VarispeedShifter final : public PitchShifter
{
public:
    explicit VarispeedShifter (double windowMs);

    void prepare (double sampleRate, int maxBlockSamples) override;
    void reset() override;
    void setRatio (float ratio) override;
    void process (const float* in, float* out, int numSamples) override;
    int getLatencySamples() const override;

private:
    DelayBuffer ring;        // моно, длиной в окно с запасом на точки интерполяции
    double windowMs = 60.0;
    double window = 2.0;     // длина окна в сэмплах, всегда чётная
    double sweep = 1.0;      // задержка первого ридера внутри окна, [0, window)
    double step = 0.0;       // 1 - ratio: на столько едет sweep за сэмпл
    int latency = 1;         // minDelay + window/2, константа между вызовами prepare
};

/** HQ-движок (#38): Signalsmith Stretch, фазовый вокодер с phase locking.
    MIT, вендорен копией в libs/signalsmith-stretch. Varispeed расстраивает хвост
    неустранимо (ADR 0004), этот — нет: спектр переносится по частоте, а не читается
    с переменной скоростью, и сетки f0 + k/T у него не возникает.

    Библиотека — шаблон на 35 КБ заголовка, поэтому она спрятана в pimpl: тянуть её
    в каждую единицу трансляции ради одного класса дорого по времени сборки, а больше
    её никто не видит. См. ADR 0005. */
class SignalsmithShifter final : public PitchShifter
{
public:
    SignalsmithShifter();
    ~SignalsmithShifter() override;

    void prepare (double sampleRate, int maxBlockSamples) override;
    void reset() override;
    void setRatio (float ratio) override;
    void setFormantHold (bool shouldHold) override;
    void process (const float* in, float* out, int numSamples) override;
    int getLatencySamples() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    int latency = 0;       // inputLatency + outputLatency, константа между вызовами prepare
    float ratio = 1.0f;    // последнее заданное значение: движок дёргаем только на смене
    float tonalityLimit = 0.0f;   // предел транспонирования, доля sample rate
    bool formantHold = true;      // последнее заданное значение, дёргаем только на смене
};

/** Эталон: вход копируется в выход, ratio игнорируется, латентность ноль. Была
    заглушкой на время сборки голосов, осталась мерой — голос на ней обязан быть
    бит-в-бит равен обычному дилею, и это удобная база сравнения для #19. */
class UnityShifter final : public PitchShifter
{
public:
    void prepare (double, int) override {}
    void reset() override {}
    void setRatio (float) override {}
    void process (const float* in, float* out, int numSamples) override
    {
        for (int i = 0; i < numSamples; ++i) out[i] = in[i];
    }
    int getLatencySamples() const override { return 0; }
};
