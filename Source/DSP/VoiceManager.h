#pragma once
#include <array>
#include <vector>
#include "Voice.h"

class DelayBuffer;

/** Пул голосов: раздача, кража, рендер сегмента. Память только в prepare. Задача #13. */
class VoiceManager
{
public:
    /** ponytail: 8 хватает фоновому слою. Поднимать до 16 после профилирования (#31),
        а не заранее — стоимость линейна по голосам. */
    static constexpr int maxVoices = 8;

    /** numOutChannels убран из сигнатуры: раскладку голос узнаёт в addTo, дублировать
        её ещё и здесь значило бы держать два источника правды об одном и том же. */
    void prepare (double sampleRate, int maxBlockSamples);
    void reset();

    void setDelaySamples (double delaySamples);   // латентность питчера вычитает голос
    void setEnvelope (float attackMs, float releaseMs);
    void setVoiceLimit (int numVoices);           // параметр Voices, не больше maxVoices

    void noteOn (int midiNote, float velocity, float ratio, float pan);
    void noteOff (int midiNote);
    void allNotesOff();

    /** Педаль сустейна (CC 64): отпущенные клавиши держатся до её подъёма. */
    void setSustain (bool down);

    /** Рендер одного сегмента между MIDI-событиями: каждый живой голос подмешивается в out. */
    void process (float* const* out, int numOutChannels, int startSample, int numSamples,
                  const DelayBuffer& source);

private:
    /** Свободный голос, иначе кража: самый старый в release, иначе самый тихий. */
    Voice& findVoiceFor (int midiNote);

    std::array<Voice, maxVoices> voices;
    std::vector<float> scratch;   // два моно-буфера подряд, общие на все голоса
    unsigned nextAge = 0;
    int blockSize = 0;
    int voiceLimit = maxVoices;
    double sampleRate = 44100.0;
    double delaySamples = 2.0;
    bool sustainDown = false;
};
