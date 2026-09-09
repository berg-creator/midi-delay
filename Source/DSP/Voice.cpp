#include "Voice.h"
#include "DelayBuffer.h"

#include <algorithm>
#include <cmath>

namespace
{
    /** Fade-out при краже. 5 мс — верх диапазона из ADR 0001: короче начинает щёлкать
        на низах, длиннее слышно как проглоченную ноту. */
    constexpr double stealFadeMs = 5.0;

    constexpr float pi = 3.14159265358979323846f;
    constexpr float sqrt2 = 1.41421356237309504880f;
}

void Voice::prepare (double sampleRate, int maxBlockSamples)
{
    const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;

    // Единственная строка, которую поменяет #14: обвязка голоса от движка не зависит.
    shifter = std::make_unique<UnityShifter>();
    shifter->prepare (sr, std::max (1, maxBlockSamples));

    stealSamples = std::max (1.0, sr * stealFadeMs * 0.001);
    setEnvelope (sr * 0.01, sr * 0.3);
    reset();
}

void Voice::reset()
{
    stage = Stage::idle;
    level = 0.0f;
    peak = 0.0f;
    step = 0.0f;
    note = -1;
    pendingNote = -1;
    sustained = false;

    if (shifter != nullptr)
        shifter->reset();
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
    // У UnityShifter она нулевая, так что сейчас это тождество — но #17 получает её даром.
    const double latency = shifter != nullptr ? shifter->getLatencySamples() : 0;
    readOffset = std::max (0.0, newDelaySamples - latency);
}

void Voice::setAge (unsigned newAge) { age = newAge; }

void Voice::start (int midiNote, float velocity, float ratio, float pan)
{
    note = midiNote;
    peak = std::clamp (velocity, 0.0f, 1.0f);
    step = static_cast<float> (peak / attackSamples);
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
    setDelaySamples (newDelaySamples);
    level = 0.0f;

    if (shifter != nullptr)
        shifter->reset();

    start (midiNote, velocity, ratio, pan);
}

void Voice::noteOff()
{
    if (stage == Stage::idle || stage == Stage::release || stage == Stage::stealing)
        return;

    sustained = false;
    stage = Stage::release;
    step = static_cast<float> (level / releaseSamples);   // спад ровно за release, с любого уровня
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
    step = static_cast<float> (std::max (level, 1.0e-6f) / stealSamples);
}

void Voice::setSustained (bool shouldHold) { sustained = shouldHold; }
bool Voice::isSustained() const            { return sustained; }

bool Voice::isActive() const        { return stage != Stage::idle; }
bool Voice::isReleasing() const     { return stage == Stage::release || stage == Stage::stealing; }
float Voice::getEnvelopeLevel() const { return level; }
unsigned Voice::getAge() const      { return age; }
int Voice::getMidiNote() const      { return note; }

float Voice::nextEnvelope()
{
    switch (stage)
    {
        case Stage::attack:
            level += step;
            if (level >= peak) { level = peak; stage = Stage::sustain; }
            break;

        case Stage::release:
        case Stage::stealing:
            level -= step;
            if (level <= 0.0f)
            {
                level = 0.0f;

                if (stage == Stage::stealing && pendingNote >= 0)
                {
                    // Голос перезапускается сам, посреди сегмента. Питчер при этом не
                    // сбрасывается: огибающая здесь ровно ноль, разрыва не слышно,
                    // а ratio доедет к следующему сегменту.
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
    for (int k = 0; k < numSamples; ++k)
    {
        const double d = readOffset + static_cast<double> (numSamples - k);
        float s = 0.0f;

        for (int ch = 0; ch < numSourceChannels; ++ch)
            s += source.read (ch, d);

        scratchIn[k] = s * sourceScale;
    }

    shifter->process (scratchIn, scratchOut, numSamples);

    for (int k = 0; k < numSamples; ++k)
    {
        const float s = scratchOut[k] * nextEnvelope();

        for (int ch = 0; ch < numOutChannels; ++ch)
            out[ch][startSample + k] += s * gain[ch < 2 ? ch : 1];
    }
}
