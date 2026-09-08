#pragma once
#include <memory>
#include "PitchShifter.h"

class DelayBuffer;

/** Одна звучащая MIDI-нота: своя позиция чтения, ratio, огибающая, пан, свой питчер.
    Голос ничего не знает про feedback — feedback берётся до питч-стадии и живёт
    в процессоре (ADR 0001). */
class Voice
{
public:
    /** Создаёт питчер и выделяет его память. Только не из аудиопотока. */
    void prepare (double sampleRate, int maxBlockSamples);

    /** delaySamples — позиция чтения, из которой латентность питчера уже вычтена (#17). */
    void noteOn (int midiNote, float velocity, float ratio, double delaySamples, float pan);
    void noteOff();

    /** Кража голоса (#13): fade-out 5 мс, после него голос сам перезапустится
        с параметрами pending-ноты. Щелчка нет, отдельной очереди нет. */
    void steal (int midiNote, float velocity, float ratio, double delaySamples, float pan);

    bool isActive() const;
    bool isReleasing() const;
    float getEnvelopeLevel() const;   // для выбора самого тихого при краже
    unsigned getAge() const;          // порядковый номер запуска, меньше — старше
    int getMidiNote() const;

    /** Push: голос подмешивает себя в out, а не отдаёт свой блок наружу (ADR 0001).
        scratchIn/scratchOut — общие на все голоса моно-буферы длиной maxBlockSamples,
        владелец VoiceManager. startSample/numSamples режут блок по MIDI-событиям. */
    void addTo (float* const* out, int numOutChannels, int startSample, int numSamples,
                const DelayBuffer& source, float* scratchIn, float* scratchOut);

private:
    std::unique_ptr<PitchShifter> shifter;
};
