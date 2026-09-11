#include "VoiceManager.h"
#include "DelayBuffer.h"

#include <algorithm>
#include <cmath>
#include <limits>

void VoiceManager::prepare (double newSampleRate, int maxBlockSamples)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    blockSize = std::max (1, maxBlockSamples);

    for (auto& v : voices)
        v.prepare (sampleRate, blockSize);

    // Единственная аллокация пула: три моно-буфера подряд на все голоса сразу — вход
    // питчера, его выход и выход правого питчера классического ping-pong (#55).
    scratch.assign (static_cast<size_t> (blockSize) * 3u, 0.0f);
    nextAge = 0;
    sustainDown = false;

    // prepare пересоздал движки, а значит сбросил и латч: вернуть выбор параметров.
    for (auto& v : voices)
    {
        v.setEngine (engine);
        v.setFormantHold (formantHold);
    }
}

void VoiceManager::reset()
{
    for (auto& v : voices)
        v.reset();

    pingPongRight = false;
    nextAge = 0;
    sustainDown = false;
}

void VoiceManager::setDelaySamples (double newDelaySamples)
{
    // Только запоминается: раздача звучащим голосам была бы прыжком позиции чтения,
    // то есть склейкой двух кусков кольца встык. Голос забирает это число один раз,
    // в noteOn и в steal. Развилка описана в самой задаче #20: «либо с кроссфейдом,
    // либо на границе следующей ноты» — кроссфейд стоил бы второго питчера на голос,
    // а граница ноты бесплатна и для MIDI-ведомого дилея честнее: хвост, который
    // уже поёт, не обязан перестраиваться под ручку, которую крутят прямо сейчас.
    delaySamples = newDelaySamples;
}

void VoiceManager::setEnvelope (float attackMs, float releaseMs)
{
    const double attack  = sampleRate * static_cast<double> (attackMs)  * 0.001;
    const double release = sampleRate * static_cast<double> (releaseMs) * 0.001;

    for (auto& v : voices)
        v.setEnvelope (attack, release);
}

void VoiceManager::setVoiceLimit (int numVoices)
{
    voiceLimit = std::clamp (numVoices, 1, maxVoices);
}

void VoiceManager::setWidth (float widthPercent)
{
    // 0..200 % на 0..1: на 100 % крайние голоса стоят в половине панорамы, на 200 %
    // упираются в борта. Ширина больше 200 % смысла не имеет — дальше бортов некуда.
    spread = std::clamp (widthPercent, 0.0f, 200.0f) * 0.005f;
}

void VoiceManager::setStereoLayout (StereoLayout newLayout)
{
    if (newLayout == layout)
        return;

    layout = newLayout;

    for (auto& v : voices)
        v.setCrossed (newLayout == StereoLayout::crossed);
}

float VoiceManager::panForSlot (int slot) const
{
    const int half = std::max (1, voiceLimit / 2);
    const int step = (slot + 1) / 2;
    const float sign = (slot % 2) == 1 ? 1.0f : -1.0f;

    return spread * sign * static_cast<float> (step) / static_cast<float> (half);
}

int VoiceManager::findVoiceFor (int)
{
    for (int i = 0; i < voiceLimit; ++i)
        if (! voices[i].isActive())
            return i;

    // Свободных нет. Сначала самый старый голос в release, иначе самый тихий,
    // при равном уровне — снова самый старый (ADR 0001, стратегия кражи).
    int oldestReleasing = -1;
    unsigned oldestReleasingAge = std::numeric_limits<unsigned>::max();
    int quietest = 0;
    float quietestLevel = std::numeric_limits<float>::max();
    unsigned quietestAge = std::numeric_limits<unsigned>::max();

    for (int i = 0; i < voiceLimit; ++i)
    {
        const auto age = voices[i].getAge();

        if (voices[i].isReleasing() && age < oldestReleasingAge)
        {
            oldestReleasingAge = age;
            oldestReleasing = i;
        }

        const float level = voices[i].getEnvelopeLevel();

        if (level < quietestLevel || (level == quietestLevel && age < quietestAge))
        {
            quietestLevel = level;
            quietestAge = age;
            quietest = i;
        }
    }

    return oldestReleasing >= 0 ? oldestReleasing : quietest;
}

void VoiceManager::setEngine (PitchEngine newEngine)
{
    if (newEngine == engine)
        return;

    engine = newEngine;

    for (auto& v : voices)
        v.setEngine (newEngine);
}

void VoiceManager::setFormantHold (bool shouldHold)
{
    if (shouldHold == formantHold)
        return;

    formantHold = shouldHold;

    for (auto& v : voices)
        v.setFormantHold (shouldHold);
}

int VoiceManager::getLatencySamples (PitchEngine which) const
{
    // Голоса одинаковые и подготовлены одним prepare — спрашивать можно любой.
    return voices[0].getLatencySamples (which);
}

void VoiceManager::noteOn (int midiNote, float velocity, float ratio)
{
    if (layout == StereoLayout::pairs)
    {
        // Хор парой (#55, развилка 1б — прототип до вердикта сессии 22): нота поётся двумя
        // голосами по бортам, чуть расстроенными и сдвинутыми, — ширина с первой ноты
        // ценой полифонии. Velocity пополам — это мощность пополам: две некоррелированные
        // половины звучат как один голос, а не на 3 dB громче, и A/B не судит громкость.
        // ponytail: без свободных голосов пара может украсть сама себя и прозвучать одним;
        // если пары выиграют — искать второй слот в обход первого.
        const float detune = std::exp2 (pairDetuneCents / 1200.0f);
        const double offset = pairOffsetMs * 0.001 * sampleRate;

        startVoice (findVoiceFor (midiNote), midiNote, velocity * 0.5f, ratio * detune, delaySamples, -spread);
        startVoice (findVoiceFor (midiNote), midiNote, velocity * 0.5f, ratio / detune, delaySamples + offset, spread);
        return;
    }

    // Тишина проверяется до раздачи слота: после неё голос этой же ноты уже активен.
    const bool silent = std::none_of (voices.begin(), voices.end(),
                                      [] (const Voice& v) { return v.isActive(); });
    const int slot = findVoiceFor (midiNote);

    // Пан приходит не снаружи, а от раскладки и номера слота (#23): кто именно из голосов
    // возьмёт ноту, знает только пул, и снаружи это число взять неоткуда.
    float pan = panForSlot (slot);

    if (layout == StereoLayout::alternate)
    {
        // Ping-pong по нотам: слот ни при чём, сторона чередуется по порядку нот. Нота
        // в тишину — в центр (#55, развилка 2а): иначе одна удержанная нота в Free уходила
        // в левый борт и там оставалась, а с Auto Free звучит ping-pong по умолчанию.
        // Дальше чередование, начиная слева.
        pan = silent ? 0.0f : (pingPongRight ? spread : -spread);
        pingPongRight = ! silent && ! pingPongRight;
    }
    else if (layout == StereoLayout::crossed)
    {
        // Классический ping-pong: сторону задаёт кольцо, а не нота. Левое чтение голоса —
        // на левый край размаха, правое зеркально (Voice::start).
        pan = -spread;
    }

    startVoice (slot, midiNote, velocity, ratio, delaySamples, pan);
}

void VoiceManager::startVoice (int slot, int midiNote, float velocity, float ratio, double delay, float pan)
{
    auto& v = voices[static_cast<size_t> (slot)];
    v.setAge (nextAge++);

    if (v.isActive())
        v.steal (midiNote, velocity, ratio, delay, pan);
    else
        v.noteOn (midiNote, velocity, ratio, delay, pan);
}

void VoiceManager::noteOff (int midiNote)
{
    for (auto& v : voices)
        if (v.isActive() && ! v.isReleasing() && v.getMidiNote() == midiNote)
        {
            if (sustainDown)
                v.setSustained (true);
            else
                v.noteOff();
        }
}

void VoiceManager::allNotesOff()
{
    // Педаль при этом не поднимается, но держать ей уже нечего: гасим всё через release.
    for (auto& v : voices)
        v.noteOff();
}

void VoiceManager::setSustain (bool down)
{
    sustainDown = down;

    if (! down)
        for (auto& v : voices)
            if (v.isSustained())
                v.noteOff();
}

void VoiceManager::process (float* const* out, int numOutChannels, int startSample, int numSamples,
                            const DelayBuffer& source)
{
    if (numSamples <= 0 || numSamples > blockSize)
        return;

    float* scratchIn  = scratch.data();
    float* scratchOut = scratchIn + blockSize;
    float* scratchOutRight = scratchOut + blockSize;

    for (auto& v : voices)
        v.addTo (out, numOutChannels, startSample, numSamples, source, scratchIn, scratchOut, scratchOutRight);
}
