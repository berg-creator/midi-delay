#pragma once

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

    virtual void process (const float* in, float* out, int numSamples) = 0;

    /** Внутренняя латентность движка в сэмплах. Прячется в delay time (#17), поэтому
        обязана быть постоянной между вызовами prepare() и не зависеть от ratio. */
    virtual int getLatencySamples() const = 0;
};

/** Движок MVP: два ридера в пол-окна, чтение со скоростью ratio, equal-power кроссфейд.
    Окно 50–80 мс, оно же латентность. Реализация — задача #14. */
class VarispeedShifter final : public PitchShifter
{
public:
    explicit VarispeedShifter (double windowMs);

    void prepare (double sampleRate, int maxBlockSamples) override;
    void reset() override;
    void setRatio (float ratio) override;
    void process (const float* in, float* out, int numSamples) override;
    int getLatencySamples() const override;
};

/** Заглушка на время M2: вход копируется в выход, ratio игнорируется, латентность ноль.
    Нужна не «для будущего», а чтобы голоса, огибающие и кража собирались и проверялись
    до появления настоящего движка. Голос на ней звучит как обычный дилей.
    Задача #14 меняет одну строку в Voice::prepare. */
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
