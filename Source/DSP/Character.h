#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

/** Характер хвоста (#56): Tape и Lo-Fi на входе кольца.

    До сессии 20 всё, что красило хвост, кроме фильтров, стояло на отводе обратной связи,
    и на feedback 0 — а это Chord Pad, в котором плагин открывается, — хвост был чистой
    транспонированной копией. Характер стоит там, где его слышно всегда: после фильтров
    петли, прямо перед записью в кольцо. Первый хвост окрашен один раз — голос читает
    кольцо, — каждый следующий круг ещё раз, как у ленты. Сухой выход идёт мимо кольца
    и не красится. Считается на канал, а не на голос: цена не зависит от числа голосов.

    Обе обработки крутятся всегда, даже в Clean, и подмешиваются множителем — схема
    диффузора и фильтров петли (#45, #22): состояние всегда прогрето сигналом, и ввод
    характера не выталкивает в кольцо звук, застоявшийся в фильтре минуту назад.
    На нулевой глубине множитель ровно ноль, и выход бит-в-бит равен входу. Подмес
    линейный: окрашенный сигнал с чистым сильно коррелирован, и equal-power дал бы горб,
    как в ADR 0003.

    Какой характер подмешан и на какой глубине, решает процессор. Смена характера — это
    глубина старого плавно в ноль, смена, глубина нового из нуля: кроссфейда двух
    окрашенных версий нет нигде, и ADR на него не нужен.

    Без зависимости от JUCE, как Diffuser и DelayBuffer. */
class Character
{
public:
    /** Порядок тот же, что у пунктов параметра character. */
    enum Type { clean = 0, tape = 1, loFi = 2 };

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;

        emphasisCoeff   = onePole (emphasisHz);
        envelopeAttack  = onePole (1000.0 / (twoPi * envelopeAttackMs));
        envelopeRelease = onePole (1000.0 / (twoPi * envelopeReleaseMs));
        loFiLowCoeff    = onePole (loFiLowHz);
        loFiHighCoeff   = onePole (loFiHighHz);
        wowStep         = wowHz / sampleRate;
        flutterStep     = flutterHz / sampleRate;

        clear();
        setSegmentDepth (0.0f);
    }

    /** Всё состояние в ноль, генераторы с начала. Зерно шума фиксировано: прогон после
        prepare повторяется до сэмпла, и провал теста обязан повторяться тоже. */
    void clear()
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            preEmphasis[ch] = deEmphasis[ch] = tapeTone[ch] = 0.0f;
            loFiLow[ch] = loFiHigh[ch] = held[ch] = envelope[ch] = 0.0f;
            saturatorLast[ch] = 0.0;
        }

        wow = flutter = Wander {};
        holdPhase = 0.0;
        noise = noiseSeed;
    }

    /** То, что считается через pow и exp, — раз на сегмент, а не на сэмпл. Глубина едет
        рампой в 50 мс и за сегмент сдвигается на долю рампы, а шаг коэффициента
        однополюсника или выборки-хранения сигнал не рвёт: их состояние непрерывно. */
    void setSegmentDepth (float depth)
    {
        const double d = std::clamp (static_cast<double> (depth), 0.0, 1.0);

        tapeToneCoeff = onePole (tapeBrightHz * std::pow (tapeDullHz / tapeBrightHz, d));
        holdStep = std::min (1.0, loFiTopRateHz * std::pow (loFiBottomRateHz / loFiTopRateHz, d) / sampleRate);
        quantSteps = static_cast<float> (std::pow (2.0, loFiTopBits - 1.0 - (loFiTopBits - loFiBottomBits) * d));
    }

    /** Раз на сэмпл, до цикла каналов, — всё, что у каналов общее. Возвращает добавку
        к смещению чтения петли под wow, в сэмплах. */
    float advance (int type, float depth)
    {
        active = type;
        blend = std::min (1.0f, depth * blendSlope);
        tapeDrive = tapeDriveMin + (tapeDriveMax - tapeDriveMin) * depth;
        loFiDrive = 1.0f + (loFiDriveMax - 1.0f) * depth;
        noiseLevel = depth * (type == loFi ? loFiNoise : tapeNoise);

        // Одна фаза выборки-хранения на оба канала: иначе каналы брали бы отсчёты
        // в разные моменты, и центр поплыл бы.
        holdPhase += holdStep;
        holdNow = holdPhase >= 1.0;

        if (holdNow)
            holdPhase -= 1.0;

        // Wow случайный, а не синус: у ленточных режимов Valhalla модуляция случайная,
        // а дрейф нашей петли (#46) периодический — и это одна из причин «пластиково».
        // Генераторы крутятся всегда; в Clean их вклад умножается на ноль.
        const float slow = wander (wow, wowStep);
        const float fast = wander (flutter, flutterStep);
        const float wowMs     = type == tape ? tapeWowMs     : type == loFi ? loFiWowMs     : 0.0f;
        const float flutterMs = type == tape ? tapeFlutterMs : type == loFi ? loFiFlutterMs : 0.0f;

        // Смещение только в плюс — по той же причине, что у дрейфа #46: знакопеременное
        // упиралось бы в нижний предел кольца на коротком времени.
        return depth * static_cast<float> (sampleRate * 0.001)
             * (wowMs * 0.5f * (slow + 1.0f) + flutterMs * 0.5f * (fast + 1.0f));
    }

    /** Один сэмпл одного канала, после advance. */
    float process (int ch, float x)
    {
        // Огибающая входа — для шума. Шум идёт только по сигналу и с потолком: постоянный
        // шум с входа кольца при feedback 95 % копился бы втрое по амплитуде и держал бы
        // петлю вечно, а шум по огибающей гаснет вместе с хвостом.
        const float magnitude = std::abs (x);
        envelope[ch] += (magnitude > envelope[ch] ? envelopeAttack : envelopeRelease)
                      * (magnitude - envelope[ch]);

        const float hiss = nextNoise() * std::min (envelope[ch] * noiseLevel, noiseCeiling);

        // --- Tape: эмфазис, насыщение, деэмфазис, шипение, темнота ---------------
        // Верх перед насыщением поднят, после — опущен ровно обратным фильтром: в малом
        // сигнале пара прозрачна, а на громком верх упирается раньше низа. Сибилянты
        // сглаживаются, как на ленте, и призвуки насыщения выходят тёмными, а не колючими.
        const float emphasised = (1.0f + emphasisBoost) * x - emphasisBoost * preEmphasis[ch];
        preEmphasis[ch] += emphasisCoeff * (x - preEmphasis[ch]);

        const float saturated = saturate (ch, emphasised);

        const float restored = (saturated + emphasisBoost * deEmphasis[ch]) / (1.0f + emphasisBoost);
        deEmphasis[ch] += emphasisCoeff * (restored - deEmphasis[ch]);

        tapeTone[ch] += tapeToneCoeff * (restored + hiss - tapeTone[ch]);

        // --- Lo-Fi: полоса, жёсткий клип, шум, выборка-хранение, разрядность ------
        // Порядок дешёвого АЦП без антиалиасингового фильтра: низ срезан, клип, выборка
        // на пониженной частоте — и всё, что выше её половины, заворачивается вниз.
        // Алиасинг здесь цель, а не дефект. Верх режется после, как фильтром
        // восстановления: ступеньки выборки сглажены, завёрнутое осталось.
        loFiLow[ch] += loFiLowCoeff * (x - loFiLow[ch]);
        const float clipped = std::clamp ((x - loFiLow[ch]) * loFiDrive, -1.0f, 1.0f) / loFiDrive;

        if (holdNow)
            held[ch] = clipped + hiss;

        // Усечение к нулю, а не округление, и это не мелочь. Округлитель в петле
        // залипает: при feedback 95 % любое значение до десяти шагов округляется само
        // в себя, и хвост не кончается никогда. Усечённый модуль не бывает больше входа,
        // поэтому петля гаснет до настоящего нуля.
        const float crushed = std::trunc (held[ch] * quantSteps) / quantSteps;
        loFiHigh[ch] += loFiHighCoeff * (crushed - loFiHigh[ch]);

        const float coloured = active == tape ? tapeTone[ch] : active == loFi ? loFiHigh[ch] : x;
        return x + blend * (coloured - x);
    }

private:
    /** Случайное блуждание: новая случайная цель раз в период, между целями S-кривая.
        Производная на стыках нулевая — а производная смещения чтения и есть уход высоты,
        так что высота не скачет ступенькой на каждой новой цели. */
    struct Wander { double phase = 0.0; float from = 0.0f, to = 0.0f; };

    float wander (Wander& w, double step) noexcept
    {
        w.phase += step;

        if (w.phase >= 1.0)
        {
            w.phase -= 1.0;
            w.from = w.to;
            w.to = nextNoise();
        }

        const auto t = static_cast<float> (w.phase);
        return w.from + (w.to - w.from) * t * t * (3.0f - 2.0f * t);
    }

    /** Кубическое мягкое насыщение с антипроизводной первого порядка (ADAA). Без неё
        нелинейность на 48 кГц заворачивает призвуки выше Найквиста обратно вниз — та самая
        цифровая жёсткость, от которой Tape и уводит. Цена — полсэмпла задержки и мягкий
        завал верха, у ленты он и так есть. В малом сигнале усиление ровно единица:
        drive двигает порог насыщения, а не громкость. */
    float saturate (int ch, float input) noexcept
    {
        const double x = input;
        const double last = saturatorLast[ch];
        const double g = tapeDrive;
        saturatorLast[ch] = x;

        // На почти равных отсчётах частное теряет точность, и берётся сама функция
        // в середине отрезка — это её же предел.
        if (std::abs (x - last) < 1.0e-5)
            return static_cast<float> (softClip (g * 0.5 * (x + last)) / g);

        return static_cast<float> ((softClipIntegral (g * x) - softClipIntegral (g * last))
                                   / (g * g * (x - last)));
    }

    /** u - u^3/3 до единицы, дальше полка 2/3: склейка гладкая и по значению, и по наклону. */
    static double softClip (double u) noexcept
    {
        return std::abs (u) <= 1.0 ? u - u * u * u / 3.0 : std::copysign (2.0 / 3.0, u);
    }

    static double softClipIntegral (double u) noexcept
    {
        const double a = std::abs (u);
        return a <= 1.0 ? u * u * 0.5 - u * u * u * u / 12.0 : a * 2.0 / 3.0 - 0.25;
    }

    /** xorshift32: белый шум в [-1, 1) без аллокаций и без чужого состояния. */
    float nextNoise() noexcept
    {
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        return static_cast<float> (static_cast<std::int32_t> (noise)) * (1.0f / 2147483648.0f);
    }

    /** a = 1 - exp(-2*pi*f/fs), с клампом: срез за Найквистом на низкой частоте
        дискретизации дал бы a > 1 и раскачку однополюсника. */
    float onePole (double hz) const noexcept
    {
        return static_cast<float> (std::clamp (1.0 - std::exp (-twoPi * std::max (0.0, hz) / sampleRate), 0.0, 1.0));
    }

    static constexpr double twoPi = 6.283185307179586;

    /** Подмес на полную уже к Age 10. Ниже — чистый и окрашенный вперемешку, а у них
        разная фаза, и держать эту зону широкой значило бы держать гребёнку, как у ввода
        диффузора (#45). */
    static constexpr float blendSlope = 10.0f;

    // Tape. Эмфазис +6 dB выше 1,5 кГц. Drive от почти линейного (насыщение с уровня 4)
    // до плотного (с уровня 0,33): Age двигает порог, а не громкость. Полоса темнеет
    // с 16 до 4 кГц — у изношенной ленты верх уходит первым. Wow около 1,5 мс, flutter
    // на порядок мельче. Все числа — стартовые, их выбирает ухо в сессии 21.
    static constexpr double emphasisHz = 1500.0;
    static constexpr float emphasisBoost = 1.0f;
    static constexpr float tapeDriveMin = 0.25f, tapeDriveMax = 3.0f;
    static constexpr double tapeBrightHz = 16000.0, tapeDullHz = 4000.0;
    static constexpr float tapeWowMs = 1.5f, tapeFlutterMs = 0.08f;

    // Lo-Fi. Полоса 200 Гц - 4 кГц, как у дешёвого сэмплера или радио. Разрядность
    // с 16 до 6 бит и частота выборки с 48 до 2 кГц — к Age 50 это 11 бит и около 10 кГц.
    // Клип без сглаживания: алиасинг тут цель. Wow глубокий и хаотический.
    static constexpr double loFiLowHz = 200.0, loFiHighHz = 4000.0;
    static constexpr float loFiDriveMax = 4.0f;
    static constexpr double loFiTopRateHz = 48000.0, loFiBottomRateHz = 2000.0;
    static constexpr double loFiTopBits = 16.0, loFiBottomBits = 6.0;
    static constexpr float loFiWowMs = 3.5f, loFiFlutterMs = 0.3f;

    // Шум: доля огибающей на полной глубине и потолок -40 dBFS. Огибающая быстрая на атаке
    // и медленная на спаде — шум не дёргается на каждом периоде голоса.
    static constexpr float tapeNoise = 0.02f, loFiNoise = 0.04f, noiseCeiling = 0.01f;
    static constexpr double envelopeAttackMs = 2.0, envelopeReleaseMs = 60.0;

    // Случайное блуждание: wow около герца, flutter около восьми.
    static constexpr double wowHz = 1.1, flutterHz = 8.0;
    static constexpr std::uint32_t noiseSeed = 0x9e3779b9u;

    double sampleRate = 44100.0;
    float emphasisCoeff = 1.0f, envelopeAttack = 1.0f, envelopeRelease = 1.0f;
    float loFiLowCoeff = 1.0f, loFiHighCoeff = 1.0f, tapeToneCoeff = 1.0f;
    double wowStep = 0.0, flutterStep = 0.0, holdStep = 1.0, holdPhase = 0.0;
    float quantSteps = 32768.0f;

    // Снимается в advance, читается в process: общее на оба канала в этом сэмпле.
    int active = clean;
    float blend = 0.0f, tapeDrive = tapeDriveMin, loFiDrive = 1.0f, noiseLevel = 0.0f;
    bool holdNow = true;

    // Шины не бывают шире стерео — см. isBusesLayoutSupported процессора.
    float preEmphasis[2] {}, deEmphasis[2] {}, tapeTone[2] {};
    float loFiLow[2] {}, loFiHigh[2] {}, held[2] {}, envelope[2] {};
    double saturatorLast[2] {};

    Wander wow, flutter;
    std::uint32_t noise = noiseSeed;
};
