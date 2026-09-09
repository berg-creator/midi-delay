#include "Voice.h"
#include "DelayBuffer.h"

#include <algorithm>
#include <cmath>

namespace
{
    /** Fade-out при краже. 5 мс — верх диапазона из ADR 0001: короче начинает щёлкать
        на низах, длиннее слышно как проглоченную ноту. */
    constexpr double stealFadeMs = 5.0;

    /** Окно varispeed, оно же удвоенная латентность движка. Константа, а не параметр:
        латентность обязана быть неизменной между вызовами prepare (ADR 0002).

        240 мс, а не 50-80 из ANALYSIS §5, и это измеренное решение, а не вкус.
        Varispeed расстраивает хвост тем сильнее, чем короче окно: сдвиг ложится на
        сетку шага |1 - ratio| / окно, и на 60 мс средняя расстройка на ±7 полутонах
        доходила до 24 центов, а в худшей точке до 257. На 240 мс те же цифры — 4-6
        и 46. Позволить себе такое окно может только этот плагин: латентность здесь
        прячется в delay time (ANALYSIS §5), а не платится задержкой хоста.
        Цена — минимальный осмысленный delay time вырос до 120 мс. Разбор механизма
        в prompts/PROGRESS.md, находки сессии 08. */
    constexpr double pitchWindowMs = 240.0;

    /** Окно движка режима Follow. Там дилея нет, прятать латентность некуда, и она
        репортится хосту — значит должна быть маленькой (ADR 0006). Секунды, потому
        что окно у Signalsmith и есть вся его латентность.

        0,09, а не меньше: на 0,06 худшая расстройка доходит до 58 центов, а в Follow
        хвост звучит одновременно с сухим сигналом и обязан стоять с ним в унисон.
        Замер и пороги — test_pitch_shifter, проверка 7. */
    constexpr float followWindowSeconds = 0.09f;

    /** Заливка окна питчера идёт порциями через стек: одна виртуальная process()
        на сэмпл обошлась бы в полторы тысячи вызовов на каждую ноту. */
    constexpr int primeChunk = 64;

    constexpr float pi = 3.14159265358979323846f;
    constexpr float sqrt2 = 1.41421356237309504880f;
}

void Voice::prepare (double sampleRate, int maxBlockSamples)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;

    // Обвязка голоса от движка не зависит: UnityShifter остаётся эталоном в тестах,
    // на нём голос обязан быть бит-в-бит равен обычному дилею.
    const int block = std::max (1, maxBlockSamples);

    fastShifter = std::make_unique<VarispeedShifter> (pitchWindowMs);
    fastShifter->prepare (sr, block);

    hqShifter = std::make_unique<SignalsmithShifter>();
    hqShifter->prepare (sr, block);

    followShifter = std::make_unique<SignalsmithShifter> (followWindowSeconds);
    followShifter->prepare (sr, block);

    shifter = engineFor (wantEngine);

    stealSamples = std::max (1.0, sr * stealFadeMs * 0.001);
    setEnvelope (sr * 0.01, sr * 0.3);
    reset();
}

void Voice::reset()
{
    stage = Stage::idle;
    level = 0.0f;
    peak = 0.0f;
    startLevel = 0.0f;
    phase = 0.0f;
    step = 0.0f;
    note = -1;
    pendingNote = -1;
    sustained = false;
    needsPrime = false;

    // Все: неактивный движок тоже держит окно истории, и оставить его грязным значило бы
    // выдать чужой хвост при следующем переключении Quality или режима.
    if (fastShifter   != nullptr) fastShifter->reset();
    if (hqShifter     != nullptr) hqShifter->reset();
    if (followShifter != nullptr) followShifter->reset();
}

PitchShifter* Voice::engineFor (PitchEngine engine) const
{
    switch (engine)
    {
        case PitchEngine::fast:   return fastShifter.get();
        case PitchEngine::follow: return followShifter.get();
        case PitchEngine::hq:
        default:                  return hqShifter.get();
    }
}

void Voice::setEngine (PitchEngine engine)
{
    wantEngine = engine;

    // Молчащий голос переключается сразу, звучащий — доигрывает на своём движке.
    // Иначе пришлось бы посреди ноты залить окно нового движка, а латентность у них
    // разная, и позиция чтения уехала бы на живом звуке. Голоса освобождаются на
    // каждом release, так что на слух переключение доезжает за одну ноту.
    if (stage == Stage::idle)
        shifter = engineFor (engine);
}

int Voice::getLatencySamples (PitchEngine engine) const
{
    const PitchShifter* e = engineFor (engine);
    return e != nullptr ? e->getLatencySamples() : 0;
}

void Voice::setEnvelope (double attack, double release)
{
    attackSamples  = std::max (1.0, attack);
    releaseSamples = std::max (1.0, release);
}

void Voice::setDelaySamples (double newDelaySamples)
{
    delaySamples = newDelaySamples;

    // Латентность питчера прячется в delay time, а не репортится хосту (ANALYSIS §5).
    // Процессор держит delay time не ниже латентности выбранного движка (#17), так что
    // в норме здесь ничего не срезается. Кламп остаётся на один случай: Quality
    // переключили с HQ на Fast, предел упал до 120 мс, а голос доигрывает на HQ —
    // его латентность больше нового предела. Он придёт позже заказанного, и это
    // на одну ноту, ровно как и обещано в ADR 0005.
    const double latency = shifter != nullptr ? shifter->getLatencySamples() : 0;
    readOffset = std::max (0.0, newDelaySamples - latency);
}

void Voice::setAge (unsigned newAge) { age = newAge; }

void Voice::start (int midiNote, float velocity, float ratio, float pan)
{
    note = midiNote;

    // Кривая velocity: корень, то есть velocity управляет энергией, а не амплитудой.
    // Мягче линейной, как и просит #16: на velocity 64 хвост садится на 3 dB, а не на 6,
    // и тихая игра не пропадает под сухим сигналом. Полный размах громкости при этом
    // остаётся — от 1.0 до 0.09 на velocity 1. Константа, а не параметр: настраивать
    // тут нечего, у кривой нет свободного числа, а шестнадцатая ручка в списке стоит
    // дороже, чем разница между sqrt и чем-нибудь ещё мягким.
    peak = std::sqrt (std::clamp (velocity, 0.0f, 1.0f));

    level = 0.0f;
    phase = 0.0f;
    step = static_cast<float> (1.0 / attackSamples);
    stage = Stage::attack;
    sustained = false;
    pendingNote = -1;

    // Равномощный пан, нормированный на единицу в центре: без sqrt2 голос в центре
    // сел бы на 3 dB тише моно-суммы, из которой он и собран. Разводка по ширине — #23.
    const float angle = 0.25f * pi * (std::clamp (pan, -1.0f, 1.0f) + 1.0f);
    gain[0] = std::cos (angle) * sqrt2;
    gain[1] = std::sin (angle) * sqrt2;

    if (shifter != nullptr)
        shifter->setRatio (ratio);
}

void Voice::noteOn (int midiNote, float velocity, float ratio, double newDelaySamples, float pan)
{
    // Латч движка: только здесь, до расчёта позиции чтения — она считается
    // от латентности активного движка, а у движков она разная.
    shifter = engineFor (wantEngine);

    setDelaySamples (newDelaySamples);

    if (shifter != nullptr)
        shifter->reset();

    // Окно питчера сейчас пустое, залить его нечем: источник знает только addTo.
    needsPrime = true;

    start (midiNote, velocity, ratio, pan);
}

void Voice::noteOff()
{
    if (stage == Stage::idle || stage == Stage::release || stage == Stage::stealing)
        return;

    sustained = false;
    stage = Stage::release;

    // Спад ровно за release с любого уровня, в том числе с недобранного очень короткой
    // нотой: фаза едет от единицы к нулю, а форму даёт кривая в nextEnvelope.
    startLevel = level;
    phase = 1.0f;
    step = static_cast<float> (1.0 / releaseSamples);
}

void Voice::steal (int midiNote, float velocity, float ratio, double newDelaySamples, float pan)
{
    pendingNote = midiNote;
    pendingVelocity = velocity;
    pendingRatio = ratio;
    pendingPan = pan;
    delaySamples = newDelaySamples;

    sustained = false;
    stage = Stage::stealing;
    startLevel = level;
    phase = 1.0f;
    step = static_cast<float> (1.0 / stealSamples);
}

void Voice::setSustained (bool shouldHold) { sustained = shouldHold; }
bool Voice::isSustained() const            { return sustained; }

bool Voice::isActive() const        { return stage != Stage::idle; }
bool Voice::isReleasing() const     { return stage == Stage::release || stage == Stage::stealing; }
float Voice::getEnvelopeLevel() const { return level; }
unsigned Voice::getAge() const      { return age; }
int Voice::getMidiNote() const      { return note; }

/** Форма огибающей (#16). Машина состояний та же, что была; изменилось только то,
    что едет по прямой — теперь это фаза фронта 0..1, а уровень получается из неё кривой.
    Цена — один множитель на сэмпл, зато оба фронта попадают в свои концы точно:
    атака ровно в peak, спад ровно в ноль, и голос гарантированно освобождается.

    Атака вогнутая, 1-(1-p)^2: у прямой на верхушке излом, и на низах он слышен щелчком
    при коротких атаках — как раз там, где #16 требует обратного. Спад выпуклый, p^2:
    прямая обрывает хвост на последних миллисекундах, а квадрат уходит в ноль с нулевым
    наклоном, то есть так, как затухает всё остальное в этом плагине. */
float Voice::nextEnvelope()
{
    switch (stage)
    {
        case Stage::attack:
            phase += step;

            if (phase >= 1.0f) { phase = 1.0f; level = peak; stage = Stage::sustain; }
            else               { level = peak * (2.0f - phase) * phase; }

            break;

        case Stage::release:
        case Stage::stealing:
            phase -= step;

            if (phase > 0.0f)
            {
                // Кража — линейный fade-out 5 мс: она обязана быть незаметной, а не
                // красивой, и на такой длине кривая только растянула бы её хвост (ADR 0001).
                level = stage == Stage::release ? startLevel * phase * phase
                                                : startLevel * phase;
            }
            else
            {
                phase = 0.0f;
                level = 0.0f;

                if (stage == Stage::stealing && pendingNote >= 0)
                {
                    // Голос перезапускается сам, посреди сегмента. Питчер при этом не
                    // сбрасывается, и с varispeed это уже не удобство, а необходимость:
                    // его окно хранит валидную историю по тому же readOffset, а сброс
                    // открыл бы 30 мс тишины, залить которые отсюда нечем — источника
                    // здесь нет. По той же причине здесь не меняется и движок: смена
                    // Quality доедет до этого голоса со следующей ноты с чистого листа.
                    // Огибающая тут ровно ноль, ratio доедет к сегменту.
                    setDelaySamples (delaySamples);
                    start (pendingNote, pendingVelocity, pendingRatio, pendingPan);
                }
                else
                {
                    stage = Stage::idle;
                    note = -1;
                }
            }
            break;

        case Stage::idle:
        case Stage::sustain:
        default:
            break;
    }

    return level;
}

void Voice::addTo (float* const* out, int numOutChannels, int startSample, int numSamples,
                   const DelayBuffer& source, float* scratchIn, float* scratchOut)
{
    if (stage == Stage::idle || numSamples <= 0 || shifter == nullptr)
        return;

    const int numSourceChannels = source.getNumChannels();
    const float sourceScale = numSourceChannels > 0 ? 1.0f / static_cast<float> (numSourceChannels) : 0.0f;

    // Голос читает моно-сумму: питчер у него один, стерео он делает паном (ADR 0001).
    const auto readMono = [&source, numSourceChannels, sourceScale] (double d)
    {
        float s = 0.0f;

        for (int ch = 0; ch < numSourceChannels; ++ch)
            s += source.read (ch, d);

        return s * sourceScale;
    };

    // Заливка окна питчера историей, которая предшествует первому сэмплу сегмента.
    // Без неё нота открывалась бы тишиной длиной в латентность движка — 30 мс.
    // Выход выбрасывается: он и есть та самая тишина.
    if (needsPrime)
    {
        float primeIn[primeChunk], primeOut[primeChunk];

        for (int j = shifter->getLatencySamples(); j > 0; )
        {
            const int n = std::min (j, primeChunk);

            for (int k = 0; k < n; ++k)
                primeIn[k] = readMono (readOffset + static_cast<double> (numSamples + j - k));

            shifter->process (primeIn, primeOut, n);
            j -= n;
        }

        needsPrime = false;
    }

    for (int k = 0; k < numSamples; ++k)
        scratchIn[k] = readMono (readOffset + static_cast<double> (numSamples - k));

    shifter->process (scratchIn, scratchOut, numSamples);

    for (int k = 0; k < numSamples; ++k)
    {
        const float s = scratchOut[k] * nextEnvelope();

        for (int ch = 0; ch < numOutChannels; ++ch)
            out[ch][startSample + k] += s * gain[ch < 2 ? ch : 1];
    }
}
